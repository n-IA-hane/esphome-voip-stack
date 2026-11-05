"""Real hold/reINVITE/ACK owners feed the RTP worker and jitter after resume."""
import json
import subprocess

import pytest

from test_audio_hold_runtime import hold_source
from test_refer_dialog_runtime import ROOT, block
from test_rtp_offer_reservation_runtime import reservation_binary as reservation_binary


def resume_source():
    component = ROOT / 'esphome/components/voip_stack'
    transport = (component / 'sip_transport.cpp').read_text()
    header = (component / 'sip_transport.h').read_text()
    parser = (component / 'sip_message.cpp').read_text()
    code = hold_source().split('\nint main(', 1)[0]
    code = code.replace('uint32_t millis(){return 100;}',
                        'std::atomic<uint32_t> clock_ms{100};uint32_t millis(){return clock_ms.load();}')
    code = '#include <atomic>\n#include <thread>\n#include <chrono>\n#include <cinttypes>\n' + code
    code = code.replace('struct RetainedCall {', r'''
    constexpr uint32_t SIP_T1_MS=500,SIP_TRANSACTION_TIMEOUT_MS=32000;
    constexpr uint16_t SIP_T2_MS=4000;
    ''' + '\n'.join(block(parser, marker) for marker in
                     ['bool time_reached(', 'std::string cseq_method(', 'std::string via_branch('])
                        + '\nstruct RetainedCall {')
    completed = block(header, 'struct CompletedServerTransaction {')
    code = code.replace('struct{bool awaiting_ack=false;}completed_invite_;',
                        completed + ';CompletedServerTransaction completed_invite_,completed_control_;')
    extra = r'''
     std::mutex dialog_mutex_;
     std::atomic<bool>rtp_task_terminate_{false},rtp_task_quiesced_{false},rtp_ssrc_latched_{false};
     std::atomic<uint32_t>latched_rtp_ip_v4_{0},latched_rtp_ssrc_{0},rtp_rx_packets_{0},rtp_rx_bytes_{0},rtp_rx_datagrams_{0};
     std::atomic<uint16_t>latched_rtp_port_{0};
     void*rtp_cleanup_done_=nullptr,*rtp_task_done_=nullptr;
     std::atomic<unsigned>delivered{0};unsigned quiescent=0;
     void emit_audio_frame_(const uint8_t*,size_t,uint16_t,uint32_t,bool,uint32_t);
     void rtp_task_();void close_media_session_(){media_active_=false;}
     void finish_audio_path_stop_(){}void emit_media_quiesced_(){++quiescent;}
     bool send_bye_unlocked_(const std::string&){assert(false);return false;}
     void reset_dialog_(){assert(false);}
     bool dispatch_is_udp_()const{return true;}void wake_sip_task_(){}
     std::string current_request;
     uint32_t current_peer_ip=0;uint16_t current_peer_port=0;
     int sip_server_=-1;sockaddr_in sip_peer_{};
     void remember_completed_response_(const std::string&,uint32_t,uint16_t,const std::string&,const std::string&);
     uint16_t acknowledge_completed_invite_(const std::string&,const sockaddr_in&,bool* =nullptr);
    '''
    code = code.replace('struct SipTransport {', 'struct SipTransport {' + extra)
    code = code.replace('void reset_rtp_latch_(){++latch_resets;}', 'void reset_rtp_latch_();')
    old = '++finals;status=s;answer=body;if(on_final)on_final();return !fail_send;'
    replacement = r'''++finals;status=s;answer=body;if(on_final)on_final();if(fail_send)return false;
     const std::string response="SIP/2.0 "+std::to_string(s)+" OK\r\nFrom: "+header_value(current_request,"From")+
      "\r\nTo: "+header_value(current_request,"To")+"\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
     remember_completed_response_(current_request,current_peer_ip,current_peer_port,"INVITE",response);
     return sendto(sip_server_,response.data(),response.size(),0,reinterpret_cast<const sockaddr*>(&sip_peer_),sizeof(sip_peer_))==ssize_t(response.size());'''
    assert old in code
    code = code.replace(old, replacement)
    for marker in ['void SipTransport::reset_rtp_latch_(',
                   'void SipTransport::remember_completed_response_(',
                   'uint16_t SipTransport::acknowledge_completed_invite_(']:
        code += '\n' + block(transport, marker)
    code += r'''
    #define USE_ESP32
    #include "esphome/components/voip_stack/rtp_jitter_buffer.h"
    #include "esphome/components/voip_stack/transport.h"
    #undef USE_ESP32
    using esphome::voip_stack::TransportAudioFrame;
    struct VoipStack {
     RtpJitterBuffer*rx_jitter_buffer_=nullptr;size_t rx_jitter_frame_alloc_bytes_=320;
     void*rx_task_handle_=nullptr;std::atomic<unsigned>media_rx_queue_depth_{0},media_rx_queue_drops_{0};
     AudioFormat get_current_rx_audio_format_(){AudioFormat f;f.frame_ms=10;return f;}
     void enqueue_rx_frame_(const TransportAudioFrame&);
    };
    VoipStack*app=nullptr;
    std::vector<bool>source_changes;
    void SipTransport::emit_audio_frame_(const uint8_t*p,size_t bytes,uint16_t seq,uint32_t stamp,bool changed,uint32_t format){
     app->enqueue_rx_frame_({p,bytes,seq,stamp,true,changed,format});source_changes.push_back(changed);
     delivered.fetch_add(1,std::memory_order_release);
    }
    constexpr int pdTRUE=1;constexpr unsigned portMAX_DELAY=~0U;
    unsigned ulTaskNotifyTake(int,unsigned){return 1;}
    void xTaskNotifyGive(void*){}void xSemaphoreGive(void*){}void vTaskDelete(void*){}
    const char*socket_errno_name(int){return "host";}const char*socket_errno_text(int){return "host";}
    #undef USE_ESPHOME_VOIP_DTMF
    #define VOIP_STARTUP_EVENT(...) ((void)0)
    '''
    code += '\n' + block(parser, 'size_t rtp_payload_to_pcm(')
    code += '\n' + block(transport, 'void SipTransport::rtp_task_(')
    code += '\n' + block((component / 'voip_audio.cpp').read_text(), 'void VoipStack::enqueue_rx_frame_(')
    code += r'''
    int main(int argc,char**argv){
     assert(argc==2);const std::string mode=argv[1];SipTransport t;AudioFormat pcm;pcm.sample_rate=16000;pcm.frame_ms=10;
     const bool initial_preference=mode=="initial-preference";
     const bool wide_rx=mode=="resume-wide-rx";
     const bool same_source=mode.find("same-source")!=std::string::npos;
     const bool change_port=mode.find("port-change")!=std::string::npos;
     const bool bad_ip=mode.find("bad-ip")!=std::string::npos;
     const bool bad_pt=mode.find("bad-pt")!=std::string::npos;
     const bool bad_size=mode.find("bad-size")!=std::string::npos;
     const bool no_resume=mode.rfind("no-resume",0)==0;
     const bool ptime_case=mode.rfind("resume-",0)==0||initial_preference;
     const bool omitted=mode!="resume-explicit-ptime"&&mode!="resume-explicit20";
     const bool extra=mode.rfind("extra-reinvite",0)==0;
     t.offer_tx_formats_.count=ptime_case?2:1;
     t.offer_tx_formats_.formats[0]=pcm;if(ptime_case){t.offer_tx_formats_.formats[0].frame_ms=20;t.offer_tx_formats_.formats[1]=pcm;}
     auto receive=pcm;if(wide_rx)receive.sample_rate=48000;
     t.offer_rx_formats_.count=wide_rx?2:1;t.offer_rx_formats_.formats[0]=receive;
     if(wide_rx)t.offer_rx_formats_.formats[1]=pcm;t.remote_ip_v4_=INADDR_LOOPBACK;
     t.rtp_socket_=socket(AF_INET,SOCK_DGRAM,0);int peer=socket(AF_INET,SOCK_DGRAM,0);assert(t.rtp_socket_>=0&&peer>=0);
     bool wrong_ip=false;
     auto bind_port=[&](int fd){sockaddr_in p{};p.sin_family=AF_INET;p.sin_addr.s_addr=htonl(wrong_ip?0x7f000002:INADDR_LOOPBACK);
      assert(bind(fd,reinterpret_cast<sockaddr*>(&p),sizeof(p))==0);socklen_t n=sizeof(p);assert(getsockname(fd,reinterpret_cast<sockaddr*>(&p),&n)==0);return p;};
     const auto target=bind_port(t.rtp_socket_);auto source=bind_port(peer);
     const int sip_peer=socket(AF_INET,SOCK_DGRAM,0);t.sip_server_=socket(AF_INET,SOCK_DGRAM,0);assert(sip_peer>=0&&t.sip_server_>=0);
     const auto sip_server=bind_port(t.sip_server_);t.sip_peer_=bind_port(sip_peer);
     auto wire_request=[&](const std::string&message){
      assert(sendto(sip_peer,message.data(),message.size(),0,reinterpret_cast<const sockaddr*>(&sip_server),sizeof(sip_server))==ssize_t(message.size()));
      char received[8192];sockaddr_in from{};socklen_t length=sizeof(from);const auto bytes=recvfrom(t.sip_server_,received,sizeof(received),0,reinterpret_cast<sockaddr*>(&from),&length);
      assert(bytes==ssize_t(message.size())&&std::string(received,bytes)==message&&from.sin_port==t.sip_peer_.sin_port);
      return std::string(received,bytes);
     };
     auto close_sip=[&](){close(sip_peer);close(t.sip_server_);};
     std::string initial="v=0\r\no=peer 7 1 IN IP4 127.0.0.1\r\nc=IN IP4 127.0.0.1\r\nm=audio "+std::to_string(ntohs(source.sin_port))+
      (wide_rx?" RTP/AVP 96 97\r\na=rtpmap:96 L16/48000/1\r\na=x-voip-stack-flow:96 send\r\n"
       "a=rtpmap:97 L16/16000/1\r\na=x-voip-stack-flow:97 recv\r\na=x-voip-stack-independent-ptime:1\r\n":
       " RTP/AVP 96\r\na=rtpmap:96 L16/16000/1\r\n")+
      std::string(initial_preference?"":"a=ptime:10\r\n")+"a=sendrecv\r\n";
     assert(t.learn_remote_rtp_from_sdp_(initial,INADDR_LOOPBACK)&&t.rx==receive);
     if(initial_preference){assert(t.tx.frame_ms==20);close_sip();close(peer);close(t.rtp_socket_);
      std::cout<<"{\"tx_ms_after\":20,\"rx_ms_after\":10}\n";return 0;}
     assert(t.tx==pcm);
     const auto before_payload=t.accepted;const auto before_fd=t.rtp_socket_;const auto before_call=t.call_id_;
     auto request=[&](const std::string&method,const std::string&body,unsigned cseq){
      return method+" sip:esp@127.0.0.1 SIP/2.0\r\nCall-ID: "+t.call_id_+
       "\r\nFrom: <sip:peer@127.0.0.1>;tag=remote\r\nTo: <sip:esp@127.0.0.1>;tag=local\r\nCSeq: "+std::to_string(cseq)+" "+method+
       "\r\nVia: SIP/2.0/UDP 127.0.0.1;branch=z9hG4bK-resume-"+std::to_string(cseq)+"-"+method+
       "\r\nContent-Type: application/sdp\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;};
     auto reinvite=[&](std::string body,unsigned cseq){
      const auto origin=body.find("o=peer 7 1 ");if(origin!=std::string::npos)body.replace(origin,11,"o=peer 7 "+std::to_string(cseq)+" ");
      t.current_request=wire_request(request("INVITE",body,cseq));t.current_peer_ip=INADDR_LOOPBACK;t.current_peer_port=ntohs(t.sip_peer_.sin_port);
      assert(!t.completed_invite_.awaiting_ack);const bool okay=t.handle_reinvite_(t.current_request,t.sip_peer_);assert(okay);
      if(t.status==200){assert(t.completed_invite_.awaiting_ack);bool terminate=false;
       char response[8192];const auto bytes=recv(sip_peer,response,sizeof(response),0);assert(bytes>0&&std::string(response,bytes).rfind("SIP/2.0 200 ",0)==0);
       const auto ack=wire_request(request("ACK","",cseq));assert(t.acknowledge_completed_invite_(ack,t.sip_peer_,&terminate)==200);
       assert(!terminate&&!t.completed_invite_.awaiting_ack);
      }return t.status;};
     if(ptime_case){
      std::string held=initial;held.replace(held.find("a=sendrecv"),10,"a=inactive");assert(reinvite(held,2)==200);
      auto resume=initial;if(omitted)resume.erase(resume.find("a=ptime:10\r\n"),12);
      if(mode=="resume-bound10")resume+="a=maxptime:10\r\n";
      if(mode=="resume-bound5")resume+="a=maxptime:5\r\n";
      if(mode=="resume-explicit20")resume.replace(resume.find("a=ptime:10"),10,"a=ptime:20");
      if(mode=="resume-unconfigured10")t.offer_tx_formats_.count=1;
      if(mode=="resume-other-mapping")resume.replace(resume.find("L16/16000"),9,"L16/32000");
      const auto status=reinvite(resume,3);
      assert(t.tx==pcm&&t.rx==receive&&t.accepted==before_payload);
      std::cout<<"{\"mode\":\""<<mode<<"\",\"status\":"<<status<<",\"reason\":\""<<t.reason<<"\",\"tx_ms_after\":"<<unsigned(t.tx.frame_ms)
       <<",\"rx_ms_after\":"<<unsigned(t.rx.frame_ms)<<",\"still_paused\":"<<!t.audio_receive_enabled_<<"}\n";
      close_sip();close(peer);close(t.rtp_socket_);return 0;
     }
     uint8_t storage[16*320]{};RtpJitterBuffer jitter(storage,320,16,1);VoipStack phone;phone.rx_jitter_buffer_=&jitter;app=&phone;
     std::thread worker([&](){t.rtp_task_();});
     auto wait_for=[&](unsigned n){const auto start=std::chrono::steady_clock::now();
      while(t.delivered.load(std::memory_order_acquire)<n&&std::chrono::steady_clock::now()-start<std::chrono::seconds(2))std::this_thread::sleep_for(std::chrono::milliseconds(1));
      assert(t.delivered.load(std::memory_order_acquire)==n);};
     unsigned total=0,played=0,missing=0;uint8_t output[320];
     auto consume=[&](){size_t actual=0;uint16_t seq=0;const auto got=jitter.read(output,sizeof(output),&seq,nullptr,nullptr,&actual);
      if(got==RtpJitterBuffer::ReadResult::FRAME){assert(actual==320);for(unsigned i=0;i<160;++i){uint16_t v;std::memcpy(&v,output+i*2,2);assert(v==uint16_t(seq+i));}++played;}
      if(got==RtpJitterBuffer::ReadResult::MISSING)++missing;clock_ms+=10;};
     bool invalid_phase=false;
     auto packet=[&](uint16_t seq,uint8_t ssrc,uint32_t offset=0,bool admitted=true){uint8_t p[332]{};p[0]=0x80;p[1]=invalid_phase&&bad_pt?97:96;p[2]=seq>>8;p[3]=seq;p[11]=ssrc;const uint32_t ts=uint32_t(seq)*160+offset;
      p[4]=ts>>24;p[5]=ts>>16;p[6]=ts>>8;p[7]=ts;for(unsigned i=0;i<160;++i){const uint16_t v=seq+i;p[12+2*i]=v>>8;p[13+2*i]=v;}
      const auto before_datagrams=t.rtp_rx_datagrams_.load();const size_t bytes=sizeof(p)-(invalid_phase&&bad_size?1:0);
      assert(sendto(peer,p,bytes,0,reinterpret_cast<const sockaddr*>(&target),sizeof(target))==ssize_t(bytes));
      if(admitted){wait_for(++total);}else{
       const auto start=std::chrono::steady_clock::now();
       while(t.rtp_rx_datagrams_==before_datagrams&&std::chrono::steady_clock::now()-start<std::chrono::seconds(2))std::this_thread::sleep_for(std::chrono::milliseconds(1));
       assert(t.rtp_rx_datagrams_==before_datagrams+1);std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }};
     for(unsigned n=0;n<4;++n){packet(500+n,42);consume();}assert(played==4);
     if(!no_resume){
      auto hold=initial;hold.replace(hold.find("a=sendrecv"),10,"a=inactive");assert(reinvite(hold,2)==200&&!t.audio_receive_enabled_);
      for(unsigned n=0;n<20;++n)consume();assert(played==4&&missing==0);
      assert(reinvite(initial,3)==200&&t.audio_receive_enabled_);
     }
     const bool pending_after_resume=t.audio_receive_reset_pending_;
     const bool reacquire_after_resume=t.rtp_source_reacquire_pending_;
     if(extra)assert(reinvite(initial,4)==200);
     const bool pending_before_rtp=t.audio_receive_reset_pending_;const unsigned played_before=played;
     const bool reacquire_before_rtp=t.rtp_source_reacquire_pending_;
     const auto old_port=ntohs(source.sin_port);
     auto rebind=[&](bool bad){const int retired=peer;peer=socket(AF_INET,SOCK_DGRAM,0);assert(peer>=0);
      wrong_ip=bad;source=bind_port(peer);close(retired);};
     if(change_port||bad_ip)rebind(bad_ip);
     unsigned rejected_admitted=0;bool pending_after_rejected=pending_before_rtp,reacquire_after_rejected=reacquire_before_rtp;
     if(bad_ip||bad_pt||bad_size){
      invalid_phase=true;const auto before=t.delivered.load();
      for(unsigned n=0;n<4;++n)packet(90+n,99,0,false);
      rejected_admitted=t.delivered.load()-before;pending_after_rejected=t.audio_receive_reset_pending_;
      reacquire_after_rejected=t.rtp_source_reacquire_pending_;invalid_phase=false;if(bad_ip)rebind(false);
     }
     // A restarted RTP source chooses a new SSRC and an independent sequence.
     // Both control and witness receive the identical new source and packets.
     for(unsigned n=0;n<4;++n){packet(same_source?504+n:100+n,same_source?42:99,same_source?3200:0,!no_resume);if(!no_resume)consume();}
     const auto resumed_admitted=t.rtp_rx_packets_.load();
     const bool reacquire_after_rtp=t.rtp_source_reacquire_pending_,pending_after_rtp=t.audio_receive_reset_pending_;
     // Once the first real packet consumed resume admission, another simultaneous
     // port+SSRC change cannot use the downstream reset as a permanent exemption.
     rebind(false);for(unsigned n=0;n<4;++n)packet(300+n,77,0,false);
     const auto post_admission_rebind_admitted=t.rtp_rx_packets_.load()-resumed_admitted;
     const auto counters=jitter.counters();assert(t.tx==pcm&&t.rx==pcm&&t.accepted==before_payload&&t.call_id_==before_call&&t.rtp_socket_==before_fd);
     t.rtp_running_=false;t.rtp_task_terminate_=true;uint8_t wake=0;sendto(peer,&wake,1,0,reinterpret_cast<const sockaddr*>(&target),sizeof(target));worker.join();
     assert(t.rtp_task_quiesced_&&t.quiescent==1);close_sip();close(peer);close(t.rtp_socket_);
     std::cout<<"{\"mode\":\""<<mode<<"\",\"pending_after_resume\":"<<pending_after_resume<<",\"pending_before_RTP\":"<<pending_before_rtp
      <<",\"reacquire_after_resume\":"<<reacquire_after_resume<<",\"reacquire_before_RTP\":"<<reacquire_before_rtp
      <<",\"rejected_admitted\":"<<rejected_admitted<<",\"pending_after_rejected\":"<<pending_after_rejected<<",\"reacquire_after_rejected\":"<<reacquire_after_rejected
      <<",\"reacquire_after_RTP\":"<<reacquire_after_rtp<<",\"pending_after_RTP\":"<<pending_after_rtp
      <<",\"old_port\":"<<old_port<<",\"new_port\":"<<ntohs(source.sin_port)<<",\"post_admission_rebind_admitted\":"<<post_admission_rebind_admitted
      <<",\"resumed_played\":"<<played-played_before<<",\"late\":"<<counters.late<<",\"RTP_admitted\":"<<t.rtp_rx_packets_
      <<",\"last_four_source_changed\":[";for(unsigned n=4;n<source_changes.size();++n)std::cout<<(n==4?"":",")<<source_changes[n];std::cout<<"]}\n";
    }
    '''
    return code


@pytest.fixture(scope="module")
def resume_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("audio-resume")
    core = folder / "esphome/core"
    core.mkdir(parents=True)
    (core / "defines.h").write_text("#pragma once\n")
    (core / "helpers.h").write_text("#pragma once\n#include <mutex>\nnamespace esphome {using Mutex=std::mutex;using LockGuard=std::lock_guard<Mutex>;}\n")
    cpp = folder / "resume.cpp"
    cpp.write_text(resume_source())
    binary = folder / "resume"
    command = ["g++", "-std=c++17", "-pthread", "-O1", "-I", str(folder), "-I", str(ROOT), str(cpp), "-o", str(binary)]
    built = subprocess.run(command, capture_output=True, text=True, cwd=folder)
    assert built.returncode == 0, built.stderr
    return binary


def result(binary, mode):
    run = subprocess.run([str(binary), mode], capture_output=True, text=True, timeout=8, cwd=binary.parent)
    assert run.returncode == 0, run.stdout + run.stderr
    return json.loads(run.stdout)


@pytest.mark.parametrize("mode", ["control", "extra-reinvite", "control-same-source", "extra-reinvite-same-source"])
def test_resumed_source_reaches_jitter_after_each_distinct_acked_reinvite(resume_binary, mode):
    measured = result(resume_binary, mode)
    assert measured["pending_after_resume"] == 1
    assert measured["pending_before_RTP"] == 1
    assert measured["resumed_played"] == 4
    assert measured["late"] == 0
    assert measured["RTP_admitted"] == 8
    assert measured["last_four_source_changed"] == [1, 0, 0, 0]


@pytest.mark.parametrize("extra", [False, True])
@pytest.mark.parametrize("port_change,same_ssrc", [(False, True), (False, False), (True, True), (True, False)])
def test_resume_reacquires_only_first_valid_negotiated_source(resume_binary, extra, port_change, same_ssrc):
    mode = "extra-reinvite" if extra else "control"
    mode += "-port-change" if port_change else ""
    mode += "-same-source" if same_ssrc else ""
    measured = result(resume_binary, mode)
    assert measured["reacquire_after_resume"] == measured["reacquire_before_RTP"] == 1
    assert measured["resumed_played"] == 4
    assert measured["RTP_admitted"] == 8
    assert measured["late"] == 0
    assert measured["last_four_source_changed"] == [1, 0, 0, 0]
    assert measured["pending_after_RTP"] == measured["reacquire_after_RTP"] == 0
    assert measured["post_admission_rebind_admitted"] == 0


@pytest.mark.parametrize("extra", [False, True])
@pytest.mark.parametrize("invalid", ["bad-ip", "bad-pt", "bad-size"])
def test_rejected_packets_cannot_consume_resume_reacquisition(resume_binary, extra, invalid):
    mode = ("extra-reinvite" if extra else "control") + "-port-change-" + invalid
    measured = result(resume_binary, mode)
    assert measured["rejected_admitted"] == 0
    assert measured["pending_after_rejected"] == measured["reacquire_after_rejected"] == 1
    assert measured["resumed_played"] == 4
    assert measured["RTP_admitted"] == 8
    assert measured["last_four_source_changed"] == [1, 0, 0, 0]
    assert measured["pending_after_RTP"] == measured["reacquire_after_RTP"] == 0
    assert measured["post_admission_rebind_admitted"] == 0


