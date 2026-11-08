"""Rotate the real jitter blocks and retain the RX loan across call revisions."""

from pathlib import Path
import subprocess

import pytest

from test_rx_playout_owner import owner_source

ROOT = Path(__file__).resolve().parents[1]


def build_exchange(folder, source):
    folder.mkdir(parents=True, exist_ok=True)
    helper = folder / "esphome/core/helpers.h"
    helper.parent.mkdir(parents=True)
    helper.write_text(
        "#pragma once\n#include <mutex>\n"
        "namespace esphome {using Mutex=std::mutex;struct LockGuard {"
        "std::lock_guard<Mutex> guard;explicit LockGuard(Mutex&m):guard(m){}};}\n"
    )
    binary = folder / "exchange"
    result = subprocess.run(
        ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g", "-pthread",
         "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_ESP32",
         "-DUSE_ESPHOME_VOIP_STACK_SPEAKER", "-I", str(folder), "-I", str(ROOT),
         str(source), "-o", str(binary)],
        capture_output=True, text=True, timeout=60,
    )
    assert result.returncode == 0, result.stderr
    return binary


def test_jitter_rotation_reset_metadata_fallback_and_no_allocations(tmp_path):
    binary = build_exchange(tmp_path / "jitter", ROOT / "tests/test_jitter_exchange.cpp")
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr


@pytest.fixture(scope="module")
def rx_exchange_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("rx-jitter-exchange")
    # Keep the production RX methods, revision owner and actual jitter. Only
    # the speaker adapter gains a sample observer for the bounded task run.
    source = owner_source().split("using Phase=VoipStack::RxPlayoutPhase;", 1)[0]
    source = source.replace(
        "std::function<void()>on_start;",
        "std::function<void()>on_start;std::function<void(const uint8_t*,size_t)>on_play;",
    ).replace(
        "size_t play(const uint8_t*,size_t bytes,uint32_t){",
        "size_t play(const uint8_t*data,size_t bytes,uint32_t){",
    ).replace("accepted+=bytes;return bytes;", "if(on_play)on_play(data,bytes);accepted+=bytes;return bytes;")
    source = "#include <array>\n" + source + r'''
class InspectableJitter:public RtpJitterBuffer {
 public:
 using RtpJitterBuffer::RtpJitterBuffer;
 void check_outside(const uint8_t*outside){
  for(unsigned i=0;i<slot_count_;++i){
   assert(outside!=slots_[i].pcm);
   for(unsigned j=0;j<i;++j)assert(slots_[i].pcm!=slots_[j].pcm);
  }
 }
};
int main(int argc,char**argv){
 assert(argc==2);const bool fallback=std::string(argv[1])=="fallback";
 const bool word_aligned=std::string(argv[1])=="word-aligned";
 const size_t capacity=fallback?1275:320;
 alignas(16) std::array<uint8_t,16*1275+16>storage{};
 alignas(16) std::array<uint8_t,1275+16>network{};
 alignas(16) std::array<uint8_t,320>pcm{};
 std::array<uint8_t,320>packet{};
 Speaker sink;sink.state=Speaker::RUNNING;VoipStack phone;
 uint8_t*const storage_base=storage.data()+(word_aligned?4:0);
 uint8_t*const network_base=network.data()+(word_aligned?12:0);
 InspectableJitter jitter(storage_base,capacity,16,1);
 assert(jitter.has_exchange_alignment(network_base)==!fallback);
 phone.speaker_=&sink;phone.rx_jitter_buffer_=&jitter;
 phone.rx_network_chunk_=network_base;phone.rx_jitter_frame_alloc_bytes_=capacity;
 phone.rx_audio_chunk_=pcm.data();phone.rx_audio_chunk_alloc_bytes_=pcm.size();
 phone.format={16000,PcmFormat::S16LE,1,10,AudioCodec::PCM};
 phone.reset_rx_audio_();phone.rx_playout_state_=(phone.rx_audio_revision_<<3U)|4U;
 phone.call_state_=CallState::IN_CALL;yield_on_wait=true;
 unsigned produced=0,expected=0,played=0,borrowed=0;
 auto enqueue=[&](){
  for(unsigned i=0;i<packet.size();++i)packet[i]=uint8_t(produced*17+i);
  phone.on_audio_received_({packet.data(),packet.size(),uint16_t(produced),produced*160,
                            true,false,pack_audio_format(phone.format)});
  ++produced;
 };
 sink.on_play=[&](const uint8_t*data,size_t bytes){
  assert(bytes==packet.size());
  for(unsigned i=0;i<bytes;++i)assert(data[i]==uint8_t(expected*17+i));
  jitter.check_outside(data);
  if(data!=network_base)++borrowed;
  ++expected;
  if(++played==40)throw Paused{};
  if(played%7==0){
   // Discard a pending burst and change call revision without restarting the
   // task. Its currently borrowed slot must stay outside the reset pool.
   phone.reset_rx_audio_(true);expected=produced;
   for(unsigned n=0;n<4;++n)enqueue();
  }else enqueue();
 };
 for(unsigned n=0;n<4;++n)enqueue();
 try{phone.rx_task_();assert(false);}catch(const Paused&){}
 assert(played==40&&phone.rx_audio_revision_==6);
 assert(phone.rx_network_chunk_==network_base);
 assert(fallback?borrowed==0:borrowed>0);
 const auto counters=jitter.counters();
 assert(!counters.drops&&!counters.missing&&!counters.duplicates&&!counters.late);
}
'''
    cpp = folder / "rx.cpp"
    cpp.write_text(source)
    return build_exchange(folder / "build", cpp)


@pytest.mark.parametrize("mode", ["exchange", "word-aligned", "fallback"])
def test_real_rx_retains_exchange_pointer_across_pending_bursts_and_revisions(rx_exchange_binary, mode):
    result = subprocess.run([str(rx_exchange_binary), mode], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
