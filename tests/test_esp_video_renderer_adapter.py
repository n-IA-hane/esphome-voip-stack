"""The optional adapter preserves the production video sink lease contract."""

from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_jpeg_adapter_releases_only_accepted_leases(tmp_path):
    defines = tmp_path / "esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("#pragma once\n#define USE_ESPHOME_VOIP_STACK_VIDEO\n#define USE_ESPHOME_VOIP_STACK_VIDEO_JPEG\n")
    source = tmp_path / "adapter.cpp"
    source.write_text(r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#define USE_ESP_VIDEO_RENDERER_VOIP
namespace esphome::esp_video_renderer {
struct EspVideoRenderer {
 bool accepting=true,prepared=false,active=false;unsigned submissions=0;
 unsigned get_width()const{return 320;}unsigned get_height()const{return 240;}unsigned get_framerate()const{return 2;}
 bool prepare(unsigned fps){assert(fps==2);prepared=true;return true;}
 bool set_active(bool enabled){active=enabled;return true;}
 void stop(){active=prepared=false;}
 bool submit_jpeg(const uint8_t*,size_t){submissions++;return accepting;}
};}
#include "esphome/components/esp_video_renderer/voip_adapter.h"
int main(){
 using namespace esphome;esp_video_renderer::EspVideoRenderer renderer;
 esp_video_renderer::EspVideoRendererSink sink(&renderer);
 auto capability=sink.get_receive_video_capability();assert(capability.valid());assert(capability.is_jpeg());
 assert(sink.start_video(capability)&&renderer.prepared);assert(sink.set_video_active(true)&&renderer.active);
 unsigned releases=0;uint8_t jpeg[]={0xff,0xd8,0xff,0xd9};
 voip_stack::EncodedVideoAccessUnit leased{jpeg,sizeof jpeg,0,true,[](void*ctx){++*static_cast<unsigned*>(ctx);},&releases};
 assert(sink.consume_video_access_unit(leased));assert(releases==1);
 renderer.accepting=false;assert(!sink.consume_video_access_unit(leased));assert(releases==1);
 // Rejected leases remain with their caller, as in VideoRtpSession.
 leased.release();assert(releases==2);
 capability.width=640;assert(!sink.start_video(capability));
 sink.stop_video();assert(!renderer.active&&!renderer.prepared);
}
''')
    binary = tmp_path / "adapter"
    subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-I", str(tmp_path), "-I", str(ROOT), str(source), "-o", str(binary),
    ], check=True, capture_output=True, text=True)
    subprocess.run([str(binary)], check=True, capture_output=True, text=True)
