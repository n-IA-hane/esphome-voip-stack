"""The actual HA fallback accepts either canonical contact storage policy."""
import subprocess

import pytest

from test_phonebook_storage_runtime import ROOT, compile_storage, storage_program
from test_refer_dialog_runtime import block


@pytest.mark.parametrize("external", [False, True], ids=["internal", "signaling-psram"])
def test_actual_ha_fallback_copies_contact_ip_at_the_owned_route_boundary(tmp_path, external):
    # Reuse the maintained allocator stub and actual ContactEntry header.
    compile_storage(tmp_path, external=external)
    implementation = (ROOT / "esphome/components/voip_stack/voip_fsm.cpp").read_text()
    function = block(implementation, "void VoipStack::start_call_attempt_(")
    fallback = block(function, "if (via_ha) {").replace(
        "const auto *ha = this->phonebook_.find(this->ha_peer_name_);", "const auto *ha = contact;")
    fixture = storage_program().split("ContactEntry row(", 1)[0]
    fixture += """
std::string route(const ContactEntry *contact) {
  std::string dial_ip = "stale"; uint16_t dial_port=999, dial_rtp_port=999;
  bool dial_sip_tcp=true, dial_directional_audio_v1=true, via_ha=true;
""" + fallback + """
  if (contact) { assert(dial_port==5062 && dial_rtp_port==40000 && !dial_sip_tcp && !dial_directional_audio_v1); }
  else { assert(dial_port==0 && dial_rtp_port==0 && !dial_sip_tcp && !dial_directional_audio_v1); }
  return dial_ip;
}
int main(){ContactEntry contact; contact.ip="192.0.2.7";contact.port=5062;contact.rtp_port=40000;
  assert(route(&contact)=="192.0.2.7");assert(route(nullptr).empty());
}
"""
    source = tmp_path / "route.cpp"
    source.write_text(fixture)
    binary = tmp_path / "route"
    command = ["g++", "-std=c++17", "-g", "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
               "-DUSE_ESP32", "-DESP_PLATFORM", "-DUSE_ESPHOME_VOIP_DIRECTORY"]
    if external:
        command.append("-DUSE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM")
    result = subprocess.run(command + ["-I", str(tmp_path), "-I", str(ROOT), str(source), "-o", str(binary)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.mark.parametrize("directory,external", [(False, False), (True, False), (True, True)])
def test_public_selection_getters_do_not_return_destroyed_strings(tmp_path, directory, external):
    compile_storage(tmp_path, external=external)
    header = (ROOT / "esphome/components/voip_stack/voip_stack.h").read_text()
    implementation = (ROOT / "esphome/components/voip_stack/voip_settings.cpp").read_text()
    declarations = header[header.index("  void prev_contact();") + len("  void prev_contact();"):
                          header.index("  uint16_t get_current_contact_port() const;")]
    first = implementation.index("VoipStack::get_current_destination() const")
    start = implementation.rfind("\n", 0, first) + 1
    if implementation[:start].rstrip().endswith("#ifdef USE_ESPHOME_VOIP_DIRECTORY"):
        start = implementation.rfind("#ifdef USE_ESPHOME_VOIP_DIRECTORY", 0, start)
    definitions = implementation[start:implementation.index("uint16_t VoipStack::get_current_contact_port() const")]
    fixture = storage_program().split("ContactEntry row(", 1)[0]
    fixture += "struct VoipStack { Phonebook phonebook_;\n" + declarations + "};\n" + definitions
    fixture += r'''
#include <type_traits>
int main(){VoipStack phone;ContactEntry contact;
 contact.name="A deliberately long selected name";contact.route="sip:selected@pbx.example.test";contact.ip="192.0.2.7";
 contact.port=5060;contact.endpoint_type=ContactEndpointType::SIP;
 assert(phone.phonebook_.add_entry(contact)==AddResult::Added);
#ifdef USE_ESPHOME_VOIP_DIRECTORY
 static_assert(std::is_same_v<decltype(phone.get_current_destination()),std::string>);
 static_assert(std::is_same_v<decltype(phone.get_current_destination_route()),std::string>);
 static_assert(std::is_same_v<decltype(phone.get_current_contact_ip()),std::string>);
 const auto name=phone.get_current_destination(),route=phone.get_current_destination_route(),ip=phone.get_current_contact_ip();
 phone.phonebook_.remove_one(contact.name);
 assert(name=="A deliberately long selected name"&&route=="sip:selected@pbx.example.test"&&ip=="192.0.2.7");
#else
 static_assert(std::is_same_v<decltype(phone.get_current_destination()),const std::string&>);
 assert(&phone.get_current_destination()==&phone.phonebook_.current()->name);
 assert(&phone.get_current_destination_route()==&phone.phonebook_.current()->route);
 assert(&phone.get_current_contact_ip()==&phone.phonebook_.current()->ip);
#endif
}
'''
    source, binary = tmp_path / "selection.cpp", tmp_path / "selection"
    source.write_text(fixture)
    flags = (["-DUSE_ESPHOME_VOIP_DIRECTORY"] if directory else [])
    if external:
        flags.append("-DUSE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM")
    result = subprocess.run(["g++", "-std=c++17", "-Werror=return-local-addr", "-g",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_ESP32", "-DESP_PLATFORM",
        *flags, "-I", str(tmp_path), "-I", str(ROOT), str(source), "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr
