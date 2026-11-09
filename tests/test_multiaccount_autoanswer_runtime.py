"""Two registered accounts exercise the real application and SIP busy gates.

Socket ingress, registration, SDP, auto-answer and termination use production
methods. Speaker readiness, device ownership and scheduler draining remain
explicit host adapters, as in the existing application preparation fixture.
"""

import os
from pathlib import Path
import subprocess

import pytest

from test_delayed_offer_runtime import delayed_source
from test_refer_dialog_runtime import ROOT, SOURCE, block


@pytest.fixture(scope="module")
def autoanswer_busy_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("multiaccount-autoanswer")
    defines = folder / "esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    code = "#include <functional>\n" + delayed_source(registered=True)
    code = code.replace("uint32_t esp_random(){return 42;}",
                        "uint32_t esp_random(){static uint32_t next=42;return ++next;}")
    code = code.replace("struct VoipStack {", "struct VoipStack {\n bool sink_ready=false;bool can_accept_session_()const;")
    code = code.replace("bool should_accept_session_()const{return true;}",
                        "std::function<bool()>accept_session;bool should_accept_session_()const{return !accept_session||accept_session();}")
    code = code.replace("bool prepare_speaker_for_current_rx_(){return true;}",
                        "bool prepare_speaker_for_current_rx_(){return sink_ready;}")
    # Registration cleanup downcasts the production owning pointer. Route
    # that observer to the real transport while retaining hardware adapters.
    code = code.replace("AppTransport t;AppTransport*transport_=&t;", r'''
 AppTransport t;
 struct AppTransportHandle {
  AppTransport*adapter;
  operator AppTransport*()const{return adapter;}
  AppTransport*operator->()const{return adapter;}
  SipTransport*get()const{return wire_transport;}
 }transport_{&t};
''')
    registration = (ROOT / "tests/fixtures/registration_transport.cpp").read_text()
    code += "\n" + registration[:registration.index("void start_account_call(")]
    code += "\n" + (ROOT / "tests/fixtures/multiaccount_global_busy.cpp").read_text()
    code += "\n" + block((SOURCE / "voip_fsm.cpp").read_text(), "bool VoipStack::can_accept_session_(")
    code += r'''
std::string offered_invite(Peer&peer,const std::string&account,const std::string&id,bool competing=false){
 auto invite=initial_invite(peer,account,id);
 const std::string body=std::string("v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n")+
  (competing?"m=audio 31000 RTP/AVP 9\r\na=rtpmap:9 G722/8000\r\na=ptime:20\r\n":
             "m=audio 30000 RTP/AVP 96\r\na=rtpmap:96 L16/16000/1\r\na=ptime:16\r\n")+"a=sendrecv\r\n";
 const auto at=invite.find("Content-Length: 3");assert(at!=std::string::npos);
 invite.replace(at,strlen("Content-Length: 3"),"Content-Type: application/sdp\r\nContent-Length: "+std::to_string(body.size()));
 invite.resize(invite.find("\r\n\r\n")+4);return invite+body;
}
void dispatch(SipTransport&s,VoipStack&phone,size_t&handled){
 while(handled<s.delivered.size())phone.on_sip_signal_received_(s.delivered[handled++]);
 phone.drain();
}
void finish_owned(SipTransport&s,VoipStack&phone,Peer&peer,size_t&handled){
 const auto id=s.call_id_;
 peer.send_to(s,request(peer,"BYE",2,"",id,s.local_tag_,s.remote_tag_));
 expect_response(peer,200,id);dispatch(s,phone,handled);
 assert(phone.call_state_==CallState::IDLE&&phone.identity.call_id.empty()&&!phone.mic_owned);
 assert(phone.pending_media_start_==VoipStack::ApplicationMediaStart::NONE);
 assert(s.call_id_.empty()&&!s.media_active_&&!s.terminal_transaction_pending_locked_());
 assert(phone.can_accept_session_());
}
int main(int argc,char**argv){
 assert(argc==6);
 Peer office(std::string(argv[1])=="tcp"),home(std::string(argv[2])=="tcp");
 SipTransport s;wire_transport=&s;bind_account(s,office);bind_home(s,home);
 s.call_id_.clear();s.local_tag_.clear();s.remote_tag_.clear();s.media_active_=false;
 s.accept_direct_calls_=false;audio_format_list_default(&s.offer_tx_formats_);audio_format_list_default(&s.offer_rx_formats_);
 VoipStack phone;size_t handled=0;s.accept_session=[&](){return phone.can_accept_session_();};
 const bool home_first=std::string(argv[3])=="home",automatic=std::string(argv[4])=="on";
 const std::string phase=argv[5];
 Peer&first=home_first?home:office;Peer&second=home_first?office:home;
 const std::string first_account=home_first?"home":"office",second_account=home_first?"office":"home";
 phone.auto_answer_=phase=="ringing"||phase=="ringing-auto"?false:automatic;
 phone.sink_ready=phase=="in_call";
 const auto invite=offered_invite(first,first_account,"owned-first");
 first.send_to(s,invite);expect_response(first,100,"owned-first");
 assert(s.delivered.size()==1&&s.call_id_=="owned-first"&&s.active_account_==(home_first?1:0));
 if(phase!="queued"){
  dispatch(s,phone,handled);
  if(!phone.auto_answer_){
   expect_response(first,180,"owned-first");
   assert(phone.call_state_==CallState::RINGING);
   if(phase!="ringing"&&phase!="ringing-auto"){phone.answer_call();phone.drain();}
  }
  if(phase=="ringing-auto")phone.auto_answer_=true;
  if(phase=="in_call"){
   expect_response(first,200,"owned-first");
   first.send_to(s,request(first,"ACK",1,"","owned-first",s.local_tag_,s.remote_tag_));
   dispatch(s,phone,handled);
   assert(phone.call_state_==CallState::IN_CALL&&phone.mic_owned&&s.media_active_);
  }else if(phase=="connecting"){
   assert(phone.call_state_==CallState::CONNECTING&&phone.incoming_setup_pending_);
   assert(phone.pending_media_start_==VoipStack::ApplicationMediaStart::ANSWER&&!s.media_active_);
   assert(first.drain().empty());
  }
 }
 // A distinct registered account must never reach the application's auto-answer.
 const ActiveSnapshot owned(s);const auto app_id=phone.identity.call_id;
 const auto app_state=phone.call_state_.load();const auto tx=phone.tx,rx=phone.rx;
 const auto action=phone.pending_media_start_;const auto revision=phone.rx_audio_revision_.load();
 const auto mic_starts=phone.mic_starts,prepares=phone.t.prepares;const bool mic=phone.mic_owned;
 const auto rtp_ip=s.remote_rtp_ip_v4_.load();const auto rtp_port=s.remote_rtp_port_.load();
 assert(rtp_port==30000);
 auto unchanged=[&](){
  owned.unchanged(s);
  assert(s.remote_rtp_ip_v4_==rtp_ip&&s.remote_rtp_port_==rtp_port);
  // Preserve the deliberately queued first signal until its explicit turn.
  if(phase!="queued")dispatch(s,phone,handled);
  assert(phone.identity.call_id==app_id&&phone.call_state_==app_state);
  assert(phone.tx==tx&&phone.rx==rx&&phone.pending_media_start_==action);
  assert(phone.rx_audio_revision_==revision&&phone.mic_owned==mic&&phone.mic_starts==mic_starts&&phone.t.prepares==prepares);
  assert(first.drain().empty());
 };
 // Busy admission must precede SDP/media admission. An unrelated G722 offer
 // cannot redirect the first PCM stream or replace its negotiated formats.
 const auto competing=offered_invite(second,second_account,"owned-second",true);
 for(unsigned repeat=0;repeat<2;++repeat){
  second.send_to(s,competing);const auto rejected=second.drain();
  assert(rejected.size()==1&&rejected[0].rfind("SIP/2.0 486 ",0)==0);
  assert(header_value(rejected[0],"Call-ID")=="owned-second");unchanged();
  // RFC3261 non-2xx ACK belongs to the rejected transaction, including its
  // original Via branch. Neither it nor its duplicate owns the first call.
  auto ack=competing.substr(0,competing.find("\r\n\r\n")+4);ack.replace(0,6,"ACK");
  auto at=ack.find("CSeq: 1 INVITE");assert(at!=std::string::npos);
  ack.replace(at,strlen("CSeq: 1 INVITE"),"CSeq: 1 ACK");
  at=ack.find("Content-Length:");const auto end=ack.find("\r\n",at);
  ack.replace(at,end-at,"Content-Length: 0");
  const auto to=header_value(ack,"To");at=ack.find("To: "+to);
  ack.replace(at,strlen("To: ")+to.size(),"To: "+header_value(rejected[0],"To"));
  assert(via_branch(header_value(ack,"Via"))==via_branch(header_value(competing,"Via")));
  for(unsigned duplicate=0;duplicate<2;++duplicate){
   second.send_to(s,ack);assert(second.drain().empty());unchanged();
  }
 }
 // Complete only the first generation, then independently admit the other line.
 if(phase=="queued")dispatch(s,phone,handled);
 if(phone.call_state_==CallState::RINGING){
  if(phase=="queued")expect_response(first,180,"owned-first");
  phone.answer_call();phone.drain();
 }
 if(phone.call_state_!=CallState::IN_CALL){
  phone.sink_ready=true;phone.pump_application_media_start_();
  expect_response(first,200,"owned-first");
  first.send_to(s,request(first,"ACK",1,"","owned-first",s.local_tag_,s.remote_tag_));dispatch(s,phone,handled);
 }
 assert(phone.call_state_==CallState::IN_CALL&&phone.identity.call_id=="owned-first");
 finish_owned(s,phone,first,handled);
 assert(s.registration_.is_registered("office")&&s.registration_.is_registered("home"));
 phone.auto_answer_=true;phone.sink_ready=true;
 second.send_to(s,offered_invite(second,second_account,"after-busy"));expect_response(second,100,"after-busy");
 dispatch(s,phone,handled);expect_response(second,200,"after-busy");
 second.send_to(s,request(second,"ACK",1,"","after-busy",s.local_tag_,s.remote_tag_));dispatch(s,phone,handled);
 assert(phone.call_state_==CallState::IN_CALL&&phone.identity.call_id=="after-busy"&&s.active_account_==(home_first?0:1));
 finish_owned(s,phone,second,handled);
 for(auto&flow:s.registration_flows_)s.close_tcp_flow_(flow);
 s.close_tcp_flow_(s.tcp_flow_);s.close_tcp_flow_(s.inbound_tcp_flow_);close(s.sip_socket_);
}
'''
    cpp = folder / "autoanswer_busy.cpp"
    cpp.write_text(code)
    library = Path(os.environ["ESP_AUDIO_LIBS_SOURCE"])
    sources = ["src/resample/resampler.cpp", "src/resample/art_resampler.cpp", "src/resample/art_biquad.cpp",
               "src/quantization_utils.cpp", "src/memory_utils.cpp", "src/dsp/dsps_dotprod_f32_ansi.c"]
    binary = folder / "autoanswer_busy"
    built = subprocess.run([
        "g++", "-std=c++17", "-g", "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
        "-DUSE_ESPHOME_VOIP_REGISTRATION", "-I", str(folder), "-I", str(ROOT), "-I", str(SOURCE),
        "-I", str(library / "include"), str(cpp), str(SOURCE / "sip_registration.cpp"),
        *(str(library / source) for source in sources), "-lcrypto", "-o", str(binary),
    ], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize("office_transport,home_transport", [("udp", "udp"), ("udp", "tcp"), ("tcp", "udp"), ("tcp", "tcp")])
@pytest.mark.parametrize("first_account", ["home", "office"])
@pytest.mark.parametrize("automatic,phase", [
    ("on", "in_call"), ("off", "in_call"), ("on", "connecting"), ("off", "connecting"),
    ("on", "queued"), ("off", "queued"), ("off", "ringing"), ("on", "ringing-auto"),
])
def test_registered_accounts_cannot_bypass_application_busy_with_autoanswer(
    autoanswer_busy_binary, office_transport, home_transport, first_account, automatic, phase,
):
    result = subprocess.run([
        autoanswer_busy_binary, office_transport, home_transport, first_account, automatic, phase,
    ], capture_output=True, text=True, timeout=8)
    assert result.returncode == 0, result.stdout + result.stderr
