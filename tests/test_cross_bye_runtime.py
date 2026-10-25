"""Compose real SIP socket I/O and deferred application teardown.

Only hardware/media adapters, the initial confirmed call and the host scheduler
are fixtures. The scheduler deliberately leaves the real deferred BYE callback
pending while a local stop arrives; no production delay or protocol outcome is
injected. The desired assertions must fail while a retired Call-ID can be
assigned back into SipTransport.
"""

import subprocess

import pytest

from test_incoming_setup_runtime import incoming_source
from test_refer_dialog_runtime import ROOT, SOURCE, block
from test_refer_wire_runtime import wire_source


def cross_bye_source():
    transport = (SOURCE / "sip_transport.cpp").read_text()
    fsm = (SOURCE / "voip_fsm.cpp").read_text()
    owner = (SOURCE / "voip_stack.cpp").read_text()
    header = (SOURCE / "voip_stack.h").read_text()
    wire = wire_source()
    original_emit = "void emit_sip_signal_(SipSignal s){delivered.push_back(std::move(s));}"
    assert original_emit in wire
    wire = wire.replace(original_emit, """
 void *signal_context=nullptr;
 void (*signal_callback)(void *,SipSignal)=nullptr;
 void emit_sip_signal_(SipSignal s){
  delivered.push_back(s);
  if(signal_callback)signal_callback(signal_context,std::move(s));
 }
""", 1)
    for signature in ["bool SipTransport::send_bye(", "void SipTransport::disconnect("]:
        implementation = block(transport, signature)
        declaration = implementation[:implementation.index("{")].replace("SipTransport::", "").strip() + ";"
        wire = wire.replace("struct SipTransport: SipPhoneTransport {",
                            "struct SipTransport: SipPhoneTransport {\n" + declaration + "\n", 1)
        wire += "\n" + implementation

    app = incoming_source()
    app = app.replace("uint32_t millis(){return 100;}", "")
    for level in ["I", "W", "D"]:
        app = app.replace(f"#define ESP_LOG{level}(...)\n", "")
    # Reuse the existing publication/media fixture, forwarding every terminal
    # decision to the real transport rather than accepting a mocked BYE.
    app = app.replace("struct Transport {", "struct Transport {\n SipTransport *sip=nullptr;", 1)
    app = app.replace("Snapshot snapshot()const{return {};}",
                      "Snapshot snapshot()const{return {sip->terminal_transaction_pending_locked_(),0};}")
    app = app.replace("void disconnect(){++disconnects;}",
                      "void disconnect(){++disconnects;sip->disconnect();}")
    app = app.replace("bool is_connected()const{return true;}",
                      "bool is_connected()const{return sip->running_.load();}")
    app = app.replace("bool send_final_response(const std::string&,uint16_t status,const std::string&);",
                      "bool send_bye(const std::string&id){return sip->send_bye(id);}\n"
                      " bool send_final_response(const std::string&,uint16_t status,const std::string&);")
    app = app.replace("struct VoipStack {", "struct VoipStack {\n" + block(header, "bool is_active() const {") + r"""
 std::atomic<bool>audio_devices_active_{true};
 void stop();
 void hangup_call_if_id(const std::string&);
 static void transport_sip_signal_callback_(void*,SipSignal);
 void enable_loop_soon_any_context(){}
""", 1)
    app = app.replace("bool send_sip_bye_(const std::string&){return true;}",
                      "bool send_sip_bye_(const std::string&);")
    app = app.replace("bool ignore_if_idle_or_stale_(const char*,const std::string&)const{return false;}",
                      "bool ignore_if_idle_or_stale_(const char*,const std::string&)const;")
    for signature in ["void VoipStack::stop(", "bool VoipStack::send_sip_bye_(",
                      "bool VoipStack::ignore_if_idle_or_stale_("]:
        app += "\n" + block(fsm, signature)
    app += "\n" + block(owner, "void VoipStack::transport_sip_signal_callback_(")
    app += "\n" + block((SOURCE / "voip_settings.cpp").read_text(), "void VoipStack::hangup_call_if_id(")

    # Reuse the socket peers and SIP builders, not the REFER-specific scenarios.
    peers = (ROOT / "tests/fixtures/refer_wire_main.cpp").read_text()
    peers = peers[:peers.index("std::string begin(")]
    fixture = (ROOT / "tests/fixtures/cross_bye.cpp").read_text()
    return wire + "\n" + app + "\n" + peers + "\n" + fixture


@pytest.fixture(scope="module")
def cross_bye_binary(tmp_path_factory):
    directory = tmp_path_factory.mktemp("cross-bye-wire")
    source = directory / "cross_bye.cpp"
    source.write_text(cross_bye_source())
    binary = directory / "cross_bye"
    result = subprocess.run(
        ["g++", "-std=c++17", "-Wall", "-Wextra", "-I", str(ROOT), str(source), "-o", str(binary)],
        capture_output=True, text=True,
    )
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("tcp", [0, 1], ids=["udp", "tcp"])
def test_remote_bye_then_local_stop_never_resurrects_dialog(cross_bye_binary, tcp):
    result = subprocess.run([str(cross_bye_binary), "deferred-stop", str(tcp)],
                            capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.mark.parametrize("operation,old_tcp,new_tcp", [
    (operation, old_tcp, new_tcp)
    for operation in ["stale-transport", "stale-api", "late-bye"]
    for old_tcp, new_tcp in [(0, 0), (0, 1), (1, 0), (1, 1)]
])
def test_old_termination_cannot_retarget_redial(cross_bye_binary, old_tcp, new_tcp, operation):
    result = subprocess.run([str(cross_bye_binary), operation, str(old_tcp), str(new_tcp)],
                            capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.mark.parametrize("tcp", [0, 1], ids=["udp", "tcp"])
@pytest.mark.parametrize("identity", ["current", "implicit", "logical"])
def test_current_dialog_bye_keeps_supported_identity_forms(cross_bye_binary, tcp, identity):
    result = subprocess.run([str(cross_bye_binary), "valid-stop", str(tcp), identity],
                            capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr
