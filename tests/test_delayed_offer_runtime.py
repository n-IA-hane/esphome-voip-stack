"""Actual UDP/TCP ACK parsing must finalize the application's media contract."""

import os
from pathlib import Path
import subprocess

import pytest
from test_incoming_setup_runtime import incoming_source
from test_refer_dialog_runtime import block
from test_refer_wire_runtime import wire_source

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "esphome/components/voip_stack"


def delayed_source():
    code = wire_source()
    parser = (COMPONENT / "sip_message.cpp").read_text()
    sdp = (COMPONENT / "sip_sdp.cpp").read_text()
    transport = (COMPONENT / "sip_transport.cpp").read_text()
    responses = (COMPONENT / "sip_request_response.cpp").read_text()
    extra = ""
    for marker in ("std::string sip_request_uri(", "bool parse_rtpmap_format(", "bool parse_audio_media_line("):
        extra += block(parser, marker) + "\n"
    code = code.replace("struct SipPhoneTransport {", extra + "\nstruct SipPhoneTransport {")
    declarations = ""
    for old, marker, source in (
        ("std::string build_sdp_offer_(){", "std::string SipTransport::build_sdp_offer_(", sdp),
        ("bool learn_remote_rtp_from_sdp_(", "bool SipTransport::learn_remote_rtp_from_sdp_(", sdp),
        ("void set_media_config_(", "void SipTransport::set_media_config_(", transport),
        ("bool handle_invite_(", "bool SipTransport::handle_invite_(", transport),
        ("bool send_response_(", "bool SipTransport::send_response_(", responses),
    ):
        body = block(source, marker)
        declaration = body[:body.index("{")].replace("SipTransport::", "").strip()
        declaration = declaration.replace("bool remote_is_answer)", "bool remote_is_answer = false)")
        declaration = declaration.replace("const RtpAudioPayloads *rx_payloads)", "const RtpAudioPayloads *rx_payloads = nullptr)")
        if "send_response_" in marker:
            declaration = 'bool send_response_(uint16_t,const char*,const std::string & = "",const std::string & = "")'
        code = code.replace(block(code, old), declaration + ";", 1)
        code += "\n" + body
    for marker, source in (
        ("std::string SipTransport::wrap_sdp_envelope_(", sdp),
        ("void SipTransport::capture_remote_media_shape_(", sdp),
        ("std::string SipTransport::rejected_media_answer_(", sdp),
        ("std::string SipTransport::build_sdp_answer_(", sdp),
        ("bool SipTransport::send_answer(", transport),
    ):
        body = block(source, marker)
        declarations += body[:body.index("{")].replace("SipTransport::", "").strip() + ";\n"
        code += "\n" + body
    declarations = declarations.replace("const char *direction) const;", 'const char *direction = "sendrecv") const;')
    code = code.replace(" uint16_t sip_port_=5060;", declarations + r'''
 AudioFormatList offer_tx_formats_,offer_rx_formats_;int media_config_lock_=0;
 uint16_t rtp_port_=40000;size_t udp_max_payload_=1200;
 PcmReceiveConverter pcm_receive_converter_;
 bool prepare_media_path(){return pcm_receive_converter_.prepare(negotiated_rx_payloads_,selected_rx_format_);}
 bool commit_media_path(){open_media_session_();return true;}
 void disconnect(){if(!terminal_transaction_pending_locked_())reset_dialog_();}
 void defer_udp_provisional_response_(const std::string&,uint32_t,uint16_t){assert(false);}
 bool handle_reinvite_(const std::string&,const sockaddr_in&){assert(false);return false;}
 uint16_t sip_port_=5060;''', 1)
    code = code.replace("enum class MediaLifecyclePhase {IDLE}", "enum class MediaLifecyclePhase {IDLE,CLEANING,SHUTTING_DOWN}")
    code = code.replace("void local_ip_for_peer_(uint32_t,std::string*out){", "void local_ip_for_peer_(uint32_t,std::string*out)const{")
    code = code.replace("RtpAudioPayloads*p=nullptr){", "RtpAudioPayloads*p=nullptr)const{")
    code = '#define USE_ESPHOME_VOIP_STACK_SPEAKER\n#include "esphome/components/voip_stack/pcm_receive_converter.h"\n#define portENTER_CRITICAL(...) ((void)0)\n#define portEXIT_CRITICAL(...) ((void)0)\n' + code
    app = incoming_source().replace("uint32_t millis(){return 100;}", "")
    app = app.replace("struct Transport {", "struct AppTransport {").replace("Transport t;Transport*transport_", "AppTransport t;AppTransport*transport_")
    app = app.replace("bool Transport::send_final_response", "bool AppTransport::send_final_response")
    app = app.replace("bool prepare_media_path(){++prepares;return prepare_ok;}", "bool prepare_media_path(){++prepares;return prepare_ok&&wire_transport->prepare_media_path();}")
    app = app.replace("bool commit_media_path(){return commit_ok;}", "bool commit_media_path(){return commit_ok&&wire_transport->commit_media_path();}")
    app = app.replace("Snapshot snapshot()const{return {};}", "Snapshot snapshot()const{return {wire_transport->terminal_transaction_pending_locked_(),0};}")
    app = app.replace("void disconnect(){++disconnects;}", "void disconnect(){++disconnects;wire_transport->disconnect();}")
    app = app.replace("bool send_sip_answer_(const std::string&){return true;}", "bool send_sip_answer_(const std::string&id){return t.final_ok&&wire_transport->send_answer(id,rx,tx);}")
    app = app.replace("bool send_sip_bye_(const std::string&){return true;}", "bool send_sip_bye_(const std::string&id){return wire_transport->send_bye_unlocked_(id);}")
    app = app.replace("bool send_sip_ringing_(const std::string&){return t.ringing_ok;}", 'bool send_sip_ringing_(const std::string&){return wire_transport->send_response_(180,"Ringing");}')
    app = app.replace("void answer_call(){}", "void answer_call();bool is_ringing()const{return call_state_==CallState::RINGING;}bool is_connected()const{return true;}")
    app += "\n" + block((COMPONENT / "voip_fsm.cpp").read_text(), "void VoipStack::answer_call(")
    app = app.replace("void set_current_media_formats_(const AudioFormat&,const AudioFormat&,const AudioFormat&,const AudioFormat&){}", "AudioFormat tx,rx;void set_current_media_formats_(const AudioFormat&,const AudioFormat&,const AudioFormat&t,const AudioFormat&r){tx=t;rx=r;}")
    app = app.replace("void set_audio_devices_active_(bool on){if(!on)++media_stops;}", "bool mic_owned=false;unsigned mic_starts=0;void set_audio_devices_active_(bool on){bool requested=on&&tx.is_valid();if(requested&&!mic_owned)++mic_starts;mic_owned=requested;if(!on)++media_stops;}")
    helpers = (ROOT / "tests/fixtures/refer_wire_main.cpp").read_text().split("void auth_case(")[0]
    return code + "\nSipTransport *wire_transport=nullptr;\n" + app + "\n" + helpers


