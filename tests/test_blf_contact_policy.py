"""Production LDAP phone identity, Phonebook and automatic BLF watch policy."""
import subprocess
from test_refer_dialog_runtime import ROOT, SOURCE, block


def test_ldap_phone_accounts_and_explicit_external_policy_drive_watches(tmp_path):
    core = tmp_path / "esphome/core"
    core.mkdir(parents=True)
    (core / "defines.h").write_text("")
    code = r'''
#include "phonebook.h"
#include "sip_blf.h"
#include "directory_contact.h"
#define ESP_LOGW(...) ((void)0)
#include <cassert>
#include <memory>
using namespace esphome::voip_stack;
using namespace esphome::ldap_directory;
struct SipTransport{std::vector<BlfWatchConfig>desired;bool set_blf_watches(const std::vector<BlfWatchConfig>&v){desired=v;return true;}};
struct VoipStack{Phonebook phonebook_;bool blf_ready_=true,blf_watch_contacts_=true;uint8_t blf_capacity_=16;
 std::string blf_account_id_="ha",default_line_="qa";std::unique_ptr<SipTransport>transport_=std::make_unique<SipTransport>();
 bool is_registered(const std::string&)const{return true;}
 std::string get_contact_account(std::string_view)const;bool set_blf_watches(const std::vector<BlfWatchConfig>&);void sync_blf_contacts_();};
'''
    settings = (SOURCE / "voip_settings.cpp").read_text()
    facade = (SOURCE / "voip_blf.cpp").read_text()
    code += block(settings, "std::string VoipStack::get_contact_account(") + "\n"
    for marker in ["bool VoipStack::set_blf_watches(", "void VoipStack::sync_blf_contacts_("]:
        code += block(facade, marker) + "\n"
    code += r'''
int main(){
 VoipStack phone;
 for (const std::string username : {"Browser primary", "Caffè 20%@Casa", "Desk, One"}) {
  Entry source{"uid=fixture,dc=voip", {{"uid", {username}}, {"displayName", {username}}, {"ou", {"sip_account"}}}};
  DirectoryContact mapped;assert(map_contact(source,"pbx.example.test",5060,false,&mapped,true));
  ContactEntry row;row.name=mapped.name;row.id=mapped.id;row.route=mapped.uri;
  row.kind=ContactKind::SIP_ACCOUNT;row.account_id="ha";row.auto_blf=true;
  assert(phone.phonebook_.add_entry(row)==AddResult::Added);
 }
 phone.sync_blf_contacts_();const auto&w=phone.transport_->desired;assert(w.size()==3);
 for(const auto&row:w){assert(row.target=="sip:"+sip_uri_user_encode(row.contact_id)+"@pbx.example.test"&&row.account_id=="ha");}
 ContactEntry group;group.id="Night group";group.name="Night group";group.route="sip:Night%20group@pbx.example.test";
 group.kind=ContactKind::GROUP;group.account_id="ha";group.auto_blf=false;
 assert(phone.phonebook_.add_entry(group)==AddResult::Added&&!contact_can_auto_blf(group));
 ContactEntry external;external.id="sip:103@office.test";external.name="External phone";external.route=external.id;
 external.kind=ContactKind::CONTACT;external.auto_blf=true;external.account_id="office";
 assert(phone.phonebook_.add_entry(external)==AddResult::Added);phone.sync_blf_contacts_();
 assert(phone.transport_->desired.size()==4&&phone.transport_->desired.back().account_id=="office");
 // The same literal username on another line retains that line's ownership.
 ContactEntry scoped=*phone.phonebook_.at(0);scoped.account_id="office";scoped.route="sip:Browser%20primary@office.test";
 assert(phone.phonebook_.add_entry(scoped)==AddResult::Added);phone.sync_blf_contacts_();
 assert(phone.transport_->desired.size()==5&&phone.transport_->desired.back().account_id=="office");
 assert(!phone.phonebook_.find_id("Browser primary"));
}
'''
    cpp = tmp_path / "policy.cpp"
    cpp.write_text(code)
    binary = tmp_path / "policy"
    ldap = ROOT / "esphome/components/ldap_directory"
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_ESP32", "-DUSE_ESPHOME_VOIP_BLF",
        "-DUSE_ESPHOME_VOIP_REGISTRATION", "-DUSE_ESPHOME_VOIP_CONTACT_METADATA", "-I", str(tmp_path),
        "-I", str(ROOT), "-I", str(SOURCE), "-I", str(ldap), str(cpp), str(ldap / "directory_contact.cpp"),
        str(ldap / "ldap_protocol.cpp"), str(ROOT / "esphome/components/phone_client/net_stream.cpp"),
        "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
