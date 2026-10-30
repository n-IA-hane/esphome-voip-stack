"""Execute the LDAP stage owner across worker/main-loop handoff and cancellation."""
from pathlib import Path
import subprocess

import pytest

from test_refer_dialog_runtime import block

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "esphome/components/ldap_directory"


@pytest.fixture(scope="module")
def lifecycle_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("ldap-lifecycle")
    cpp = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include "esphome/components/ldap_directory/directory_contact.h"
#define USE_ESP32
#include "esphome/components/voip_stack/phonebook.h"
#undef USE_ESP32
#define ESP_LOGW(...) ((void)0)
using namespace esphome;
uint32_t millis() { return 1000; }
namespace owner_test {
using namespace esphome::ldap_directory;
bool locked=false;
unsigned connects=0,client_creations=0;
bool search_ok=true;
Entry entry(const std::string&name) {
 Entry out;out.attributes={{"uid",{"entry-"+name}},{"cn",{name}},
   {"labeledURI",{"sip:"+name+"@example.test"}}};return out;
}
Entry next_entry=entry("new");
struct Stream {
 bool connect(const std::string&,uint16_t,uint32_t,bool,const std::string&){++connects;return true;}
 void close(){}
};
// Only the network collaborator is controlled. The stage-owning methods below
// are extracted unchanged from the production component.
struct Client {
 explicit Client(Stream&){++client_creations;}
 void set_receive_buffer(char*,size_t){}
 bool bind(const std::string&,const std::string&,uint32_t){return true;}
 bool search_each(const Search&,const std::function<bool(Entry&&)>&consume,uint32_t){
  Entry copy=next_entry;return consume(std::move(copy))&&search_ok;
 }
 void unbind(uint32_t){}
 uint32_t result_code()const{return search_ok?0:4;}
};
struct Component {
 unsigned timers=0;
 bool timer_active=false;
 template<typename F>void set_timeout(const char*,uint32_t,F){++timers;timer_active=true;}
 void cancel_timeout(const char*){timer_active=false;}
 void status_clear_warning(){}
 void status_set_warning(){}
};
template<typename... Ts>struct Trigger {
 unsigned calls=0;std::function<void()> callback;
 void trigger(const Ts&...){++calls;if(callback)callback();}
};
struct Parent {
 std::vector<voip_stack::ContactEntry> rows;
 unsigned commits=0;
 std::function<void()> callback;
 voip_stack::DirectoryCommitResult commit_contact_directory(const void*,voip_stack::ContactEntry*data,size_t count,const std::string&){
  assert(!locked);++commits;rows.clear();
  for(size_t i=0;i<count;++i)rows.push_back(std::move(data[i]));
  auto action=callback;if(action)action();
  return voip_stack::DirectoryCommitResult::APPLIED;
 }
 voip_stack::DirectoryCommitResult commit_contact_directory(const void*p,std::vector<voip_stack::ContactEntry>rows,const std::string&a){
  return commit_contact_directory(p,rows.data(),rows.size(),a);
 }
};
class LdapDirectory:public Component {
 public:
 struct Configuration {
  std::string host="ldap.example.test",base_dn="dc=voip",bind_dn="{username}",filter="(cn=*)",username="phone",password="fixture",ca_pem,sip_host="example.test";
  uint16_t port=389,sip_port=5060;
  bool tls=false,sip_tcp=false,credentials_ready=true;
 } config_;
 std::atomic<uint32_t> generation_{1};
 uint32_t timeout_ms_=5000,refresh_ms_=300000,last_success_=0,result_generation_=1,result_code_=0;
 bool active_=false,enabled_=true,pending_result_=false,ready_=true,managed_by_ha_=true;
 uint8_t max_entries_=4;
 char receive_buffer_[8192]{};size_t receive_size_=8192;
 voip_stack::ContactEntry *stage_;
 size_t stage_count_=0,stage_capacity_=4,contact_count_=0;
 const char*status_="updating";
 std::string account_id_="ha";
 Parent *parent_;
 Stream stream_;
 Trigger<uint32_t> updated_,error_;
 unsigned wakes=0,main_notifications=0;
 explicit LdapDirectory(Parent&parent):parent_(&parent){
  stage_=static_cast<voip_stack::ContactEntry*>(::operator new[](stage_capacity_*sizeof(*stage_)));
  for(size_t i=0;i<stage_capacity_;++i)new(&stage_[i])voip_stack::ContactEntry();
 }
 ~LdapDirectory(){for(size_t i=0;i<stage_capacity_;++i)stage_[i].~ContactEntry();::operator delete[](stage_);}
 void lock_(){assert(!locked);locked=true;}
 void unlock_(){assert(locked);locked=false;}
 void disable_loop(){}
 bool is_failed()const{return false;}
 void wake_worker_(){++wakes;}
 void notify_main_(){++main_notifications;}
 void interrupt_worker_(){++generation_;wake_worker_();}
 void set_server(const std::string&,uint16_t,bool){}
 void set_directory(const std::string&,const std::string&,const std::string&){}
 void worker_run_();void loop();void reset_stage_();void refresh();
 void configure_directory(std::string,int32_t,std::string,std::string,std::string,bool,bool);
 void pending(const char*name){
  stage_[0].id=name;stage_[0].name=name;stage_[0].route=name;stage_count_=1;
  result_generation_=generation_.load();pending_result_=true;
 }
};
METHODS
}
int main(int argc,char**argv){
 using namespace owner_test;
 assert(argc==2);const std::string scenario=argv[1];Parent phone;LdapDirectory source(phone);
 voip_stack::ContactEntry old;old.id="old";old.name="old";old.route="old";phone.rows.push_back(old);
 if(scenario=="pending-refresh"){
  source.pending("first");source.refresh();assert(source.active_);
  source.worker_run_();assert(connects==0&&client_creations==0&&source.active_&&source.stage_count_==1);
  phone.callback=[&]{assert(source.pending_result_);source.refresh();source.worker_run_();assert(connects==0&&source.stage_count_==1);};
  source.loop();assert(phone.rows.size()==1&&phone.rows[0].name=="first");
  assert(source.stage_count_==0&&!source.pending_result_&&source.active_);
  phone.callback={};source.worker_run_();assert(connects==1&&source.pending_result_&&source.stage_count_==1);
  source.loop();assert(phone.rows[0].name=="new"&&source.stage_count_==0&&!source.active_);
 }else if(scenario=="stale-before"){
  source.pending("stale");++source.generation_;source.loop();
  assert(phone.rows[0].name=="old"&&phone.commits==0&&source.timers==0&&source.updated_.calls==0&&source.error_.calls==0);
  assert(!source.pending_result_&&source.stage_count_==0);
 }else if(scenario=="cancel-callback"){
  source.pending("stale");phone.callback=[&]{phone.callback={};source.configure_directory("",389,"","","ha",false,false);};
  source.loop();assert(phone.rows.empty()&&!source.enabled_&&std::string(source.status_)=="disabled");
  assert(source.timers==0&&source.updated_.calls==0&&source.error_.calls==0&&!source.pending_result_&&source.stage_count_==0);
 }else if(scenario=="search-error"){
  search_ok=false;source.active_=true;source.worker_run_();
  assert(source.pending_result_&&source.result_code_==4&&source.stage_count_==1);
  source.loop();assert(phone.rows[0].name=="old"&&phone.commits==0&&source.error_.calls==1&&source.updated_.calls==0);
  assert(std::string(source.status_)=="error"&&source.stage_count_==0);
 }else if(scenario=="trigger-reconfigure"){
  source.pending("new");source.updated_.callback=[&]{source.configure_directory("",389,"","","ha",false,false);};
  source.loop();assert(!source.timer_active&&!source.enabled_&&std::string(source.status_)=="disabled"&&phone.rows.empty());
 }else assert(false);
 assert(!locked);
}
'''
    source = (SOURCE / "ldap_directory.cpp").read_text()
    methods = ["void LdapDirectory::worker_run_()", "void LdapDirectory::loop()",
               "void LdapDirectory::reset_stage_()", "void LdapDirectory::refresh()",
               "void LdapDirectory::configure_directory("]
    cpp = cpp.replace("METHODS", "\n".join(block(source, marker) for marker in methods))
    path = folder / "lifecycle.cpp"
    path.write_text(cpp)
    binary = folder / "lifecycle"
    built = subprocess.run(["g++", "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie",
        "-DUSE_ESPHOME_VOIP_DIRECTORY", "-I", str(ROOT), str(path),
        str(SOURCE / "ldap_protocol.cpp"), str(SOURCE / "directory_contact.cpp"),
        str(ROOT / "esphome/components/phone_client/net_stream.cpp"), "-o", str(binary)],
        capture_output=True, text=True, timeout=90)
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize("scenario", ["pending-refresh", "stale-before", "cancel-callback", "search-error", "trigger-reconfigure"])
def test_actual_stage_owner_cancellation_and_reentrant_callbacks(lifecycle_binary, scenario):
    result = subprocess.run([lifecycle_binary, scenario], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
