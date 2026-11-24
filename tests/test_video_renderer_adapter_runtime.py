"""Compile the actual Native adapter with the real neutral frame interface."""
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


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
