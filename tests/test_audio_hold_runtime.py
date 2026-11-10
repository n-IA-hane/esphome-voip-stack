"""Established audio hold uses the production re-INVITE and RTP owners."""
import os
import shutil
import subprocess

import pytest

from test_inactive_video_reinvite_runtime import source as inactive_video_source
from test_refer_dialog_runtime import ROOT, SOURCE, block


def hold_source():
    source = inactive_video_source().split("\nint main(", 1)[0]
    source = r'''
#include <cerrno>
#include <cstdint>
#include <functional>
#include <sys/socket.h>
#include <unistd.h>
unsigned uxQueueSpacesAvailable(int*){return 8;}
uint32_t millis(){return 100;}
''' + source
    extra = r'''
 mutable std::mutex media_config_lock_,rtp_socket_mutex_;
 std::atomic<bool> rtp_running_{true},refer_media_waiting_{false};
 std::atomic<uint32_t> dtmf_generation_{0},rtp_timestamp_{10000},rtp_tx_packets_{0},rtp_tx_bytes_{0};
 std::atomic<uint16_t> rtp_sequence_{100};uint32_t rtp_ssrc_=1234,rtp_tx_clock_ms_=0,rtp_tx_clock_timestamp_=0;
 int rtp_socket_=-1,queue=0;int*dtmf_queue_=&queue;
 std::function<void()> on_final;
 bool remote_audio_expected() const;
 bool received_audio_format_(uint8_t,AudioFormat*,const uint8_t* =nullptr,size_t=0)const;
 bool can_send_dtmf_locked_(const DtmfFormat&,uint32_t)const;
 bool send_dtmf_packet_(const DtmfSender::Report&,const DtmfFormat&,uint32_t,uint32_t,uint16_t*);
 void send_audio_frame(const uint8_t*,size_t);
'''
    if "struct AudioSdpDirection {" not in (SOURCE / "sip_transport.h").read_text():
        extra += "std::atomic<bool> audio_send_enabled_{true},audio_receive_enabled_{true},audio_receive_reset_pending_{false};\n"
    source = source.replace("struct SipTransport {", "struct SipTransport {" + extra)
    source = source.replace("++finals;status=s;answer=body;return !fail_send;",
                            "++finals;status=s;answer=body;if(on_final)on_final();return !fail_send;")
    header = (SOURCE / "sip_transport.h").read_text()
    expected = block(header, "bool remote_audio_expected() const override")
    source += "\n" + expected.replace("bool remote_audio_expected() const override", "bool SipTransport::remote_audio_expected() const")
    source += "\n#define portENTER_CRITICAL(p) (p)->lock()\n#define portEXIT_CRITICAL(p) (p)->unlock()\n"
    for filename, markers in (
        ("sip_message.cpp", ["size_t pcm_to_rtp_payload("]),
        ("sip_transport.cpp", ["void SipTransport::send_audio_frame(", "bool SipTransport::received_audio_format_("]),
        ("sip_dtmf.cpp", ["bool SipTransport::can_send_dtmf_locked_(", "bool SipTransport::send_dtmf_packet_("]),
    ):
        implementation = (SOURCE / filename).read_text()
        source += "\n" + "\n".join(block(implementation, marker) for marker in markers)
    return source + r'''
int main(int argc,char**argv){
 assert(argc==2);const std::string mode=argv[1];
 SipTransport t;AudioFormat pcm;pcm.sample_rate=16000;pcm.frame_ms=10;
 if(mode.rfind("d1-",0)==0){
  auto receive=pcm;receive.sample_rate=48000;
  t.offer_tx_formats_.count=1;t.offer_tx_formats_.formats[0]=pcm;
  t.offer_rx_formats_.count=2;t.offer_rx_formats_.formats[0]=receive;t.offer_rx_formats_.formats[1]=pcm;
  t.remote_ip_v4_=0xc0000202;
  const std::string initial="v=0\r\nc=IN IP4 192.0.2.2\r\nm=audio 50000 RTP/AVP 96 97\r\n"
   "a=rtpmap:96 L16/48000/1\r\na=x-voip-stack-flow:96 send\r\n"
   "a=rtpmap:97 L16/16000/1\r\na=x-voip-stack-flow:97 recv\r\n"
   "a=x-voip-stack-independent-ptime:1\r\na=ptime:10\r\na=sendrecv\r\n";
  assert(t.learn_remote_rtp_from_sdp_(initial,0xc0000202)&&t.tx==pcm&&t.rx==receive);
  const auto old_payloads=t.accepted;auto proposal=initial;
  const bool single=mode.rfind("d1-single-",0)==0;
  const std::string direction=mode.substr(single?10:3);proposal.replace(proposal.find("a=sendrecv"),10,"a="+direction);
  if(single){
   proposal.replace(proposal.find("RTP/AVP 96 97"),13,direction=="sendonly"?"RTP/AVP 96":"RTP/AVP 97");
   const std::string omitted=direction=="sendonly"
    ?"a=rtpmap:97 L16/16000/1\r\na=x-voip-stack-flow:97 recv\r\n"
    :"a=rtpmap:96 L16/48000/1\r\na=x-voip-stack-flow:96 send\r\n";
   const auto position=proposal.find(omitted);assert(position!=std::string::npos);proposal.erase(position,omitted.size());
  }
  const auto request="INVITE sip:esp@192.0.2.1 SIP/2.0\r\nFrom: <sip:peer@192.0.2.2>;tag=remote\r\n"
   "To: <sip:esp@192.0.2.1>;tag=local\r\nCSeq: 2 INVITE\r\nVia: SIP/2.0/UDP 192.0.2.2;branch=z9hG4bK-d1hold\r\n"
   "Content-Type: application/sdp\r\nContent-Length: "+std::to_string(proposal.size())+"\r\n\r\n"+proposal;
  sockaddr_in peer{};peer.sin_addr.s_addr=htonl(0xc0000202);peer.sin_port=htons(5060);
  const bool accepted=t.handle_reinvite_(request,peer);
  std::cerr<<"D1 reoffer single="<<single<<" direction="<<direction<<" status="<<t.status<<" reason="<<t.reason<<"\n";
  assert(accepted&&t.status==200&&"A one-way reoffer needs only the active direction mapping");
  assert(t.tx==pcm&&t.rx==receive&&t.accepted==old_payloads);
  const auto start=t.answer.find("m=audio ");const auto end=t.answer.find("\r\n",start);
  uint16_t port=0;bool payloads[128]{};assert(parse_audio_media_line(t.answer.substr(start,end-start),&port,payloads));
  std::cerr<<"D1 "<<direction<<" answer="<<t.answer.substr(start,end-start)<<"\n";
  if(direction=="sendonly"){
   assert(payloads[96]&&!payloads[97]&&"recvonly answer must advertise receive mappings only");
   assert(t.answer.find("a=recvonly\r\n")!=std::string::npos);
  }else if(direction=="recvonly"){
   assert(!payloads[96]&&payloads[97]&&"sendonly answer must advertise transmit mappings only");
   assert(t.answer.find("a=sendonly\r\n")!=std::string::npos);
  }else{assert(direction=="inactive"&&payloads[96]&&payloads[97]);}
  return 0;
 }
 t.offer_tx_formats_.count=t.offer_rx_formats_.count=1;
 t.offer_tx_formats_.formats[0]=t.offer_rx_formats_.formats[0]=pcm;
 t.remote_ip_v4_=INADDR_LOOPBACK;
 t.rtp_socket_=socket(AF_INET,SOCK_DGRAM,0);int listener=socket(AF_INET,SOCK_DGRAM,0);
 assert(t.rtp_socket_>=0&&listener>=0);
 sockaddr_in peer{};peer.sin_family=AF_INET;peer.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 assert(bind(listener,reinterpret_cast<sockaddr*>(&peer),sizeof(peer))==0);
 socklen_t size=sizeof(peer);assert(getsockname(listener,reinterpret_cast<sockaddr*>(&peer),&size)==0);
 const std::string initial="v=0\r\nc=IN IP4 127.0.0.1\r\nm=audio "+std::to_string(ntohs(peer.sin_port))+" RTP/AVP 96 109\r\n"
  "a=rtpmap:96 L16/16000/1\r\na=rtpmap:109 telephone-event/8000\r\na=fmtp:109 0-15\r\na=ptime:10\r\na=sendrecv\r\n";
 assert(t.learn_remote_rtp_from_sdp_(initial,INADDR_LOOPBACK));
 const auto old_tx=t.tx,old_rx=t.rx;const auto old_payloads=t.accepted;
 const DtmfConfig old_dtmf{t.dtmf_tx,t.dtmf_rx};assert(old_dtmf.tx.valid());
 const int old_socket=t.rtp_socket_;const auto old_ssrc=t.rtp_ssrc_;const auto old_id=t.call_id_;
 const auto old_ip=t.remote_rtp_ip_v4_.load();const auto old_port=t.remote_rtp_port_.load();
 uint8_t samples[320]{};for(unsigned i=0;i<sizeof(samples);++i)samples[i]=static_cast<uint8_t>(i*7);
 uint16_t expected_sequence=100;
 auto drain=[&](bool audio){
  uint8_t packet[400];const int got=recv(listener,packet,sizeof(packet),MSG_DONTWAIT);
  if(!audio){assert(got==-1&&(errno==EAGAIN||errno==EWOULDBLOCK));return;}
  assert(got==332&&(packet[1]&127)==96);
  uint16_t sequence;std::memcpy(&sequence,packet+2,2);assert(ntohs(sequence)==expected_sequence++);
  uint32_t ssrc;std::memcpy(&ssrc,packet+8,4);assert(ntohl(ssrc)==old_ssrc);
  for(unsigned i=0;i<320;i+=2){assert(packet[12+i]==samples[i+1]&&packet[13+i]==samples[i]);}
 };
 auto audio=[&](bool expected){t.send_audio_frame(samples,sizeof(samples));drain(expected);};
 auto dtmf=[&](bool enabled){
  assert(t.can_send_dtmf_locked_(old_dtmf.tx,0)==enabled);
  DtmfSender::Report report{};report.event=5;report.timestamp=10000;report.duration=160;report.marker=true;
  assert(t.send_dtmf_packet_(report,old_dtmf.tx,0,old_ssrc,nullptr)==enabled);
  uint8_t packet[32];const int got=recv(listener,packet,sizeof(packet),MSG_DONTWAIT);
  if(!enabled){assert(got==-1&&(errno==EAGAIN||errno==EWOULDBLOCK));return;}
  assert(got==16&&(packet[1]&127)==109&&packet[12]==5);
  uint16_t sequence;std::memcpy(&sequence,packet+2,2);assert(ntohs(sequence)==expected_sequence++);
 };
 AudioFormat received;assert(t.received_audio_format_(96,&received,samples,sizeof(samples))&&received==old_rx);
 assert(t.remote_audio_expected());audio(true);dtmf(true);
 auto proposal=initial;
 const bool zero=mode=="zero-connection";
 const bool send=mode=="recvonly";
 const bool receive=mode=="sendonly";
 const bool fail=mode=="send-failure",reject=mode=="rejected-audio";
 const auto direction=(fail||reject)?"inactive":mode.c_str();
 if(zero)proposal.replace(proposal.find("127.0.0.1"),9,"0.0.0.0");
 else proposal.replace(proposal.find("a=sendrecv"),10,"a="+std::string(direction));
 if(reject)proposal.replace(proposal.find("L16/16000"),9,"L16/48000");
 auto invite=[&](const std::string&body,unsigned cseq){return "INVITE sip:esp@127.0.0.1 SIP/2.0\r\nFrom: <sip:peer@127.0.0.1>;tag=remote\r\n"
  "To: <sip:esp@127.0.0.1>;tag=local\r\nCSeq: "+std::to_string(cseq)+" INVITE\r\nVia: SIP/2.0/UDP 127.0.0.1;branch=z9hG4bK-hold"+std::to_string(cseq)+"\r\n"
  "Content-Type: application/sdp\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;};
 t.fail_send=fail;
 t.on_final=[&](){
  // The established configuration remains visible until final publication;
  // the proposal fence blocks packets while its direction is uncommitted.
  assert(t.remote_audio_expected()&&t.tx==old_tx&&t.rx==old_rx&&t.accepted==old_payloads);
  audio(false);dtmf(false);
 };
 const bool result=t.handle_reinvite_(invite(proposal,2),peer);
 std::cerr<<"hold status="<<t.status<<" reason="<<t.reason<<"\n";
 if(reject)assert(result&&t.status==488);
 else if(fail)assert(!result&&t.status==200);
 else assert(result&&t.status==200&&"An established audio hold reoffer must be accepted");
 assert(t.tx==old_tx&&t.rx==old_rx&&t.accepted==old_payloads&&t.media_active_);
 assert(t.rtp_socket_==old_socket&&t.rtp_ssrc_==old_ssrc&&t.call_id_==old_id);
 assert(t.remote_rtp_ip_v4_==old_ip&&t.remote_rtp_port_==old_port);
 assert((DtmfConfig{t.dtmf_tx,t.dtmf_rx}==old_dtmf));
 if(fail||reject){
  assert(t.remote_dialog_cseq_==1&&t.last_invite_cseq_number_==1&&t.remote_audio_expected());
  assert(t.received_audio_format_(96,&received,samples,sizeof(samples)));audio(true);dtmf(true);
 }else{
  assert(t.remote_dialog_cseq_==2&&t.last_invite_cseq_number_==2);
  const std::string answer_direction=send?"sendonly":receive?"recvonly":"inactive";
  const auto audio_answer=t.answer.substr(0,t.answer.find("m=video"));
  assert(audio_answer.find("a="+answer_direction+"\r\n")!=std::string::npos);
  assert(audio_answer.find("L16/16000/1")!=std::string::npos);
  assert(t.remote_audio_expected()==receive);
  assert(t.received_audio_format_(96,&received,samples,sizeof(samples))==receive);
  audio(send);dtmf(send);
  // Resume uses the same dialog, socket, codec/PT map and RTP sequence owner.
  t.on_final={};assert(t.handle_reinvite_(invite(initial,3),peer)&&t.status==200);
  assert(t.remote_audio_expected()&&t.received_audio_format_(96,&received,samples,sizeof(samples)));
  assert(t.tx==old_tx&&t.rx==old_rx&&t.accepted==old_payloads&&t.call_id_==old_id);
  assert(t.rtp_socket_==old_socket&&t.rtp_ssrc_==old_ssrc&&t.remote_dialog_cseq_==3);
  audio(true);dtmf(true);
 }
 close(t.rtp_socket_);close(listener);
}
'''


