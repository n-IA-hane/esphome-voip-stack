"""Watch and optional presence rows have one strict bounded allocation policy."""
import subprocess
import pytest
from test_refer_dialog_runtime import SOURCE


@pytest.fixture(scope="module", params=[(False,False,False),(False,True,True),(True,True,True)], ids=["blf-internal","presence-external","mwi-presence-external"])
def blf_pool_binary(tmp_path_factory, request):
    mwi, presence, external = request.param
    folder = tmp_path_factory.mktemp("blf-pool")
    core = folder / "esphome/core"
    core.mkdir(parents=True)
    (core / "defines.h").write_text("")
    (core / "helpers.h").write_text('''#pragma once
#include <cstdlib>
namespace esphome{template<class T>struct RAMAllocator{using value_type=T;enum{ALLOC_EXTERNAL=1};explicit RAMAllocator(int=0){}T*allocate(size_t n){return static_cast<T*>(std::malloc(n*sizeof(T)));}void deallocate(T*p,size_t){std::free(p);}};}
''')
    (folder / "esp_heap_caps.h").write_text('''#pragma once
#include <cstdlib>
constexpr unsigned MALLOC_CAP_SPIRAM=1,MALLOC_CAP_INTERNAL=2,MALLOC_CAP_8BIT=4;
inline unsigned allocations=0,frees=0,fail_at=0,wrong_heap=0;
inline void*heap_caps_malloc(size_t n,unsigned caps){++allocations;
#ifdef EXPECT_EXTERNAL
if(caps!=(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT))++wrong_heap;
#else
if(caps!=(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT))++wrong_heap;
#endif
return allocations==fail_at?nullptr:std::malloc(n);}
inline void heap_caps_free(void*p){if(p)++frees;std::free(p);}
''')
    code = r'''
#include "sip_mwi.h"
#include <cassert>
#include <cstdio>
using namespace esphome::voip_stack;
struct Inspect:SipMwi {static size_t watch(){return sizeof(BlfWatch);}static size_t transaction(){return sizeof(Account);}};
int main(int argc,char**argv){
 assert(argc==3);const unsigned capacity=std::atoi(argv[1]),failure=std::atoi(argv[2]);unsigned successful=0;
 {Inspect owner;const auto initial=allocations;
#ifdef USE_ESPHOME_VOIP_MWI
 assert(initial==1);
#else
 assert(initial==0&&"MWI-off must not allocate a mailbox row");
#endif
 if(failure)fail_at=allocations+failure;
 const bool configured=owner.configure_blf(capacity);
 const unsigned rows=1
#ifdef USE_ESPHOME_VOIP_PRESENCE
 +1
#endif
 ;
 if(failure<=rows&&failure!=0){assert(!configured&&owner.blf_capacity()==0);}
 else{assert(configured&&owner.blf_capacity()==capacity);assert(allocations==initial+rows);}
 assert(!owner.configure_blf(0)&&!owner.configure_blf(65));assert(wrong_heap==0);
 successful=allocations-(fail_at&&fail_at<=allocations?1:0);
 printf("watch=%zu transaction=%zu capacity=%u presence=%u\n",Inspect::watch(),Inspect::transaction(),capacity,rows-1);
 }
 assert(frees==successful);
}
'''
    cpp = folder / "pool.cpp"
    cpp.write_text(code)
    binary = folder / "pool"
    flags = ["-DUSE_ESPHOME_VOIP_MWI"] if mwi else []
    if presence:
        flags += ["-DUSE_ESPHOME_VOIP_PRESENCE"]
    if external:
        flags += ["-DUSE_ESPHOME_VOIP_BLF_PSRAM", "-DUSE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM", "-DEXPECT_EXTERNAL"]
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g", "-fsanitize=address,undefined",
        "-fno-pie", "-no-pie", "-DUSE_ESP32", "-DUSE_ESPHOME_VOIP_REGISTRATION", "-DUSE_ESPHOME_VOIP_BLF", *flags,
        "-I", str(folder), "-I", str(SOURCE), str(cpp), str(SOURCE / "sip_mwi.cpp"), str(SOURCE / "sip_blf.cpp"),
        str(SOURCE / "sip_registration.cpp"), "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("capacity,failure", [(1,0),(16,0),(64,0),(2,1),(2,2)])
def test_blf_pool_bounds_heap_policy_and_oom(blf_pool_binary, capacity, failure):
    result = subprocess.run([blf_pool_binary,str(capacity),str(failure)], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
    print(result.stdout, end="")
