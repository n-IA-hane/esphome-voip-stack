"""Compile the complete JPEG source TU against the real camera consumer contract."""

from pathlib import Path
import os
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    opening = text.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


@pytest.mark.parametrize("debug", [False, True], ids=["normal", "debug"])
def test_negotiated_budget_and_session_tokens_cross_the_camera_boundary(tmp_path, debug):
    camera = Path(os.environ.get("VIDEO_CAMERA_SOURCE", ROOT.parent / "esphome-esp-video-camera"))
    header = (camera / "components/esp_video_camera/esp_video_camera.h").read_text()
    types = "\n".join(block(header, name) + ";" for name in ("struct JpegFrame", "class JpegFrameConsumer"))
    # Compile both sides of the public signature, including its default arguments.
    marker = "bool start_jpeg_frame_consumer("
    start = header.index(marker)
    signature = header[start : header.index(";", start)]
    for name in ("esp_jpeg_video_source.cpp", "esp_jpeg_video_source.h"):
        shutil.copyfile(ROOT / "esphome/components/esp_jpeg_video_source" / name, tmp_path / name)
    stubs = {
        "esphome/core/defines.h": "#pragma once\n",
        "esphome/core/log.h": "#pragma once\n#define ESP_LOGI(...)\n#define ESP_LOGE(...)\n#define ESP_LOGCONFIG(...)\n",
        "esphome/core/component.h": """#pragma once
namespace esphome {namespace setup_priority {constexpr float DATA=100;}
class Component {public:virtual ~Component()=default;virtual void setup(){}virtual void on_shutdown(){}
virtual void dump_config(){}virtual float get_setup_priority()const{return 0;}bool is_failed()const{return failed_;}
void mark_failed(){failed_=true;}bool failed_=false;};}
""",
        "freertos/FreeRTOS.h": "#pragma once\nconstexpr int portMAX_DELAY=0;\n",
        "freertos/semphr.h": """#pragma once
#include <mutex>
using StaticSemaphore_t=std::mutex;using SemaphoreHandle_t=std::mutex*;
inline auto xSemaphoreCreateMutexStatic(StaticSemaphore_t*p){return p;}
inline void xSemaphoreTake(SemaphoreHandle_t p,int){p->lock();}inline void xSemaphoreGive(SemaphoreHandle_t p){p->unlock();}
""",
        "esphome/components/esp_video_camera/esp_video_camera.h": """#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
namespace esphome::esp_video_camera {
""" + types + """
struct ESPVideoCamera {
 JpegFrameConsumer*consumer=nullptr;std::atomic<bool>active{false};float requested_fps=0;uint32_t token=0;
 bool reject=false;std::function<void()>start_hook;
 bool register_jpeg_frame_consumer(JpegFrameConsumer*c){consumer=c;return true;}
 """ + signature + """ {if(start_hook)start_hook();if(reject||consumer!=consumer_)return false;requested_fps=max_fps;this->token=token;active=true;return true;}
 void stop_jpeg_frame_consumer(JpegFrameConsumer*){active=false;}
};}
""",
    }
    # Avoid shadowing the named consumer member with the production argument.
    stubs["esphome/components/esp_video_camera/esp_video_camera.h"] = stubs[
        "esphome/components/esp_video_camera/esp_video_camera.h"
    ].replace("consumer!=consumer_", "consumer!=this->consumer")
    for name, value in stubs.items():
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(value)
    (tmp_path / "main.cpp").write_text(r'''
#include "esp_jpeg_video_source.h"
#include <cassert>
#include <chrono>
#include <future>
using namespace esphome;
int main(){
 esp_video_camera::ESPVideoCamera camera;esp_jpeg_video_source::EspJpegVideoSource source;
 source.set_camera(&camera);source.setup();auto cap=source.get_video_capability();cap.max_fps=5;
 unsigned published=0;auto callback=[](void*ctx,const voip_stack::EncodedVideoAccessUnit&f){assert(f.data&&f.size==4&&f.key_frame);++*static_cast<unsigned*>(ctx);};
 assert(source.start_video(callback,&published,cap)&&camera.requested_fps==5&&camera.active);
 const uint32_t first=camera.token;uint8_t jpeg[4]{0xff,0xd8,0xff,0xd9};
 source.consume_jpeg_frame({jpeg,4,0,first});source.consume_jpeg_frame({jpeg,4,1,first});
 assert(published==2); // Admission already happened before camera encoding.
 source.stop_video();assert(!camera.active);source.consume_jpeg_frame({jpeg,4,2,first});assert(published==2);
 cap.max_fps=15;assert(source.start_video(callback,&published,cap)&&camera.requested_fps==10&&camera.token!=first);
 source.consume_jpeg_frame({jpeg,4,3,first});assert(published==2);
 source.consume_jpeg_frame({jpeg,4,3,camera.token});assert(published==3);
 source.stop_video();camera.reject=true;assert(!source.start_video(callback,&published,cap)&&!camera.active);
 source.consume_jpeg_frame({jpeg,4,4,camera.token});assert(published==3);camera.reject=false;
 std::promise<void>entered,release;auto ready=entered.get_future();auto unblock=release.get_future();
 camera.start_hook=[&]{entered.set_value();unblock.wait();};
 auto starter=std::async(std::launch::async,[&]{return source.start_video(callback,&published,cap);});ready.wait();
 auto stopper=std::async(std::launch::async,[&]{source.stop_video();});
 assert(stopper.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout);
 release.set_value();assert(starter.get());stopper.get();assert(!camera.active);
 source.consume_jpeg_frame({jpeg,4,5,camera.token});assert(published==3);
}
''')
    binary = tmp_path / "jpeg_source"
    flags = ["-DUSE_ESPHOME_VOIP_STACK_VIDEO_DEBUG"] if debug else []
    build = subprocess.run([
        "g++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_ESP_IDF",
        "-DUSE_ESPHOME_VOIP_STACK_VIDEO", "-DUSE_ESPHOME_VOIP_STACK_VIDEO_JPEG", *flags,
        "-I", str(tmp_path), "-I", str(ROOT), str(tmp_path / "esp_jpeg_video_source.cpp"),
        str(tmp_path / "main.cpp"), "-o", str(binary),
    ], capture_output=True, text=True)
    assert build.returncode == 0, build.stdout + build.stderr
    run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=8)
    assert run.returncode == 0, run.stdout + run.stderr