def test_simultaneous_port_and_ssrc_change_without_resume_is_still_rejected(resume_binary):
    measured = result(resume_binary, "no-resume-port-change")
    assert measured["reacquire_after_resume"] == measured["reacquire_before_RTP"] == 0
    assert measured["resumed_played"] == 0
    assert measured["RTP_admitted"] == 4
    assert measured["last_four_source_changed"] == []
    assert measured["post_admission_rebind_admitted"] == 0


@pytest.mark.parametrize("mode", ["resume-explicit-ptime", "resume-omits-ptime", "resume-bound10", "resume-wide-rx"])
def test_same_dialog_resume_preserves_admitted_tx_when_preference_is_absent(resume_binary, mode):
    measured = result(resume_binary, mode)
    assert measured["status"] == 200
    assert measured["tx_ms_after"] == 10
    assert measured["rx_ms_after"] == 10
    assert measured["still_paused"] == 0


@pytest.mark.parametrize("mode", ["resume-explicit20", "resume-bound5", "resume-unconfigured10", "resume-other-mapping"])
def test_retained_tx_cannot_override_explicit_preference_configuration_or_bounds(resume_binary, mode):
    measured = result(resume_binary, mode)
    assert measured["status"] == 488
    assert measured["tx_ms_after"] == measured["rx_ms_after"] == 10
    assert measured["still_paused"] == 1


