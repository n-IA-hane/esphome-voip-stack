"""Dialog context exchanges retain message buffers without allocation or copies."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_signaling_moves_reuse_fixed_policy_storage(tmp_path):
    stub = tmp_path / 'esphome/core/helpers.h'
    stub.parent.mkdir(parents=True)
    stub.write_text('''#pragma once
#include <cstdlib>
inline int allocated=0, released=0;
namespace esphome {
template<class T> struct RAMAllocator {
 using value_type=T;
 enum {ALLOC_EXTERNAL=1};
 explicit RAMAllocator(int flags=0):flags_(flags){}
 T* allocate(size_t n){++allocated;return static_cast<T*>(std::malloc(n*sizeof(T)));}
 void deallocate(T*p,size_t){++released;std::free(p);}
 int flags_;
};
}
''')
    cpp = tmp_path / 'move.cpp'
    cpp.write_text('''#include <cassert>
#include <utility>
#include "esphome/components/voip_stack/sip_signaling_string.h"
int main(){
 using esphome::voip_stack::SipSignalingString;
 {
  SipSignalingString a,b;
  a=std::string(2048,'a');b=std::string(2048,'b');
  const char *pa=a.data(),*pb=b.data();const int before=allocated;
  std::swap(a,b);
  assert(a.data()==pb && b.data()==pa && allocated==before);
  SipSignalingString c(std::move(a));assert(c.data()==pb && allocated==before);
  b=std::move(c);assert(b.data()==pb && allocated==before);
  const std::string expected(2048,'b');
  assert(b==expected && expected==b && !(b!=expected) && !(expected!=b));
  assert(b!=std::string("different") && std::string("different")!=b);
  assert((std::string("prefix")+b)==std::string("prefix")+expected);
  assert(("prefix"+b)==std::string("prefix")+expected);
  assert(b.capacity()>=b.size() && allocated==before);
  const std::string_view borrowed=b;
  assert(borrowed.data()==b.data() && borrowed==expected && b==borrowed && borrowed==b && allocated==before);
  const int freed=released;b.clear();assert(b.empty() && released==freed+1);
 }
 assert(allocated==released);
}
''')
    exe = tmp_path / 'move'
    subprocess.run(['g++','-std=c++17','-fsanitize=address,undefined',
                    '-DUSE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM','-I',str(tmp_path),'-I',str(ROOT),str(cpp),'-o',str(exe)],
                   check=True,capture_output=True,text=True)
    subprocess.run([str(exe)],check=True,capture_output=True,text=True)
