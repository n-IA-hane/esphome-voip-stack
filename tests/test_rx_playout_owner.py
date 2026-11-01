"""Real RX owner, callback readiness and a deterministic producer/consumer clock."""

from pathlib import Path
import subprocess

import pytest

from test_refer_dialog_runtime import block

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "esphome/components/voip_stack"


def owner_source(*, baseline_burst=False):
    fsm = (COMPONENT / "voip_fsm.cpp").read_text()
    audio = (COMPONENT / "voip_audio.cpp").read_text()
    source = (ROOT / "tests/fixtures/application_media_preparation.cpp").read_text()
    for marker in ("void VoipStack::start_speaker_for_current_rx_(", "bool VoipStack::prepare_speaker_for_current_rx_(",
                   "void VoipStack::begin_application_media_start_(", "void VoipStack::pump_application_media_start_(",
                   "void VoipStack::finish_application_media_start_(", "void VoipStack::set_in_call_("):
        source += "\n" + block(fsm, marker)
    for marker in ("VoipStack::RxPlayoutPhase VoipStack::rx_playout_phase_(", "bool VoipStack::prepare_rx_playout_(",
                   "bool VoipStack::activate_rx_playout_(", "void VoipStack::speaker_output_progress_(",
                   "bool VoipStack::play_rx_preroll_(", "void VoipStack::reset_rx_audio_(",
                   "void VoipStack::play_silence_frame_(", "void VoipStack::enqueue_rx_frame_("):
        source += "\n" + block(audio, marker)
    if baseline_burst:
        old_audio = subprocess.check_output(["git", "show", "bc4ec509:esphome/components/voip_stack/voip_audio.cpp"],
                                            cwd=ROOT, text=True)
        old_fsm = subprocess.check_output(["git", "show", "bc4ec509:esphome/components/voip_stack/voip_fsm.cpp"],
                                          cwd=ROOT, text=True)
    else:
        old_audio, old_fsm = audio, fsm
    for marker in ("void VoipStack::play_rx_frame_(", "void VoipStack::rx_task_("):
        source += "\n" + block(old_audio, marker)
    source += "\n" + block(old_fsm, "void VoipStack::on_audio_received_(")
    return source + r'''
using Phase=VoipStack::RxPlayoutPhase;
using Action=VoipStack::ApplicationMediaStart;
uint32_t tag(unsigned revision,Phase phase){return (revision<<3U)|static_cast<unsigned>(phase);}
int main(int argc,char**argv){
 assert(argc==2);const std::string mode=argv[1];clock_ms=100;
 Speaker sink;sink.state=Speaker::RUNNING;Component provider;VoipStack phone;
 phone.speaker_=&sink;phone.speaker_feedback_source_=&provider;
 uint8_t storage[16*960]{},pcm[960]{},network[960]{};RtpJitterBuffer jitter(storage,960,16,4);
 phone.rx_jitter_buffer_=&jitter;phone.rx_audio_chunk_=pcm;phone.rx_audio_chunk_alloc_bytes_=960;
 phone.rx_network_chunk_=network;phone.reset_rx_audio_();auto revision=phone.rx_audio_revision_.load();
 if(mode=="burst"){
  phone.call_state_=CallState::CONNECTING;phone.rx_playout_state_=tag(revision,Phase::ACTIVE);yield_on_wait=true;
  for(unsigned n=0;n<44;++n){
   clock_ms=100+n*4/10; // Forty-four already-arrived packets dispatched in18ms.
   phone.on_audio_received_({pcm,960,uint16_t(n),n*480,true,false,pack_audio_format(phone.format)});
   try{phone.rx_task_();}catch(const Paused&){}
  }
  assert(!jitter.counters().drops&&"Established RX must drain while public UI is CONNECTING");
  assert(sink.accepted==44*960&&jitter.depth()==0);return 0;
 }
 phone.call_state_=CallState::CALLING;phone.calling_start_time_=clock_ms;
 if(mode=="legacy"){
  phone.speaker_feedback_source_=nullptr;phone.begin_application_media_start_(Action::ORIGINATE);
  assert(phone.invites==1&&phone.activate_rx_playout_());const auto accepted=sink.accepted;
  assert(!phone.play_rx_preroll_(revision,10)&&sink.accepted==accepted);return 0;
 }
 if(mode=="offer"){
  phone.call_state_=CallState::CONNECTING;phone.audio_devices_active_=false;
  phone.begin_application_media_start_(Action::OFFER);assert(phone.transport.answers==0);
  phone.play_rx_preroll_(revision,10);phone.speaker_output_progress_(480,110000);
  phone.pump_application_media_start_();assert(phone.transport.answers==1&&!phone.audio_devices_active_);
  assert(phone.rx_playout_phase_(revision)==Phase::READY&&phone.call_state_==CallState::CONNECTING);
  phone.audio_devices_active_=true;phone.begin_application_media_start_(Action::CONFIRM);
  assert(phone.committed==1&&phone.rx_playout_phase_(revision)==Phase::ACTIVE);return 0;
 }
 phone.begin_application_media_start_(Action::ORIGINATE);
 assert(phone.invites==0&&phone.rx_playout_phase_(revision)==Phase::PREPARING);
 phone.speaker_output_progress_(480,100001); // Another/old output before any current source write.
 assert(phone.rx_playout_phase_(revision)==Phase::PREPARING);
 phone.play_rx_preroll_(revision,10);assert(phone.rx_playout_phase_(revision)==Phase::WAITING_OUTPUT);
 phone.speaker_output_progress_(0,110000);phone.speaker_output_progress_(480,99999);
 assert(phone.rx_playout_phase_(revision)==Phase::WAITING_OUTPUT&&phone.invites==0);
 if(mode=="cancel"){
  phone.request_call_termination_({});phone.speaker_output_progress_(480,120000);
  assert(phone.rx_playout_phase_(phone.rx_audio_revision_)==Phase::STOPPED&&!phone.invites);return 0;
 }
 phone.speaker_output_progress_(480,110000);assert(phone.rx_playout_phase_(revision)==Phase::READY);
 if(mode=="error"){
  provider.error=true;phone.pump_application_media_start_();assert(!phone.invites);
  provider.error=false;
 }
 phone.pump_application_media_start_();assert(phone.invites==1);
 if(mode=="format"){
  const auto old_revision=revision;phone.format.sample_rate=16000;clock_ms+=10;sink.ready_at=clock_ms;
  phone.begin_application_media_start_(Action::CONFIRM);
  assert(phone.rx_audio_revision_!=old_revision&&phone.committed==0);
  sink.progress();phone.pump_application_media_start_();sink.progress();phone.pump_application_media_start_();
  revision=phone.rx_audio_revision_;
  const auto accepted=sink.accepted;phone.play_rx_frame_(pcm,320,SilenceReason::NONE,0,old_revision);
  assert(sink.accepted==accepted);
  phone.play_rx_preroll_(revision,10);phone.speaker_output_progress_(160,110000);
  assert(phone.rx_playout_phase_(revision)==Phase::WAITING_OUTPUT); // Old completion cannot resolve it.
  phone.speaker_output_progress_(160,clock_ms*1000);phone.pump_application_media_start_();
  assert(phone.committed==1&&phone.rx_playout_phase_(revision)==Phase::ACTIVE);return 0;
 }
 if(mode=="late-first"){
  assert(phone.activate_rx_playout_());phone.call_state_=CallState::IN_CALL;
  const auto before=sink.accepted;
  for(unsigned n=0;n<250;++n)phone.play_rx_preroll_(revision,10);
  assert(clock_ms==2610&&sink.accepted==before+250*960&&!phone.first_audio_received_);
  phone.first_audio_received_=true;assert(!phone.play_rx_preroll_(revision,10));return 0;
 }
 if(mode=="active-wake"){
  const auto before=clock_ms;const auto accepted=sink.accepted;
  on_wait=[&](){on_wait={};assert(phone.activate_rx_playout_());skip_wait=true;};
  phone.play_rx_preroll_(revision,10);
  assert(clock_ms==before&&sink.accepted==accepted&&phone.rx_playout_phase_(revision)==Phase::ACTIVE);return 0;
 }
 if(mode=="wrap"){
  phone.rx_audio_revision_=UINT32_MAX;phone.rx_playout_state_=tag(UINT32_MAX,Phase::PREPARING);
  phone.rx_prepare_started_us_=UINT32_MAX-5;
  phone.play_silence_frame_(SilenceReason::PREROLL,0,UINT32_MAX);
  phone.speaker_output_progress_(480,3);assert(phone.rx_playout_phase_(UINT32_MAX)==Phase::READY);
  phone.reset_rx_audio_();phone.speaker_output_progress_(480,4);
  assert(phone.rx_audio_revision_==0&&phone.rx_playout_phase_(0)==Phase::STOPPED);return 0;
 }
 assert(mode=="ready"||mode=="error");
 phone.begin_application_media_start_(Action::CONFIRM);
 assert(phone.rx_audio_revision_==revision&&phone.rx_playout_phase_(revision)==Phase::ACTIVE);
 assert(phone.committed==1&&phone.call_state_==CallState::IN_CALL);
}
'''


