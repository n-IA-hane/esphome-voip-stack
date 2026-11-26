"""Compile the actual Native adapter with the real neutral frame interface."""
import os
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("timeline", ["normal", "backwards", "stalled", "wrap"])
def test_p4_jpeg_admission_is_identical_with_and_without_the_voip_adapter(tmp_path, timeline):
    renderer = Path(os.environ.get("VIDEO_RENDERER_SOURCE", ROOT.parent / "esphome-video-renderer"))
    source = (renderer / "esphome/components/p4_video_renderer/p4_video_renderer.cpp").read_text()
    start = source.index("bool P4VideoRenderer::submit_frame(")
    submit = source[start : source.index("\n}", start) + 2]
    core = tmp_path / "esphome/core"
    core.mkdir(parents=True)
    (core / "defines.h").write_text("#pragma once\n")
    cpp = tmp_path / "p4_adapter.cpp"
    cpp.write_text(r'''
#define USE_ESPHOME_VOIP_STACK_VIDEO
#define USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
#define USE_ESPHOME_VOIP_STACK_VIDEO_RENDERER
#define USE_P4_VIDEO_RENDERER_JPEG
#include <atomic>
#include <cassert>
#include <cstring>
#include <string>
#include "esphome/components/voip_stack/video_renderer_adapter.h"
using namespace esphome;
uint32_t clock_us=0;uint32_t micros(){return clock_us;}void xTaskNotifyGive(void*){}
class P4VideoRenderer : public video_renderer::EncodedFrameRenderer {
 public:
  std::atomic<bool>rx_running_{true},rx_active_{true};std::atomic<int>pending_surface_{-1};
  std::atomic<uint8_t>rx_slot_state_{0};video_renderer::FrameCadence cadence_;
  uint8_t storage[32]{};uint8_t*rx_au_=storage;size_t rx_au_capacity_=sizeof(storage),rx_au_size_=0;
  uint64_t rx_timestamp_us_=0;uint32_t rx_au_generation_=0;bool rx_key_frame_=false;
  video_renderer::JpegFrameInfo rx_au_jpeg_info_{};
  std::atomic<uint32_t>rx_session_generation_{1},rx_admitted_frames_{0};void*rx_task_handle_=nullptr;
  video_renderer::RendererCapabilities get_capabilities()const override{return {{video_renderer::Codec::JPEG,320,240,5},320,240,32,false};}
  bool prepare(const video_renderer::StreamFormat&f)override{return f.valid();}
  bool start()override{return true;}bool set_active(bool active)override{rx_active_=active;if(active)cadence_.reset(5);return true;}
  void stop()override{rx_active_=false;}bool submit_frame(const video_renderer::EncodedFrame&)override;
  void consume_slot(){rx_slot_state_.store(0);}
};
@SUBMIT@
int main(int argc,char**argv){
 assert(argc==2);const std::string timeline=argv[1];uint8_t payload[4]{0xff,0xd8,0xff,0xd9};
 P4VideoRenderer direct,wrapped;voip_stack::P4RendererVideoSink sink(&wrapped);
 auto format=sink.get_receive_video_capability();assert(sink.start_video(format)&&sink.set_video_active(true));
 assert(direct.set_active(true));unsigned direct_count=0,wrapped_count=0,direct_release=0,wrapped_release=0;
 video_renderer::EncodedFrame frame{};frame.data=payload;frame.size=4;frame.key_frame=true;
 voip_stack::EncodedVideoAccessUnit unit{};unit.data=payload;unit.size=4;unit.key_frame=true;
 frame.release_callback=[](void*ctx){++*static_cast<unsigned*>(ctx);};frame.release_ctx=&direct_release;
 unit.release_callback=frame.release_callback;unit.release_ctx=&wrapped_release;
 for(unsigned i=0;i<=20;++i){
  clock_us=i*50000;frame.presentation_time={i*50000ULL,true};
  unit.timestamp_90khz=timeline=="backwards"?90000-i*4500:timeline=="stalled"?0:timeline=="wrap"?UINT32_MAX-1000+i*4500:i*4500;
  const bool a=direct.submit_frame(frame),b=sink.consume_video_access_unit(unit);
  assert(a==b);direct_count+=a;wrapped_count+=b;direct.consume_slot();wrapped.consume_slot();
 }
 assert(direct_count==6&&wrapped_count==6&&direct_release==6&&wrapped_release==6);
}
'''.replace("@SUBMIT@", submit))
    binary = tmp_path / "p4_adapter"
    build = subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-fno-pie", "-no-pie", "-I", str(tmp_path), "-I", str(ROOT), "-I", str(renderer),
        str(cpp), "-o", str(binary),
    ], capture_output=True, text=True)
    assert build.returncode == 0, build.stdout + build.stderr
    run = subprocess.run([str(binary), timeline], capture_output=True, text=True, timeout=8)
    assert run.returncode == 0, run.stdout + run.stderr


