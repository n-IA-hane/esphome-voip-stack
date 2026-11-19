"""Registered native calls preserve SIP URI authority and explicit line ownership."""

import subprocess

from test_refer_dialog_runtime import ROOT, SOURCE, block


def test_uri_targets_and_line_calls_preserve_authority_and_parameters(tmp_path):
    defines = tmp_path / "esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    parser = (SOURCE / "sip_message.cpp").read_text()
    code = r'''
#include "esphome/components/voip_stack/phonebook.h"
#include "esphome/components/voip_stack/sip_uri_user.h"
#include "esphome/components/voip_stack/sip_record.h"
#include "esphome/components/voip_stack/sip_types.h"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <memory>
using namespace esphome::voip_stack;
'''
    for marker in ["std::string trim_copy(", "std::string sip_uri_user_decode(",
                   "std::string sip_prefix_target_user(", "std::string sip_target_uri(",
                   "std::string sip_normalize_dial_target("]:
        code += block(parser, marker) + "\n"
    for marker in ("std::string name_addr_uri(", "std::string strip_angle_uri(", "std::string sip_quoted(",
                   "std::string sip_name_addr(", "std::string sip_display_name_from_header(", "std::string sip_user_from_header("):
        code += block(parser, marker) + "\n"
    code += r'''
struct SipTransport{void clear_account_line(){}};
struct VoipStack {
 std::unique_ptr<SipTransport>transport_=std::make_unique<SipTransport>();
 Phonebook phonebook_;std::string dialed,line,correlation;
 std::string pending_line_,pending_dialplan_target_;bool force_direct_call_=false;
 bool can_start_call_(){return true;}
 bool is_registered(const std::string&account){return account=="ha";}
 void call_(const std::string&target,const std::string&id){dialed=target;line=force_direct_call_?"direct":"wrong";correlation=id;}
 void start_with_id_(const std::string&id){dialed=pending_dialplan_target_;line=pending_line_;correlation=id;}
 void start_call_on_line(const std::string&,const std::string&);
 void call_via_ha(const std::string&,const std::string&);
 void start_call_on_line_(const std::string&,const std::string&,const std::string&);
 struct Failure{void trigger(const std::string&,const std::string&){}}call_failed_trigger_;
 template<class F>void defer(F f){f();}
};
'''
    registration = (SOURCE / "voip_registration.cpp").read_text()
    code += block(registration, "void VoipStack::start_call_on_line(") + "\n"
    code += block(registration, "void VoipStack::start_call_on_line_(") + "\n"
    code += block((SOURCE / "voip_settings.cpp").read_text(), "void VoipStack::call_via_ha(") + r'''
int main(){
 const std::string angle_label=R"(Desk <A> "Sales")";
 const std::string angle_uri="sip:"+sip_uri_user_encode(angle_label)+"@pbx.example";
 const auto address=sip_name_addr(angle_uri,angle_label);
 assert(name_addr_uri(address)==angle_uri);
 assert(sip_display_name_from_header(address,64)==angle_label);
 assert(sip_user_from_header(address)==angle_label);
 assert(name_addr_uri("<sip:a@pbx><sip:b@pbx>").empty());

 assert(sip_uri_user_decode("Caff%C3%A8%2020%25%40Casa")=="Caffè 20%@Casa");
 assert(sip_uri_user_decode("Caff%0d%0aName").empty()&&sip_uri_user_decode("bad%2").empty());
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
 VoipStack phone;
 phone.start_call_on_line("101","direct");assert(phone.dialed=="101"&&phone.line=="direct");
 phone.start_call_on_line("101","ha");assert(phone.dialed=="101"&&phone.line=="ha");
 phone.start_call_on_line("SIP:Home%20Assistant@pbx.example.test:5070;transport=TCP","ha");
 assert(phone.dialed=="sip:Home%20Assistant@pbx.example.test:5070;transport=TCP"&&phone.line=="ha");
 for(const std::string name:{"Desk|Other", "Desk,Other", "Bad;Label","Caffè 20%@Casa"}){
  phone.start_call_on_line(name,"ha");assert(phone.dialed==name);
 }
 for(const std::string bad:{"sips:101@host", "SIPS:101@host", "http://host/101", "sip:101@host;transport=tls",
     "sip:101@host;transport=udp;transport=tcp", "sip:101@host:0", "sip:101@host:65536", "sip:101@host:bad",
     "sip:101@.bad", "sip:101@host;=bad", "sip:101@[::1]", "sip:101@host?Subject=bad", "sip:bad%00@host",
     "sip:101@host\r\nHeader", "\r\n101", "101\r\n"}) {
  phone.dialed="unchanged";phone.start_call_on_line(bad,"ha");assert(phone.dialed=="unchanged");
 }
 phone.dialed="unchanged";phone.start_call_on_line(std::string("sip:bad\0@host",13),"ha");assert(phone.dialed=="unchanged");
 phone.start_call_on_line("101","unknown");assert(phone.dialed=="unchanged");
 assert(phone.pending_line_.empty());
 phone.call_via_ha("101","case-sensitive-ID@ha");
 assert(phone.line=="direct" && phone.correlation=="case-sensitive-ID@ha");
 phone.dialed="unchanged";phone.call_via_ha("101","invalid\r\nID");assert(phone.dialed=="unchanged");
}
'''
    source = tmp_path / "target.cpp"
    source.write_text(code)
    binary = tmp_path / "target"
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_ESP32", "-DUSE_ESPHOME_VOIP_REGISTRATION",
        "-I", str(tmp_path), "-I", str(ROOT), str(source), "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
