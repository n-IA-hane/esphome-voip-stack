"""One bounded Phonebook retains source ownership and manual overrides."""

from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("blf", [False, True], ids=["without-blf", "with-blf"])
def test_directory_commit_preserves_manual_and_other_providers_atomically(tmp_path, blf):
    source = tmp_path / "directory.cpp"
    source.write_text(r'''
#include "esphome/components/voip_stack/phonebook.h"
#include <cassert>
using namespace esphome::voip_stack;
ContactEntry contact(const std::string &id,const std::string &route,const std::string &name) {
 ContactEntry e;e.id=id;e.route=route;e.name=name;return e;
}
int main() {
 Phonebook book;int provider_a,provider_b;
 assert(book.add_entry(contact("manual","101","Manual"))==AddResult::Added);
 auto remote=contact("ldap-remote","102","Remote");remote.kind=ContactKind::ESP;
 auto manual_override=contact("ldap-manual","101","Automatic override");
 assert(book.replace_directory(&provider_a,{manual_override,remote})==DirectoryCommitResult::APPLIED);
 assert(book.size()==2&&book.find("Manual")&&book.find_id("ldap-remote"));
 assert(book.select("ldap-remote"));
 remote.route="103";remote.name="Renamed";
 assert(book.replace_directory(&provider_a,{remote})==DirectoryCommitResult::APPLIED);
 assert(book.current_name()=="Renamed"&&book.current_route()=="103");
 assert(book.current()->origin==ContactOrigin::LDAP&&book.current()->provider==&provider_a);
 assert(book.replace_directory(&provider_a,{remote})==DirectoryCommitResult::UNCHANGED);
#ifdef USE_ESPHOME_VOIP_BLF
 assert(remote.auto_blf);
 remote.auto_blf=false;
 assert(book.replace_directory(&provider_a,{remote})==DirectoryCommitResult::APPLIED);
 assert(!book.current()->auto_blf);
 assert(book.replace_directory(&provider_a,{remote})==DirectoryCommitResult::UNCHANGED);
 remote.auto_blf=true;
 assert(book.replace_directory(&provider_a,{remote})==DirectoryCommitResult::APPLIED);
 assert(book.current()->auto_blf);
 Phonebook merged;auto manual=contact("one","201","Manual copy");manual.ip="127.0.0.1";manual.port=5060;manual.endpoint_type=ContactEndpointType::SIP;
 assert(merged.add_entry(manual)==AddResult::Added);
 manual.auto_blf=false;
 assert(merged.add_entry(manual)==AddResult::EndpointReplaced);
 assert(!merged.current()->auto_blf);
 assert(merged.add_entry(manual)==AddResult::Noop);
#endif
 assert(book.replace_directory(&provider_b,{remote})==DirectoryCommitResult::CONFLICT);
 assert(book.current()->provider==&provider_a&&book.size()==2);
 auto other=contact("other","104","Other");
 assert(book.replace_directory(&provider_b,{other})==DirectoryCommitResult::APPLIED);
 assert(book.replace_directory(&provider_a,{remote,remote})==DirectoryCommitResult::INVALID);
 assert(book.size()==3&&book.find_id("ldap-remote"));
 std::vector<ContactEntry> oversized;
 for(unsigned i=0;i<63;++i)oversized.push_back(contact("bulk"+std::to_string(i),"route"+std::to_string(i),"Name"+std::to_string(i)));
 assert(book.replace_directory(&provider_a,std::move(oversized))==DirectoryCommitResult::CAPACITY);
 assert(book.size()==3&&book.find_id("ldap-remote"));
 assert(book.replace_directory(&provider_a,{})==DirectoryCommitResult::APPLIED);
 assert(book.size()==2&&book.find("Manual")&&book.find_id("other")&&!book.find_id("ldap-remote"));
 assert(book.at(book.size())==nullptr);
 // The legacy push roster cycle owns only its records. An independently
 // authoritative LDAP snapshot survives an empty or incomplete push cycle.
 assert(book.commit_cycle({},1));
 assert(book.size()==1&&book.find_id("other")&&!book.find_id("manual"));
 assert(book.find_id("other")->missing_count==0);
}
''')
    binary = tmp_path / "directory"
    built = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_ESP32", "-DUSE_ESPHOME_VOIP_DIRECTORY",
        *(["-DUSE_ESPHOME_VOIP_BLF"] if blf else []),
        "-I", str(ROOT), str(source), "-o", str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr


