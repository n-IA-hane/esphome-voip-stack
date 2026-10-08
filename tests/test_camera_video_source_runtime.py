"""Compile the production camera adapter against a requester-aware camera."""

from pathlib import Path
import shutil
import subprocess
import os
import sys


ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "esphome/components/voip_stack"


def test_one_shot_requests_survive_callback_clear_and_preserve_other_streams(tmp_path):
    for name in ("camera_video_source.cpp", "camera_video_source.h"):
        shutil.copyfile(COMPONENT / name, tmp_path / name)
    headers = {
        "esphome/core/defines.h": "#define USE_ESPHOME_VOIP_STACK_VIDEO\n#define USE_ESPHOME_VOIP_STACK_VIDEO_JPEG\n#define USE_ESPHOME_VOIP_STACK_VIDEO_CAMERA\n",
        "esphome/core/helpers.h": "#pragma once\n#include <mutex>\nnamespace esphome {using Mutex=std::mutex; using LockGuard=std::lock_guard<Mutex>;}\n",
        "esphome/core/hal.h": "#pragma once\n#include <cstdint>\nextern uint32_t fake_ms; inline uint32_t millis(){return fake_ms;}\n",
        "esphome/core/log.h": "#define ESP_LOGI(...)\n",
        "video.h": """#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
namespace esphome::voip_stack {
struct VideoCapability { uint8_t payload_type{},packetization_mode{},max_fps{}; std::string encoding,profile_level_id;
bool level_asymmetry_allowed{}; uint16_t width{},height{};
bool valid()const{return width&&height&&max_fps;} bool is_jpeg()const{return encoding=="JPEG";} };
struct EncodedVideoAccessUnit {const uint8_t *data; size_t size; uint32_t timestamp; bool keyframe;};
using EncodedVideoAccessUnitCallback=void(*)(void*,const EncodedVideoAccessUnit&);
class EncodedVideoSource {public: virtual ~EncodedVideoSource()=default;
virtual VideoCapability get_video_capability()const=0; virtual bool prepare_video(const VideoCapability&)=0;
virtual bool start_video(EncodedVideoAccessUnitCallback,void*,const VideoCapability&)=0; virtual void stop_video()=0;};}
""",
        "esphome/components/camera/camera.h": """#pragma once
#include <cstdint>
#include <memory>
#include <vector>
namespace esphome::camera {
enum CameraRequester:uint8_t {IDLE,API_REQUESTER,WEB_REQUESTER};
class CameraImage {public: virtual ~CameraImage()=default; virtual uint8_t *get_data_buffer()=0;
virtual size_t get_data_length()=0; virtual bool was_requested_by(CameraRequester)const=0;};
class CameraListener {public: virtual ~CameraListener()=default;
virtual void on_camera_image(const std::shared_ptr<CameraImage>&)=0;};
class Camera {public: virtual ~Camera()=default; virtual void request_image(CameraRequester)=0;
void add_listener(CameraListener *listener){listeners_.push_back(listener);} std::vector<CameraListener*> listeners_;};}
""",
    }
    for name, content in headers.items():
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
    (tmp_path / "main.cpp").write_text(r'''
#include "camera_video_source.h"
#include <cassert>
using namespace esphome;
uint32_t fake_ms=100;
struct Image:camera::CameraImage {
  uint8_t data[4]={0xff,0xd8,0xff,0xd9}; unsigned requesters;
  explicit Image(unsigned bits):requesters(bits){}
  uint8_t *get_data_buffer()override{return data;}
  size_t get_data_length()override{return sizeof(data);}
  bool was_requested_by(camera::CameraRequester r)const override{return requesters&(1U<<r);}
};
struct Camera:camera::Camera {
  unsigned single_requesters_=0,stream_requesters_=0,requests=0; bool in_callback=false;
  void request_image(camera::CameraRequester r)override {
    assert(!in_callback); single_requesters_|=1U<<r; requests++;
  }
  void produce() {
    if (!(single_requesters_|stream_requesters_)) return;
    auto image=std::make_shared<Image>(single_requesters_|stream_requesters_);
    in_callback=true;
    for(auto *listener:listeners_) listener->on_camera_image(image);
    in_callback=false;
    // ESPHome ESP32Camera::loop clears this after notifying all listeners.
    single_requesters_=0;
  }
};
int main() {
  Camera camera; voip_stack::CameraJpegVideoSource source; unsigned frames=0;
  source.set_camera(&camera); source.register_listener(); source.register_listener();
  assert(camera.listeners_.size()==1);
  auto cap=source.get_video_capability(); cap.max_fps=10;
  auto callback=[](void *ctx,const voip_stack::EncodedVideoAccessUnit &frame){
    assert(frame.size==4&&frame.data[0]==0xff); ++*static_cast<unsigned*>(ctx);
  };
  assert(source.prepare_video(cap)); assert(source.start_video(callback,&frames,cap));
  assert(camera.requests==0);  // no camera access from a media worker
  for(unsigned i=0;i<4;i++) {
    source.loop(); assert(camera.single_requesters_&(1U<<camera::WEB_REQUESTER));
    camera.produce(); assert(camera.single_requesters_==0); fake_ms+=100;
  }
  assert(frames==4); assert(camera.requests==4);
  source.loop(); camera.produce(); // throttle: another frame in the same interval
  source.loop(); camera.produce(); assert(frames==5);
  camera.stream_requesters_=(1U<<camera::WEB_REQUESTER)|(1U<<camera::API_REQUESTER);
  source.stop_video(); auto requested=camera.requests;
  source.loop(); camera.produce(); assert(camera.requests==requested); assert(frames==5);
  assert(camera.stream_requesters_==((1U<<camera::WEB_REQUESTER)|(1U<<camera::API_REQUESTER)));
  assert(source.start_video(callback,&frames,cap)); fake_ms+=100;
  source.loop(); camera.produce(); assert(frames==6);
  source.stop_video(); source.stop_video(); source.loop();
}
''')
    binary = tmp_path / "camera"
    subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable",
        "-fsanitize=address,undefined", "-I", str(tmp_path),
        str(tmp_path / "camera_video_source.cpp"), str(tmp_path / "main.cpp"), "-o", str(binary),
    ], check=True, capture_output=True, text=True)
    subprocess.run([str(binary)], check=True, capture_output=True, text=True)


