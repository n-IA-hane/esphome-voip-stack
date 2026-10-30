"""Measure actual directory commits, allocator placement and warm reuse."""
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def storage_program():
    return r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <new>
#include <vector>
#include <esp_heap_caps.h>
struct alignas(std::max_align_t) Header {size_t bytes;unsigned caps;};
size_t live_bytes=0,peak_bytes=0,window_base=0,allocations=0,external_bytes=0,internal_bytes=0;
bool measuring=false,fail_external=false;
void *record_allocate(size_t bytes,unsigned caps){
 if(fail_external&&(caps&MALLOC_CAP_SPIRAM))return nullptr;
 auto*h=static_cast<Header*>(std::malloc(sizeof(Header)+bytes));if(!h)return nullptr;
 h->bytes=bytes;h->caps=caps;live_bytes+=bytes;
 if(measuring){++allocations;peak_bytes=std::max(peak_bytes,live_bytes);if(caps&MALLOC_CAP_SPIRAM)external_bytes+=bytes;else internal_bytes+=bytes;}
 return h+1;
}
void record_free(void*p){if(!p)return;auto*h=static_cast<Header*>(p)-1;live_bytes-=h->bytes;std::free(h);}
void *operator new(size_t size){auto*p=record_allocate(size,0);if(!p)throw std::bad_alloc();return p;}
void *operator new[](size_t size){return ::operator new(size);}
void operator delete(void*p)noexcept{record_free(p);}void operator delete[](void*p)noexcept{record_free(p);}
void operator delete(void*p,size_t)noexcept{record_free(p);}void operator delete[](void*p,size_t)noexcept{record_free(p);}
void *heap_caps_malloc(size_t n,unsigned caps){return record_allocate(n,caps);}void heap_caps_free(void*p){record_free(p);}
void begin(){window_base=live_bytes;peak_bytes=live_bytes;allocations=external_bytes=internal_bytes=0;measuring=true;}
struct Metrics{size_t peak,calls,external,internal;};
Metrics end(){measuring=false;return {peak_bytes-window_base,allocations,external_bytes,internal_bytes};}
#include "esphome/components/voip_stack/phonebook.h"
using namespace esphome::voip_stack;
ContactEntry row(unsigned i){ContactEntry e;e.id="row"+std::to_string(i);e.name="Name"+std::to_string(i);e.route="sip:row"+std::to_string(i)+"@pbx.example.test";return e;}
std::vector<ContactEntry> snapshot(){std::vector<ContactEntry> v;v.reserve(9);for(unsigned i=0;i<9;++i)v.push_back(row(i));return v;}
int main(){
 Phonebook book;int provider;
#ifdef AFTER_MEMORY
#ifdef USE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM
 fail_external=true;assert(!book.prepare_directory_storage()&&book.empty());fail_external=false;
#endif
 begin();assert(book.prepare_directory_storage());const auto boot=end();
#ifdef USE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM
 assert(boot.calls==1&&boot.external==64*sizeof(ContactEntry)&&boot.internal==0);
#else
 assert(boot.calls==0);
#endif
#endif
 ContactEntry manual;manual.id="manual";manual.name="Manual";manual.route="100";assert(book.add_entry(manual)==AddResult::Added);
 auto first=snapshot();begin();assert(book.replace_directory(&provider,std::move(first))==DirectoryCommitResult::APPLIED);const auto cold=end();
 assert(book.size()==10&&book.select("row5"));
 auto same=snapshot();begin();assert(book.replace_directory(&provider,std::move(same))==DirectoryCommitResult::UNCHANGED);const auto warm=end();
 assert(book.current()->stable_id()=="row5");
 auto changed=snapshot();changed[5].name="Renamed";begin();assert(book.replace_directory(&provider,std::move(changed))==DirectoryCommitResult::APPLIED);const auto update=end();
 assert(book.current_name()=="Renamed"&&book.current()->stable_id()=="row5");
 begin();
 for(unsigned n=0;n<100;++n){
  const auto*entry=book.find_id(std::string_view("row5"));assert(entry&&book.find(std::string_view("Renamed"))==entry);
  const std::string_view id=entry->stable_id(),name=entry->name,route=entry->route;
  assert(id.data()==entry->id.data()&&name.data()==entry->name.data()&&route.data()==entry->route.data());
  assert(book.current_name()==name&&book.current_route()==route);
 }
 const auto queries=end();assert(queries.calls==0);
#ifdef USE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM
 const auto*route_header=reinterpret_cast<const Header*>(book.current()->route.data())-1;
 assert(route_header->caps&MALLOC_CAP_SPIRAM);
#endif
 std::cout<<"entry="<<sizeof(ContactEntry)<<" cold_peak="<<cold.peak<<" warm_peak="<<warm.peak<<" warm_alloc="<<warm.calls<<" update_alloc="<<update.calls<<std::endl;
 assert(warm.calls==0&&update.calls==0);
#ifdef AFTER_MEMORY
 std::array<ContactEntry,9> raw;for(unsigned i=0;i<9;++i)raw[i]=row(i);
 raw[5].name="Borrowed";const auto *payload=raw[5].route.data();
 begin();assert(book.replace_directory(&provider,raw.data(),raw.size())==DirectoryCommitResult::APPLIED);const auto borrow=end();
 assert(borrow.calls==0&&book.find_id("row5")->route.data()==payload&&book.current_name()=="Borrowed");
 for(auto&entry:raw)entry=ContactEntry{};
 assert(book.find_id("row5")->route=="sip:row5@pbx.example.test");
 assert(book.replace_directory(&provider,nullptr,1)==DirectoryCommitResult::INVALID);
 assert(book.replace_directory(&provider,raw.data(),65)==DirectoryCommitResult::CAPACITY);
 assert(book.current_name()=="Borrowed");
#endif
}
'''


def compile_storage(folder, *, external, include_root=ROOT, after=True):
    folder.mkdir(parents=True, exist_ok=True)
    (folder / 'esp_heap_caps.h').write_text('''#pragma once
#include <cstddef>
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_INTERNAL 2U
#define MALLOC_CAP_8BIT 4U
void *heap_caps_malloc(std::size_t,unsigned);
void heap_caps_free(void*);
''')
    core = folder / 'esphome/core'
    core.mkdir(parents=True, exist_ok=True)
    (core / 'defines.h').write_text('')
    (core / 'helpers.h').write_text('''#pragma once
#include <esp_heap_caps.h>
namespace esphome { template<class T> struct RAMAllocator {
using value_type=T;enum {ALLOC_EXTERNAL=1};explicit RAMAllocator(int flags=0):flags_(flags){}
T*allocate(size_t n){return static_cast<T*>(heap_caps_malloc(n*sizeof(T),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));}
void deallocate(T*p,size_t){heap_caps_free(p);}int flags_;
}; }
''')
    source = folder / 'storage.cpp'
    source.write_text(storage_program())
    binary = folder / 'storage'
    command = ['g++', '-std=c++17', '-O1', '-g', '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-DUSE_ESP32', '-DESP_PLATFORM', '-DUSE_ESPHOME_VOIP_DIRECTORY']
    if external:
        command.append('-DUSE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM')
    if after:
        command.append('-DAFTER_MEMORY')
    command += ['-I', str(folder), '-I', str(include_root), str(source), '-o', str(binary)]
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize('external', [False, True], ids=['internal-count-bound', 'signaling-psram'])
def test_phonebook_storage_reuses_capacity_and_handles_boot_failure(tmp_path, external):
    binary = compile_storage(tmp_path, external=external)
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.mark.parametrize('signaling', [False, True])
def test_without_directory_preserves_legacy_string_fields_and_references(tmp_path, signaling):
    source = tmp_path / 'legacy.cpp'
    source.write_text('''#include "esphome/components/voip_stack/phonebook.h"
#include <type_traits>
#include <utility>
#include <cassert>
using namespace esphome::voip_stack;
static_assert(std::is_same_v<decltype(ContactEntry{}.name),std::string>);
static_assert(std::is_same_v<decltype(ContactEntry{}.route),std::string>);
static_assert(std::is_same_v<decltype(std::declval<const Phonebook&>().current_name()),const std::string&>);
static_assert(std::is_same_v<decltype(std::declval<const Phonebook&>().current_route()),const std::string&>);
static_assert(std::is_same_v<decltype(std::declval<const Phonebook&>().current_ip()),const std::string&>);
int main(){Phonebook book;assert(book.add_one("Kitchen")==AddResult::Added);
const auto*entry=book.find("Kitchen");assert(entry && &book.current_name()==&entry->name);}
''')
    binary = tmp_path / 'legacy'
    flags = ['-DUSE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM'] if signaling else []
    result = subprocess.run(['g++', '-std=c++17', '-DUSE_ESP32', *flags, '-I', str(ROOT),
                             str(source), '-o', str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