def test_directory_switch_retires_legacy_ha_and_self_aor_only(tmp_path):
    from test_refer_dialog_runtime import SOURCE, block
    settings = (SOURCE / "voip_settings.cpp").read_text()
    message = (SOURCE / "sip_message.cpp").read_text()
    stub = tmp_path / "esphome/core/defines.h"
    stub.parent.mkdir(parents=True)
    stub.write_text("")
    code = r'''
#include "esphome/components/voip_stack/phonebook.h"
#include "esphome/components/voip_stack/sip_account_view.h"
#include <array>
#include <cctype>
#include <cassert>
#include <algorithm>
using namespace esphome::voip_stack;
constexpr size_t VOIP_STACK_MAX_ROUTE_ID_LEN=64;
'''
    for signature in ["std::string trim_copy(", "static uint8_t hex_value(", "static bool is_sip_uri_unreserved(",
                      "std::string sip_uri_user_encode(", "std::string sip_prefix_target_user(", "std::string sip_target_uri("]:
        code += block(message, signature) + "\n"
    code += r'''
struct VoipStack {
 struct DirectoryProvider {const void *owner=nullptr;std::string account_id;};
 std::array<DirectoryProvider,4> directory_providers_{};
 Phonebook phonebook_;std::string device_route_id_="local";bool ha_roster_enabled_=true;
 unsigned updates=0;
 void publish_phonebook_changed_(){++updates;}
 bool get_account_snapshot(const std::string&id,SipAccountSnapshot*out) {
  if(id!="ha")return false;out->config.username="local";out->config.domain="pbx.example.test";out->config.port=5060;return true;
 }
 void set_ha_roster_enabled(bool);
 DirectoryCommitResult commit_contact_directory(const void*,std::vector<ContactEntry>,const std::string&);
 DirectoryCommitResult commit_contact_directory(const void*,ContactEntry*,size_t,const std::string&);
};
ContactEntry contact(const char*id,const char*route){ContactEntry c;c.id=id;c.name=id;c.route=route;return c;}
'''
    code += block(settings, "void VoipStack::set_ha_roster_enabled(") + "\n"
    code += block(settings, "DirectoryCommitResult VoipStack::commit_contact_directory(") + "\n"
    code += block(settings, "DirectoryCommitResult VoipStack::commit_contact_directory(const void *provider, ContactEntry *") + r'''
int main(){
 VoipStack phone;int directory;
 assert(phone.phonebook_.add_entry(contact("manual","123"))==AddResult::Added);
 assert(phone.phonebook_.replace_provider(ContactOrigin::HA,&phone,{contact("old-ha","124")})==DirectoryCommitResult::APPLIED);
 phone.set_ha_roster_enabled(false);assert(!phone.ha_roster_enabled_&&phone.updates==1);
 assert(phone.phonebook_.size()==1&&phone.phonebook_.find_id("manual"));
 phone.set_ha_roster_enabled(false);assert(phone.updates==1);
 auto self=contact("uuid-local","sip:local@pbx.example.test:5060");
 auto other=contact("uuid-other","sip:local@other.example.test:5060");
 auto other_port=contact("uuid-port","sip:local@pbx.example.test:5070");
 assert(phone.commit_contact_directory(&directory,{self,other,other_port},"ha")==DirectoryCommitResult::APPLIED);
 assert(phone.phonebook_.size()==3&&!phone.phonebook_.find_id("uuid-local"));
 assert(phone.phonebook_.find_id("uuid-other")&&phone.phonebook_.find_id("uuid-port"));
 assert(phone.directory_providers_[0].owner==&directory&&phone.directory_providers_[0].account_id=="ha");
 assert(phone.commit_contact_directory(&directory,{},"ha")==DirectoryCommitResult::APPLIED);
 assert(phone.phonebook_.size()==1&&phone.phonebook_.find_id("manual"));
 assert(phone.directory_providers_[0].owner==nullptr&&phone.directory_providers_[0].account_id.empty());
}
'''
    source = tmp_path / "provider.cpp"
    source.write_text(code)
    binary = tmp_path / "provider"
    built = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation", "-g", "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_ESP32", "-DUSE_ESPHOME_VOIP_DIRECTORY", "-DUSE_ESPHOME_VOIP_REGISTRATION", "-I", str(tmp_path), "-I", str(ROOT), str(source), "-o", str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    run = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert run.returncode == 0, run.stderr
