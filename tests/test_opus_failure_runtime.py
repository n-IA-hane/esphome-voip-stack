"""Compile the complete production Opus wrapper against faulting codec APIs."""

from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "esphome/components/voip_stack"


def test_opus_allocation_configuration_and_processing_retry(tmp_path):
    core = tmp_path / "esphome/core"
    core.mkdir(parents=True)
    (core / "defines.h").write_text("#pragma once\n")
    (core / "log.h").write_text(
        "#pragma once\n#include <cstdio>\n#define ESP_LOGE(tag,...) do{(void)tag;if(false)std::printf(__VA_ARGS__);}while(false)\n#define ESP_LOGI ESP_LOGE\n#define ESP_LOGW ESP_LOGE\n"
    )
    (tmp_path / "opus.h").write_text(r"""
#pragma once
#include <cstdint>
using opus_int32=int32_t;using opus_int16=int16_t;
struct OpusEncoder{};struct OpusDecoder{};
constexpr int OPUS_OK=0,OPUS_APPLICATION_VOIP=1,OPUS_SIGNAL_VOICE=2,OPUS_RESET_STATE=3;
#define OPUS_SET_BITRATE(x) 4
#define OPUS_SET_COMPLEXITY(x) 5
#define OPUS_SET_SIGNAL(x) 6
OpusEncoder* opus_encoder_create(opus_int32,int,int,int*);
OpusDecoder* opus_decoder_create(opus_int32,int,int*);
void opus_encoder_destroy(OpusEncoder*);void opus_decoder_destroy(OpusDecoder*);
int opus_encoder_ctl(OpusEncoder*,int,...);int opus_decoder_ctl(OpusDecoder*,int,...);
int opus_encode(OpusEncoder*,const opus_int16*,int,unsigned char*,opus_int32);
int opus_decode(OpusDecoder*,const unsigned char*,opus_int32,opus_int16*,int,int);
const char*opus_strerror(int);
""")
    cpp = tmp_path / "test.cpp"
    cpp.write_text(r"""
#include <cassert>
#include <set>
#include "opus_rtp_codec.h"
using namespace esphome::voip_stack;
std::set<void*> live;int at=0,fail=0;bool encode_fail=false,decode_fail=false;
bool fails(){return ++at==fail;}
OpusEncoder*opus_encoder_create(opus_int32,int,int,int*e){if(fails()){*e=-1;return nullptr;}*e=0;auto*p=new OpusEncoder;live.insert(p);return p;}
OpusDecoder*opus_decoder_create(opus_int32,int,int*e){if(fails()){*e=-1;return nullptr;}*e=0;auto*p=new OpusDecoder;live.insert(p);return p;}
void opus_encoder_destroy(OpusEncoder*p){assert(live.erase(p)==1);delete p;}
void opus_decoder_destroy(OpusDecoder*p){assert(live.erase(p)==1);delete p;}
int opus_encoder_ctl(OpusEncoder*,int,...){return fails()?-1:0;}
int opus_decoder_ctl(OpusDecoder*,int,...){return 0;}
const char*opus_strerror(int){return "test";}
int opus_encode(OpusEncoder*,const opus_int16*,int,unsigned char*,opus_int32){return encode_fail?-1:20;}
int opus_decode(OpusDecoder*,const unsigned char*,opus_int32,opus_int16*,int n,int){return decode_fail?-1:n;}
int main(){
 AudioFormat tx,rx;tx.codec=rx.codec=AudioCodec::OPUS;tx.frame_ms=rx.frame_ms=20;
 for(int failure=1;failure<=5;failure++){
  {OpusRtpCodec codec;at=0;fail=failure;assert(!codec.configure(&tx,&rx,1200));assert(live.empty());assert(!codec.ready_for(tx,rx));
   at=0;fail=0;assert(codec.configure(&tx,&rx,1200));assert(codec.ready_for(tx,rx)&&live.size()==2);
   uint8_t pcm[640]{},payload[1200]{};
   encode_fail=true;assert(codec.encode(pcm,sizeof(pcm),tx,payload,sizeof(payload))==0);assert(live.size()==2);
   encode_fail=false;assert(codec.encode(pcm,sizeof(pcm),tx,payload,sizeof(payload))==20);
   decode_fail=true;assert(codec.decode(payload,20,rx,pcm,sizeof(pcm))==0);assert(live.size()==2);
   decode_fail=false;assert(codec.decode(payload,20,rx,pcm,sizeof(pcm))==sizeof(pcm));
   assert(codec.decode(nullptr,20,rx,pcm,sizeof(pcm))==0);assert(codec.decode(payload,1276,rx,pcm,sizeof(pcm))==0);
   assert(codec.decode(payload,20,rx,pcm,1)==0);assert(codec.decode(nullptr,0,rx,pcm,sizeof(pcm))==sizeof(pcm));
   codec.reset();codec.reset_decoder();assert(codec.ready_for(tx,rx));
  }
  assert(live.empty());
 }
}
""")
    exe = tmp_path / "opus"
    subprocess.run(
        [
            "g++",
            "-std=c++17",
            "-DUSE_ESPHOME_VOIP_STACK_OPUS",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-I",
            str(tmp_path),
            "-I",
            str(COMPONENT),
            str(COMPONENT / "opus_rtp_codec.cpp"),
            str(cpp),
            "-o",
            str(exe),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    subprocess.run([str(exe)], check=True, capture_output=True, text=True)