def test_native_esp32_camera_id_generates_the_standard_camera_adapter(tmp_path):
    config = (ROOT / "tests/minimal_no_text.yaml").read_text().replace(
        "../esphome/components", str(ROOT / "esphome/components")
    )
    for old, new in (("GPIO9", "GPIO40"), ("GPIO10", "GPIO41"), ("GPIO11", "GPIO42")):
        config = config.replace(old, new)
    config = config.replace("  id: phone\n", """  id: phone
  video:
    codec: jpeg
    camera_id: camera_native
    width: 640
    height: 480
    framerate: 10
""")
    config += """
psram:
  mode: octal
  speed: 80MHz
i2c:
  id: camera_i2c
  sda: GPIO4
  scl: GPIO5
esp32_camera:
  id: camera_native
  name: Camera
  external_clock:
    pin: GPIO15
    frequency: 20MHz
  i2c_id: camera_i2c
  data_pins: [GPIO11, GPIO9, GPIO8, GPIO10, GPIO12, GPIO18, GPIO17, GPIO16]
  vsync_pin: GPIO6
  href_pin: GPIO7
  pixel_clock_pin: GPIO13
  frame_buffer_location: PSRAM
  resolution: 640x480
  max_framerate: 10 fps
  idle_framerate: 0.1 fps
  frame_buffer_count: 1
  pixel_format: jpeg
"""
    path = tmp_path / "camera.yaml"
    path.write_text(config)
    result = subprocess.run([
        os.environ.get("ESPHOME_PYTHON", sys.executable), "-m", "esphome", "compile", str(path), "--only-generate",
    ], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    generated = next(tmp_path.rglob("main.cpp")).read_text()
    assert "set_video_camera(camera_native, 640, 480, 10)" in generated
