"""The registered-account busy gate preserves the real application media setter."""

import os
from pathlib import Path
import subprocess

import pytest

from test_delayed_offer_runtime import delayed_source
from test_refer_dialog_runtime import ROOT, SOURCE, block


def preservation_source():
    code = "#include <functional>\n" + delayed_source(registered=True)
    code = code.replace("uint32_t esp_random(){return 42;}",
                        "uint32_t esp_random(){static uint32_t next=42;return ++next;}")
    code = code.replace("struct VoipStack {", "struct VoipStack {\n bool sink_ready=false;bool can_accept_session_()const;")
    code = code.replace("bool should_accept_session_()const{return true;}",
                        "std::function<bool()>accept_session;bool should_accept_session_()const{return !accept_session||accept_session();}")
    code = code.replace("bool prepare_speaker_for_current_rx_(){return true;}",
                        "bool prepare_speaker_for_current_rx_(){return sink_ready;}")
    code = code.replace("AppTransport t;AppTransport*transport_=&t;", r'''
 AppTransport t;
 struct AppTransportHandle {
  AppTransport*adapter;operator AppTransport*()const{return adapter;}
  AppTransport*operator->()const{return adapter;}SipTransport*get()const{return wire_transport;}
 }transport_{&t};
''')
    header = (SOURCE / "voip_stack.h").read_text()
    # Keep the production packed formats, playout conversion and frame budget.
    code = code.replace("struct CurrentMediaFormats {AudioFormat tx,rx;};",
                        block(header, "struct CurrentMediaFormats {") + ";")
    code = code.replace("CurrentMediaFormats snapshot_current_media_formats_()const{return {};}",
                        block(header, "CurrentMediaFormats snapshot_current_media_formats_() const"))
    old = "AudioFormat tx,rx;void set_current_media_formats_(const AudioFormat&,const AudioFormat&,const AudioFormat&t,const AudioFormat&r){tx=t;rx=r;}"
    assert code.count(old) == 1
    code = code.replace(old, r'''
 mutable int media_format_mutex_=0;CurrentMediaFormats current_media_formats_;
 std::atomic<uint32_t>current_tx_audio_format_packed_{0},current_rx_audio_format_packed_{0};
 std::atomic<size_t>current_tx_audio_frame_bytes_{0};
 AudioFormat&tx=current_media_formats_.tx;AudioFormat&rx=current_media_formats_.rx;
''' + block(header, "void set_current_media_formats_(") + "\n"
        + block(header, "AudioFormat get_current_tx_audio_format_() const") + "\n"
        + block(header, "AudioFormat get_current_rx_audio_format_() const"))
    # Outgoing preparation also enters through the production FSM method. Only
    # the selected contact, hardware and publication boundaries are adapters.
    code = code.replace("void start_call_attempt_(bool){assert(false);}", r'''
 void start_call_attempt_(bool,const std::string& = {});
 AudioFormat tx_audio_format_{DEFAULT_AUDIO_FORMAT},rx_audio_format_{DEFAULT_AUDIO_FORMAT};
 bool force_direct_call_=false;std::string pending_line_,default_line_="office";
 Trigger outgoing_call_trigger_,bridge_request_trigger_;
 std::string get_current_contact_ip()const{return "127.0.0.1";}
 uint16_t get_current_contact_port()const{return 5060;}
 uint16_t get_current_contact_rtp_port()const{return 30000;}
 bool get_current_contact_sip_transport_tcp()const{return false;}
 bool get_current_contact_directional_audio_v1()const{return false;}
 std::string get_current_destination_route()const{return "101";}
 void set_remote_sip_transport_tcp(bool value){t.set_sip_signaling_transport(value);}
 void set_remote_endpoint(const std::string&host,uint16_t port,uint16_t rtp){wire_transport->set_remote(host,port,rtp);}
''')
    code = code.replace("void*find(const std::string&){return nullptr;}",
        "struct Entry{std::string ip;uint16_t port=5060,rtp_port=30000;bool sip_transport_tcp=false,directional_audio_v1=false;};Entry*find(const std::string&){return nullptr;}")
    code = code.replace("const std::string&e){identity={a,b,c,d,e,\"\"};}",
                        "const std::string&e,bool=false){identity={a,b,c,d,e,\"\"};}")
    code = code.replace("void publish_state_(){}", "void publish_state_(){}void publish_sip_snapshot_(){}")
    code = code.replace("void set_sip_signaling_transport(bool){}",
        "void set_sip_signaling_transport(bool tcp){wire_transport->set_sip_signaling_transport(tcp);}void set_peer_directional_audio_v1(bool enabled){wire_transport->peer_directional_audio_v1_=enabled;}")
    code = code.replace("struct Snapshot {bool terminal_transaction_pending=false;unsigned media_lifecycle_phase=0;};",
        "struct Snapshot {bool terminal_transaction_pending=false;unsigned media_lifecycle_phase=0;uint32_t remote_ip_v4=0;uint16_t remote_sip_port=0;bool sip_tcp=false;};")
    code = code.replace("return {wire_transport->terminal_transaction_pending_locked_(),0};",
        "return {wire_transport->terminal_transaction_pending_locked_(),0,wire_transport->remote_ip_v4_.load(),wire_transport->remote_sip_port_.load(),wire_transport->remote_sip_tcp_.load()};")
    code = code.replace("std::atomic<bool> running_{true},transport_stopping_{false},audio_receive_reset_pending_{false};",
        "std::atomic<bool> running_{true},transport_stopping_{false},audio_receive_reset_pending_{false},rtp_source_reacquire_pending_{false};")
    transport = (SOURCE / "sip_transport.cpp").read_text()
    code = code.replace(" uint16_t sip_port_=5060;", "bool parse_remote_(const std::string&);void set_remote(const std::string&,uint16_t,uint16_t);void set_sip_signaling_transport(bool);void request_tcp_client_close_();\n uint16_t sip_port_=5060;", 1)
    code += "\n" + block(transport, "bool SipTransport::parse_remote_(")
    code += "\n" + block(transport, "void SipTransport::set_remote(")
    code += "\n" + block(transport, "void SipTransport::set_sip_signaling_transport(")
    code += "\n" + block(transport, "void SipTransport::request_tcp_client_close_(")
    code += "\n" + block((SOURCE / "sip_message.cpp").read_text(), "size_t rtp_payload_to_pcm(")
    registration = (ROOT / "tests/fixtures/registration_transport.cpp").read_text()
    code += "\n" + registration[:registration.index("void start_account_call(")]
    code += "\n" + (ROOT / "tests/fixtures/multiaccount_global_busy.cpp").read_text()
    fsm = (SOURCE / "voip_fsm.cpp").read_text()
    code += "\n" + block(fsm, "bool VoipStack::can_accept_session_(")
    code += "\n" + block(fsm, "void VoipStack::start_call_attempt_(")
    return code


