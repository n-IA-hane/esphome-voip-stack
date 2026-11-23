"""Actual UI getter reads primitive status without copying owned SIP IDs."""

import subprocess

from test_refer_dialog_runtime import SOURCE, block


def test_status_queries_allocate_nothing_and_keep_owned_callback_info(tmp_path):
    defines = tmp_path / "esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    adapter = (SOURCE / "sip_registration_transport.cpp").read_text()
    owner = (SOURCE / "voip_blf.cpp").read_text()
    code = r'''
#include "sip_blf.h"
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <string_view>
#include <type_traits>
bool measuring=false;unsigned allocations=0;
void*operator new(size_t n){if(measuring)++allocations;auto*p=std::malloc(n);if(!p)throw std::bad_alloc();return p;}
void*operator new[](size_t n){return ::operator new(n);}
void operator delete(void*p)noexcept{std::free(p);}void operator delete[](void*p)noexcept{std::free(p);}
void operator delete(void*p,size_t)noexcept{std::free(p);}void operator delete[](void*p,size_t)noexcept{std::free(p);}
using namespace esphome::voip_stack;
struct LockGuard{explicit LockGuard(int&){};};
struct Cached {
 BlfStateInfo info,other;
 std::array<bool,2> requested{{true,false}};
 Cached(){info.contact_id="stable-directory-identity-123456789";info.account_id="long-account-identity-1234";
  info.available=true;info.presence=PresenceState::OPEN;info.state=BlfState::ACTIVE;info.active_dialogs=1;info.version=7;other=info;other.account_id="other-account-identity-1234";other.state=BlfState::RINGING;other.active_dialogs=0;other.ringing_dialogs=1;}
 size_t blf_capacity()const{return 2;}
 bool blf_watch_requested(size_t i)const{return requested.at(i);}
 const BlfStateInfo*blf_state(size_t i)const{return i==0?&info:i==1?&other:nullptr;}
};
struct SipTransport {
 int dialog_mutex_=0;Cached mwi_;
'''
    code += """template<class Result> bool copy_blf_state_(std::string_view,Result*,std::string_view = {});
bool get_blf_state(std::string_view,BlfStatus*,std::string_view = {});
bool get_blf_info(std::string_view,BlfStateInfo*,std::string_view = {});
};
"""
    for marker in ("template<typename Result> bool SipTransport::copy_blf_state_(",
                   "bool SipTransport::get_blf_state(", "bool SipTransport::get_blf_info("):
        code += block(adapter, marker) + "\n"
    code += """struct VoipStack {
std::unique_ptr<SipTransport> transport_=std::make_unique<SipTransport>();bool blf_ready_=true;
std::string blf_account_id_="long-account-identity-1234";
BlfStatus get_contact_blf(std::string_view,std::string_view = {});
BlfStateInfo get_contact_blf_info_(std::string_view,std::string_view = {});
};
"""
    code += block(owner, "BlfStatus VoipStack::get_contact_blf(") + "\n"
    code += block(owner, "BlfStateInfo VoipStack::get_contact_blf_info_(") + "\n"
    code += r'''
int main(){
 VoipStack phone;const std::string_view id=phone.transport_->mwi_.info.contact_id;
 measuring=true;allocations=0;
 for(unsigned n=0;n<20;++n){auto state=phone.get_contact_blf(id);assert(state.available&&state.version==7&&state.active_dialogs==1);}
 phone.transport_->mwi_.requested[1]=true;
 for(unsigned n=0;n<20;++n){
  assert(!phone.get_contact_blf(id).available);
  assert(phone.get_contact_blf(id,"long-account-identity-1234").state==BlfState::ACTIVE);
  assert(phone.get_contact_blf(id,"other-account-identity-1234").state==BlfState::RINGING);
  assert(phone.get_contact_blf(id,"direct").state==BlfState::ACTIVE);
  assert(!phone.get_contact_blf(id,"missing").available);
 }
 phone.transport_->mwi_.requested[1]=false;
 phone.blf_account_id_.clear();
 assert(!phone.get_contact_blf(id,"direct").available);
 measuring=false;const auto queries=allocations;
 measuring=true;allocations=0;
'''
    read = "get_contact_blf_info_"
    code += f'for(unsigned n=0;n<20;++n){{auto info=phone.{read}(id);assert(info.contact_id==id&&info.account_id=="long-account-identity-1234");}}\n'
    code += r'''
 measuring=false;const auto owned=allocations;
 std::cout<<"query_allocations="<<queries<<" owned_allocations="<<owned<<std::endl;
 assert(owned>0&&"owned callbacks must keep their lifetime-safe metadata");
 assert(queries==0&&"row status queries must not allocate owned SIP identity strings");
}
'''
    source = tmp_path / "query.cpp"
    source.write_text(code)
    binary = tmp_path / "query"
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_ESPHOME_VOIP_BLF",
        "-I", str(tmp_path), "-I", str(SOURCE), str(source), "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr
