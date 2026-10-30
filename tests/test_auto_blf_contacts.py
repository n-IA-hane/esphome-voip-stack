"""Real LDAP records become bounded SIP watches only when the provider permits it."""
from pathlib import Path
import subprocess

import pytest

from test_refer_dialog_runtime import block

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "esphome/components/voip_stack"


def ldap_record(kind):
    from ldap3.protocol.rfc4511 import LDAPMessage
    from pyasn1.codec.ber.encoder import encode

    message = LDAPMessage()
    message["messageID"] = 7
    entry = message["protocolOp"]["searchResEntry"]
    entry["object"] = "uid=target,ou=phonebook,dc=example"
    attributes = {"uid": "stable-contact-target", "cn": "Contact", "labeledURI": "sip:target@example.test"}
    if kind:
        attributes["ou"] = kind
    for index, (name, value) in enumerate(attributes.items()):
        entry["attributes"][index]["type"] = name
        entry["attributes"][index]["vals"][0] = value
    return encode(message).hex()


@pytest.fixture(scope="module")
def auto_blf_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("auto-blf")
    defines = folder / "esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    harness = (ROOT / "tests/test_sip_mwi_engine.cpp").read_text()
    adapter = (ROOT / "esphome/components/ldap_directory/ldap_directory.cpp").read_text()
    mapping = adapter[adapter.index("      auto &contact = this->stage_[this->stage_count_];"):
                      adapter.index("      ++this->stage_count_;", adapter.index("      auto &contact = this->stage_"))]
    code = r'''
#define USE_ESP32
#include "esphome/components/voip_stack/phonebook.h"
#undef USE_ESP32
#include "esphome/components/ldap_directory/directory_contact.h"
'''
    code += harness[:harness.index("int main(")]
    code += r'''
using namespace esphome::ldap_directory;
namespace voip_stack = esphome::voip_stack;
struct Adapter {
 ContactEntry stage_[1];size_t stage_count_=0;bool managed_by_ha_=false;
 ContactEntry map(const Entry &entry) {
  DirectoryContact mapped;assert(map_contact(entry,"example.test",5060,false,&mapped));
'''
    code += mapping + r'''
  return std::move(stage_[0]);
 }
};
struct SipTransport {
 std::vector<BlfWatchConfig> desired;
 bool set_blf_watches(const std::vector<BlfWatchConfig>&rows){desired=rows;return true;}
};
struct VoipStack {
 Phonebook phonebook_;bool blf_ready_=true,blf_watch_contacts_=true;uint8_t blf_capacity_=16;
 std::string blf_account_id_="desk",default_line_="desk";
 struct Provider{const void*owner=nullptr;std::string account_id;};std::array<Provider,1>directory_providers_{};
 std::unique_ptr<SipTransport>transport_=std::make_unique<SipTransport>();
 std::string get_contact_account(std::string_view)const;
 bool set_blf_watches(const std::vector<BlfWatchConfig>&);
 void sync_blf_contacts_();
};
'''
    code += block((SOURCE / "voip_settings.cpp").read_text(), "std::string VoipStack::get_contact_account(")
    owner = (SOURCE / "voip_blf.cpp").read_text()
    code += block(owner, "bool VoipStack::set_blf_watches(")
    code += block(owner, "void VoipStack::sync_blf_contacts_(")
    code += r'''
int main(int argc,char**argv){
 assert(argc==6);const bool managed=std::string(argv[1])=="1";
 const bool explicit_watch=std::string(argv[2])=="explicit";const bool expected=std::string(argv[3])=="1";
 const std::string hex=argv[4];std::string encoded;
 for(size_t i=0;i<hex.size();i+=2)encoded.push_back(static_cast<char>(std::stoul(hex.substr(i,2),nullptr,16)));
 Result decoded;assert(decode_message(encoded,&decoded)&&decoded.operation==0x64);
 Adapter adapter;adapter.managed_by_ha_=managed;auto entry=adapter.map(decoded.entry);
 VoipStack phone;int provider;assert(phone.phonebook_.replace_directory(&provider,{entry})==DirectoryCommitResult::APPLIED);
 assert(phone.phonebook_.size()==1&&phone.phonebook_.find_id(entry.stable_id())->route=="sip:target@example.test");
 if(explicit_watch)assert(phone.set_blf_watches({{std::string(entry.stable_id()),"desk",std::string(entry.route)}}));
 else phone.sync_blf_contacts_();
 assert(phone.transport_->desired.size()==size_t(expected));
 Harness wire(std::string(argv[5])=="tcp");SipRegistration registration;assert(registration.set_account(wire.config,0));
 auto &account=registration.account(0);account.bound=true;account.target_ip=INADDR_LOOPBACK;
 account.target_port=wire.first.port;account.contact="sip:alice@127.0.0.1:5060";
 assert(wire.client.configure_blf(16));
 for(size_t i=0;i<phone.transport_->desired.size();++i){const auto&w=phone.transport_->desired[i];
  assert(w.account_id=="desk");assert(wire.client.set_blf_watch(i,0,account,w.contact_id,w.target,0));}
 wire.client.tick(0);
 if(expected){
  std::string packet=wire.first.receive();
  while(packet.find("Event: presence")==std::string::npos)packet+=wire.first.receive();
  assert(packet.find("SUBSCRIBE sip:target@example.test SIP/2.0")!=std::string::npos);
  assert(packet.find("Event: dialog")!=std::string::npos&&packet.find("Event: presence")!=std::string::npos);
 }else{
  assert(wire.first.quiet());wire.client.tick(60000);wire.client.tick(3600000);assert(wire.first.quiet());
  uint32_t remaining=0;assert(!wire.client.next_deadline(3600000,&remaining));
 }
}
'''
    source = folder / "auto_blf.cpp"
    source.write_text(code)
    binary = folder / "auto_blf"
    command = ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g", "-fsanitize=address,undefined",
               "-fno-omit-frame-pointer", "-fno-pie", "-no-pie", "-DUSE_ESPHOME_VOIP_REGISTRATION",
               "-DUSE_ESPHOME_VOIP_BLF", "-DUSE_ESPHOME_VOIP_PRESENCE", "-DUSE_ESPHOME_VOIP_DIRECTORY",
               "-I", str(folder), "-I", str(ROOT), "-I", str(SOURCE), str(source),
               str(SOURCE / "sip_mwi.cpp"), str(SOURCE / "sip_blf.cpp"), str(SOURCE / "sip_registration.cpp"),
               str(ROOT / "esphome/components/ldap_directory/ldap_protocol.cpp"),
               str(ROOT / "esphome/components/ldap_directory/directory_contact.cpp"),
               str(ROOT / "esphome/components/phone_client/net_stream.cpp"), "-lcrypto", "-o", str(binary)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=90)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("transport", ["udp", "tcp"])
@pytest.mark.parametrize("kind,managed,expected", [
    ("contact", True, False), ("group", True, False), ("automation", True, False), ("SERVICE", True, False),
    ("esphome", True, True), ("sip_account", True, True), ("browser", True, True),
    ("", True, True), ("future-provider-kind", True, True),
    ("contact", False, True), ("group", False, True), ("service", False, True), ("", False, True),
])
def test_ldap_policy_controls_actual_subscription_requests(auto_blf_binary, kind, managed, expected, transport):
    result = subprocess.run([str(auto_blf_binary), str(int(managed)), "auto", str(int(expected)),
                             ldap_record(kind), transport], capture_output=True, text=True, timeout=8)
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("kind", ["contact", "group", "automation", "service"])
def test_explicit_watches_override_only_the_automatic_directory_policy(auto_blf_binary, kind):
    result = subprocess.run([str(auto_blf_binary), "1", "explicit", "1", ldap_record(kind), "udp"],
                            capture_output=True, text=True, timeout=8)
    assert result.returncode == 0, result.stderr