@pytest.fixture(scope="module")
def delayed_binary(tmp_path_factory):
    location = os.environ.get("ESP_AUDIO_LIBS_SOURCE")
    assert location, "ESP_AUDIO_LIBS_SOURCE is required"
    library = Path(location)
    path = tmp_path_factory.mktemp("delayed-offer")
    cpp = path / "delayed.cpp"
    cpp.write_text(delayed_source() + r'''
AudioFormat fmt(unsigned ptime){AudioFormat f;f.frame_ms=ptime;return f;}
void assert_reusable(SipTransport&s,Peer&peer,VoipStack&phone,std::string invite,const std::string&answer){
 phone.auto_answer_=true;phone.t.prepare_ok=phone.t.commit_ok=phone.t.final_ok=true;
 const unsigned starts=phone.mic_starts;auto id=invite.find("Call-ID: delayed");invite.replace(id,16,"Call-ID: second");
 peer.send_to(s,invite);assert(peer.drain().size()==1);phone.on_sip_signal_received_(s.delivered.back());phone.drain();assert(peer.drain().size()==1);
 assert(phone.call_state_==CallState::CONNECTING&&!phone.mic_owned&&phone.mic_starts==starts);
 auto ack=request(peer,"ACK",1,"","second",s.local_tag_,s.remote_tag_);auto len=ack.find("Content-Length: 0");
 ack.replace(len,17,"Content-Type: application/sdp\r\nContent-Length: "+std::to_string(answer.size()));ack+=answer;
 peer.send_to(s,ack);phone.on_sip_signal_received_(s.delivered.back());phone.drain();assert(phone.call_state_==CallState::IN_CALL);
 peer.send_to(s,request(peer,"BYE",2,"","second",s.local_tag_,s.remote_tag_));assert(peer.drain().size()==1);
 phone.on_sip_signal_received_(s.delivered.back());phone.drain();
 assert(phone.call_state_==CallState::IDLE&&!phone.mic_owned&&phone.identity.call_id.empty());
 assert(s.call_id_.empty()&&s.pending_bye_.empty()&&!s.completed_invite_.awaiting_ack);
}
int main(int argc,char**argv){
 const bool tcp=std::string(argv[1])=="tcp";const std::string direction=argv[2];
 const std::string scenario=argc>3?argv[3]:"normal";
 SipTransport s;wire_transport=&s;Peer peer(tcp);setup(s,peer);
 s.call_id_.clear();s.local_tag_.clear();s.remote_tag_.clear();s.media_active_=false;
 s.offer_tx_formats_.count=s.offer_rx_formats_.count=2;
 s.offer_tx_formats_.formats[0]=s.offer_rx_formats_.formats[0]=fmt(16);
 s.offer_tx_formats_.formats[1]=s.offer_rx_formats_.formats[1]=fmt(10);
 if(scenario=="resample"){auto f=fmt(10);f.sample_rate=48000;s.offer_rx_formats_.formats[2]=f;s.offer_rx_formats_.count=3;}
 VoipStack phone;phone.tx_audio_formats_=s.offer_tx_formats_;phone.rx_audio_formats_=s.offer_rx_formats_;
 phone.auto_answer_=scenario!="manual";
 if(scenario=="sendfail")phone.t.final_ok=false;
 auto invite=request(peer,"INVITE",1,"","delayed");auto tag=invite.find(";tag=local");invite.erase(tag,10);
 peer.send_to(s,invite);assert(peer.drain().size()==1);assert(s.delivered.size()==1);
 phone.on_sip_signal_received_(s.delivered[0]);phone.drain();auto accepted=peer.drain();
 if(scenario=="sendfail"){
  assert(accepted.empty()&&!phone.incoming_answer_accepted_&&s.pending_bye_.empty());
  assert(phone.call_state_==CallState::IDLE&&!phone.mic_owned&&phone.identity.call_id.empty());
  const std::string answer="v=0\r\nc=IN IP4 127.0.0.1\r\nm=audio 40002 RTP/AVP 96\r\na=rtpmap:96 L16/16000/1\r\na=ptime:10\r\na=sendrecv\r\n";
  assert_reusable(s,peer,phone,invite,answer);cleanup(s);return 0;
 }
 if(scenario=="manual"){
  assert(phone.call_state_==CallState::RINGING&&accepted.size()==1&&accepted[0].rfind("SIP/2.0 180",0)==0);
  phone.answer_call();phone.drain();accepted=peer.drain();
 }
 assert(accepted.size()==1&&accepted[0].rfind("SIP/2.0 200",0)==0);
 assert(phone.incoming_answer_accepted_);
 assert(phone.call_state_==CallState::CONNECTING&&!phone.mic_owned&&phone.mic_starts==0&&"Provisional formats must not activate devices or establish a call");
 auto ack=request(peer,"ACK",1,"","delayed",s.local_tag_,s.remote_tag_);
 const std::string maps=scenario=="resample"?"96 97\r\na=rtpmap:96 L16/16000/1\r\na=rtpmap:97 L16/48000/1":"96\r\na=rtpmap:96 L16/16000/1";
 const std::string answer="v=0\r\nc=IN IP4 127.0.0.1\r\nm=audio 40002 RTP/AVP "+maps+"\r\na=ptime:10\r\na="+direction+"\r\n";
 auto length=ack.find("Content-Length: 0");ack.replace(length,17,"Content-Type: application/sdp\r\nContent-Length: "+std::to_string(answer.size()));ack+=answer;
 auto stale=ack;auto cseq=stale.find("CSeq: 1 ACK");assert(cseq!=std::string::npos);stale.replace(cseq,11,"CSeq: 2 ACK");
 peer.send_to(s,stale);assert(s.delivered.size()==1&&!s.media_active_&&phone.call_state_==CallState::CONNECTING);
 if(scenario=="missing"){
  auto body=ack.find("\r\n\r\n");ack.resize(body+4);auto len=ack.find("Content-Length:");ack.replace(len,ack.find("\r\n",len)-len,"Content-Length: 0");
 }
 if(scenario=="invalid"){
  auto map=ack.find("L16/16000/1");assert(map!=std::string::npos);ack.replace(map,11,"L16/32000/1");
 }
 if(scenario=="cancel"){
  phone.request_call_termination_({CallEndReason::LOCAL_HANGUP,nullptr,SipTerminationAction::AUTO,true});phone.drain();
  assert(s.terminate_after_invite_ack_&&peer.drain().empty()&&!phone.mic_owned);
  peer.send_to(s,ack);auto bye=peer.drain();assert(bye.size()==1&&bye[0].rfind("BYE ",0)==0&&s.delivered.size()==1);
  peer.send_to(s,response(bye[0],200));phone.on_sip_signal_received_(s.delivered.back());phone.drain();
  assert(phone.call_state_==CallState::IDLE&&!phone.mic_owned);
  assert_reusable(s,peer,phone,invite,answer);cleanup(s);return 0;
 }
 if(scenario=="prepare")phone.t.prepare_ok=false;
 if(scenario=="commit")phone.t.commit_ok=false;
 if(scenario=="timeout"){
  clock_ms+=SIP_TRANSACTION_TIMEOUT_MS+1;s.pump_udp_retransmits_unlocked_();
 }else peer.send_to(s,ack);
 if(scenario=="missing"||scenario=="invalid"||scenario=="prepare"||scenario=="commit"||scenario=="timeout"){
  assert(s.delivered.size()==2);phone.on_sip_signal_received_(s.delivered.back());phone.drain();
  auto bye=peer.drain();assert(bye.size()==1&&bye[0].rfind("BYE ",0)==0);
  assert(phone.call_state_!=CallState::IN_CALL&&!phone.mic_owned&&!s.pending_bye_.empty());
  peer.send_to(s,ack);assert(peer.drain().empty()&&s.delivered.size()==2&&!s.media_active_);
  peer.send_to(s,response(bye[0],200));phone.on_sip_signal_received_(s.delivered.back());phone.drain();
  assert(phone.call_state_==CallState::IDLE&&phone.identity.call_id.empty()&&s.call_id_.empty());
  assert_reusable(s,peer,phone,invite,answer);cleanup(s);return 0;
 }
 assert(s.delivered.size()==2&&"ACK must publish the final media contract once");
 phone.on_sip_signal_received_(s.delivered.back());phone.drain();
 assert(phone.call_state_==CallState::IN_CALL&&s.media_active_);
 assert(phone.tx.is_valid()==(direction!="sendonly"));assert(phone.rx.is_valid()==(direction!="recvonly"));
 assert(phone.mic_owned==phone.tx.is_valid());
 if(phone.rx.is_valid())assert(phone.rx.frame_ms==10);
 if(phone.tx.is_valid())assert(phone.tx.nominal_frame_bytes()==320);
 if(scenario=="resample"){
  assert(phone.rx.sample_rate==48000);std::vector<uint8_t> source(320,1),output(960);
  const size_t generated=s.pcm_receive_converter_.convert(fmt(10),source.data(),source.size(),output.data(),output.size());
  assert(generated>800&&generated<=960&&"Final ACK must prepare the actual receive resampler before admission");
 }
 peer.send_to(s,ack);assert(s.delivered.size()==2&&"Duplicate ACK must not finalize twice");
 peer.send_to(s,request(peer,"BYE",2,"","delayed",s.local_tag_,s.remote_tag_));assert(peer.drain().size()==1);
 phone.on_sip_signal_received_(s.delivered.back());phone.drain();assert(phone.call_state_==CallState::IDLE&&!phone.mic_owned);
 // A fresh generation must be immediately reusable after final cleanup.
 assert_reusable(s,peer,phone,invite,answer);
 cleanup(s);
}
''')
    sources = ["src/resample/resampler.cpp", "src/resample/art_resampler.cpp", "src/resample/art_biquad.cpp", "src/quantization_utils.cpp", "src/memory_utils.cpp", "src/dsp/dsps_dotprod_f32_ansi.c"]
    binary = path / "delayed"
    result = subprocess.run(["g++", "-std=c++17", "-I", str(ROOT), "-I", str(library / "include"), str(cpp), *(str(library / source) for source in sources), "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("transport", ["udp", "tcp"])
@pytest.mark.parametrize("direction", ["sendrecv", "sendonly", "recvonly"])
@pytest.mark.parametrize("scenario", ["normal", "manual"])
def test_delayed_ack_finalizes_application_format_and_device_ownership(delayed_binary, transport, direction, scenario):
    result = subprocess.run([str(delayed_binary), transport, direction, scenario], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("transport", ["udp", "tcp"])
@pytest.mark.parametrize("scenario", ["resample", "missing", "invalid", "prepare", "commit", "timeout", "cancel", "sendfail"])
def test_delayed_ack_failure_or_resampler_uses_final_contract(delayed_binary, transport, scenario):
    result = subprocess.run([str(delayed_binary), transport, "sendrecv", scenario], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
