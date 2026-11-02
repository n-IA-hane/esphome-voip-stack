"""Actual UI getter reads primitive status without copying owned SIP IDs."""

import subprocess

from test_refer_dialog_runtime import SOURCE, block


def test_status_queries_allocate_nothing_and_keep_owned_callback_info(tmp_path):
    defines = tmp_path / "esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    header = (SOURCE / "sip_blf.h").read_text()
    adapter = (SOURCE / "sip_registration_transport.cpp").read_text()
    owner = (SOURCE / "voip_blf.cpp").read_text()
    lightweight = "struct BlfStatus" in header
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
 BlfStateInfo info;
 Cached(){info.contact_id="stable-directory-identity-123456789";info.account_id="long-account-identity-1234";
  info.available=true;info.presence=PresenceState::OPEN;info.state=BlfState::ACTIVE;info.active_dialogs=1;info.version=7;}
 size_t blf_capacity()const{return 1;}
 const BlfStateInfo*blf_state(size_t i)const{return i==0?&info:nullptr;}
};
struct SipTransport {
 int dialog_mutex_=0;Cached mwi_;
'''
    if lightweight:
        code += '''template<class Result> bool copy_blf_state_(std::string_view,Result*);
bool get_blf_state(std::string_view,BlfStatus*);
bool get_blf_info(std::string_view,BlfStateInfo*);
};
'''
        code += block(adapter, "template<typename Result> bool SipTransport::copy_blf_state_(") + "\n"
        code += block(adapter, "bool SipTransport::get_blf_state(") + "\n"
        code += block(adapter, "bool SipTransport::get_blf_info(") + "\n"
        result_type = "BlfStatus"
    else:
        code += 'bool get_blf_state(std::string_view,BlfStateInfo*);\n};\n'
        code += block(adapter, "bool SipTransport::get_blf_state(") + "\n"
        result_type = "BlfStateInfo"
    code += f'''struct VoipStack {{
std::unique_ptr<SipTransport> transport_=std::make_unique<SipTransport>();bool blf_ready_=true;
{result_type} get_contact_blf(std::string_view);
'''
    if lightweight:
        code += 'BlfStateInfo get_contact_blf_info_(std::string_view);\n};\n'
        code += block(owner, "BlfStatus VoipStack::get_contact_blf(") + "\n"
        code += block(owner, "BlfStateInfo VoipStack::get_contact_blf_info_(") + "\n"
    else:
        code += '};\n'
        code += block(owner, "BlfStateInfo VoipStack::get_contact_blf(") + "\n"
    code += r'''
int main(){
 VoipStack phone;const std::string_view id=phone.transport_->mwi_.info.contact_id;
 measuring=true;allocations=0;
 for(unsigned n=0;n<20;++n){auto state=phone.get_contact_blf(id);assert(state.available&&state.version==7&&state.active_dialogs==1);}
 measuring=false;const auto queries=allocations;
 measuring=true;allocations=0;
'''
    read = "get_contact_blf_info_" if lightweight else "get_contact_blf"
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
