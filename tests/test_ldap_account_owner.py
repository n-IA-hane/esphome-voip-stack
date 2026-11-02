"""Shared LDAP credentials follow the real registration revision and ownership."""
from pathlib import Path
import subprocess

from test_refer_dialog_runtime import block

ROOT = Path(__file__).resolve().parents[1]


def test_actual_account_snapshot_revision_offline_and_removal(tmp_path):
    defines = tmp_path / "esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    source = r'''
#include "esphome/components/voip_stack/sip_account_view.h"
#include "esphome/components/voip_stack/phonebook.h"
#include <atomic>
#include <cassert>
#include <mutex>
namespace esphome::voip_stack {
class SipTransport {
 public:
 std::mutex dialog_mutex_;SipRegistration registration_;
 using LockGuard=std::lock_guard<std::mutex>;
 bool get_account_snapshot(const std::string&,SipAccountSnapshot*);
};
SNAPSHOT_METHOD
class VoipStack {
 public:
 SipTransport transport;Phonebook phonebook;
 bool get_account_snapshot(const std::string&id,SipAccountSnapshot*out){return transport.get_account_snapshot(id,out);}
 DirectoryCommitResult commit_contact_directory(const void*p,std::vector<ContactEntry>rows,const std::string&){return phonebook.replace_directory(p,std::move(rows));}
};
}
namespace esphome::ldap_directory {
class LdapDirectory {
 public:
 struct Configuration {std::string username,password,sip_host;uint16_t sip_port=0;bool sip_tcp=false,credentials_ready=false;}config_;
 voip_stack::VoipStack*parent_;std::string account_id_="ha";
 uint32_t account_revision_=0,last_success_=1;bool account_loaded_=false,active_=true,ready_=true;
 size_t contact_count_=1;const char*status_="ready";
 std::atomic<uint32_t>generation_{1};unsigned refreshes=0;std::mutex mutex;
 explicit LdapDirectory(voip_stack::VoipStack&p):parent_(&p){}
 void lock_(){mutex.lock();}void unlock_(){mutex.unlock();}
 void interrupt_worker_(){++generation_;}void refresh(){++refreshes;}
 void refresh_if_stale(){}void account_changed_();
};
ACCOUNT_METHOD
}
int main(){
 using namespace esphome;voip_stack::VoipStack parent;ldap_directory::LdapDirectory directory(parent);
 auto&registration=parent.transport.registration_;
 voip_stack::SipAccountConfig account;account.id="ha";account.registrar="pbx.example.test";account.domain=account.registrar;
 account.username="phone";account.password="fixture-password";account.port=5062;account.tcp=true;
 assert(registration.set_account(account,1));directory.account_changed_();
 assert(directory.account_loaded_&&directory.config_.credentials_ready&&directory.config_.username=="phone");
 assert(directory.config_.password==account.password&&directory.config_.sip_host==account.registrar&&directory.config_.sip_port==5062&&directory.config_.sip_tcp);
 auto revision=directory.account_revision_;auto generation=directory.generation_.load();assert(directory.refreshes==1);
 voip_stack::SipAccountSnapshot snapshot;assert(parent.transport.get_account_snapshot("ha",&snapshot));
 // Consumers own a copy, not a pointer or reference into the reusable slot.
 snapshot.config.password="changed-copy";assert(registration.account(0).config.password==account.password);
 voip_stack::ContactEntry row;row.id="directory-id";row.name="Directory";row.route="101";
 assert(parent.phonebook.replace_directory(&directory,{row})==voip_stack::DirectoryCommitResult::APPLIED);
 assert(parent.phonebook.add_one("Manual")==voip_stack::AddResult::Added);
 // A temporary offline state is not permanent account removal.
 directory.account_changed_();assert(directory.generation_==generation&&directory.refreshes==1&&parent.phonebook.size()==2);
 account.password="next-fixture";assert(registration.set_account(account,2));directory.account_changed_();
 assert(directory.account_revision_!=revision&&directory.generation_>generation&&directory.refreshes==2&&directory.config_.password==account.password);
 assert(registration.remove_account("ha"));directory.account_changed_();
 assert(!directory.account_loaded_&&!directory.config_.credentials_ready&&!directory.active_&&directory.contact_count_==0&&directory.last_success_==0);
 assert(parent.phonebook.size()==1&&parent.phonebook.find("Manual"));
 assert(!parent.transport.get_account_snapshot("ha",&snapshot)&&!parent.transport.get_account_snapshot("ha",nullptr));
}
'''
    core = ROOT / "esphome/components/voip_stack"
    source = source.replace("SNAPSHOT_METHOD", block((core / "sip_registration_transport.cpp").read_text(), "bool SipTransport::get_account_snapshot("))
    source = source.replace("ACCOUNT_METHOD", block((ROOT / "esphome/components/ldap_directory/ldap_directory.cpp").read_text(), "void LdapDirectory::account_changed_()"))
    cpp = tmp_path / "account.cpp"
    cpp.write_text(source)
    binary = tmp_path / "account"
    built = subprocess.run(["g++", "-std=c++17", "-O1", "-g", "-pthread", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_ESP32", "-DUSE_ESPHOME_VOIP_DIRECTORY",
        "-DUSE_ESPHOME_VOIP_CONTACT_METADATA", "-DUSE_ESPHOME_VOIP_REGISTRATION", "-I", str(tmp_path),
        "-I", str(ROOT), str(cpp), str(core / "sip_registration.cpp"), "-lcrypto", "-o", str(binary)],
        capture_output=True, text=True, timeout=45)
    assert built.returncode == 0, built.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr
