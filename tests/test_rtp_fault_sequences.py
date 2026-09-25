"""Production jitter buffer with loss, reordering, malformed frames and retry."""

from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_rtp_fault_sequences_preserve_samples_and_recover(tmp_path):
    helper = tmp_path / "esphome/core/helpers.h"
    helper.parent.mkdir(parents=True)
    helper.write_text(
        "namespace esphome { struct Mutex {}; struct LockGuard { explicit LockGuard(Mutex &) {} }; }\n"
    )
    cpp = tmp_path / "rtp.cpp"
    cpp.write_text(r"""
#include <cassert>
#include <initializer_list>
#include "esphome/components/voip_stack/rtp_jitter_buffer.h"
using B=esphome::voip_stack::RtpJitterBuffer;
int main(){
 for(unsigned base:{100U,65534U}){
  uint8_t storage[8*4]{},payload[4]{1,2,3,4},out[4]{};B b(storage,4,8,1);
  auto push=[&](unsigned seq,uint32_t stamp){return b.push({payload,4,static_cast<uint16_t>(seq),stamp,true,false});};
  auto read=[&](unsigned seq,uint32_t stamp){uint16_t got_seq=0;uint32_t got_stamp=0;assert(b.read(out,4,&got_seq,&got_stamp)==B::ReadResult::FRAME);assert(got_seq==uint16_t(seq)&&got_stamp==stamp);assert(!memcmp(out,payload,4));};
  assert(push(base,0xfffffff0U));assert(!push(base,0xfffffff0U));assert(b.counters().duplicates==1);
  assert(push(base+2,0x10));assert(push(base+1,0));
  read(base,0xfffffff0U);read(base+1,0);read(base+2,0x10);
  assert(!push(base,0xfffffff0U));assert(b.counters().late==1);
  assert(push(base+4,0x30));assert(b.read(out,4)==B::ReadResult::MISSING);assert(b.counters().missing==1);read(base+4,0x30);
  auto bad=B::Frame{nullptr,4,uint16_t(base+5),0,true,false};assert(!b.push(bad));
  bad.pcm=payload;bad.bytes=5;assert(!b.push(bad));
  for(unsigned i=5;i<30;i++)push(base+i,i*16);
  assert(b.counters().depth<=8);assert(b.counters().drops>0);
  b.reset();auto c=b.counters();assert(!c.depth&&!c.drops&&!c.late&&!c.missing&&!c.duplicates);
  assert(push(500,8000));read(500,8000);assert(b.counters().depth==0);
 }
}
""")
    exe = tmp_path / "rtp"
    subprocess.run(
        [
            "g++",
            "-std=c++17",
            "-DUSE_ESP32",
            "-fsanitize=address,undefined",
            "-I",
            str(tmp_path),
            "-I",
            str(ROOT),
            str(cpp),
            "-o",
            str(exe),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    subprocess.run([str(exe)], check=True, capture_output=True, text=True)
