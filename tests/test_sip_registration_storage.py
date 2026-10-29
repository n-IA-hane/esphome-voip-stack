"""Run production overlay/reset methods with an independent key-scoped NVS model."""
import subprocess

import pytest

from test_refer_dialog_runtime import SOURCE, block


@pytest.mark.parametrize("capacity", [1, 2, 3, 4])
def test_registration_overlay_storage_contracts(tmp_path, capacity):
    header = (SOURCE / "voip_stack.h").read_text()
    source = (SOURCE / "voip_registration.cpp").read_text()
    saved_account = block(header, "struct SavedRegistrationAccount") + ";"
    saved = block(header, "struct SavedRegistration {") + ";"
    methods = "\n".join(block(source, name) for name in [
        "void VoipStack::add_registration_account(",
        "bool VoipStack::load_registration_saved_(", "bool VoipStack::write_registration_saved_(",
        "bool VoipStack::save_registration_override_(", "bool VoipStack::reset_account(",
    ])
    copier = block(source, "template <size_t N>") if "template <size_t N>" in source else block(source, "template<size_t N>")
    prefix = r'''
#include "sip_registration.h"
#include <array>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <algorithm>
#include <vector>
#include <cstddef>
using namespace esphome::voip_stack;
using nvs_handle_t=unsigned;constexpr int ESP_OK=0;
std::vector<uint8_t> flash_blob;bool fail_set=false,fail_commit=false;
int nvs_get_blob(unsigned,const char*key,void*data,size_t*size){
 assert(std::string(key)=="accounts");if(flash_blob.empty())return 1;
 if(!data){*size=flash_blob.size();return 0;}
 if(*size<flash_blob.size()){*size=flash_blob.size();return 2;}
 std::memcpy(data,flash_blob.data(),flash_blob.size());*size=flash_blob.size();return 0;
}
int nvs_set_blob(unsigned,const char*key,const void*data,size_t size){
 assert(std::string(key)=="accounts");if(fail_set)return 3;
 auto*p=static_cast<const uint8_t*>(data);flash_blob.assign(p,p+size);return 0;
}
int nvs_commit(unsigned){if(fail_commit){fail_commit=false;return 4;}return 0;}
template<class T>struct RAMAllocator{
 enum{ALLOC_INTERNAL,ALLOC_EXTERNAL};explicit RAMAllocator(int){}
 T*allocate(size_t n){return static_cast<T*>(std::malloc(n*sizeof(T)));}
 void deallocate(T*p,size_t){std::free(p);}
};
struct SipTransport {
 using AccountCommit=bool(*)(void*,const std::string&,const SipAccountConfig*);
 bool present=false;SipAccountConfig active;
 bool set_account(const SipAccountConfig&c,AccountCommit commit,void*ctx){
  if(commit&&!commit(ctx,c.id,&c))return false;active=c;present=true;return true;}
 bool remove_account(const std::string&id,AccountCommit commit,void*ctx,bool allow_missing=false){
  if(!present&&!allow_missing)return false;
  if(commit&&!commit(ctx,id,nullptr))return false;present=false;return true;}
};
struct VoipStack {
 REPLACE_ACCOUNT
 REPLACE_SAVED
 std::unique_ptr<SavedRegistration> registration_saved_=std::make_unique<SavedRegistration>();
 unsigned registration_nvs_handle_=1;
 const SipAccountConfigView* registration_baseline_=nullptr;
 std::unique_ptr<std::vector<SipAccountConfig>> registration_cpp_baseline_;
 SipAccountConfig registration_baseline_at_(size_t i)const{
  return registration_cpp_baseline_?(*registration_cpp_baseline_)[i]:registration_baseline_[i].materialize();}
 bool registration_baseline_matches_(size_t i,const std::string&id)const{
  return registration_cpp_baseline_?(*registration_cpp_baseline_)[i].id==id:id==registration_baseline_[i].id;}
 void add_registration_account(const std::string&,const std::string&,uint16_t,const std::string&,const std::string&,
 const std::string&,const std::string&,const std::string&,uint16_t,bool,uint32_t,bool);
 size_t registration_baseline_count_=0;
 std::unique_ptr<SipTransport>transport_=std::make_unique<SipTransport>();
 std::string default_line_="direct";
 bool load_registration_saved_();bool write_registration_saved_(const SavedRegistration&);
 bool save_registration_override_(const std::string&,const SipAccountConfig*,bool);
 bool reset_account(const std::string&);
};
'''.replace("REPLACE_ACCOUNT", saved_account).replace("REPLACE_SAVED", saved)
    main = r'''
SipAccountConfig account(std::string id){SipAccountConfig c;c.id=id;c.registrar=c.domain="pbx.example";c.username="100";c.password="test-only";return c;}
int main(){
 VoipStack v;auto first=account("first");
 assert(v.save_registration_override_(first.id,&first,false));assert(v.registration_saved_->accounts[0].used);
 auto before=flash_blob;fail_set=true;auto second=account("second");auto failed=first;failed.password="changed-test";
 assert(!v.save_registration_override_(failed.id,&failed,false));assert(flash_blob==before);fail_set=false;
 fail_commit=true;assert(!v.save_registration_override_(failed.id,&failed,false));assert(flash_blob==before);
 // Runtime-only removals free their whole config slot, rather than consuming it with a tombstone.
 assert(v.save_registration_override_(first.id,nullptr,true));
 assert(!v.registration_saved_->accounts[0].used);
 assert(v.save_registration_override_(second.id,&second,false));
 assert(v.save_registration_override_(second.id,nullptr,true));
 auto third=account("third");assert(v.save_registration_override_(third.id,&third,false));
 // Removing a YAML account uses only the separate bounded ID list.
 const SipAccountConfigView baseline[]{
  {"first","pbx.example","pbx.example","100","test-only","","",5060,5060,3600,false,true}};
 v.registration_baseline_=baseline;v.registration_baseline_count_=1;
 assert(v.save_registration_override_(first.id,nullptr,true));
 assert(std::string(v.registration_saved_->removed[0].data())=="first");
 assert(v.reset_account("first"));assert(v.registration_saved_->removed[0][0]==0);
 assert(v.transport_->present&&v.transport_->active.id=="first");
 // The incremental C++ builder retains the immutable rows before adding.
 if(SipRegistration::CAPACITY>1){
  v.add_registration_account("added","pbx.example",5060,"pbx.example","101","test","","",5060,false,3600,true);
  assert(v.registration_cpp_baseline_&&v.registration_baseline_count_==2);
  assert(v.registration_baseline_matches_(0,"first")&&v.registration_baseline_matches_(1,"added"));
 }
 // Clearing an orphan saved removal works even if no runtime account exists.
 v.transport_->present=false;v.registration_baseline_count_=0;
 std::strcpy(v.registration_saved_->removed[0].data(),"orphan");
 assert(v.reset_account("orphan"));assert(v.registration_saved_->removed[0][0]==0);
 // Decode a valid on-flash pool of a different capacity without losing its account.
 const size_t header=offsetof(VoipStack::SavedRegistration,accounts);
 for(uint16_t old_count:{uint16_t(1),uint16_t(2),uint16_t(3),uint16_t(4)}){
  flash_blob.assign(header+old_count*sizeof(VoipStack::SavedRegistrationAccount)+old_count*33,0);
  uint32_t version=2;std::memcpy(flash_blob.data(),&version,4);
  std::memcpy(flash_blob.data()+4,&old_count,2);std::memcpy(flash_blob.data()+6,&old_count,2);
  VoipStack::SavedRegistrationAccount row{};row.used=1;std::strcpy(row.id,"restored");
  std::strcpy(row.registrar,"pbx.example");std::strcpy(row.domain,"pbx.example");std::strcpy(row.username,"100");
  std::memcpy(flash_blob.data()+header,&row,sizeof(row));
  VoipStack reboot;assert(reboot.load_registration_saved_());
  assert(reboot.registration_saved_->accounts[0].used&&std::string(reboot.registration_saved_->accounts[0].id)=="restored");
 }
}
'''
    defines = tmp_path / "esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    cpp = tmp_path / "storage.cpp"
    cpp.write_text(prefix + copier + "\n" + methods + "\n" + main)
    output = tmp_path / "storage"
    result = subprocess.run(["g++", "-std=c++17", "-g", "-Wall", "-Wextra", "-fsanitize=address,undefined",
        "-DUSE_ESPHOME_VOIP_REGISTRATION", f"-DVOIP_REGISTRATION_CAPACITY={capacity}",
        "-I", str(tmp_path), "-I", str(SOURCE), str(cpp), "-o", str(output)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([output], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
