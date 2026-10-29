"""Run current production video resource methods with bounded host substitutes.

The actual headers, RTP/JPEG parser and selected implementation methods are
compiled. RTOS task scheduling, PSRAM allocation and codec/PPA APIs are test
adapters. These tests do not qualify a physical display or hardware encoder.
"""

import os
from pathlib import Path
import re
import resource
import shutil
import subprocess

import pytest


ROOT = Path(os.environ.get("VOIP_STACK_REPO", Path(__file__).resolve().parents[1]))
VIDEO = ROOT / "esphome/components/voip_stack"
H264 = ROOT / "esphome/components/esp_h264_video_source"


def method(source, name):
    signature = re.search(
        rf"(?:\b(?:bool|void)\s+)?{re.escape(name)}\([^;{{}}]*\)"
        r"(?:\s+const)?(?:\s*:\s*[^{}]+)?\s*\{",
        source,
        re.DOTALL,
    )
    assert signature is not None, name
    start = source.index("{", signature.start())
    depth = 0
    for index in range(start, len(source)):
        depth += (source[index] == "{") - (source[index] == "}")
        if depth == 0:
            return source[signature.start() : index + 1]
    raise AssertionError(name)


def write_headers(tmp_path, headers):
    for name, content in headers.items():
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)


RTOS = r"""
#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
using BaseType_t=int;
using TickType_t=unsigned;
using TaskHandle_t=void*;
using StackType_t=unsigned;
struct StaticTask_t {};
struct StaticSemaphore_t {bool given=false,mutex=false;std::mutex lock;};
using SemaphoreHandle_t=StaticSemaphore_t*;
inline std::function<void(SemaphoreHandle_t)> before_mutex;
constexpr unsigned portMAX_DELAY=UINT32_MAX;
constexpr int pdTRUE=1;
#define pdMS_TO_TICKS(ms) (ms)
inline SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *s){return s;}
inline int xSemaphoreTake(SemaphoreHandle_t s,unsigned){
 if(s->mutex){if(before_mutex)before_mutex(s);s->lock.lock();return pdTRUE;}
 bool value=s->given;s->given=false;return value;
}
inline void xSemaphoreGive(SemaphoreHandle_t s){if(s->mutex)s->lock.unlock();else s->given=true;}
inline void xTaskNotifyGive(TaskHandle_t){}
inline unsigned ulTaskNotifyTake(int,unsigned){return 0;}
"""


def compile_harness(tmp_path, source, *, defines=(), sources=()):
    cpp = tmp_path / "case.cpp"
    cpp.write_text(source)
    binary = tmp_path / "case"
    subprocess.run(
        [
            "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-variable", "-fsanitize=address,undefined", "-g",
            *[f"-D{name}" for name in defines], "-I", str(tmp_path),
            str(cpp), *[str(path) for path in sources], "-o", str(binary),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    return binary


def run(binary, *args):
    result = subprocess.run(
        [str(binary), *map(str, args)], capture_output=True, text=True,
        preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)),
    )
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.fixture(scope="module", params=[("JPEG", False), ("JPEG", True), ("H264", False), ("H264", True)])
def video_pool_binary(tmp_path_factory, request):
    codec, paced = request.param
    tmp_path = tmp_path_factory.mktemp(f"video-pool-{codec}-{paced}")
    for name in ("video_rtp.h", "video.h", "rtp_jpeg.h"):
        shutil.copyfile(VIDEO / name, tmp_path / name)
    write_headers(tmp_path, {
        "esphome/core/defines.h": "#pragma once\n",
        "audio_core_task_utils.h": "#pragma once\n",
        "esphome/core/helpers.h": "#pragma once\n#include <mutex>\nnamespace esphome {using Mutex=std::mutex;using LockGuard=std::lock_guard<Mutex>;}\n",
        "freertos/FreeRTOS.h": RTOS,
        "freertos/semphr.h": '#include "FreeRTOS.h"\n',
        "freertos/task.h": '#include "FreeRTOS.h"\n',
    })
    source = (VIDEO / "video_rtp.cpp").read_text()
    names = ["VideoRtpSession::VideoRtpSession", "VideoRtpSession::~VideoRtpSession",
             "VideoRtpSession::allocate_persistent_buffers_", "VideoRtpSession::acquire_reassembly_slot_",
             "VideoRtpSession::reset_reassembly_", "VideoRtpSession::queue_access_unit_"]
    if "bool VideoRtpSession::prepare_buffers()" in source:
        names.append("VideoRtpSession::prepare_buffers")
    if codec == "JPEG":
        names += ["VideoRtpSession::ensure_jpeg_quantization_cache_", "VideoRtpSession::handle_jpeg_payload_"]
    methods = "\n".join(method(source, name) for name in names)
    prepare = "prepare_buffers()" if "VideoRtpSession::prepare_buffers" in methods else "allocate_persistent_buffers_()"
    harness = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <string>
#define protected public
#include "video_rtp.h"
#undef protected
#define ESP_LOGE(...) ((void)0)
constexpr unsigned MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
std::unordered_map<void*,size_t> allocations;
size_t allocation_calls=0, fail_at=SIZE_MAX;
void *heap_caps_malloc(size_t bytes,unsigned flags){
  assert(flags==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
  if(++allocation_calls==fail_at)return nullptr;
  auto *p=std::malloc(bytes);assert(p);allocations[p]=bytes;return p;
}
void heap_caps_free(void *p){assert(allocations.erase(p)==1);std::free(p);}
namespace esphome::voip_stack {
METHODS
void VideoRtpSession::stop(){}
void VideoRtpSession::quiesce_tasks_(){}
void VideoRtpSession::close_sockets_(){}
void VideoRtpSession::wake_task_(){}
struct Source:EncodedVideoSource {
 bool leased=false;
 bool uses_access_unit_leases()const override{return leased;}
 VideoCapability get_video_capability()const override{return {};}
 bool prepare_video(const VideoCapability&)override{return true;}
 bool start_video(EncodedVideoAccessUnitCallback,void*,const VideoCapability&)override{return true;}
 void stop_video()override{}
};
struct Sink:EncodedVideoSink {
 unsigned delivered=0; const uint8_t *first=nullptr;size_t limit=0;
 size_t max_encoded_access_unit_bytes()const override{return limit;}
 VideoCapability get_receive_video_capability()const override{return {};}
 bool start_video(const VideoCapability&)override{return true;}
 bool set_video_active(bool)override{return true;}
 void stop_video()override{}
 bool consume_video_access_unit(const EncodedVideoAccessUnit &unit)override{
   assert(unit.release_callback==nullptr);
   if(first==nullptr)first=unit.data;
   assert(first==unit.data);++delivered;return true;
 }
};
}
int main(int argc,char **argv){
 using namespace esphome::voip_stack;
 assert(argc==3);unsigned role=std::stoul(argv[1]),failure=std::stoul(argv[2]);
 Source source;Sink sink;source.leased=role&8;sink.limit=(role&16)?1024:0;
 {
  VideoRtpSession session(false);
  assert(allocations.empty() && "bind endpoints before allocating media pools");
  session.set_source((role&1)?&source:nullptr);
  session.set_sink((role&2)?&sink:nullptr);
  fail_at=failure;
  bool ready=session.PREPARE;
  if(!ready){
   // A setup retry must reuse the acquired prefix and must not allocate in a call.
   fail_at=SIZE_MAX;assert(session.PREPARE);
  }
  unsigned rx=0,tx=0;
  for(const auto &slot:session.receive_access_units_)rx+=slot.data!=nullptr;
  for(const auto &slot:session.tx_access_units_)tx+=slot.data!=nullptr;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  assert(rx==((role&2)?1U:0U));
  assert((session.jpeg_quantization_cache_!=nullptr)==bool(role&2));
  if(role&2){
   const uint8_t packet[]={0,0,0,0,0,50,1,1,0x12,0x34,0x56};
   const size_t calls=allocation_calls;
   for(unsigned i=0;i<1000;i++)assert(session.handle_jpeg_payload_(packet,sizeof(packet),true,9000*i));
   assert(sink.delivered==1000 && allocation_calls==calls);
   if(sink.limit){
    std::vector<uint8_t> too_large(sink.limit+16,0x55);
    const uint8_t header[]={0,0,0,0,0,50,1,1};
    std::copy(header,header+8,too_large.begin());
    assert(!session.handle_jpeg_payload_(too_large.data(),too_large.size(),true,90000001));
    assert(sink.delivered==1000 && allocation_calls==calls);
   }
  }
#else
  assert(rx==((role&2)?3U:0U));
#endif
  assert(tx==((role&1)&&!source.leased?session.kTxAccessUnitSlots:0U));
  if((role&1)&&source.leased){
   session.running_=true;session.sender_running_=true;session.send_enabled_=true;
   uint8_t payload[]{1,2,3,4};unsigned releases=0;
   EncodedVideoAccessUnit frame{payload,sizeof(payload),42,true,
     [](void *p){++*static_cast<unsigned*>(p);},&releases};
   for(unsigned n=0;n<session.kTxAccessUnitSlots;n++)session.queue_access_unit_(frame);
   assert(releases==0);
   session.queue_access_unit_(frame);assert(releases==1); // Full queue returns its lease.
   for(auto &slot:session.tx_access_units_){
    assert(slot.access_unit.data==payload&&slot.data==nullptr&&slot.state==1);
    slot.access_unit.release();slot.access_unit={};slot.state=0;
   }
   assert(releases==session.kTxAccessUnitSlots+1);
   // An incorrectly borrowed frame from a lease-only source cannot write null memory.
   session.queue_access_unit_(EncodedVideoAccessUnit{payload,sizeof(payload),42,true});
   for(auto &slot:session.tx_access_units_)assert(slot.state==0);
   session.running_=false;session.sender_running_=false;
  }
  size_t bytes=0;for(const auto &entry:allocations)bytes+=entry.second;
  size_t expected=rx*(sink.limit?sink.limit:session.kMaxAccessUnitBytes)+tx*session.kMaxAccessUnitBytes;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  if(role&2)expected+=RtpJpegDepacketizer::kQuantizationCacheBytes;
#endif
  assert(bytes==expected);
  const size_t calls=allocation_calls;assert(session.PREPARE);assert(allocation_calls==calls);
  std::printf("rx=%u tx=%u allocated_bytes=%zu retry=%d\n",rx,tx,bytes,!ready);
 }
 assert(allocations.empty());
}
'''.replace("METHODS", methods).replace("PREPARE", prepare)
    defines = ["USE_ESP32", "USE_ESPHOME_VOIP_STACK_VIDEO", f"USE_ESPHOME_VOIP_STACK_VIDEO_{codec}"]
    if paced:
        defines.append("USE_ESPHOME_VOIP_STACK_VIDEO_HOSTED_AUDIO_PACING")
    return compile_harness(tmp_path, harness, defines=defines, sources=[VIDEO / "rtp_jpeg.cpp"] if codec == "JPEG" else [])


@pytest.mark.parametrize("role", [0, 1, 2, 3, 9, 11, 18, 19],
                         ids=["none", "source", "sink", "duplex", "leased-source", "leased-duplex", "bounded-sink", "bounded-duplex"])
def test_video_pool_matches_codec_and_bound_endpoints(video_pool_binary, role):
    run(video_pool_binary, role, 0)


@pytest.mark.parametrize("failure", [1, 2, 3, 4, 5])
def test_video_pool_partial_allocation_retries_without_leaking(video_pool_binary, failure):
    run(video_pool_binary, 3, failure)


@pytest.fixture(scope="module")
def h264_shutdown_binary(tmp_path_factory):
    tmp_path = tmp_path_factory.mktemp("h264-shutdown")
    shutil.copyfile(H264 / "esp_h264_video_source.h", tmp_path / "esp_h264_video_source.h")
    write_headers(tmp_path, {
        "esphome/core/defines.h": "#pragma once\n",
        "esphome/core/component.h": r'''#pragma once
#include <atomic>
namespace esphome {
namespace setup_priority {constexpr float DATA=600;}
class Component {public:
 virtual ~Component()=default;
 virtual void setup(){} virtual void loop(){} virtual void on_shutdown(){}
 virtual void dump_config(){} virtual float get_setup_priority()const{return 0;}
 void mark_failed(){failed=true;} bool is_failed()const{return failed;}
 void disable_loop(){loop_enabled=false;}
 void enable_loop_soon_any_context(){loop_enabled=true;}
 bool failed=false; std::atomic<bool> loop_enabled{false};
};}
''',
        "esphome/components/esp_video_camera/esp_video_camera.h": r'''#pragma once
#include <cstdint>
#include <cstddef>
namespace esphome::esp_video_camera {
enum class RawVideoPixelFormat {RGB565_LE,YUV420_OUYY_EVYY};
struct RawVideoFrame {const uint8_t *data{};size_t size{};RawVideoPixelFormat pixel_format{};
uint16_t width{},height{},stride_bytes{};uint32_t timestamp_90khz{};uint16_t rotation_degrees{};};
class RawVideoFrameConsumer {public:virtual ~RawVideoFrameConsumer()=default;
virtual void consume_raw_video_frame(const RawVideoFrame&)=0;};
class ESPVideoCamera {public:
 unsigned starts=0,stops=0;bool active=false;
 void stop_raw_frame_consumer(void*){++stops;active=false;}
 bool start_raw_frame_consumer(void*){++starts;active=true;return true;}}
;}
''',
        "driver/ppa.h": "#pragma once\nusing ppa_client_handle_t=void*;\n",
        "esp_h264_enc_single_hw.h": "#pragma once\nusing esp_h264_enc_handle_t=void*;\n",
        "freertos/FreeRTOS.h": RTOS,
        "freertos/semphr.h": '#include "FreeRTOS.h"\n',
        "freertos/task.h": '#include "FreeRTOS.h"\n',
    })
    (tmp_path / "esphome/components/voip_stack").mkdir(parents=True, exist_ok=True)
    shutil.copyfile(VIDEO / "video.h", tmp_path / "esphome/components/voip_stack/video.h")
    source = (H264 / "esp_h264_video_source.cpp").read_text()
    names = [
        "on_shutdown", "stop_video", "stop_tx_task_", "wait_for_tx_idle_", "tx_slots_idle_",
        "release_encoded_slot_", "release_tx_slot_", "close_encoder_", "free_resources_", "tx_task_",
        "consume_raw_video_frame",
        "prepare_video", "start_video",
    ]
    optional = ["loop", "finish_shutdown_", "reap_tx_task_", "request_video_stop_", "notify_tx_idle_", "finish_raw_callback_", "video_ready_"]
    names += [name for name in optional if f"EspH264VideoSource::{name}(" in source]
    methods = "\n".join(method(source, f"EspH264VideoSource::{name}") for name in names)
    harness = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
#include <algorithm>
#include <thread>
#include <condition_variable>
#define protected public
#include "esp_h264_video_source.h"
#undef protected
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
unsigned freed=0,closed=0,deleted=0,ppa_closed=0,reaped=0;
void (*during_conversion)(esphome::esp_h264_video_source::EspH264VideoSource*)=nullptr;
void heap_caps_free(void *p){++freed;std::free(p);}
void esp_h264_enc_close(void*){++closed;}
void esp_h264_enc_del(void*){++deleted;}
void ppa_unregister_client(void*){++ppa_closed;}
struct Finished {};
[[noreturn]] void vTaskSuspend(void*){throw Finished{};}
namespace esphome::voip_audio_core {
void cleanup_managed_pinned_task(void **handle,unsigned **,unsigned,bool){++reaped;*handle=nullptr;}
[[noreturn]] void finish_managed_pinned_task(SemaphoreHandle_t done){xSemaphoreGive(done);throw Finished{};}
}
namespace esphome::esp_h264_video_source {
METHODS
void EspH264VideoSource::setup(){}
void EspH264VideoSource::dump_config(){}
voip_stack::VideoCapability EspH264VideoSource::get_video_capability()const{return {};}
void EspH264VideoSource::request_key_frame(){}
bool EspH264VideoSource::transform_to_encoder_yuv_(const esp_video_camera::RawVideoFrame&,uint8_t*){
 if(during_conversion!=nullptr)during_conversion(this);
 return false;
}
bool EspH264VideoSource::set_encoder_bitrate_(uint32_t){return true;}
bool EspH264VideoSource::force_encoder_idr_(){return true;}
bool EspH264VideoSource::encode_frame_(const uint8_t*,uint32_t,bool,uint32_t){return true;}
}
namespace esphome::voip_stack {bool h264_level_fits(const std::string&,const std::string&){return true;}}
int main(int argc,char **argv){
 using namespace esphome::esp_h264_video_source;
 assert(argc==2);const unsigned scenario=std::stoul(argv[1]);
 EspH264VideoSource source;
 StaticSemaphore_t control{},done{},idle{};control.mutex=true;
 source.control_mutex_=&control;source.tx_done_=&done;source.tx_idle_=&idle;
 source.encoder_=reinterpret_cast<void*>(5);source.ppa_=reinterpret_cast<void*>(6);
 source.tx_task_handle_=reinterpret_cast<void*>(4);
 source.tx_active_=true;source.tx_task_running_=true;source.encoder_ready_=true;
 for(auto &slot:source.tx_slots_)slot.yuv=static_cast<uint8_t*>(std::malloc(32));
 for(auto &slot:source.encoded_slots_){slot.owner=&source;slot.data=static_cast<uint8_t*>(std::malloc(32));}
 if(scenario==4){
  esphome::esp_video_camera::ESPVideoCamera camera;source.camera_=&camera;
  source.tx_active_=false;done.given=true;
  esphome::voip_stack::VideoCapability capability;
  capability.payload_type=103;capability.width=400;capability.height=400;capability.max_fps=10;
  capability.encoding="H264";capability.packetization_mode=1;capability.profile_level_id="42c01e";
  std::mutex mutex;std::condition_variable condition;bool at_lock=false,continue_start=false;
  std::thread::id starter_id;bool started=true;
  before_mutex=[&](SemaphoreHandle_t s){
   if(s!=&control || std::this_thread::get_id()!=starter_id)return;
   std::unique_lock lock(mutex);
   if(at_lock)return;
   at_lock=true;condition.notify_all();condition.wait(lock,[&]{return continue_start;});
  };
  std::thread starter([&]{starter_id=std::this_thread::get_id();
   started=source.start_video([](void*,const esphome::voip_stack::EncodedVideoAccessUnit&){},nullptr,capability);
  });
  {std::unique_lock lock(mutex);condition.wait(lock,[&]{return at_lock;});}
  source.on_shutdown();assert(freed==4);
  {std::lock_guard lock(mutex);continue_start=true;}condition.notify_all();starter.join();
  assert(!started && camera.starts==0 && !camera.active);
  before_mutex={};return 0;
 }
 if(scenario==0){
   source.tx_slots_[0].state=3;source.encoded_slots_[0].state=1;
 }else if(scenario==1){source.tx_slots_[0].state=1;
 }else if(scenario==2){source.encoded_slots_[0].state=1;done.given=true;
 }else{
  done.given=true;
  during_conversion=[](EspH264VideoSource *s){
   s->on_shutdown();
   assert(freed==0 && "camera conversion still owns its slot and PPA client");
  };
 }
 if(scenario==3)source.consume_raw_video_frame({});
 else source.on_shutdown();
 assert(freed==0 && closed==0 && deleted==0 && ppa_closed==0 && "shutdown must retain every owned resource");
 if(scenario==0){
  assert(source.tx_task_handle_!=nullptr && source.tx_slots_[0].state==3);
  source.release_tx_slot_(&source.tx_slots_[0]);
  EspH264VideoSource::release_encoded_slot_(&source.encoded_slots_[0]);
  // The actual worker tail publishes completion and schedules owner cleanup.
  try{source.tx_task_();}catch(Finished&){}
 }else if(scenario==1){
  try{source.tx_task_();}catch(Finished&){}
  assert(source.tx_slots_[0].state==1 && "worker completion cannot steal a camera callback's slot");
  source.release_tx_slot_(&source.tx_slots_[0]);
 }else if(scenario==2){EspH264VideoSource::release_encoded_slot_(&source.encoded_slots_[0]);}
 assert(source.loop_enabled && "the last owner must wake cleanup without polling");
 source.loop();
 assert(freed==4 && closed==1 && deleted==1 && ppa_closed==1 && reaped==1);
 assert(!source.loop_enabled && source.tx_task_handle_==nullptr);
 source.on_shutdown();source.loop();
 // A late camera delivery after shutdown is rejected without using PPA/PCM.
 source.consume_raw_video_frame({});
 assert(freed==4 && closed==1 && deleted==1 && ppa_closed==1 && reaped==1);
 std::printf("scenario=%u retained_then_released=4 cleanup_once=1\n",scenario);
}
'''.replace("METHODS", methods)
    return compile_harness(tmp_path, harness, defines=[
        "USE_ESP_IDF", "USE_ESPHOME_VOIP_STACK_VIDEO", "USE_ESPHOME_VOIP_STACK_VIDEO_H264",
    ])


@pytest.mark.parametrize("scenario", [0, 1, 2, 3, 4], ids=["blocked-worker", "camera-callback", "leased-access-unit", "stop-during-conversion", "start-after-shutdown"])
def test_h264_shutdown_waits_for_actual_resource_owners(h264_shutdown_binary, scenario):
    run(h264_shutdown_binary, scenario)