@pytest.fixture(scope="module")
def hold_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("audio-hold")
    stub = folder / "esphome/core/defines.h"
    stub.parent.mkdir(parents=True)
    stub.write_text("#pragma once\n")
    cpp = folder / "hold.cpp"
    cpp.write_text(hold_source())
    binary = folder / "hold"
    built = subprocess.run(
        ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation",
         "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-pthread", "-I", str(folder),
         "-I", str(ROOT), str(cpp), "-o", str(binary)],
        capture_output=True, text=True,
    )
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize("mode", ["inactive", "sendonly", "recvonly", "zero-connection", "send-failure", "rejected-audio"])
def test_established_audio_hold_keeps_mapping_and_gates_actual_udp(hold_binary, mode):
    result = subprocess.run([str(hold_binary), mode], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.mark.parametrize("direction", ["sendonly", "recvonly", "inactive"])
def test_asymmetric_hold_answer_advertises_only_its_active_mapping(hold_binary, direction):
    result = subprocess.run([str(hold_binary), "d1-" + direction], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.mark.parametrize("direction", ["sendonly", "recvonly"])
def test_one_way_reoffer_needs_only_active_direction_mapping(hold_binary, direction):
    result = subprocess.run([str(hold_binary), "d1-single-" + direction], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr


def test_held_playout_mutes_received_and_plc_pcm_and_restores_original_bytes(tmp_path):
    from test_rx_playout_owner import owner_source

    source = "#include <vector>\n" + owner_source().split("\nint main(", 1)[0]
    source = source.replace("struct Speaker {", "struct Speaker {\n std::vector<uint8_t> output;")
    source = source.replace(
        "size_t play(const uint8_t*,size_t bytes,uint32_t){if(!is_running())return 0;accepted+=bytes;return bytes;}",
        "size_t play(const uint8_t*data,size_t bytes,uint32_t){if(!is_running())return 0;accepted+=bytes;output.insert(output.end(),data,data+bytes);return bytes;}",
    )
    source += r'''
int main(){
 Speaker sink;sink.state=Speaker::RUNNING;VoipStack phone;phone.speaker_=&sink;
 uint8_t scratch[960]{},network[960];for(unsigned i=0;i<960;++i)network[i]=uint8_t(i*7);
 const std::vector<uint8_t> original(network,network+960);
 phone.rx_audio_chunk_=scratch;phone.rx_audio_chunk_alloc_bytes_=960;phone.call_state_=CallState::IN_CALL;
 const auto revision=phone.rx_audio_revision_.load();phone.rx_playout_state_=tag(revision,Phase::ACTIVE);
 phone.play_rx_frame_(network,960,SilenceReason::NONE,10,revision);assert(sink.output==original);
 sink.output.clear();phone.transport.paused=true;
 phone.play_rx_frame_(network,960,SilenceReason::NONE,10,revision);
 assert(sink.output==std::vector<uint8_t>(960,0)&&std::vector<uint8_t>(network,network+960)==original);
 sink.output.clear();std::memset(scratch,0x7f,sizeof(scratch));
 // Existing PLC reaches the same NONE path and must also remain silent.
 phone.play_rx_frame_(scratch,960,SilenceReason::NONE,10,revision);
 assert(sink.output==std::vector<uint8_t>(960,0));
 sink.output.clear();phone.transport.paused=false;
 phone.play_rx_frame_(network,960,SilenceReason::NONE,10,revision);
 assert(sink.output==original&&phone.rx_audio_revision_==revision&&phone.call_state_==CallState::IN_CALL);
}
'''
    helper = tmp_path / "esphome/core/helpers.h"
    helper.parent.mkdir(parents=True)
    helper.write_text("namespace esphome {struct Mutex {};struct LockGuard {explicit LockGuard(Mutex&) {}};}\n")
    cpp = tmp_path / "hold-playout.cpp"
    cpp.write_text(source)
    binary = tmp_path / "hold-playout"
    built = subprocess.run(
        ["g++", "-std=c++17", "-DUSE_ESP32", "-DUSE_ESPHOME_VOIP_STACK_SPEAKER",
         "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-I", str(tmp_path),
         "-I", str(ROOT), str(cpp), "-o", str(binary)], capture_output=True, text=True,
    )
    assert built.returncode == 0, built.stderr
    run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
    assert run.returncode == 0, run.stdout + run.stderr


def test_actual_rtp_worker_discards_held_audio_and_marks_only_first_resumed_frame(tmp_path):
    from test_dtmf_worker_runtime import worker_source

    source = worker_source().split("\nint main(", 1)[0]
    source = source.replace("std::vector<uint16_t>audio_seen;",
                            "std::vector<uint16_t>audio_seen;std::vector<bool>changes;std::atomic<unsigned>delivered{0};")
    source = source.replace("uint32_t,bool,uint32_t){", "uint32_t,bool changed,uint32_t){")
    source = source.replace("audio_seen.push_back(seq);}",
                            "audio_seen.push_back(seq);changes.push_back(changed);delivered.fetch_add(1,std::memory_order_release);}")
    source += r'''
int main(){
 SipTransport dut;dut.audio.frame_ms=10;dut.audio.sample_rate=16000;
 dut.offer_rx_formats_.count=1;dut.offer_rx_formats_.formats[0]=dut.audio;
 assert(dut.negotiated_rx_payloads_.add(96,dut.audio));
 dut.rtp_socket_=socket(AF_INET,SOCK_DGRAM,0);int peer=socket(AF_INET,SOCK_DGRAM,0);
 assert(dut.rtp_socket_>=0&&peer>=0);dut.rtp_port_=bind_udp(dut.rtp_socket_);dut.remote_rtp_port_=bind_udp(peer);
 const auto old_fd=dut.rtp_socket_;const auto old_epoch=dut.dtmf_generation_.load();
 sockaddr_in target{};target.sin_family=AF_INET;target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);target.sin_port=htons(dut.rtp_port_);
 std::thread worker([&](){dut.rtp_task_();});
 auto wait_for=[&](auto predicate){const auto start=millis();while(!predicate()&&millis()-start<1500)std::this_thread::sleep_for(std::chrono::milliseconds(1));assert(predicate());};
 auto packet=[&](uint16_t seq){
  uint8_t p[332]{};p[0]=0x80;p[1]=96;p[2]=seq>>8;p[3]=seq;p[11]=42;
  const uint32_t stamp=uint32_t(seq)*160;p[4]=stamp>>24;p[5]=stamp>>16;p[6]=stamp>>8;p[7]=stamp;
  for(unsigned i=0;i<160;++i){const uint16_t value=seq+i;p[12+i*2]=value>>8;p[13+i*2]=value;}
  assert(sendto(peer,p,sizeof(p),0,reinterpret_cast<sockaddr*>(&target),sizeof(target))==sizeof(p));
 };
 packet(500);wait_for([&](){return dut.delivered.load(std::memory_order_acquire)==1;});
 dut.audio_receive_enabled_=false;packet(501);
 wait_for([&](){return dut.rtp_rx_datagrams_.load(std::memory_order_acquire)>=2;});
 assert(dut.delivered==1&&dut.rtp_rx_packets_==1);
 dut.audio_receive_reset_pending_=true;dut.audio_receive_enabled_=true;
 packet(502);wait_for([&](){return dut.delivered.load(std::memory_order_acquire)==2;});
 packet(503);wait_for([&](){return dut.delivered.load(std::memory_order_acquire)==3;});
 assert(dut.rtp_socket_==old_fd&&dut.dtmf_generation_==old_epoch&&!dut.audio_receive_reset_pending_);
 {LockGuard lock(dut.media_lifecycle_mutex_);dut.request_audio_path_stop_locked_();dut.rtp_task_terminate_=true;}
 worker.join();
 assert((dut.audio_seen==std::vector<uint16_t>{500,502,503}));
 assert((dut.changes==std::vector<bool>{false,true,false}));
 assert(dut.quiescent==1&&dut.rtp_socket_==-1&&dut.rtp_task_quiesced_);
 int reuse=socket(AF_INET,SOCK_DGRAM,0);assert(bind(reuse,reinterpret_cast<sockaddr*>(&target),sizeof(target))==0);
 close(reuse);close(peer);
}
'''
    cpp = tmp_path / "hold-worker.cpp"
    cpp.write_text(source)
    binary = tmp_path / "hold-worker"
    built = subprocess.run(
        ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation",
         "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-pthread", "-I", str(SOURCE),
         str(cpp), "-o", str(binary)], capture_output=True, text=True,
    )
    assert built.returncode == 0, built.stderr
    run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=8)
    assert run.returncode == 0, run.stdout + run.stderr


@pytest.fixture(scope="module")
def video_hold_binary(tmp_path_factory):
    source = "#include <cerrno>\n#include <sys/socket.h>\n#include <unistd.h>\n" + inactive_video_source().split("\nint main(", 1)[0]
    source = source.replace("struct Child {", "struct Child {\n bool request_send_direction(bool value){send=value;return true;}")
    source = source.replace("struct SipTransport {", r'''struct SipTransport {
 bool apply_video_direction_answer_(const std::string&,uint32_t,bool*);
 int signaling_socket=-1;sockaddr_in response_peer{};
 bool publish_response(unsigned status,const std::string&body){
  const auto response="SIP/2.0 "+std::to_string(status)+" Test\r\nContent-Type: application/sdp\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
  return sendto(signaling_socket,response.data(),response.size(),0,reinterpret_cast<sockaddr*>(&response_peer),sizeof(response_peer))==static_cast<int>(response.size());
 }
''')
    source = source.replace("status=s;reason=r;return true;", "status=s;reason=r;return publish_response(s,\"\");")
    source = source.replace("++finals;status=s;answer=body;return !fail_send;",
                            "++finals;status=s;answer=body;return !fail_send&&publish_response(s,body);")
    source = source.replace(
        "bool request_media_direction(bool,bool){++calls;return true;}",
        "bool request_media_direction(bool s,bool r){++calls;send=s;receive=r;return true;}",
    )
    source += "\n" + block((SOURCE / "sip_transport.cpp").read_text(), "bool SipTransport::apply_video_direction_answer_(")
    source += r'''
int main(int argc,char**argv){
 assert(argc==2);const std::string mode=argv[1];SipTransport t;AudioFormat pcm;pcm.frame_ms=10;
 t.offer_tx_formats_.count=t.offer_rx_formats_.count=1;
 t.offer_tx_formats_.formats[0]=t.offer_rx_formats_.formats[0]=pcm;
 t.video_rx_.encoding="JPEG";t.video_rx_.payload_type=26;t.video_rx_.width=320;t.video_rx_.height=240;t.video_rx_.max_fps=10;
 t.remote_ip_v4_=INADDR_LOOPBACK;
 const std::string initial="v=0\r\nc=IN IP4 192.168.1.200\r\nm=audio 50000 RTP/AVP 96\r\n"
  "a=rtpmap:96 L16/16000/1\r\na=ptime:10\r\na=sendrecv\r\n"
  "m=video 40040 RTP/AVP 26\r\na=rtpmap:26 JPEG/90000\r\na=rtcp:40041\r\na=sendonly\r\n";
 assert(t.learn_remote_rtp_from_sdp_(initial,0xc0a801c8));
 t.local_offered_rx_payloads_=t.accepted;t.local_offered_tx_formats_=t.offer_tx_formats_;
 assert(t.video_negotiated_&&t.video_receive_enabled_&&!t.video_send_enabled_);
 t.child.receive=true;
 const auto old_tx=t.tx,old_rx=t.rx;const auto old_payloads=t.accepted;
 const auto old_audio_ip=t.remote_rtp_ip_v4_.load();const auto old_video_ip=t.remote_video_ip_v4_;
 auto proposal=initial;
 const bool fail=mode=="send-failure",reject=mode=="rejected-audio";
 const bool audio_only=mode=="audio-only-zero";
 const bool override_video=mode=="video-override";
 const bool local_answer=mode=="local-answer";
 const bool ordinary=mode=="ordinary-inactive";
 if(ordinary)proposal.replace(proposal.find("a=sendonly"),10,"a=inactive");
 else if(local_answer)proposal.replace(proposal.find("a=sendrecv"),10,"a=inactive");
 else if(audio_only)proposal.insert(proposal.find("a=rtpmap:96"),"c=IN IP4 0.0.0.0\r\n");
 else proposal.replace(proposal.find("c=IN IP4 192.168.1.200"),std::string("c=IN IP4 192.168.1.200").size(),"c=IN IP4 0.0.0.0");
 if(override_video)proposal.insert(proposal.find("a=rtpmap:26"),"c=IN IP4 192.168.1.200\r\n");
 if(reject)proposal.replace(proposal.find("L16/16000"),9,"L16/48000");
 auto invite=[&](const std::string&body,unsigned cseq){return "INVITE sip:esp@192.168.1.192 SIP/2.0\r\n"
  "From: <sip:peer@192.168.1.200>;tag=remote\r\nTo: <sip:esp@192.168.1.192>;tag=local\r\n"
  "CSeq: "+std::to_string(cseq)+" INVITE\r\nVia: SIP/2.0/UDP 192.168.1.200;branch=z9hG4bK-zero"+std::to_string(cseq)+"\r\n"
  "Content-Type: application/sdp\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;};
 const int client=socket(AF_INET,SOCK_DGRAM,0);t.signaling_socket=socket(AF_INET,SOCK_DGRAM,0);
 assert(client>=0&&t.signaling_socket>=0);
 sockaddr_in server{};server.sin_family=AF_INET;server.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 assert(bind(t.signaling_socket,reinterpret_cast<sockaddr*>(&server),sizeof(server))==0);
 socklen_t address_size=sizeof(server);assert(getsockname(t.signaling_socket,reinterpret_cast<sockaddr*>(&server),&address_size)==0);
 auto exchange=[&](const std::string&body,unsigned cseq){
  const auto request=invite(body,cseq);
  assert(sendto(client,request.data(),request.size(),0,reinterpret_cast<sockaddr*>(&server),sizeof(server))==static_cast<int>(request.size()));
  char packet[8192];socklen_t peer_size=sizeof(t.response_peer);
  const int bytes=recvfrom(t.signaling_socket,packet,sizeof(packet),0,reinterpret_cast<sockaddr*>(&t.response_peer),&peer_size);
  assert(bytes==static_cast<int>(request.size())&&std::string(packet,bytes)==request);
  const bool result=t.handle_reinvite_(std::string(packet,bytes),t.response_peer);
  const int answer_bytes=recv(client,packet,sizeof(packet),MSG_DONTWAIT);
  if(t.fail_send)assert(answer_bytes==-1&&(errno==EAGAIN||errno==EWOULDBLOCK));
  else{
   assert(answer_bytes>0);const std::string response(packet,answer_bytes);
   assert(response.rfind("SIP/2.0 "+std::to_string(t.status)+" ",0)==0);
   if(t.status==200)assert(response.substr(response.find("\r\n\r\n")+4)==t.answer);
  }
  return result;
 };
 t.fail_send=fail;const bool result=exchange(proposal,2);
 std::cerr<<"session-zero status="<<t.status<<" reason="<<t.reason<<" receive="<<t.video_receive_enabled_<<" child="<<t.child.receive<<"\n";
 assert(t.tx==old_tx&&t.rx==old_rx&&t.accepted==old_payloads&&t.media_active_);
 assert(t.remote_rtp_ip_v4_==old_audio_ip&&t.remote_video_ip_v4_==old_video_ip);
 assert(t.child.running&&t.child.port==40040&&t.child.rtcp_port==40041);
 if(reject||fail){
  assert((reject?(result&&t.status==488):(!result&&t.status==200)));
  assert(t.audio_receive_enabled_&&t.audio_send_enabled_&&t.video_receive_enabled_&&t.child.receive);
  assert(t.child.rtcp_send);
  assert(t.remote_dialog_cseq_==1&&t.last_invite_cseq_number_==1);
 }else{
  assert(result&&t.status==200&&t.audio_receive_enabled_==ordinary&&t.audio_send_enabled_==ordinary);
  const bool video_expected=audio_only||override_video||local_answer;
  assert(t.video_receive_enabled_==video_expected&&t.child.receive==video_expected);
  assert(t.child.rtcp_send==(video_expected||ordinary));
  assert(!t.video_send_enabled_&&!t.child.send);
  const auto video_answer=t.answer.substr(t.answer.find("m=video"));
  assert(video_answer.find(video_expected?"a=recvonly\r\n":"a=inactive\r\n")!=std::string::npos);
  if(local_answer){
   bool sending=true;
   assert(t.apply_video_direction_answer_(proposal,0xc0a801c8,&sending)&&!sending);
   assert(!t.audio_send_enabled_&&!t.audio_receive_enabled_&&t.child.receive&&t.child.rtcp_send);
  }
  assert(exchange(initial,3)&&t.status==200);
  assert(t.audio_send_enabled_&&t.audio_receive_enabled_&&t.video_receive_enabled_&&t.child.receive);
  assert(t.child.rtcp_send);
  assert(t.remote_dialog_cseq_==3&&t.remote_video_rtp_port_==40040&&t.remote_video_rtcp_port_==40041);
 }
 close(t.signaling_socket);close(client);
}
'''
    folder = tmp_path_factory.mktemp("video-hold")
    stub = folder / "esphome/core/defines.h"
    stub.parent.mkdir(parents=True)
    stub.write_text("#pragma once\n")
    cpp = folder / "hold.cpp"
    cpp.write_text(source)
    binary = folder / "hold"
    built = subprocess.run(
        ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation",
         "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-pthread", "-I", str(folder),
         "-I", str(ROOT), str(cpp), "-o", str(binary)], capture_output=True, text=True,
    )
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize("mode", ["session-zero", "audio-only-zero", "video-override", "send-failure", "rejected-audio", "local-answer", "ordinary-inactive"])
def test_zero_connection_scope_reaches_existing_video_child_and_resumes(video_hold_binary, mode):
    run = subprocess.run([str(video_hold_binary), mode], capture_output=True, text=True, timeout=5)
    assert run.returncode == 0, run.stdout + run.stderr


