"""Execute real cold storage, account APIs, factory admission and joined teardown."""
from pathlib import Path
import os
import resource
import subprocess

import pytest

from test_refer_dialog_runtime import block

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "esphome/components/voip_stack"


@pytest.mark.parametrize("external,registration,fail", [
    (False, True, 0), (True, True, 0), (True, True, 1), (True, True, 2),
    (False, False, 0), (True, False, 0), (True, False, 1),
])
def test_cold_storage_admission_identity_and_teardown(tmp_path, external, registration, fail):
    core = tmp_path / "esphome/core"
    core.mkdir(parents=True)
    (core / "defines.h").write_text("")
    (core / "helpers.h").write_text(r'''#pragma once
#include <cstdlib>
#include <cstddef>
namespace probe {void*allocate(size_t,unsigned);void release(void*);}
namespace esphome {template<class T>struct RAMAllocator {
 using value_type=T;enum{ALLOC_EXTERNAL=1};int flags;
 explicit RAMAllocator(int f=0):flags(f){}
 template<class U>RAMAllocator(const RAMAllocator<U>&a):flags(a.flags){}
 T*allocate(size_t n){return static_cast<T*>(probe::allocate(n*sizeof(T),flags));}
 void deallocate(T*p,size_t){probe::release(p);}
 template<class U>bool operator==(const RAMAllocator<U>&a)const{return flags==a.flags;}
 template<class U>bool operator!=(const RAMAllocator<U>&a)const{return flags!=a.flags;}
};}
''')
    (tmp_path / "esp_heap_caps.h").write_text(r'''#pragma once
#include <cstdlib>
constexpr unsigned MALLOC_CAP_SPIRAM=1,MALLOC_CAP_INTERNAL=2,MALLOC_CAP_8BIT=4;
inline unsigned internal_objects=0;
inline void*heap_caps_malloc(size_t n,unsigned caps){
 if(caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT))++internal_objects;
 return std::malloc(n);
}
inline void heap_caps_free(void*p){std::free(p);}
''')
    header = (SOURCE / "sip_transport.h").read_text()
    cpp = (SOURCE / "sip_transport.cpp").read_text()
    scratch_start = header.index("#ifdef USE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM\n  struct SipRxScratch")
    scratch_end = header.index("  AudioFormatList offer_tx_formats_", scratch_start)
    pieces = {
        "// REAL_DELETER": block(header, "struct ReferDeleter {") + ";",
        "// REAL_SCRATCH": header[scratch_start:scratch_end],
        "// REAL_READY": block(header, "  bool initialized() const {"),
        "// REAL_START": block(cpp, "bool SipTransport::start()"),
        "// REAL_STOP": block(cpp, "void SipTransport::stop()"),
        "// REAL_ALLOCATOR": cpp[cpp.index("void *SipTransport::operator new("):cpp.index("SipTransport::SipTransport(")],
        "// REAL_DESTRUCTOR": block(cpp, "SipTransport::~SipTransport()"),
        "// REAL_FACTORY": block((SOURCE / "voip_stack.cpp").read_text(), "bool VoipStack::setup_transport_()"),
    }
    code = (ROOT / "tests/fixtures/sip_cold_storage.cpp").read_text()
    for marker, body in pieces.items():
        assert code.count(marker) == 1
        code = code.replace(marker, body)
    (tmp_path / "cold.cpp").write_text(code)
    defines = ["-DUSE_ESP32", "-DUSE_ESPHOME_VOIP_SIP_TRANSPORT", "-DVOIP_REGISTRATION_CAPACITY=4", f"-DFAIL_STAGE={fail}"]
    if external:
        defines.append("-DUSE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM")
    if registration:
        defines += ["-DUSE_ESPHOME_VOIP_REGISTRATION", "-DUSE_ESPHOME_VOIP_MWI"]
    binary = tmp_path / "cold"
    built = subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", *defines,
        "-I", str(tmp_path), "-I", str(SOURCE), str(tmp_path / "cold.cpp"),
        str(SOURCE / "sip_registration.cpp"), str(SOURCE / "sip_mwi.cpp"), "-o", str(binary),
    ], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    ran = subprocess.run([binary], capture_output=True, text=True, timeout=15)
    assert ran.returncode == 0, ran.stderr
    assert "COLD_STORAGE_OK" in ran.stdout


def test_registration_array_uses_requested_psram_region(tmp_path):
    source = Path(os.environ.get("VOIP_COLD_CONSUMER_ROOT", ROOT)) / "esphome/components/voip_stack"
    core = tmp_path / "esphome/core"
    core.mkdir(parents=True)
    (core / "defines.h").write_text("")
    (core / "helpers.h").write_text(r'''#pragma once
#include <cassert>
#include <cstdlib>
#include <cstdint>
namespace placement_probe {inline unsigned allocations=0,frees=0;inline uintptr_t first=0,last=0;}
namespace esphome {template<class T>struct RAMAllocator {
 using value_type=T;enum{ALLOC_EXTERNAL=1};int flags;
 explicit RAMAllocator(int f=0):flags(f){}
 template<class U>RAMAllocator(const RAMAllocator<U>&a):flags(a.flags){}
 T*allocate(size_t n){assert(flags==ALLOC_EXTERNAL);auto*p=std::malloc(n*sizeof(T));assert(p);
  ++placement_probe::allocations;placement_probe::first=reinterpret_cast<uintptr_t>(p);
  placement_probe::last=placement_probe::first+n*sizeof(T);return static_cast<T*>(p);}
 void deallocate(T*p,size_t){++placement_probe::frees;std::free(p);}
 template<class U>bool operator==(const RAMAllocator<U>&a)const{return flags==a.flags;}
 template<class U>bool operator!=(const RAMAllocator<U>&a)const{return flags!=a.flags;}
};}
''')
    cpp = tmp_path / "placement.cpp"
    cpp.write_text(r'''
#include "sip_registration.h"
#include <cassert>
#include <cstdint>
using namespace esphome::voip_stack;
int main(){
 {SipRegistration pool;assert(placement_probe::allocations==1);
  for(unsigned i=0;i<4;++i){
   const auto address=reinterpret_cast<uintptr_t>(&pool.account(i));
   assert(address>=placement_probe::first&&address+sizeof(SipRegistration::Account)<=placement_probe::last);
   assert(pool.account(i).config.id.empty()&&!pool.account(i).occupied);
   SipAccountConfig c;c.id="line"+std::to_string(i);c.registrar=c.domain="pbx.example";
   c.username="test";c.password=std::string(96,'s');assert(pool.set_account(c,0));
   assert(reinterpret_cast<uintptr_t>(&pool.account(i))==address);
  }
 }
 assert(placement_probe::allocations==1&&placement_probe::frees==1);
}
''')
    binary = tmp_path / "placement"
    built = subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
        "-DUSE_ESPHOME_VOIP_REGISTRATION", "-DUSE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM",
        "-DVOIP_REGISTRATION_CAPACITY=4", "-I", str(tmp_path), "-I", str(source),
        str(cpp), str(source / "sip_registration.cpp"), "-o", str(binary),
    ], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    ran = subprocess.run([binary], capture_output=True, text=True, timeout=10, preexec_fn=no_core)
    assert ran.returncode == 0, ran.stderr
