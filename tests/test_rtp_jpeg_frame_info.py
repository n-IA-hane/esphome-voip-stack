"""Verify complete RTP/JPEG geometry reaches the sink with the same bitstream."""

from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def test_complete_rtp_jpeg_frame_info_and_legacy_producers(tmp_path):
    component = ROOT / "esphome/components/voip_stack"
    source = (component / "video_rtp.cpp").read_text()
    start = source.index("bool VideoRtpSession::handle_jpeg_payload_(")
    handler = source[start:source.index("\n}", start) + 2]
    stub = tmp_path / "esphome/core"
    stub.mkdir(parents=True)
    (stub / "defines.h").write_text("#pragma once\n")
    code = r'''
#include "esphome/components/voip_stack/rtp_jpeg.h"
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <vector>
using namespace esphome::voip_stack;

struct Receipt {
 JpegFrameInfo info;
 uint32_t timestamp;
 std::vector<uint8_t>jpeg;
};
struct CaptureSink:EncodedVideoSink {
 std::vector<Receipt>frames;
 VideoCapability get_receive_video_capability()const override{return {};}
 bool start_video(const VideoCapability&)override{return true;}
 bool set_video_active(bool)override{return true;}
 void stop_video()override{}
 bool consume_video_access_unit(const EncodedVideoAccessUnit&au)override{
  assert(au.key_frame&&au.release_callback==nullptr&&au.release_ctx==nullptr);
  frames.push_back({au.jpeg_info,au.timestamp_90khz,{au.data,au.data+au.size}});
  return true;
 }
};
struct VideoRtpSession {
 CaptureSink sink;EncodedVideoSink*sink_=&sink;
 RtpJpegDepacketizer jpeg_depacketizer_;
 std::array<uint8_t,4096>storage{};
 uint8_t*reassembly_=storage.data();size_t receive_access_unit_capacity_=storage.size();
 bool reassembly_damaged_=false;
 std::atomic<uint32_t>dropped_access_units_{0},rx_access_units_ok_{0};
 bool ensure_jpeg_quantization_cache_(){assert(false);return false;}
 bool handle_jpeg_payload_(const uint8_t*,size_t,bool,uint32_t);
 bool push(const std::vector<uint8_t>&packet,bool marker,uint32_t timestamp){return handle_jpeg_payload_(packet.data(),packet.size(),marker,timestamp);}
};
HANDLER

// Construct independent RFC 2435 wire headers: dimensions in blocks of eight,
// quality 50 uses standard derived quantizers, restart variants append DRI=7.
std::vector<uint8_t>packet(uint8_t type,uint16_t width,uint16_t height,uint32_t offset=0){
 assert(width%8==0&&height%8==0&&width/8<=255&&height/8<=255);
 std::vector<uint8_t>p{0,uint8_t(offset>>16),uint8_t(offset>>8),uint8_t(offset),type,50,uint8_t(width/8),uint8_t(height/8)};
 if(type&64)p.insert(p.end(),{0,7,0xc0,0});
 p.push_back(offset?0x33:0x11);p.push_back(offset?0x44:0x22);return p;
}
void verify(const uint8_t*jpeg,size_t size,const JpegFrameInfo&info,uint8_t type,uint16_t width,uint16_t height){
 assert(info.width==width&&info.height==height);
 assert(info.sampling==((type&1)?JpegSampling::YUV420:JpegSampling::YUV422));
 RtpJpegFrameView parsed;assert(parse_jpeg_for_rtp(jpeg,size,&parsed));
 assert(parsed.width==width&&parsed.height==height&&parsed.type==(type&1));
 assert(parsed.restart_interval==((type&64)?7:0));
 assert(jpeg[0]==0xff&&jpeg[1]==0xd8&&jpeg[size-2]==0xff&&jpeg[size-1]==0xd9);
 // Inspect SOF0 independently of the production parser and returned struct.
 bool found=false;
 for(size_t i=0;i+18<size;++i){
  if(jpeg[i]!=0xff||jpeg[i+1]!=0xc0)continue;
  assert(jpeg[i+2]==0&&jpeg[i+3]==17&&jpeg[i+4]==8);
  assert((unsigned(jpeg[i+5])*256+jpeg[i+6])==height);
  assert((unsigned(jpeg[i+7])*256+jpeg[i+8])==width);
  assert(jpeg[i+9]==3&&jpeg[i+10]==1&&jpeg[i+11]==((type&1)?0x22:0x21));
  found=true;break;
 }
 assert(found);
}
void released(void*ctx){++*static_cast<unsigned*>(ctx);}
int main(){
 RtpJpegDepacketizer depacketizer;std::array<uint8_t,4096>output{};
 JpegFrameInfo info{};size_t bytes=0;uint32_t timestamp=90000;
 VideoRtpSession session;
 const std::array<std::array<uint16_t,2>,3>sizes{{{{320,240}},{{640,480}},{{480,640}}}};
 for(uint8_t type:{0,1,64,65})for(auto size:sizes){
  auto first=packet(type,size[0],size[1]);auto last=packet(type,size[0],size[1],2);
  info={800,800,JpegSampling::GRAY};bytes=999;
  assert(depacketizer.push(first.data(),first.size(),false,timestamp,output.data(),output.size(),&bytes,&info)==RtpJpegPushResult::INCOMPLETE);
  assert(!info.is_set()&&bytes==0);
  assert(depacketizer.push(last.data(),last.size(),true,timestamp,output.data(),output.size(),&bytes,&info)==RtpJpegPushResult::COMPLETE);
  verify(output.data(),bytes,info,type,size[0],size[1]);
  const auto before=session.sink.frames.size();
  assert(session.push(first,false,timestamp)&&session.sink.frames.size()==before);
  assert(session.push(last,true,timestamp)&&session.sink.frames.size()==before+1);
  const auto&frame=session.sink.frames.back();assert(frame.timestamp==timestamp);
  verify(frame.jpeg.data(),frame.jpeg.size(),frame.info,type,size[0],size[1]);
  // COMPLETE resets assembly internally. A new incomplete packet must not
  // expose the previous completion's metadata to its caller or to the sink.
  timestamp+=9000;
 }
 assert(session.rx_access_units_ok_.load()==12&&session.dropped_access_units_.load()==0);
 // Every intra-frame mismatch is rejected, including restart interval. The
 // output hint is empty even when the caller supplied stale metadata.
 for(unsigned mismatch=0;mismatch<7;++mismatch){
  auto first=packet(65,640,480);auto last=packet(65,640,480,2);
  uint32_t last_timestamp=timestamp;
  switch(mismatch){case 0:++last[6];break;case 1:++last[7];break;case 2:last[4]=64;break;case 3:++last[5];break;case 4:last[3]=3;break;case 5:++last[9];break;case 6:++last_timestamp;break;}
  assert(depacketizer.push(first.data(),first.size(),false,timestamp,output.data(),output.size(),&bytes,&info)==RtpJpegPushResult::INCOMPLETE);
  info={640,480,JpegSampling::YUV420};bytes=999;
  assert(depacketizer.push(last.data(),last.size(),true,last_timestamp,output.data(),output.size(),&bytes,&info)==RtpJpegPushResult::DROPPED);
  assert(!info.is_set()&&bytes==0);
  const auto before=session.sink.frames.size();const auto dropped=session.dropped_access_units_.load();
  assert(session.push(first,false,timestamp));assert(!session.push(last,true,last_timestamp));
  assert(session.sink.frames.size()==before&&session.dropped_access_units_.load()==dropped+1);
  timestamp+=9000;
 }
 info={640,480,JpegSampling::YUV420};bytes=999;
 assert(depacketizer.push(nullptr,0,true,timestamp,output.data(),output.size(),&bytes,&info)==RtpJpegPushResult::DROPPED);
 assert(!info.is_set()&&bytes==0);
 // A damaged assembled AU is not delivered, even if its header is complete.
 auto first=packet(0,320,240);auto last=packet(0,320,240,2);
 const auto before=session.sink.frames.size();assert(session.push(first,false,timestamp));
 session.reassembly_damaged_=true;assert(session.push(last,true,timestamp));assert(session.sink.frames.size()==before);
 timestamp+=9000;assert(session.push(first,true,timestamp));assert(session.sink.frames.size()==before+1);
 verify(session.sink.frames.back().jpeg.data(),session.sink.frames.back().jpeg.size(),session.sink.frames.back().info,0,320,240);
 // Existing push callers can omit the new output argument; both legacy AU
 // aggregate forms leave geometry absent and preserve release semantics.
 assert(depacketizer.push(first.data(),first.size(),true,timestamp,output.data(),output.size(),&bytes)==RtpJpegPushResult::COMPLETE);
 EncodedVideoAccessUnit borrowed{output.data(),bytes,timestamp,true};
 unsigned releases=0;EncodedVideoAccessUnit leased{output.data(),bytes,timestamp,true,released,&releases};
 assert(!borrowed.jpeg_info.is_set()&&!leased.jpeg_info.is_set());borrowed.release();leased.release();assert(releases==1);
}
'''.replace("HANDLER", handler)
    cpp = tmp_path / "rtp_jpeg_frame_info.cpp"
    cpp.write_text(code)
    binary = tmp_path / "rtp_jpeg_frame_info"
    result = subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
        "-DUSE_ESPHOME_VOIP_STACK_VIDEO", "-DUSE_ESPHOME_VOIP_STACK_VIDEO_JPEG",
        "-I", str(tmp_path), "-I", str(ROOT), str(cpp),
        str(component / "rtp_jpeg.cpp"), "-o", str(binary),
    ], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result.stderr


def test_h264_access_unit_keeps_legacy_producer_contract(tmp_path):
    stub = tmp_path / "esphome/core"
    stub.mkdir(parents=True)
    (stub / "defines.h").write_text("#pragma once\n")
    cpp = tmp_path / "h264_access_unit.cpp"
    cpp.write_text(r'''
#include "esphome/components/voip_stack/video.h"
#include <cassert>
#include <type_traits>
using esphome::voip_stack::EncodedVideoAccessUnit;
template<class T,class=void>struct HasJpegInfo:std::false_type{};
template<class T>struct HasJpegInfo<T,std::void_t<decltype(T{}.jpeg_info)>>:std::true_type{};
static_assert(!HasJpegInfo<EncodedVideoAccessUnit>::value);
void released(void*ctx){++*static_cast<unsigned*>(ctx);}
int main(){
 uint8_t frame[]{0,0,1,0x65};unsigned releases=0;
 EncodedVideoAccessUnit borrowed{frame,sizeof(frame),90000,true};
 EncodedVideoAccessUnit leased{frame,sizeof(frame),90000,true,released,&releases};
 borrowed.release();leased.release();assert(releases==1&&borrowed.data==frame&&leased.size==sizeof(frame));
}
''')
    binary = tmp_path / "h264_access_unit"
    result = subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
        "-DUSE_ESPHOME_VOIP_STACK_VIDEO", "-DUSE_ESPHOME_VOIP_STACK_VIDEO_H264",
        "-I", str(tmp_path), "-I", str(ROOT), str(cpp), "-o", str(binary),
    ], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result.stderr