def test_adapter_false_preserves_lease_and_clock_wrap_is_explicit(tmp_path):
    renderer = Path(os.environ.get("VIDEO_RENDERER_SOURCE", ROOT.parent / "esphome-video-renderer"))
    assert (renderer / "esphome/components/video_renderer/video_renderer.h").is_file()
    core = tmp_path / "esphome/core"
    core.mkdir(parents=True)
    (core / "defines.h").write_text("#pragma once\n")
    source = tmp_path / "adapter.cpp"
    source.write_text(r'''
#define USE_ESPHOME_VOIP_STACK_VIDEO
#define USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
#define USE_ESPHOME_VOIP_STACK_VIDEO_RENDERER
#include <cassert>
#include "esphome/components/voip_stack/video_renderer_adapter.h"
using namespace esphome;
class Renderer final : public video_renderer::EncodedFrameRenderer {
 public:
  bool accepting=false, active=false;
  uint16_t decode_width=320,decode_height=240;
  video_renderer::Codec codec=video_renderer::Codec::JPEG;
  video_renderer::StreamFormat prepared{};
  video_renderer::RendererCapabilities get_capabilities() const override {
    return {{codec,320,240,2},decode_width,decode_height,65536,false};
  }
  bool prepare(const video_renderer::StreamFormat &format) override { prepared=format;return format.valid(); }
  bool start() override { active=true; return true; }
  bool set_active(bool value) override { active=value; return true; }
  void stop() override { active=false; }
  bool submit_frame(const video_renderer::EncodedFrame &frame) override {
    if(!accepting)return false;
    frame.release();return true;
  }
};
int main(){
 Renderer renderer;voip_stack::S3RendererVideoSink sink(&renderer);
 auto caps=sink.get_receive_video_capability();assert(caps.is_jpeg()&&caps.width==320&&caps.height==240);
 assert(sink.start_video(caps));assert(sink.set_video_active(true));
 uint8_t data[]={0xff,0xd8,0xff,0xd9};unsigned releases=0;
 voip_stack::EncodedVideoAccessUnit frame{};frame.data=data;frame.size=sizeof(data);
 frame.release_callback=[](void*ctx){++*static_cast<unsigned*>(ctx);};frame.release_ctx=&releases;
 assert(!sink.consume_video_access_unit(frame));assert(releases==0);
 renderer.accepting=true;assert(sink.consume_video_access_unit(frame));assert(releases==1);
 sink.stop_video();assert(!renderer.active);
 renderer.decode_width=800;renderer.decode_height=800;
 caps=sink.get_receive_video_capability();assert(caps.is_jpeg()&&caps.width==800&&caps.height==800);
 assert(renderer.get_capabilities().preferred.width==320&&renderer.get_capabilities().preferred.height==240);
 assert(sink.start_video(caps)&&renderer.prepared.width==800&&renderer.prepared.height==800);
 caps.width=808;assert(!sink.start_video(caps));
 voip_stack::P4RendererVideoSink p4(&renderer);
 auto p4_caps=p4.get_receive_video_capability();assert(p4_caps.width==320&&p4_caps.height==240);
 voip_stack::RendererFrameClock clock;
 auto zero=clock.translate(0);assert(zero.valid&&zero.microseconds==0);
 clock.reset();assert(clock.translate(0xfffffff0U).valid);
 auto wrapped=clock.translate(0x10U);assert(wrapped.valid&&wrapped.microseconds==355);
 auto backwards=clock.translate(0);assert(!backwards.valid);
 auto recovered=clock.translate(9000);assert(recovered.valid&&recovered.microseconds>wrapped.microseconds);
}
''')
    binary=tmp_path/"adapter"
    build=subprocess.run(["g++","-std=c++17","-Wall","-Wextra","-Werror","-I",str(tmp_path),
        "-I",str(ROOT),"-I",str(renderer),str(source),"-o",str(binary)],capture_output=True,text=True)
    assert build.returncode==0,build.stdout+build.stderr
    subprocess.run([str(binary)],check=True,capture_output=True,text=True)


def test_h264_receive_capability_keeps_preferred_geometry(tmp_path):
    from test_refer_dialog_runtime import block

    renderer = Path(os.environ.get("VIDEO_RENDERER_SOURCE", ROOT.parent / "esphome-video-renderer"))
    rtp = (ROOT / "esphome/components/voip_stack/video_rtp.cpp").read_text()
    helpers = "\nnamespace esphome::voip_stack {\n" + "\n".join(block(rtp, signature) for signature in (
        "uint8_t hex_nibble(", "bool parse_profile_level_id(", "int h264_level_value(",
        "bool h264_same_subprofile(", "bool h264_level_fits(")) + "\n}\n"
    core = tmp_path / "esphome/core"
    core.mkdir(parents=True)
    (core / "defines.h").write_text("#pragma once\n")
    source = tmp_path / "h264.cpp"
    source.write_text(r'''
#define USE_ESPHOME_VOIP_STACK_VIDEO
#define USE_ESPHOME_VOIP_STACK_VIDEO_H264
#define USE_ESPHOME_VOIP_STACK_VIDEO_RENDERER
#include <cassert>
#include "esphome/components/voip_stack/video_renderer_adapter.h"
@H264_HELPERS@
using namespace esphome;
class Renderer final : public video_renderer::EncodedFrameRenderer {
 public:
  video_renderer::RendererCapabilities get_capabilities() const override {
    return {{video_renderer::Codec::H264,320,240,2},800,800,65536,false};
  }
  bool prepare(const video_renderer::StreamFormat &f) override {return f.valid();}
  bool start() override {return true;}
  bool set_active(bool) override {return true;}
  void stop() override {}
  bool submit_frame(const video_renderer::EncodedFrame &) override {return false;}
};
int main(){
 Renderer renderer;voip_stack::S3RendererVideoSink s3(&renderer);voip_stack::P4RendererVideoSink p4(&renderer);
 auto h264=s3.get_receive_video_capability(),other=p4.get_receive_video_capability();
 assert(h264.is_h264()&&h264.width==320&&h264.height==240);
 assert(h264.profile_level_id==other.profile_level_id&&other.width==320&&other.height==240);
 assert(s3.start_video(h264));h264.width=800;assert(!s3.start_video(h264));
}
'''.replace("@H264_HELPERS@", helpers))
    binary = tmp_path / "h264"
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-I", str(tmp_path), "-I", str(ROOT), "-I", str(renderer), str(source), "-o", str(binary)],
        capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    subprocess.run([str(binary)], check=True, capture_output=True, text=True)