@pytest.fixture(scope="module")
def preservation_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("multiaccount-preservation")
    defines = folder / "esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    code = preservation_source()
    code += r'''
std::string offered(Peer&p,const std::string&account,const std::string&id,bool competitor=false){
 auto invite=initial_invite(p,account,id);
 const std::string body=std::string("v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n")+
  (competitor?"m=audio 31000 RTP/AVP 9\r\na=rtpmap:9 G722/8000\r\na=ptime:20\r\n":
              "m=audio 30000 RTP/AVP 96\r\na=rtpmap:96 L16/16000/1\r\na=ptime:16\r\n")+"a=sendrecv\r\n";
 const auto at=invite.find("Content-Length: 3");assert(at!=std::string::npos);
 invite.replace(at,strlen("Content-Length: 3"),"Content-Type: application/sdp\r\nContent-Length: "+std::to_string(body.size()));
 invite.resize(invite.find("\r\n\r\n")+4);return invite+body;
}
void dispatch(SipTransport&s,VoipStack&p,size_t&handled){while(handled<s.delivered.size())p.on_sip_signal_received_(s.delivered[handled++]);p.drain();}
int main(int argc,char**argv){
 assert(argc==6);Peer office(std::string(argv[1])=="tcp"),home(std::string(argv[2])=="tcp");
 const bool home_first=std::string(argv[3])=="home",automatic=std::string(argv[4])=="on";
 const std::string phase=argv[5];const bool preparing=phase=="local-preparing";
 SipTransport s;wire_transport=&s;bind_account(s,office);bind_home(s,home);
 s.call_id_.clear();s.local_tag_.clear();s.remote_tag_.clear();s.media_active_=false;s.accept_direct_calls_=false;
 audio_format_list_default(&s.offer_tx_formats_);audio_format_list_default(&s.offer_rx_formats_);
 VoipStack phone;size_t handled=0;s.accept_session=[&](){return phone.can_accept_session_();};
 auto&first=home_first?home:office;auto&second=home_first?office:home;
 const std::string first_account=home_first?"home":"office",second_account=home_first?"office":"home";
 phone.auto_answer_=phase=="ringing"?false:automatic;phone.sink_ready=phase=="active";
 if(preparing){
  phone.default_line_=first_account;phone.start_call_attempt_(false,"owned-first");phone.drain();
  assert(phone.call_state_==CallState::CALLING&&phone.pending_media_start_==VoipStack::ApplicationMediaStart::ORIGINATE);
  assert(s.call_id_.empty()&&!s.media_active_&&!phone.can_accept_session_());assert(first.drain().empty());
 }else{
  first.send_to(s,offered(first,first_account,"owned-first"));expect_response(first,100,"owned-first");dispatch(s,phone,handled);
  if(!phone.auto_answer_){expect_response(first,180,"owned-first");if(phase=="active"){phone.answer_call();phone.drain();}}
  if(phase=="active"){
   expect_response(first,200,"owned-first");first.send_to(s,request(first,"ACK",1,"","owned-first",s.local_tag_,s.remote_tag_));dispatch(s,phone,handled);
   assert(phone.call_state_==CallState::IN_CALL&&s.media_active_&&phone.mic_owned);
  }else{assert(phone.call_state_==CallState::RINGING);phone.auto_answer_=automatic;}
 }
 // The application setter must have published actual packed geometry.
 assert(phone.get_current_tx_audio_format_().sample_rate==16000);
 assert(phone.current_tx_audio_frame_bytes_==phone.tx.nominal_frame_bytes());
 const bool reset_pending=!automatic;
 s.audio_receive_reset_pending_=reset_pending;s.rtp_source_reacquire_pending_=reset_pending;
 const ActiveSnapshot retained(s);const auto formats=phone.snapshot_current_media_formats_();
 const auto sdp_before=s.build_sdp_answer_();const auto next_account=s.next_account_;
 const bool send_enabled=s.audio_send_enabled_.load(),receive_enabled=s.audio_receive_enabled_.load();
 const auto packed_tx=phone.current_tx_audio_format_packed_.load(),packed_rx=phone.current_rx_audio_format_packed_.load();
 const auto frame_bytes=phone.current_tx_audio_frame_bytes_.load();const auto identity=phone.identity.call_id;
 const auto state=phone.call_state_.load();const auto pending=phone.pending_media_start_;
 const auto revision=phone.rx_audio_revision_.load(),rx_state=phone.rx_playout_state_.load();
 const auto starts=phone.mic_starts,watchdog=phone.watchdog_resets,resets=phone.rx_resets,prepares=phone.t.prepares;
 const auto calling=phone.calling_start_time_,ringing=phone.ringing_start_time_;
 const auto rtp_ip=s.remote_rtp_ip_v4_.load();const auto rtp_port=s.remote_rtp_port_.load();
 const auto candidate=offered(second,second_account,"competing-g722",true);
 for(unsigned repeat=0;repeat<2;++repeat){
  clock_ms+=47; // A leaked reset must not hide behind an unchanged host clock.
  second.send_to(s,candidate);const auto rejected=second.drain();assert(rejected.size()==1&&rejected[0].rfind("SIP/2.0 486 ",0)==0);
  assert(header_value(rejected[0],"Call-ID")=="competing-g722");retained.unchanged(s);dispatch(s,phone,handled);
  const auto now=phone.snapshot_current_media_formats_();
  assert(now.caller_to_dest==formats.caller_to_dest&&now.dest_to_caller==formats.dest_to_caller&&now.tx==formats.tx&&now.rx==formats.rx);
  assert(phone.current_tx_audio_format_packed_==packed_tx&&phone.current_rx_audio_format_packed_==packed_rx&&phone.current_tx_audio_frame_bytes_==frame_bytes);
  assert(phone.identity.call_id==identity&&phone.call_state_==state&&phone.pending_media_start_==pending&&phone.auto_answer_==automatic);
  assert(phone.rx_audio_revision_==revision&&phone.rx_playout_state_==rx_state&&phone.rx_resets==resets&&phone.watchdog_resets==watchdog);
  assert(phone.mic_starts==starts&&phone.t.prepares==prepares&&phone.calling_start_time_==calling&&phone.ringing_start_time_==ringing);
  assert(s.remote_rtp_ip_v4_==rtp_ip&&s.remote_rtp_port_==rtp_port&&s.audio_receive_reset_pending_==reset_pending&&s.rtp_source_reacquire_pending_==reset_pending);
  assert(s.build_sdp_answer_()==sdp_before&&s.next_account_==next_account);
  assert(s.audio_send_enabled_==send_enabled&&s.audio_receive_enabled_==receive_enabled);
  assert(first.drain().empty());
 }
 if(phase=="active"){
  AudioFormat received;assert(s.negotiated_rx_payloads_.find(96,&received)&&received==s.selected_rx_format_);
  assert(received.codec==AudioCodec::PCM&&received.nominal_frame_bytes()==512);
  std::array<uint8_t,512>input{},output{};
  for(size_t i=0;i<input.size();i+=2){input[i]=0x12;input[i+1]=0x34;}
  assert(rtp_payload_to_pcm(input.data(),input.size(),received,output.data(),output.size())==input.size());
  for(size_t i=0;i<output.size();i+=2)assert(output[i]==0x34&&output[i+1]==0x12);
 }
 if(preparing){phone.request_call_termination_({CallEndReason::LOCAL_HANGUP,nullptr,SipTerminationAction::NONE,false});phone.drain();}
 else{
  if(phone.call_state_==CallState::RINGING){phone.sink_ready=true;phone.answer_call();phone.drain();expect_response(first,200,"owned-first");first.send_to(s,request(first,"ACK",1,"","owned-first",s.local_tag_,s.remote_tag_));dispatch(s,phone,handled);}
  first.send_to(s,request(first,"BYE",2,"","owned-first",s.local_tag_,s.remote_tag_));expect_response(first,200,"owned-first");dispatch(s,phone,handled);
 }
 assert(phone.call_state_==CallState::IDLE&&phone.identity.call_id.empty()&&!phone.mic_owned);
 assert(phone.pending_media_start_==VoipStack::ApplicationMediaStart::NONE&&phone.deferred.empty());
 assert(s.call_id_.empty()&&!s.media_active_&&!s.terminal_transaction_pending_locked_()&&phone.can_accept_session_());
 assert(s.registration_.is_registered("office")&&s.registration_.is_registered("home"));
 phone.auto_answer_=true;phone.sink_ready=true;
 second.send_to(s,offered(second,second_account,"reuse-after-busy"));expect_response(second,100,"reuse-after-busy");dispatch(s,phone,handled);expect_response(second,200,"reuse-after-busy");
 second.send_to(s,request(second,"ACK",1,"","reuse-after-busy",s.local_tag_,s.remote_tag_));dispatch(s,phone,handled);
 assert(phone.call_state_==CallState::IN_CALL&&phone.identity.call_id=="reuse-after-busy"&&s.active_account_==(home_first?0:1));
 second.send_to(s,request(second,"BYE",2,"","reuse-after-busy",s.local_tag_,s.remote_tag_));expect_response(second,200,"reuse-after-busy");dispatch(s,phone,handled);
 assert(phone.call_state_==CallState::IDLE&&!phone.mic_owned&&s.call_id_.empty()&&!s.media_active_&&phone.deferred.empty());
 for(auto&flow:s.registration_flows_)s.close_tcp_flow_(flow);s.close_tcp_flow_(s.tcp_flow_);s.close_tcp_flow_(s.inbound_tcp_flow_);close(s.sip_socket_);
}
'''
    cpp = folder / "preservation.cpp"
    cpp.write_text(code)
    library = Path(os.environ["ESP_AUDIO_LIBS_SOURCE"])
    sources = ["src/resample/resampler.cpp", "src/resample/art_resampler.cpp", "src/resample/art_biquad.cpp",
               "src/quantization_utils.cpp", "src/memory_utils.cpp", "src/dsp/dsps_dotprod_f32_ansi.c"]
    binary = folder / "preservation"
    built = subprocess.run([
        "g++", "-std=c++17", "-g", "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
        "-DUSE_ESPHOME_VOIP_REGISTRATION", "-I", str(folder), "-I", str(ROOT), "-I", str(SOURCE),
        "-I", str(library / "include"), str(cpp), str(SOURCE / "sip_registration.cpp"),
        *(str(library / source) for source in sources), "-lcrypto", "-o", str(binary),
    ], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize("office,home,first,automatic,phase", [
    ("udp", "udp", "office", "off", "ringing"),
    ("tcp", "udp", "home", "on", "ringing"),
    ("tcp", "tcp", "office", "on", "active"),
    ("udp", "tcp", "home", "off", "active"),
    ("udp", "tcp", "office", "on", "local-preparing"),
    ("tcp", "udp", "home", "off", "local-preparing"),
])
def test_second_registered_account_preserves_real_packed_media_contract(preservation_binary, office, home, first, automatic, phase):
    ran = subprocess.run([preservation_binary, office, home, first, automatic, phase],
                         capture_output=True, text=True, timeout=8)
    assert ran.returncode == 0, ran.stdout + ran.stderr
