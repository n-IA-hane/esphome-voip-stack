"""Compile the real SIP signal handler with optional audio directions."""

from pathlib import Path
import subprocess

import pytest

from test_refer_dialog_runtime import block

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "esphome/components/voip_stack"


def incoming_source() -> str:
    source = (COMPONENT / "voip_fsm.cpp").read_text()
    adapter = (ROOT / "tests/fixtures/incoming_setup.cpp").read_text()
    methods = "\n".join(block(source, marker) for marker in (
        "void VoipStack::on_sip_signal_received_(",
        "void VoipStack::request_call_termination_(",
        "void VoipStack::end_call_(",
        "void VoipStack::finish_call_termination_(",
        "void VoipStack::clear_call_identity_(",
        "bool VoipStack::send_sip_final_response_(",
    ))
    return adapter + methods


@pytest.mark.parametrize("speaker", [False, True])
def test_incoming_and_refer_identity_compile_with_optional_speaker(tmp_path, speaker):
    source = tmp_path / "incoming.cpp"
    source.write_text(incoming_source() + r'''
int main() {
  VoipStack phone;
  SipSignal invite;
  invite.type=SipSignalType::INVITE; invite.call_id="incoming";
  invite.caller_name="caller"; invite.caller_route="100";
  invite.dest_name="esp"; invite.dest_route="esp";
  audio_format_list_default(&invite.caller_tx_formats);
  audio_format_list_default(&invite.caller_rx_formats);
  phone.on_sip_signal_received_(invite);
  assert(phone.call_state_==CallState::IN_CALL);
  SipSignal connected;
  connected.type=SipSignalType::CONNECTED_IDENTITY;
  connected.call_id="incoming"; connected.media_source_changed=true;
  connected.connected_name="transfer target";
  phone.on_sip_signal_received_(connected);
  assert(phone.watchdog_resets==1);
  assert(phone.rx_resets==
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
    1
#else
    0
#endif
  );
}
''')
    executable = tmp_path / "incoming"
    args = ["g++", "-std=c++17", "-I", str(ROOT)]
    if speaker:
        args.append("-DUSE_ESPHOME_VOIP_STACK_SPEAKER")
    result = subprocess.run([*args, str(source), "-o", str(executable)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    subprocess.run([str(executable)], check=True, capture_output=True, text=True)


@pytest.mark.parametrize("failure", ["prepare", "ringing", "final-send"])
def test_incoming_setup_failure_releases_identity_and_accepts_next_call(tmp_path, failure):
    source = tmp_path / "incoming_failure.cpp"
    source.write_text(incoming_source() + r'''
int main(int argc,char **argv) {
  VoipStack phone;
  const std::string failure=argv[1];
  phone.t.prepare_ok=false;
  if(failure=="ringing"){phone.auto_answer_=false;phone.t.ringing_ok=false;}
  if(failure=="final-send")phone.t.final_ok=false;
  SipSignal invite;
  invite.type=SipSignalType::INVITE; invite.call_id="failed-incoming";
  invite.caller_name="caller"; invite.caller_route="100";
  invite.dest_name="esp"; invite.dest_route="esp";
  audio_format_list_default(&invite.caller_tx_formats);
  audio_format_list_default(&invite.caller_rx_formats);
  phone.on_sip_signal_received_(invite);
  phone.drain();
  assert(last_final_status==500 && "local setup failure must send a final error");
  assert(phone.cached_id==invite.call_id && !phone.cached_reason.empty());
  assert(phone.identity.call_id.empty() && !phone.incoming_setup_pending_);
  assert(phone.call_state_==CallState::IDLE);
  assert(phone.t.disconnects==1 && phone.media_stops==1);
  phone.t.prepare_ok=true;phone.auto_answer_=true;
  invite.call_id="second-incoming";
  phone.on_sip_signal_received_(invite);
  phone.drain();
  assert(phone.call_state_==CallState::IN_CALL && phone.identity.call_id==invite.call_id);
}
''')
    executable = tmp_path / "incoming_failure"
    result = subprocess.run(["g++", "-std=c++17", "-DUSE_ESPHOME_VOIP_STACK_SPEAKER", "-I", str(ROOT),
                             str(source), "-o", str(executable)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([str(executable), failure], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