@pytest.fixture(scope="module")
def video_control_binary(tmp_path_factory):
    from test_video_resource_lifecycle import RTOS, write_headers

    folder = tmp_path_factory.mktemp("video-hold-control")
    ref = os.environ.get("VOIP_VIDEO_HOLD_SOURCE_REF")

    def read_video(name):
        if ref:
            return subprocess.check_output(
                ["git", "show", f"{ref}:esphome/components/voip_stack/{name}"], cwd=ROOT, text=True,
            )
        return (SOURCE / name).read_text()

    header = read_video("video_rtp.h")
    implementation = read_video("video_rtp.cpp")
    if "void set_rtcp_send_enabled(" not in header:
        # Baseline API adapter deliberately does nothing. Actual baseline
        # packet transmission remains untouched and must fail the hold gate.
        header = header.replace(" public:", " public:\n void set_rtcp_send_enabled(bool){}", 1)
    (folder / "video_rtp.h").write_text(header)
    shutil.copyfile(SOURCE / "video.h", folder / "video.h")
    shutil.copyfile(SOURCE / "rtp_jpeg.h", folder / "rtp_jpeg.h")
    write_headers(folder, {
        "esphome/core/defines.h": "#pragma once\n",
        "audio_core_task_utils.h": "#pragma once\n",
        "esphome/core/helpers.h": "#pragma once\n#include <mutex>\nnamespace esphome {using Mutex=std::mutex;using LockGuard=std::lock_guard<Mutex>;}\n",
        "freertos/FreeRTOS.h": RTOS,
        "freertos/semphr.h": '#include "FreeRTOS.h"\n',
        "freertos/task.h": '#include "FreeRTOS.h"\n',
    })
    markers = [
        "VideoRtpSession::VideoRtpSession(", "bool VideoRtpSession::set_negotiated(",
        "void VideoRtpSession::request_stop(", "void VideoRtpSession::close_sockets_(",
        "size_t VideoRtpSession::build_rtcp_report_sdes_(",
        "void VideoRtpSession::send_rtcp_packet_(", "void VideoRtpSession::send_rtcp_report_(",
        "void VideoRtpSession::send_rtcp_pli_(", "void VideoRtpSession::send_rtcp_bye_(",
    ]
    packet_writer = ('bool VideoRtpSession::send_rtp_packet_(' if 'bool VideoRtpSession::send_rtp_packet_(' in implementation
                     else 'bool VideoRtpSession::send_rtp_payload_(')
    markers.append(packet_writer)
    if "void VideoRtpSession::set_rtcp_send_enabled(" in implementation:
        markers.append("void VideoRtpSession::set_rtcp_send_enabled(")
    methods = "\n".join(block(implementation, marker) for marker in markers)
    helpers = "\n".join(block(implementation, marker) for marker in ("void write_be16(", "void write_be32("))
    source = r'''
#include <cassert>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#define protected public
#include "video_rtp.h"
#undef protected
#define ESP_LOGE(...) ((void)0)
uint32_t clock_ms=1000;uint32_t millis(){return clock_ms;}
namespace esphome::voip_stack {
HELPERS
METHODS
VideoRtpSession::~VideoRtpSession(){}
bool VideoRtpSession::reap_sender_task_(){return true;}
bool VideoRtpSession::reap_receive_task_(){return true;}
void VideoRtpSession::reset_reassembly_(){}
void VideoRtpSession::reset_h264_parameter_sets_(){}
void VideoRtpSession::wake_task_(){}
}
using namespace esphome::voip_stack;
int listener(uint16_t*port){
 int fd=socket(AF_INET,SOCK_DGRAM,0);assert(fd>=0);sockaddr_in address{};address.sin_family=AF_INET;
 address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);assert(bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
 socklen_t size=sizeof(address);assert(getsockname(fd,reinterpret_cast<sockaddr*>(&address),&size)==0);
 *port=ntohs(address.sin_port);return fd;
}
int main(){
 VideoRtpSession child(false);const auto *identity=&child;
 uint16_t rtp_port,rtcp_port;const int rtp_peer=listener(&rtp_port),rtcp_peer=listener(&rtcp_port);
 VideoCapability cap;cap.encoding="H264";cap.payload_type=102;cap.rtcp_feedback_pli=true;
 auto negotiate=[&](){
  assert(child.set_negotiated(cap,cap,cap,INADDR_LOOPBACK,rtp_port,INADDR_LOOPBACK,rtcp_port,true,true));
  // RTOS startup is an adapter; all packet writers and generation admission
  // above are the production owners on the same retained child object.
  child.running_=true;child.terminate_=false;child.tx_ssrc_=1234;child.remote_ssrc_=5678;
  child.rtp_socket_=socket(AF_INET,SOCK_DGRAM,0);child.rtcp_socket_=socket(AF_INET,SOCK_DGRAM,0);
  assert(child.rtp_socket_>=0&&child.rtcp_socket_>=0);
 };
 auto receive=[&](int fd,bool expected){uint8_t packet[1600];const int size=recv(fd,packet,sizeof(packet),MSG_DONTWAIT);
  if(expected)assert(size>0);else assert(size==-1&&(errno==EAGAIN||errno==EWOULDBLOCK));return size;};
 const uint8_t payload[]={0x65,1,2,3};
 auto check=[&](bool media,bool control){
  assert(child.send_rtp_payload_(payload,sizeof(payload),true,90000)==media);receive(rtp_peer,media);
  child.send_rtcp_report_();receive(rtcp_peer,control);
  clock_ms+=600;child.send_rtcp_pli_();receive(rtcp_peer,control);
 };
 negotiate();check(true,true);
 child.send_enabled_=false;child.receive_enabled_=false;
 check(false,true); // Ordinary inactive keeps RTCP, unlike zero connection.
 child.set_rtcp_send_enabled(false);check(false,false);
 // BYE's existing media stop owner runs while zero-connection hold is active.
 child.request_stop();child.send_rtcp_bye_();receive(rtcp_peer,false);
 assert(!child.running_&&!child.send_enabled_&&!child.receive_enabled_);
 // New SDP admission resets the retained gate. No setter(true) is permitted
 // here: otherwise a held call could poison the next dialog silently.
 negotiate();assert(&child==identity);check(true,true);
 child.request_stop();child.close_sockets_();close(rtp_peer);close(rtcp_peer);
}
'''.replace("HELPERS", helpers).replace("METHODS", methods)
    if 'send_rtp_packet_' in packet_writer:
        source = source.replace('const uint8_t payload[]={0x65,1,2,3};',
                                'const uint8_t payload[]={0x65,1,2,3};uint8_t packet[12+sizeof(payload)]{};std::memcpy(packet+12,payload,sizeof(payload));')
        source = source.replace('child.send_rtp_payload_(payload,', 'child.send_rtp_packet_(packet,')
    cpp = folder / "control.cpp"
    cpp.write_text(source)
    binary = folder / "control"
    built = subprocess.run(
        ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-DUSE_ESP32",
         "-DUSE_ESPHOME_VOIP_STACK_VIDEO", "-DUSE_ESPHOME_VOIP_STACK_VIDEO_H264",
         "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
         "-I", str(folder), str(cpp), "-o", str(binary)], capture_output=True, text=True,
    )
    assert built.returncode == 0, built.stderr
    return binary


def test_actual_video_control_gate_preserves_inactive_and_resets_on_redial(video_control_binary):
    run = subprocess.run([str(video_control_binary)], capture_output=True, text=True, timeout=5)
    assert run.returncode == 0, run.stdout + run.stderr
