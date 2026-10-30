"""SIP target helpers preserve LDAP URI authority and directed pickup user."""

import subprocess

from test_refer_dialog_runtime import ROOT, SOURCE, block


def test_uri_targets_and_pickup_preserve_authority_and_parameters(tmp_path):
    defines = tmp_path / "esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    parser = (SOURCE / "sip_message.cpp").read_text()
    owner = (SOURCE / "voip_blf.cpp").read_text()
    code = r'''
#include "esphome/components/voip_stack/phonebook.h"
#include "esphome/components/voip_stack/sip_blf.h"
#include "esphome/components/voip_stack/sip_types.h"
#include <algorithm>
#include <cassert>
#include <cctype>
using namespace esphome::voip_stack;
'''
    for marker in ["std::string trim_copy(", "static uint8_t hex_value(", "static bool is_sip_uri_unreserved(",
                   "std::string sip_uri_user_encode(", "std::string sip_prefix_target_user(", "std::string sip_target_uri("]:
        code += block(parser, marker) + "\n"
    code += r'''
struct VoipStack {
 Phonebook phonebook_;std::string blf_pickup_prefix_,dialed,line;
 std::string pending_line_,pending_dialplan_target_;bool force_direct_call_=false;
 bool can_start_call_(){return true;}
 BlfStateInfo get_contact_blf(const std::string&id){BlfStateInfo s;s.contact_id=id;s.account_id="ha";s.available=true;s.ringing_dialogs=1;return s;}
 bool is_registered(const std::string&account){return account=="ha";}
 void call_(const std::string&target,const std::string&){dialed=target;line=force_direct_call_?"direct":"wrong";}
 void start_with_id_(const std::string&){dialed=pending_dialplan_target_;line=pending_line_;}
 void start_call_on_line(const std::string&,const std::string&);
 bool pickup_contact(const std::string&);
};
'''
    registration = (SOURCE / "voip_registration.cpp").read_text()
    code += block(registration, "void VoipStack::start_call_on_line(") + "\n"
    code += block(owner, "bool VoipStack::pickup_contact(") + r'''
int main(){
 assert(sip_target_uri("sip:camera@pbx.example.test:5070;transport=tcp","ignored.example.test")=="sip:camera@pbx.example.test:5070;transport=tcp");
 assert(sip_target_uri("Front Door","pbx.example.test")=="sip:Front%20Door@pbx.example.test");
 assert(sip_target_uri("101","pbx.example.test")=="sip:101@pbx.example.test");
 assert(sip_prefix_target_user("sip:camera@pbx.example.test;transport=tcp","pickup.")=="sip:pickup.camera@pbx.example.test;transport=tcp");
 assert(sip_prefix_target_user("101","**")=="**101");
 const auto pbx=sip_prefix_target_user("sip:101@pbx.example.test;transport=udp","**");
 assert(pbx=="sip:%2A%2A101@pbx.example.test;transport=udp");
 assert(sip_target_uri(pbx,"ignored.example.test")==pbx);
 assert(sip_prefix_target_user("sip:bad%0D%0AHeader@host","pickup.").empty());
 assert(sip_prefix_target_user("sip:bad%xx@host","pickup.").empty());
 assert(sip_prefix_target_user("sip:camera@","pickup.").empty());
 VoipStack phone;ContactEntry camera;camera.id="stable-directory-id";camera.name="Camera";
 camera.route="sip:camera@pbx.example.test;transport=tcp";camera.kind=ContactKind::ESP;
 assert(phone.phonebook_.replace_directory(&phone,{camera})==DirectoryCommitResult::APPLIED);
 phone.blf_pickup_prefix_="pickup.";assert(phone.pickup_contact(camera.id));
 assert(phone.dialed=="sip:pickup.camera@pbx.example.test;transport=tcp"&&phone.line=="ha");
 phone.blf_pickup_prefix_="**";assert(phone.pickup_contact(camera.id));
 assert(phone.dialed=="sip:%2A%2Acamera@pbx.example.test;transport=tcp");
 phone.start_call_on_line("101","direct");assert(phone.dialed=="101"&&phone.line=="direct");
 phone.start_call_on_line("101","ha");assert(phone.dialed=="101"&&phone.line=="ha");
 phone.start_call_on_line("SIP:Home%20Assistant@pbx.example.test:5070;transport=TCP","ha");
 assert(phone.dialed=="sip:Home%20Assistant@pbx.example.test:5070;transport=TCP"&&phone.line=="ha");
 for(const std::string bad:{"sips:101@host", "SIPS:101@host", "http://host/101", "sip:101@host;transport=tls",
     "sip:101@host;transport=udp;transport=tcp", "sip:101@host:0", "sip:101@host:65536", "sip:101@host:bad",
     "sip:101@.bad", "sip:101@host;=bad", "sip:101@[::1]", "sip:101@host?Subject=bad", "sip:bad%00@host",
     "sip:101@host\r\nHeader", "Desk|Other", "Desk,Other", "Bad;Label"}) {
  phone.dialed="unchanged";phone.start_call_on_line(bad,"ha");assert(phone.dialed=="unchanged");
 }
 phone.dialed="unchanged";phone.start_call_on_line(std::string("sip:bad\0@host",13),"ha");assert(phone.dialed=="unchanged");
 phone.start_call_on_line("101","unknown");assert(phone.dialed=="unchanged");
 assert(phone.pending_line_.empty());
}
'''
    source = tmp_path / "target.cpp"
    source.write_text(code)
    binary = tmp_path / "target"
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_ESP32", "-DUSE_ESPHOME_VOIP_DIRECTORY", "-DUSE_ESPHOME_VOIP_BLF",
        "-I", str(tmp_path), "-I", str(ROOT), str(source), "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
