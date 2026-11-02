"""Subscription rows obey the requested heap, including allocation failure."""
from pathlib import Path
import subprocess
import pytest

SOURCE = Path(__file__).resolve().parents[1] / "esphome/components/voip_stack"


@pytest.mark.parametrize("capacity", [1, 2, 4])
@pytest.mark.parametrize("external", [False, True])
@pytest.mark.parametrize("fail", [False, True])
def test_mwi_pool_uses_one_strict_heap_and_handles_oom(tmp_path, capacity, external, fail):
    core = tmp_path / "esphome/core"
    core.mkdir(parents=True)
    (core / "defines.h").write_text("")
    (core / "helpers.h").write_text('''#pragma once
#include <cstdlib>
namespace esphome {template<class T> struct RAMAllocator {
using value_type=T;enum {ALLOC_EXTERNAL=1};explicit RAMAllocator(int=0){}
T*allocate(size_t n){return static_cast<T*>(std::malloc(n*sizeof(T)));}
void deallocate(T*p,size_t){std::free(p);}
};}
''')
    (tmp_path / "esp_heap_caps.h").write_text('''#pragma once
#include <cstdlib>
constexpr unsigned MALLOC_CAP_SPIRAM=1, MALLOC_CAP_INTERNAL=2, MALLOC_CAP_8BIT=4;
inline unsigned allocations=0,frees=0,observed=0;inline size_t requested=0;
inline void*heap_caps_malloc(size_t n,unsigned caps){++allocations;observed=caps;requested=n;
#ifdef FAIL_POOL
return nullptr;
#else
return std::malloc(n);
#endif
}
inline void heap_caps_free(void*p){++frees;std::free(p);}
''')
    code = '''#include "sip_mwi.h"
#include <cassert>
#include <cstdio>
using namespace esphome::voip_stack;
struct Inspect:SipMwi {static size_t row(){return sizeof(MwiAccount);}};
int main(){
 {Inspect pool;assert(allocations==1);assert(requested>=Inspect::row()*SipRegistration::CAPACITY);
#ifdef EXTERNAL_POOL
 assert(observed==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
#else
 assert(observed==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
#endif
#ifdef FAIL_POOL
 assert(!pool.initialized());assert(pool.account_retired(0));assert(!pool.state(0).available);
 pool.tick(0);uint32_t remaining;assert(!pool.next_deadline(0,&remaining));
 assert(!pool.handle_response("",0,0,false,0,0));assert(!pool.handle_notify("NOTIFY ",0,0,false,0,0));
#else
 assert(pool.initialized());assert(pool.account_retired(0));
#endif
 printf("row=%zu pool=%zu requested=%zu object=%zu\\n",Inspect::row(),Inspect::row()*SipRegistration::CAPACITY,requested,sizeof(Inspect));
 }
#ifdef FAIL_POOL
 assert(frees==0);
#else
 assert(frees==1);
#endif
}
'''
    cpp = tmp_path / "pool.cpp"
    cpp.write_text(code)
    binary = tmp_path / "pool"
    flags = ["-DUSE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM", "-DEXTERNAL_POOL"] if external else []
    if fail:
        flags += ["-DFAIL_POOL"]
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_ESP32",
        "-DUSE_ESPHOME_VOIP_REGISTRATION", "-DUSE_ESPHOME_VOIP_MWI", f"-DVOIP_REGISTRATION_CAPACITY={capacity}",
        *flags, "-I", str(tmp_path), "-I", str(SOURCE), str(cpp), str(SOURCE / "sip_mwi.cpp"),
        str(SOURCE / "sip_registration.cpp"), "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
    print(result.stdout, end="")
