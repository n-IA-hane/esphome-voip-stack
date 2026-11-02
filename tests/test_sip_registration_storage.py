"""Run production overlay/reset methods with an independent key-scoped NVS model."""
import subprocess

import pytest

from test_refer_dialog_runtime import SOURCE, block


def storage_source():
    header = (SOURCE / "voip_stack.h").read_text()
    source = (SOURCE / "voip_registration.cpp").read_text()
    saved_account = block(header, "struct SavedRegistrationAccount") + ";"
    saved = block(header, "struct SavedRegistration {") + ";"
    deleter = block(header, "struct RegistrationSavedDeleter") + ";"
    methods = "\n".join(block(source, name) for name in [
        "VoipStack::RegistrationSavedPtr VoipStack::allocate_registration_saved_(",
        "void VoipStack::add_registration_account(",
        "bool VoipStack::set_account(", "bool VoipStack::remove_account(",
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
// Select only the storage allocation policy after the independent account
// declarations. The registration engine is unchanged and compiled normally.
#ifdef TEST_STORAGE_EXTERNAL
#define USE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM
#endif
using namespace esphome::voip_stack;
using nvs_handle_t=unsigned;constexpr int ESP_OK=0;
std::vector<uint8_t> flash_blob;bool fail_set=false,fail_commit=false,fail_allocate=false;
std::vector<void*> live_allocations;unsigned allocation_region=0,nvs_sets=0,nvs_commits=0;
int nvs_get_blob(unsigned,const char*key,void*data,size_t*size){
 assert(std::string(key)=="accounts");if(flash_blob.empty())return 1;
 if(!data){*size=flash_blob.size();return 0;}
 if(*size<flash_blob.size()){*size=flash_blob.size();return 2;}
 std::memcpy(data,flash_blob.data(),flash_blob.size());*size=flash_blob.size();return 0;
}
int nvs_set_blob(unsigned,const char*key,const void*data,size_t size){
 assert(std::string(key)=="accounts");++nvs_sets;if(fail_set)return 3;
 auto*p=static_cast<const uint8_t*>(data);flash_blob.assign(p,p+size);return 0;
}
int nvs_commit(unsigned){++nvs_commits;if(fail_commit){fail_commit=false;return 4;}return 0;}
template<class T>struct RAMAllocator{
 enum{ALLOC_INTERNAL=1,ALLOC_EXTERNAL=2};unsigned region;
 explicit RAMAllocator(unsigned selected=ALLOC_INTERNAL):region(selected){}
 T*allocate(size_t n){
  allocation_region=region;if(fail_allocate)return nullptr;
  auto*p=static_cast<T*>(std::malloc(n*sizeof(T)));if(p)live_allocations.push_back(p);return p;
 }
 void deallocate(T*p,size_t){
  auto at=std::find(live_allocations.begin(),live_allocations.end(),p);
  assert(at!=live_allocations.end());live_allocations.erase(at);std::free(p);
 }
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
 REPLACE_DELETER
 using RegistrationSavedPtr=std::unique_ptr<SavedRegistration,RegistrationSavedDeleter>;
 static RegistrationSavedPtr allocate_registration_saved_(const SavedRegistration* copy_from=nullptr);
 RegistrationSavedPtr registration_saved_=allocate_registration_saved_();
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
 bool set_account(const SipAccountConfig&,bool persist=false);
 bool remove_account(const std::string&,bool persist=false);
};
'''.replace("REPLACE_ACCOUNT", saved_account).replace("REPLACE_SAVED", saved).replace("REPLACE_DELETER", deleter)
    main = r'''
SipAccountConfig account(std::string id){SipAccountConfig c;c.id=id;c.registrar=c.domain="pbx.example";c.username="100";c.password="test-only";return c;}
int main(){
 {
 VoipStack v;auto first=account("first");
 assert(live_allocations.size()==1);
#ifdef USE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM
 assert(allocation_region==RAMAllocator<int>::ALLOC_EXTERNAL);
#else
 assert(allocation_region==RAMAllocator<int>::ALLOC_INTERNAL);
#endif
 // A failed initial pool allocation still permits volatile provisioning.
 fail_allocate=true;
 {
  VoipStack without_pool;assert(!without_pool.registration_saved_);
  assert(without_pool.set_account(first,false));
  assert(!without_pool.set_account(first,true));
  assert(!without_pool.remove_account(first.id,true));
  assert(!without_pool.reset_account(first.id));
  assert(without_pool.transport_->present&&without_pool.transport_->active.id==first.id);
 }
 fail_allocate=false;assert(live_allocations.size()==1);
 assert(v.set_account(first,true));
 const auto *bytes=reinterpret_cast<const uint8_t*>(v.registration_saved_.get());
 const std::vector<uint8_t> overlay_before(bytes,bytes+sizeof(VoipStack::SavedRegistration));
 const auto flash_before=flash_blob;const auto sets_before=nvs_sets,commits_before=nvs_commits;
 auto replacement=first;replacement.password="replacement";
 fail_allocate=true;
 assert(!v.set_account(replacement,true));
 assert(!v.remove_account(first.id,true));
 assert(!v.reset_account(first.id));
 assert(std::memcmp(v.registration_saved_.get(),overlay_before.data(),overlay_before.size())==0);
 assert(flash_blob==flash_before&&nvs_sets==sets_before&&nvs_commits==commits_before);
 assert(v.transport_->present&&v.transport_->active.id==first.id&&v.transport_->active.password==first.password);
 assert(live_allocations.size()==1);
 assert(v.set_account(replacement,false));assert(v.transport_->active.password==replacement.password);
 assert(v.remove_account(first.id,false));assert(!v.transport_->present);
 assert(flash_blob==flash_before);
 fail_allocate=false;
 assert(v.set_account(first,false));
 assert(v.save_registration_override_(first.id,&first,false));assert(v.registration_saved_->accounts[0].used);
 auto before=flash_blob;fail_set=true;auto second=account("second");auto failed=first;failed.password="changed-test";
 assert(!v.save_registration_override_(failed.id,&failed,false));assert(flash_blob==before);fail_set=false;
 assert(live_allocations.size()==1);
 fail_commit=true;assert(!v.save_registration_override_(failed.id,&failed,false));assert(flash_blob==before);
 assert(live_allocations.size()==1);
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
 assert(live_allocations.empty());
}
'''
    return prefix + copier + "\n" + methods + "\n" + main


@pytest.mark.parametrize("capacity", [1, 2, 3, 4])
@pytest.mark.parametrize("external", [False, True], ids=["internal", "psram"])
def test_registration_overlay_storage_contracts(tmp_path, capacity, external):
    defines = tmp_path / "esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    cpp = tmp_path / "storage.cpp"
    cpp.write_text(storage_source())
    output = tmp_path / "storage"
    command = ["g++", "-std=c++17", "-g", "-Wall", "-Wextra", "-fsanitize=address,undefined",
        "-DUSE_ESPHOME_VOIP_REGISTRATION", f"-DVOIP_REGISTRATION_CAPACITY={capacity}",
        "-I", str(tmp_path), "-I", str(SOURCE), str(cpp), str(SOURCE / "sip_registration.cpp"), "-o", str(output)]
    if external:
        command.append("-DTEST_STORAGE_EXTERNAL")
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([output], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