def test_absent_preference_on_initial_offer_preserves_configured_tx_priority(resume_binary):
    assert result(resume_binary, "initial-preference") == {"tx_ms_after": 20, "rx_ms_after": 10}


def test_dialog_cleanup_and_new_media_preparation_clear_stale_resume_notification(reservation_binary, tmp_path):
    from test_refer_dialog_runtime import SOURCE

    code = reservation_binary.with_suffix(".cpp").read_text().split("\nint main(", 1)[0]
    code = code.replace("void reset_rtp_latch_(){}", "void reset_rtp_latch_();")
    code = code.replace("struct SipTransport: SipPhoneTransport {", r'''struct SipTransport: SipPhoneTransport {
 std::atomic<uint32_t>latched_rtp_ip_v4_{0},latched_rtp_ssrc_{0};
 std::atomic<uint16_t>latched_rtp_port_{0};std::atomic<bool>rtp_ssrc_latched_{false};
 std::atomic<bool>rtp_source_reacquire_pending_{false};
''', 1)
    code += "\n" + block((SOURCE / "sip_transport.cpp").read_text(), "void SipTransport::reset_rtp_latch_(")
    code += r'''
int main(){
 SipTransport s;s.call_id_="completed";s.rtp_port_=0;s.audio_receive_reset_pending_=true;s.rtp_source_reacquire_pending_=true;
 assert(s.prepare_media_path_locked_()&&!s.audio_receive_reset_pending_&&!s.rtp_source_reacquire_pending_);
 assert(s.commit_media_path_locked_());s.audio_receive_reset_pending_=true;s.rtp_source_reacquire_pending_=true;
 s.reset_dialog_();assert(s.call_id_.empty()&&!s.audio_receive_reset_pending_&&!s.rtp_source_reacquire_pending_);
 s.drain_cleanup();
 s.audio_receive_reset_pending_=true;s.rtp_source_reacquire_pending_=true;s.rtp_port_=0;
 assert(s.prepare_media_path_locked_()&&!s.audio_receive_reset_pending_&&!s.rtp_source_reacquire_pending_);
 assert(s.rtp_socket_>=0&&!s.rtp_ssrc_latched_);
 const auto fd=s.rtp_socket_;released(s,fd);
}
'''
    cpp = tmp_path / "reset.cpp"
    cpp.write_text(code)
    binary = tmp_path / "reset"
    built = subprocess.run(["g++", "-std=c++17", "-DUSE_ESPHOME_VOIP_STACK_SPEAKER", "-I", str(ROOT),
                            "-I", str(SOURCE), str(cpp), "-o", str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5, cwd=tmp_path)
    assert run.returncode == 0, run.stdout + run.stderr
