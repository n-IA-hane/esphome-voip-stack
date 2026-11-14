"""Actual TX writer and CapsRingBuffer over ESPHome's real BYTEBUF methods."""

from pathlib import Path
import subprocess

import esphome
import pytest

from test_refer_dialog_runtime import ROOT, SOURCE, block


@pytest.fixture(scope="module")
def tx_pcm_stride(tmp_path_factory):
    folder = tmp_path_factory.mktemp("tx-pcm-stride")
    include = folder / "include"
    files = {
        "esp_heap_caps.h": "#pragma once\n#include <cstdlib>\n#define MALLOC_CAP_8BIT 1\n#define MALLOC_CAP_INTERNAL 2\n#define MALLOC_CAP_SPIRAM 4\ninline void*heap_caps_malloc(size_t n,unsigned){return std::malloc(n);}\ninline void heap_caps_free(void*p){std::free(p);}\n",
        "esp_memory_utils.h": "#pragma once\ninline bool esp_ptr_internal(const void*){return true;}\n",
        "esphome/core/log.h": "#pragma once\n#define ESP_LOGD(...) ((void)0)\n#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n#define ESP_LOGE(...) ((void)0)\n",
        "esphome/core/helpers.h": "#pragma once\n#include <memory>\n#include <cstdlib>\nnamespace esphome {using std::make_unique;template<class T>struct RAMAllocator {enum {NONE=0,PREFER_INTERNAL=1};explicit RAMAllocator(int=0){}T*allocate(size_t n){return static_cast<T*>(std::malloc(n*sizeof(T)));}void deallocate(T*p,size_t){std::free(p);}};}\n",
        "freertos/FreeRTOS.h": "#pragma once\n#include <cstddef>\n#include <cstdint>\nusing TickType_t=unsigned;using UBaseType_t=unsigned;using BaseType_t=int;constexpr int pdPASS=1,pdTRUE=1,pdFALSE=0;\n",
        "freertos/ringbuf.h": r"""
#pragma once
#include "FreeRTOS.h"
#include <algorithm>
#include <cassert>
#include <cstring>
enum RingbufferType_t {RINGBUF_TYPE_BYTEBUF,RINGBUF_TYPE_NOSPLIT};
struct StaticRingbuffer_t {uint8_t*data=nullptr;size_t capacity=0,head=0,tail=0,used=0,held=0;void*item=nullptr;};
using RingbufHandle_t=StaticRingbuffer_t*;
inline RingbufHandle_t xRingbufferCreateStatic(size_t n,RingbufferType_t type,uint8_t*p,StaticRingbuffer_t*s){
 assert(type==RINGBUF_TYPE_BYTEBUF);*s={};s->data=p;s->capacity=n;return s;
}
inline void vRingbufferDelete(RingbufHandle_t){}
inline size_t xRingbufferGetCurFreeSize(RingbufHandle_t s){return s->capacity-s->used;}
inline void vRingbufferGetInfo(RingbufHandle_t s,void*,void*,void*,void*,UBaseType_t*n){*n=s->used-s->held;}
inline bool xRingbufferSend(RingbufHandle_t s,const void*p,size_t n,TickType_t){
 if(n>s->capacity-s->used)return false;
 const size_t first=std::min(n,s->capacity-s->tail);
 std::memcpy(s->data+s->tail,p,first);std::memcpy(s->data,static_cast<const uint8_t*>(p)+first,n-first);
 s->tail=(s->tail+n)%s->capacity;s->used+=n;return true;
}
inline void*xRingbufferReceiveUpTo(RingbufHandle_t s,size_t*n,TickType_t,size_t maximum){
 *n=0;if(s->held||!s->used)return nullptr;
 *n=std::min({s->used,maximum,s->capacity-s->head});
 s->held=*n;s->item=s->data+s->head;s->head=(s->head+*n)%s->capacity;return s->item;
}
inline void*xRingbufferReceive(RingbufHandle_t s,size_t*n,TickType_t t){return xRingbufferReceiveUpTo(s,n,t,s->capacity);}
inline void vRingbufferReturnItem(RingbufHandle_t s,void*p){assert(p==s->item);s->used-=s->held;s->held=0;s->item=nullptr;}
""",
    }
    for name, content in files.items():
        path = include / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
    audio = (SOURCE / "voip_audio.cpp").read_text()
    header = (SOURCE / "voip_stack.h").read_text()
    partial = (block(header, "struct TxPcmFragment {") + ";TxPcmFragment tx_pcm_fragment_;std::atomic<uint32_t>tx_pcm_epoch_{0};"
               if "struct TxPcmFragment {" in header else "")
    reset = ("void reset_mic_buffer_();" if "void VoipStack::reset_mic_buffer_(" in audio
             else "void reset_mic_buffer_(){if(mic_buffer_)mic_buffer_->reset();}")
    code = r"""
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>
#include "esphome/components/voip_stack/sip_types.h"
#include "esphome/components/voip_stack/audio_core_ring_buffer_caps.h"
using namespace esphome;using namespace esphome::voip_stack;
inline void xTaskNotifyGive(void*){}
static uint32_t frames_for_bytes(size_t n,size_t frame){return !n||!frame?0:(n+frame-1)/frame;}
struct VoipStack {
 voip_audio_core::RingBufferPtr mic_buffer_;AudioFormat tx_audio_format_,format_;AudioFormatList tx_audio_formats_{};
 std::atomic<unsigned>media_tx_queue_drops_{0},media_tx_queue_drop_bytes_{0};void*tx_task_handle_=nullptr;
 AudioFormat get_current_tx_audio_format_()const{return format_;}
 size_t tx_audio_chunk_bytes_()const{return format_.nominal_frame_bytes();}
 size_t tx_audio_buffer_bytes_()const;bool write_mic_buffer_(const uint8_t*,size_t);
 @MAX_CHUNK@
 @PARTIAL@
 @RESET@
};
""".replace("@MAX_CHUNK@", block(header, "size_t tx_audio_max_chunk_bytes_() const"))
    code = code.replace("@PARTIAL@", partial).replace("@RESET@", reset)
    for marker in ("size_t VoipStack::tx_audio_buffer_bytes_(", "bool VoipStack::write_mic_buffer_("):
        code += "\n" + block(audio, marker)
    if "void VoipStack::reset_mic_buffer_(" in audio:
        code += "\n" + block(audio, "void VoipStack::reset_mic_buffer_(")
    code += "\n" + block((SOURCE / "sip_message.cpp").read_text(), "size_t pcm_to_rtp_payload(")
    code += r"""
std::vector<uint8_t> pattern(size_t frames,unsigned stride){
 std::vector<uint8_t> data(frames*stride);for(size_t i=0;i<data.size();++i)
  data[i]=stride==3?uint8_t(i%3==0?0x22:i%3==1?0x11:0):uint8_t(17+(i%stride)*37);return data;
}
std::vector<uint8_t> drain(VoipStack&p){
 std::vector<uint8_t> data(p.mic_buffer_->available());assert(p.mic_buffer_->read(data.data(),data.size())==data.size());return data;
}
int main(int argc,char**argv){
 assert(argc==2);std::string mode=argv[1];VoipStack phone;
 AudioFormat format{8000,PcmFormat::S24LE,1,10,AudioCodec::PCM};
 if(mode=="stereo")format.channels=2;
 if(mode=="s16-stereo"){format.channels=2;format.pcm_format=PcmFormat::S16LE;}
 if(mode=="padded24-stereo"){format.channels=2;format.pcm_format=PcmFormat::S24LE_IN_S32;}
 phone.tx_audio_format_=phone.format_=format;phone.tx_audio_formats_.formats[0]=format;
 if(mode=="renegotiated"){
  phone.tx_audio_formats_.count=2;
  phone.tx_audio_formats_.formats[1]={8000,PcmFormat::S16LE,2,10,AudioCodec::PCM};
 }
 if(mode=="configured-shapes"){
  phone.tx_audio_formats_.count=3;
  phone.tx_audio_formats_.formats[1]={16000,PcmFormat::S16LE,2,10,AudioCodec::PCM};
  phone.tx_audio_formats_.formats[2]={8000,PcmFormat::S32LE,2,10,AudioCodec::PCM};
 }
 const size_t stride=format.channels*format.container_bytes_per_sample();
 const size_t capacity=phone.tx_audio_buffer_bytes_();
 phone.mic_buffer_=voip_audio_core::create_internal(capacity,"test PCM");assert(phone.mic_buffer_);
 if(mode=="fragmented"){
  auto pcm=pattern(160,stride);size_t offset=0;
  for(size_t n:{1U,2U,5U,7U,11U,17U}){
   assert(phone.write_mic_buffer_(pcm.data()+offset,n));offset+=n;
   assert(phone.mic_buffer_->available()%stride==0);
  }
  assert(phone.write_mic_buffer_(pcm.data()+offset,pcm.size()-offset));assert(drain(phone)==pcm);
 }else if(mode=="epoch"||mode=="renegotiated"){
  auto old=pattern(1,stride);assert(phone.write_mic_buffer_(old.data(),stride-1));
  if(mode=="epoch")phone.reset_mic_buffer_();
  else phone.format_={8000,PcmFormat::S16LE,2,10,AudioCodec::PCM};
  const auto next=pattern(20,phone.format_.channels*phone.format_.container_bytes_per_sample());
  assert(phone.write_mic_buffer_(next.data(),next.size()));assert(drain(phone)==next);
 }else if(mode=="partial-write"){
  const auto old=pattern(capacity/stride,stride);phone.write_mic_buffer_(old.data(),old.size());
  size_t held=0;auto*item=phone.mic_buffer_->receive_acquire(held,stride*2);assert(item&&held%stride==0);
  auto next=pattern(capacity/stride,stride);phone.write_mic_buffer_(next.data(),next.size());
  phone.mic_buffer_->receive_release(item);auto actual=drain(phone);
  assert(actual.size()%stride==0);assert(actual==pattern(actual.size()/stride,stride));
 }else{
  std::vector<uint8_t> all;
  if(mode=="oversized"){
   all=pattern(capacity*3/stride+5,stride);phone.write_mic_buffer_(all.data(),all.size());
  }else{
   auto callback=pattern(128,stride);
   for(unsigned n=0;n<12;++n){phone.write_mic_buffer_(callback.data(),callback.size());all.insert(all.end(),callback.begin(),callback.end());}
  }
  auto actual=drain(phone);const size_t keep=std::min(all.size(),capacity/stride*stride);
  auto expected=std::vector<uint8_t>(all.end()-keep,all.end());
  if(actual!=expected)std::fprintf(stderr,"unaligned PCM capacity=%zu stride=%zu first=%02x%02x%02x bytes=%zu\n",capacity,stride,actual[0],actual[1],actual[2],actual.size());
  assert(actual==expected);
  if(format.pcm_format!=PcmFormat::S32LE){
   std::vector<uint8_t> wire(actual.size()),network;
   const size_t bytes=format.container_bytes_per_sample(),packed=format.rtp_bytes_per_sample();
   for(size_t offset=0;offset<expected.size();offset+=bytes)
    for(size_t n=packed;n>0;--n)network.push_back(expected[offset+n-1]);
   wire.resize(pcm_to_rtp_payload(actual.data(),actual.size(),format,wire.data(),wire.size()));
   assert(wire==network); // Independent network-order oracle, including packed24 in32-bit containers.
  }
  assert(capacity%stride==0);
  if(mode=="configured-shapes")for(unsigned n=0;n<phone.tx_audio_formats_.count;++n){
   const auto&f=phone.tx_audio_formats_.formats[n];assert(capacity%(f.channels*f.container_bytes_per_sample())==0);
  }
 }
}
"""
    cpp = folder / "tx.cpp"
    cpp.write_text(code)
    upstream = Path(next(iter(esphome.__path__)))
    binary = folder / "tx"
    build = subprocess.run(["g++", "-std=c++17", "-DUSE_ESP32", "-DUSE_ESPHOME_VOIP_STACK_MIC",
        "-DUSE_ESPHOME_VOIP_STACK_AUDIO_DEBUG", "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
        "-I", str(include), "-I", str(ROOT), "-I", str(upstream.parent), str(cpp),
        str(upstream / "components/ring_buffer/ring_buffer.cpp"), "-o", str(binary)], capture_output=True, text=True)
    assert build.returncode == 0, build.stderr
    return binary


@pytest.mark.parametrize("mode", ["mono", "stereo", "s16-stereo", "padded24-stereo", "oversized", "fragmented",
                                  "epoch", "renegotiated", "partial-write", "configured-shapes"])
def test_tx_queue_retains_complete_interleaved_pcm_frames(tx_pcm_stride, mode):
    result = subprocess.run([str(tx_pcm_stride), mode], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr
