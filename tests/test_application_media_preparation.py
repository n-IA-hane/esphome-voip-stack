"""Application preparation must yield before committing the SIP/media promise."""

from pathlib import Path
import os
import re
import subprocess

import pytest

from test_incoming_setup_runtime import incoming_source
from test_refer_dialog_runtime import block

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "esphome/components/voip_stack"


def test_running_input_without_output_progress_cannot_answer(tmp_path):
    code = incoming_source()
    code = code.replace("unsigned prepares=0,disconnects=0,aborts=0;",
                        "unsigned prepares=0,disconnects=0,aborts=0,answers=0;")
    code = code.replace("bool send_sip_answer_(const std::string&){return true;}",
                        "bool send_sip_answer_(const std::string&){++t.answers;return true;}")
    code += r'''
int main(){
 VoipStack phone;phone.rx_clock_ready=false;SipSignal invite;invite.type=SipSignalType::INVITE;
 invite.call_id="running-without-output";invite.caller_name="caller";invite.caller_route="100";
 invite.dest_name="esp";invite.dest_route="esp";
 audio_format_list_default(&invite.caller_tx_formats);audio_format_list_default(&invite.caller_rx_formats);
 phone.on_sip_signal_received_(invite);
 assert(phone.t.answers==0&&"Input RUNNING without current source output progress is not prepared");
}
'''
    cpp = tmp_path / "no_output.cpp"
    cpp.write_text(code)
    executable = tmp_path / "no_output"
    built = subprocess.run(["g++", "-std=c++17", "-DUSE_ESPHOME_VOIP_STACK_SPEAKER", "-I", str(ROOT),
                            str(cpp), "-o", str(executable)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    run = subprocess.run(["bash", "-c", 'ulimit -c 0; exec "$1"', "bash", str(executable)],
                         capture_output=True, text=True)
    assert run.returncode == 0, run.stderr


@pytest.mark.parametrize("speaker", [False, True])
def test_actual_preparation_yields_preserves_prebuffer_and_cancels(tmp_path, speaker):
    fsm = (COMPONENT / "voip_fsm.cpp").read_text()
    audio = (COMPONENT / "voip_audio.cpp").read_text()
    source = (ROOT / "tests/fixtures/application_media_preparation.cpp").read_text()
    header = (COMPONENT / "voip_stack.h").read_text()
    start = header.index("  enum class ApplicationMediaStart")
    enum = header[start:header.index(";", start) + 1].strip()
    source = source.replace("enum class ApplicationMediaStart:uint8_t{NONE,ORIGINATE,ANSWER,CONFIRM,OFFER};", enum)
    for marker in ("void VoipStack::start_speaker_for_current_rx_(",
                   "bool VoipStack::prepare_speaker_for_current_rx_(",
                   "void VoipStack::begin_application_media_start_(",
                   "void VoipStack::finish_application_media_start_(",
                   "void VoipStack::pump_application_media_start_(", "void VoipStack::set_in_call_("):
        source += "\n" + block(fsm, marker)
    for marker in ("void VoipStack::reset_rx_audio_(", "void VoipStack::play_rx_frame_(",
                   "void VoipStack::play_silence_frame_(", "VoipStack::RxPlayoutPhase VoipStack::rx_playout_phase_(",
                   "bool VoipStack::prepare_rx_playout_(", "bool VoipStack::activate_rx_playout_(",
                   "void VoipStack::speaker_output_progress_(", "bool VoipStack::play_rx_preroll_("):
        source += "\n" + block(audio, marker)
    source += r'''
int main(){
 Speaker sink;VoipStack phone;phone.speaker_=&sink;
 uint8_t storage[16*960]{},pcm[960]{},out[960]{};RtpJitterBuffer jitter(storage,960,16,4);
 phone.rx_jitter_buffer_=&jitter;phone.reset_rx_audio_();unsigned claimed=phone.rx_audio_revision_;
 using Action=VoipStack::ApplicationMediaStart;static_assert(sizeof(Action)==1);
 unsigned &invites=phone.invites;
 phone.begin_application_media_start_(Action::ORIGINATE);
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
 assert(!invites&&phone.pending_media_start_!=Action::NONE);
 for(clock_ms=10;clock_ms<600;clock_ms+=10){sink.progress();phone.pump_application_media_start_();assert(!invites);}
 sink.progress();phone.pump_application_media_start_();assert(invites==1&&sink.is_running());
 assert(clock_ms==600&&"Only normal speaker progress, never an arbitrary extra delay");
#else
 assert(invites==1&&!sink.starts);
#endif
 // Current-call packets can reach CONNECTING before the public IN_CALL gate.
 phone.call_state_=CallState::CONNECTING;
 for(unsigned n=0;n<4;++n)assert(jitter.push({pcm,960,uint16_t(n),n*480,true,false}));
 phone.activate_rx_playout_();phone.set_in_call_(true);
 assert(phone.rx_audio_revision_==claimed&&jitter.depth()==4&&!jitter.counters().drops);
 assert(jitter.read(out,960)==RtpJitterBuffer::ReadResult::FRAME);
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
 phone.play_rx_frame_(out,960,SilenceReason::NONE,10,claimed);assert(sink.accepted==960);
 // An already ready matching sink adds no preparation latency.
 phone.call_state_=CallState::CALLING;unsigned starts=sink.starts;
 phone.begin_application_media_start_(Action::ORIGINATE);assert(invites==2&&sink.starts==starts);
 // Changing the actual negotiated geometry requires a completed normal stop/start.
 phone.format.sample_rate=16000;
 phone.begin_application_media_start_(Action::ORIGINATE);assert(invites==2&&sink.stop_requested);
 sink.progress();phone.pump_application_media_start_();assert(invites==2&&sink.start_requested);
 sink.progress();phone.pump_application_media_start_();assert(invites==3&&sink.info.rate==16000);
 // Cancel a queued start even while the public speaker state is still STOPPED.
 sink.state=Speaker::STOPPED;sink.ready_at=1000;phone.reset_rx_audio_();
 phone.begin_application_media_start_(Action::ORIGINATE);assert(sink.start_requested);
 unsigned old_revision=phone.rx_audio_revision_;phone.request_call_termination_({});
 clock_ms=1200;sink.progress();phone.pump_application_media_start_();
 assert(invites==3&&phone.pending_media_start_==Action::NONE&&!sink.start_requested&&!sink.is_running());
 phone.call_state_=CallState::CALLING;phone.id="next";phone.audio_devices_active_=true;phone.reset_rx_audio_();
 sink.state=Speaker::RUNNING;unsigned accepted=sink.accepted;
 phone.play_rx_frame_(out,960,SilenceReason::NONE,10,old_revision);assert(sink.accepted==accepted);
 // A readiness callback can reenter termination/new-call setup.
 sink.state=Speaker::STOPPED;sink.ready_at=1800;phone.calling_start_time_=1200;
 sink.on_start=[&](){sink.on_start={};phone.request_call_termination_({});
  phone.call_state_=CallState::CALLING;phone.id="replacement";phone.reset_rx_audio_();phone.audio_devices_active_=true;
  phone.begin_application_media_start_(Action::ORIGINATE);};
 phone.begin_application_media_start_(Action::ORIGINATE);
 assert(invites==3);
 sink.progress();phone.pump_application_media_start_();clock_ms=1800;
 sink.progress();phone.pump_application_media_start_();assert(invites==4&&phone.last_sent=="replacement");
 // The same existing setup deadline bounds a permanently unavailable sink.
 phone.call_state_=CallState::CONNECTING;phone.incoming_setup_pending_=true;
 sink.state=Speaker::STOPPED;sink.ready_at=UINT32_MAX;phone.calling_start_time_=1800;
 phone.begin_application_media_start_(Action::ANSWER);clock_ms=6800;phone.pump_application_media_start_();
 assert(phone.terminated==3&&phone.pending_media_start_==Action::NONE&&invites==4&&!phone.transport.answers);
 // A speaker-equipped endpoint negotiating no receive direction must not wait.
 phone.call_state_=CallState::CALLING;phone.id="no-receive";phone.calling_start_time_=6800;
 phone.format=disabled_audio_format();
 phone.begin_application_media_start_(Action::ORIGINATE);assert(invites==5);
#endif
}
'''
    helper = tmp_path / "esphome/core/helpers.h"
    helper.parent.mkdir(parents=True)
    helper.write_text("namespace esphome {struct Mutex {};struct LockGuard {explicit LockGuard(Mutex&){}};}\n")
    cpp = tmp_path / "application.cpp"
    cpp.write_text(source)
    executable = tmp_path / "application"
    flags = ["-DUSE_ESP32", "-I", str(tmp_path), "-I", str(ROOT)]
    if speaker:
        flags.append("-DUSE_ESPHOME_VOIP_STACK_SPEAKER")
    result = subprocess.run(["g++", "-std=c++17", *flags, str(cpp), "-o", str(executable)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run(["bash", "-c", 'ulimit -c 0; exec "$1"', "bash", str(executable)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


def test_incoming_does_not_answer_before_async_sink_is_ready(tmp_path):
    code = incoming_source()
    code = code.replace("unsigned prepares=0,disconnects=0,aborts=0;",
                        "unsigned prepares=0,disconnects=0,aborts=0,answers=0;")
    code = code.replace("bool send_sip_answer_(const std::string&){return true;}",
                        "bool send_sip_answer_(const std::string&){++t.answers;return true;}")
    code = code.replace("struct VoipStack {", "struct VoipStack {\n bool speaker_ready=false;")
    if "prepare_speaker_for_current_rx_" in code:
        code = code.replace("bool prepare_speaker_for_current_rx_(){return true;}",
                            "bool prepare_speaker_for_current_rx_(){return speaker_ready;}")
    pump = "phone.pump_application_media_start_();" if "void VoipStack::pump_application_media_start_(" in code else ""
    cpp = tmp_path / "prepare.cpp"
    cpp.write_text(code + r'''
int main(){
 VoipStack phone;SipSignal invite;invite.type=SipSignalType::INVITE;
 invite.call_id="prepare";invite.caller_name="caller";invite.caller_route="100";
 invite.dest_name="esp";invite.dest_route="esp";
 audio_format_list_default(&invite.caller_tx_formats);audio_format_list_default(&invite.caller_rx_formats);
 phone.on_sip_signal_received_(invite);
 assert(phone.t.answers==0&&"200 must wait for the asynchronous application sink");
 assert(phone.call_state_==CallState::CONNECTING&&phone.incoming_setup_pending_);
 phone.speaker_ready=true;
''' + pump + r'''
 assert(phone.t.answers==1&&phone.call_state_==CallState::IN_CALL);
}
''')
    executable = tmp_path / "prepare"
    result = subprocess.run(["g++", "-std=c++17", "-DUSE_ESPHOME_VOIP_STACK_SPEAKER", "-I", str(ROOT), str(cpp), "-o", str(executable)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run(["bash", "-c", 'ulimit -c 0; exec "$1"', "bash", str(executable)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


@pytest.fixture(scope="module")
def preparation_wire_binary(tmp_path_factory):
    from test_delayed_offer_runtime import delayed_source

    code = "#include <functional>\n" + delayed_source().replace(
        "struct VoipStack {", "struct VoipStack {\n bool sink_ready=false;bool can_accept_session_()const;")
    code = code.replace("bool should_accept_session_()const{return true;}",
                        "std::function<bool()>accept_session;bool should_accept_session_()const{return !accept_session||accept_session();}")
    code = code.replace("bool prepare_speaker_for_current_rx_(){return true;}",
                        "bool prepare_speaker_for_current_rx_(){return sink_ready;}")
    code = code.replace(
        "bool send_sip_invite_(const std::string&,const std::string&,const std::string&,const std::string&,const std::string&){++t.invites;return true;}",
        "bool send_sip_invite_(const std::string&a,const std::string&b,const std::string&c,const std::string&d,const std::string&e){return wire_transport->send_invite_unlocked_(a,b,c,d,e);}",
    )
    code = code.replace(
        "bool originate(const std::string&,uint16_t){return true;}void set_sip_signaling_transport(bool){}",
        "bool originate(const std::string&ip,uint16_t port){wire_transport->remote_ip_v4_=ntohl(inet_addr(ip.c_str()));wire_transport->remote_sip_port_=port;return true;}void set_sip_signaling_transport(bool tcp){wire_transport->remote_sip_tcp_=tcp;}",
    )
    code += "\n" + block((COMPONENT / "voip_fsm.cpp").read_text(), "bool VoipStack::can_accept_session_(")
    code += r'''
int main(int argc,char**argv){
 const bool tcp=std::string(argv[1])=="tcp";
 SipTransport s;wire_transport=&s;Peer peer(tcp);setup(s,peer);
 s.call_id_.clear();s.local_tag_.clear();s.remote_tag_.clear();s.media_active_=false;
 audio_format_list_default(&s.offer_tx_formats_);audio_format_list_default(&s.offer_rx_formats_);
 VoipStack phone;phone.prepared_peer_ip_=INADDR_LOOPBACK;phone.prepared_peer_port_=ntohs(peer.address.sin_port);phone.prepared_peer_tcp_=tcp;
 s.accept_session=[&](){return phone.can_accept_session_();};
 if(std::string(argv[2])=="outgoing"){
  phone.set_call_identity_("prepared","esp","esp","peer","peer");phone.call_state_=CallState::CALLING;
  phone.calling_start_time_=millis();
  phone.begin_application_media_start_(VoipStack::ApplicationMediaStart::ORIGINATE);
  assert(peer.drain().empty()&&s.call_id_.empty());
  phone.sink_ready=true;phone.pump_application_media_start_();
  auto messages=peer.drain();assert(messages.size()==1&&messages[0].rfind("INVITE ",0)==0);
 }else{
  const std::string body="v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n"
    "m=audio 30000 RTP/AVP 96\r\na=rtpmap:96 L16/16000/1\r\na=ptime:16\r\na=sendrecv\r\n";
  const bool collision=std::string(argv[2])=="collision";
  if(collision){
   phone.set_call_identity_("prepared","esp","esp","peer","peer");phone.call_state_=CallState::CALLING;
   phone.calling_start_time_=millis();
   phone.begin_application_media_start_(VoipStack::ApplicationMediaStart::ORIGINATE);
  }
  auto invite=request(peer,"INVITE",1,"",collision?"intruder":"prepared");
  invite.erase(invite.find(";tag=local"),10);
  invite.replace(invite.find("Content-Length: 0"),17,"Content-Type: application/sdp\r\nContent-Length: "+std::to_string(body.size()));
  invite+=body;peer.send_to(s,invite);
  auto provisional=peer.drain();
  if(collision){
   assert(provisional.size()==1&&provisional[0].rfind("SIP/2.0 486",0)==0);
   assert(s.call_id_.empty()&&s.delivered.empty()&&phone.identity.call_id=="prepared");
   phone.sink_ready=true;phone.pump_application_media_start_();
   auto outbound=peer.drain();assert(outbound.size()==1&&outbound[0].rfind("INVITE ",0)==0&&s.call_id_=="prepared");
   cleanup(s);return 0;
  }
  assert(provisional.size()==1&&provisional[0].rfind("SIP/2.0 100",0)==0);
  assert(s.delivered.size()==1);phone.on_sip_signal_received_(s.delivered.back());phone.drain();
  assert(peer.drain().empty()&&phone.call_state_==CallState::CONNECTING&&!s.media_active_);
  peer.send_to(s,invite);auto retry=peer.drain();
  assert(retry.size()==1&&retry[0].rfind("SIP/2.0 100",0)==0&&s.delivered.size()==1);
  phone.sink_ready=true;phone.pump_application_media_start_();
  auto accepted=peer.drain();assert(accepted.size()==1&&accepted[0].rfind("SIP/2.0 200",0)==0);
  assert(phone.call_state_==CallState::IN_CALL&&s.media_active_);
  peer.send_to(s,invite);retry=peer.drain();
  assert(retry.size()==1&&retry[0].rfind("SIP/2.0 200",0)==0&&s.delivered.size()==1);
 }
 cleanup(s);
}
'''
    folder = tmp_path_factory.mktemp("application-wire")
    cpp = folder / "wire.cpp"
    cpp.write_text(code)
    library = Path(os.environ["ESP_AUDIO_LIBS_SOURCE"])
    sources = ["src/resample/resampler.cpp", "src/resample/art_resampler.cpp", "src/resample/art_biquad.cpp",
               "src/quantization_utils.cpp", "src/memory_utils.cpp", "src/dsp/dsps_dotprod_f32_ansi.c"]
    binary = folder / "wire"
    result = subprocess.run(["g++", "-std=c++17", "-I", str(ROOT), "-I", str(library / "include"), str(cpp),
                             *(str(library / source) for source in sources), "-o", str(binary)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("transport", ["udp", "tcp"])
@pytest.mark.parametrize("direction", ["outgoing", "incoming", "collision"])
def test_wire_offer_and_answer_wait_for_application_sink(preparation_wire_binary, transport, direction):
    result = subprocess.run([str(preparation_wire_binary), transport, direction], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr


def test_preparation_continuation_has_no_heap_or_extra_call_identity():
    fsm = (COMPONENT / "voip_fsm.cpp").read_text()
    for marker in ("void VoipStack::begin_application_media_start_(",
                   "void VoipStack::pump_application_media_start_(",
                   "void VoipStack::finish_application_media_start_("):
        body = block(fsm, marker)
        body = re.sub(r"//[^\n]*", "", body)
        assert "std::function" not in body and "[this" not in body
        assert "new " not in body and "defer(" not in body
    header = (COMPONENT / "voip_stack.h").read_text()
    guarded = False
    stack = []
    for line in header.splitlines():
        if line.startswith("#if"):
            stack.append(guarded)
            guarded |= "USE_ESPHOME_VOIP_STACK_SPEAKER" in line
        elif line.startswith("#endif"):
            guarded = stack.pop()
        if any(field in line for field in ("ApplicationMediaStart pending_media_start_", "prepared_peer_ip_",
                                            "prepared_peer_port_", "prepared_peer_tcp_")):
            assert guarded