def build_owner(folder, *, baseline_burst=False):
    folder.mkdir(parents=True, exist_ok=True)
    helper = folder / "esphome/core/helpers.h"
    helper.parent.mkdir(parents=True, exist_ok=True)
    helper.write_text("namespace esphome {struct Mutex {};struct LockGuard {explicit LockGuard(Mutex&){}};}\n")
    source = folder / "owner.cpp"
    source.write_text(owner_source(baseline_burst=baseline_burst))
    binary = folder / "owner"
    built = subprocess.run(["g++", "-std=c++17", "-DUSE_ESP32", "-DUSE_ESPHOME_VOIP_STACK_SPEAKER",
                            "-I", str(folder), "-I", str(ROOT), str(source), "-o", str(binary)],
                           capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    return binary


@pytest.fixture(scope="module")
def owner_binary(tmp_path_factory):
    return build_owner(tmp_path_factory.mktemp("rx-owner"))


@pytest.mark.parametrize("mode", ["ready", "cancel", "error", "late-first", "active-wake", "wrap", "burst",
                                  "legacy", "offer", "format"])
def test_real_rx_owner_preparation_and_established_clock(owner_binary, mode):
    run = subprocess.run(["bash", "-c", 'ulimit -c 0; exec "$1" "$2"', "bash", str(owner_binary), mode],
                         capture_output=True, text=True, timeout=5)
    assert run.returncode == 0, run.stderr
