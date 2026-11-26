"""Actual selection/snapshot methods with normal and PSRAM directory storage."""
from pathlib import Path
import subprocess

import pytest

from test_correlated_call_behavior import correlated_fixture_source, write_consumer_includes
from test_refer_dialog_runtime import block

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / "esphome/components/voip_stack"


@pytest.fixture(scope="module", params=[False, True], ids=["internal", "signaling-psram"])
def initial_contact_binary(tmp_path_factory, request):
    folder = tmp_path_factory.mktemp("initial-contact")
    write_consumer_includes(folder)
    (folder / "esphome/core/defines.h").write_text("")
    settings = (CORE / "voip_settings.cpp").read_text()
    header = (CORE / "voip_stack.h").read_text()
    source = (ROOT / "tests/fixtures/initial_contact.cpp").read_text()
    markers = ["bool VoipStack::maybe_select_initial_contact_(", "bool VoipStack::resolve_selected_dial_target_(",
               "bool VoipStack::set_contact(", "void VoipStack::next_contact(", "void VoipStack::prev_contact(",
               "void VoipStack::publish_phonebook_changed_(", "std::string_view VoipStack::get_contact_account_view(",
               "std::string VoipStack::get_current_destination(", "std::string VoipStack::get_current_destination_route(",
               "std::string VoipStack::get_current_contact_ip(", "uint16_t VoipStack::get_current_contact_port(",
               "uint16_t VoipStack::get_current_contact_rtp_port(", "bool VoipStack::get_current_contact_sip_transport_tcp("]
    methods = "\n".join(block(settings, marker) for marker in markers)
    methods += "\n" + block((CORE / "voip_registration.cpp").read_text(), "bool VoipStack::select_line(")
    source = source.replace("CONTACT_SELECTION_CHANGED", block(header, "void contact_selection_changed_("))
    source = source.replace("METHODS", methods)
    path = folder / "initial.cpp"
    path.write_text(source)
    binary = folder / "initial"
    flags = ["-DUSE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM"] if request.param else []
    built = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_ESP32", "-DUSE_ESPHOME_VOIP_DIRECTORY",
        "-DUSE_ESPHOME_VOIP_CONTACT_METADATA", "-DUSE_ESPHOME_VOIP_REGISTRATION", *flags,
        "-I", str(folder), "-I", str(ROOT), "-I", str(CORE), str(path), "-o", str(binary)],
        capture_output=True, text=True, timeout=45)
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize("scenario", ["alphabetical", "explicit-no-directory", "account-scoped", "late-directory",
                                   "static-self", "only-self", "explicit-self", "same-user-other-host",
                                   "bare-self", "username-tie"])
def test_initial_target_order_refresh_and_manual_selection(initial_contact_binary, scenario):
    result = subprocess.run([initial_contact_binary, scenario], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr


def test_full_sip_uri_without_directory_uses_existing_originate_owner(tmp_path):
    write_consumer_includes(tmp_path)
    source = tmp_path / "direct.cpp"
    source.write_text(correlated_fixture_source() + r'''
int main(){
 VoipStack phone;phone.phonebook_.clear();
 const std::string uri="sip:door@192.0.2.30:5070;transport=tcp";
 phone.call(uri);
 assert(phone.transport.originations==1&&phone.transport.ip=="192.0.2.30");
 assert(phone.identity.dest_route==uri&&phone.identity.caller_route=="phone");
 assert(!phone.identity.call_id.empty()&&phone.invites==1);
 assert(phone.pending_dialplan_target_.empty());
}
''')
    binary = tmp_path / "direct"
    built = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-DUSE_ESP32",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-I", str(tmp_path), "-I", str(ROOT),
        "-I", str(CORE), str(source), "-o", str(binary)], capture_output=True, text=True, timeout=45)
    assert built.returncode == 0, built.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr
