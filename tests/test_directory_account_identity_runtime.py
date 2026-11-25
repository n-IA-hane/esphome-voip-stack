"""Directory self-filter follows SIP AoR, independently of hardware node names."""
from pathlib import Path
import os
import subprocess

import pytest

from test_refer_dialog_runtime import block

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def directory_identity_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("directory-identity")
    (folder / "esphome/core").mkdir(parents=True)
    (folder / "esphome/core/defines.h").write_text("")
    production = Path(os.environ.get("VOIP_TEST_PRODUCTION_ROOT", ROOT))
    core = production / "esphome/components/voip_stack"
    code = r'''
#include "phonebook.h"
#include "sip_account_view.h"
#include <cassert>
namespace esphome::voip_stack {
struct VoipStack {
 Phonebook phonebook_;std::string device_route_id_;SipAccountConfig account;bool account_known=true;
 struct DirectoryProvider {const void*owner=nullptr;std::string account_id;};std::array<DirectoryProvider,4>directory_providers_{};
 bool get_account_snapshot(const std::string&,SipAccountSnapshot*out){out->config=account;return account_known;}
 void publish_phonebook_changed_(){}
 DirectoryCommitResult commit_contact_directory(const void*,ContactEntry*,size_t,const std::string&,DirectoryCommitInfo* =nullptr);
};
METHOD
}
using namespace esphome::voip_stack;
int main(int argc,char**argv){
 assert(argc==3);VoipStack phone;phone.device_route_id_=argv[1];
 const std::string mode=argv[2];const bool external=mode=="external-ldap";
 phone.account_known=mode!="unknown-account";
 phone.account.id="office";phone.account.username="100";phone.account.domain="pbx.example";phone.account.registrar="pbx.example";
 ContactEntry entries[5];int index=0;
 for(const auto&route:{"sip:100@pbx.example","sip:phone101@pbx.example","sip:102@pbx.example","sip:100@other.example","sip:100@pbx.example:5070"}){
  auto&entry=entries[index];entry.id=index==1?"phone101":"entry-"+std::to_string(index);
  entry.name="Desk";entry.route=route;entry.account_id=external?"":"office";++index;
 }
 int provider;
 assert(phone.commit_contact_directory(&provider,entries,5,external?"":"office")==DirectoryCommitResult::APPLIED);
 const bool known=phone.account_known&&!external;
 assert(phone.phonebook_.size()==(known?4:5));
 assert((phone.phonebook_.find_id("entry-0")==nullptr)==known);
 assert(phone.phonebook_.find_id("phone101")&&phone.phonebook_.find_id("entry-2"));
 assert(phone.phonebook_.find_id("entry-3")&&phone.phonebook_.find_id("entry-4"));
 assert(!phone.phonebook_.find("Desk")); // Duplicate labels are valid; ambiguous lookup stays rejected.
}
'''
    code = code.replace("METHOD", block((core / "voip_settings.cpp").read_text(),
        "DirectoryCommitResult VoipStack::commit_contact_directory(const void *provider, ContactEntry *entries"))
    source = folder / "identity.cpp"
    source.write_text(code)
    binary = folder / "identity"
    built = subprocess.run(["g++", "-std=c++17", "-O1", "-g", "-ffunction-sections", "-fdata-sections",
        "-Wl,--gc-sections", "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_ESP32",
        "-DUSE_ESPHOME_VOIP_DIRECTORY", "-DUSE_ESPHOME_VOIP_REGISTRATION", "-DUSE_ESPHOME_VOIP_CONTACT_METADATA",
        "-I", str(folder), "-I", str(core), "-I", str(production / "tests/fixtures"), str(source), str(core / "sip_registration.cpp"),
        "-o", str(binary)], capture_output=True, text=True, timeout=45)
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize("node", ["phone101", "different-node"])
@pytest.mark.parametrize("mode", ["known-account", "external-ldap", "unknown-account"])
def test_account_identity_keeps_unrelated_matching_node_name(directory_identity_binary, node, mode):
    result = subprocess.run([directory_identity_binary, node, mode], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr
