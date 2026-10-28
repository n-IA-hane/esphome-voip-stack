"""Exercise the real renderer with threaded RTOS, decoder and display boundaries."""

from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "esphome/components/esp_video_renderer"


@pytest.mark.parametrize("psram", [True, False])
def test_borrowed_jpeg_generations_and_display_surface_lifetime(tmp_path, psram):
    headers = {
        "esphome/core/defines.h": "#pragma once\n#define USE_ESP_IDF\n",
        "esphome/core/automation.h": """#pragma once
namespace esphome { template<typename... Ts> struct Trigger {int count=0; void trigger(Ts...){count++;}};
template<typename... Ts> class Action {public: virtual ~Action()=default; virtual void play(const Ts &...)=0;}; }
""",
        "esphome/core/component.h": """#pragma once
#include <atomic>
namespace esphome {namespace setup_priority {constexpr float DATA=600;}
class Component {public: virtual ~Component()=default; virtual void setup(){};virtual void loop(){};virtual void dump_config(){};
virtual void on_shutdown(){};virtual float get_setup_priority()const{return 0;}
bool is_failed()const{return failed;} void mark_failed(){failed=true;}
void enable_loop_soon_any_context(){loop_enabled=true;}void enable_loop(){loop_enabled=true;}
void disable_loop(){loop_enabled=false;} bool failed=false;std::atomic<bool> loop_enabled{true};};}
""",
        "esphome/core/hal.h": "#pragma once\n#include <atomic>\n#include <cstdint>\nextern std::atomic<uint32_t> fake_ms; inline uint32_t millis(){return fake_ms.load();} inline uint32_t micros(){return millis()*1000;}\n",
        "esphome/core/log.h": "#pragma once\n#define ESP_LOGE(...) do {} while (false)\n#define ESP_LOGW(...) do {} while (false)\n#define ESP_LOGCONFIG(...) do {} while (false)\n#define ESP_LOGI(...) do {} while (false)\n",
        "freertos/FreeRTOS.h": """#pragma once
#include <cstdint>
using TickType_t=uint32_t;using UBaseType_t=unsigned;using StackType_t=uint32_t;
struct StaticTask_t {};constexpr int tskNO_AFFINITY=-1;constexpr unsigned portNUM_PROCESSORS=2;
constexpr int pdTRUE=1,pdFALSE=0,pdPASS=1;constexpr TickType_t portMAX_DELAY=0xffffffff;
#define pdMS_TO_TICKS(x) (x)
""",
        "freertos/semphr.h": """#pragma once
#include "FreeRTOS.h"
#include <condition_variable>
#include <mutex>
#include <chrono>
struct Semaphore {std::mutex mutex;std::condition_variable cv;int count=0;};
struct StaticSemaphore_t {Semaphore value;}; using SemaphoreHandle_t=Semaphore*;
inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t*s){s->value.count=1;return &s->value;}
inline SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t*s){return &s->value;}
inline int xSemaphoreTake(SemaphoreHandle_t s,TickType_t ticks){std::unique_lock lock(s->mutex);
if(ticks==portMAX_DELAY)s->cv.wait(lock,[&]{return s->count;});
else if(ticks&&!s->cv.wait_for(lock,std::chrono::milliseconds(ticks),[&]{return s->count;}))return 0;
if(!s->count)return 0; s->count--;return 1;}
inline void xSemaphoreGive(SemaphoreHandle_t s){{std::lock_guard lock(s->mutex);s->count=1;}s->cv.notify_one();}
""",
        "freertos/task.h": """#pragma once
#include "FreeRTOS.h"
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
struct Task {std::mutex mutex;std::condition_variable cv;unsigned notices=0;std::thread thread;};
using TaskHandle_t=Task*;inline std::vector<Task*> tasks;inline thread_local Task* self_task;
inline TaskHandle_t xTaskCreateStaticPinnedToCore(void(*fn)(void*),const char*,uint32_t bytes,void*ctx,unsigned,StackType_t*stack,StaticTask_t*control,int affinity){
assert(bytes==8192&&stack&&control&&affinity==tskNO_AFFINITY);auto *task=new Task;tasks.push_back(task);
task->thread=std::thread([=]{self_task=task;fn(ctx);});return task;}
inline void xTaskNotifyGive(TaskHandle_t t){{std::lock_guard lock(t->mutex);t->notices++;}t->cv.notify_one();}
inline unsigned ulTaskNotifyTake(int,TickType_t){std::unique_lock lock(self_task->mutex);
self_task->cv.wait(lock,[]{return self_task->notices;});auto n=self_task->notices;self_task->notices=0;return n;}
inline void vTaskDelete(void*){}
inline void join_tasks(){for(auto*t:tasks){t->thread.join();delete t;}tasks.clear();}
""",
        "freertos/idf_additions.h": '#pragma once\n#include "task.h"\n',
        "esp_heap_caps.h": """#pragma once
#include <cstdlib>
constexpr unsigned MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2,MALLOC_CAP_INTERNAL=4;
extern bool expected_external;inline bool fail_allocations=false;inline unsigned allocation_count=0;
inline void check_caps(unsigned caps){assert(bool(caps&MALLOC_CAP_SPIRAM)==expected_external);assert(bool(caps&MALLOC_CAP_INTERNAL)!=expected_external);allocation_count++;}
inline void*heap_caps_aligned_alloc(size_t a,size_t n,unsigned caps){check_caps(caps);if(fail_allocations)return nullptr;void*p=nullptr;return posix_memalign(&p,a,n)==0?p:nullptr;}
inline void*heap_caps_malloc(size_t n,unsigned caps){check_caps(caps);return fail_allocations?nullptr:malloc(n);}
inline void heap_caps_free(void*p){free(p);}
""",
        "sdkconfig.h": "#pragma once\n#define CONFIG_JD_USE_ROM 1\n",
        "jpeg_decoder.h": """#pragma once
#include <cstdint>
#include <cstring>
#include <mutex>
#include <condition_variable>
constexpr int ESP_OK=0,JPEG_IMAGE_FORMAT_RGB565=1;
enum esp_jpeg_image_scale_t {JPEG_IMAGE_SCALE_0=0,JPEG_IMAGE_SCALE_1_2,JPEG_IMAGE_SCALE_1_4};
struct esp_jpeg_image_cfg_t {uint8_t*indata=nullptr;uint32_t indata_size=0;uint8_t*outbuf=nullptr;uint32_t outbuf_size=0;
int out_format=0,out_scale=0;struct {bool swap_color_bytes=false;}flags;
struct {void*working_buffer=nullptr;size_t working_buffer_size=0;}advanced;};
struct esp_jpeg_image_output_t {uint16_t width=0,height=0;size_t output_len=0;};
inline std::mutex decode_mutex;inline std::condition_variable decode_cv;
inline bool hold_decode=false,entered_decode=false;inline unsigned completed_decodes=0;
inline int esp_jpeg_decode(esp_jpeg_image_cfg_t*cfg,esp_jpeg_image_output_t*info){
assert(cfg->advanced.working_buffer&&cfg->advanced.working_buffer_size==3100);
assert(cfg->out_format==JPEG_IMAGE_FORMAT_RGB565&&!cfg->flags.swap_color_bytes);
std::unique_lock lock(decode_mutex);entered_decode=true;decode_cv.notify_all();
decode_cv.wait(lock,[]{return !hold_decode;});
info->height=((unsigned(cfg->indata[7])<<8)|cfg->indata[8])>>cfg->out_scale;
info->width=((unsigned(cfg->indata[9])<<8)|cfg->indata[10])>>cfg->out_scale;
info->output_len=info->width*info->height*2;
assert(info->output_len<=cfg->outbuf_size);memset(cfg->outbuf,cfg->indata[25],info->output_len);
completed_decodes++;decode_cv.notify_all();return ESP_OK;}
""",
        "lvgl.h": """#pragma once
#include <cstdint>
#include <vector>
constexpr int LV_OBJ_FLAG_HIDDEN=1,LV_OBJ_FLAG_SCROLLABLE=2,LV_EVENT_REFR_READY=3,LV_IMAGE_HEADER_MAGIC=0x19,LV_COLOR_FORMAT_RGB565=18;
struct lv_image_dsc_t {struct {unsigned magic=0,cf=0,w=0,h=0,stride=0;}header;size_t data_size=0;const uint8_t*data=nullptr;};
struct lv_event_t{void*data;};struct lv_display_t {void(*callback)(lv_event_t*)=nullptr;void*ctx=nullptr;};
struct lv_obj_t{lv_display_t*display;const lv_image_dsc_t*src=nullptr;unsigned flags=0;};
inline lv_obj_t image;inline lv_obj_t*lv_image_create(lv_obj_t*c){image.display=c->display;return &image;}
inline void lv_obj_center(lv_obj_t*){} inline void lv_obj_add_flag(lv_obj_t*i,int f){i->flags|=f;}
inline void lv_obj_remove_flag(lv_obj_t*i,int f){i->flags&=~f;}
inline lv_display_t*lv_obj_get_display(lv_obj_t*i){return i->display;}
inline void lv_display_add_event_cb(lv_display_t*d,void(*cb)(lv_event_t*),int,void*ctx){d->callback=cb;d->ctx=ctx;}
inline void*lv_event_get_user_data(lv_event_t*e){return e->data;}
inline void lv_image_set_src(lv_obj_t*i,const lv_image_dsc_t*d){i->src=d;}
inline void lv_obj_set_size(lv_obj_t*,unsigned,unsigned){} inline void lv_obj_invalidate(lv_obj_t*){}
inline void refresh_ready(lv_display_t&d){lv_event_t e{d.ctx};d.callback(&e);}
""",
    }
    for name, body in headers.items():
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(body)
    harness = tmp_path / "main.cpp"
    harness.write_text(r'''
#include <cassert>
#include <chrono>
#include <thread>
#include "esp_video_renderer.h"
#include "esp_heap_caps.h"
bool expected_external=EXPECTED_EXTERNAL;
using namespace esphome::esp_video_renderer;
std::atomic<uint32_t> fake_ms{0};
struct Renderer:EspVideoRenderer {
  using EspVideoRenderer::release_setup_resources_;
  unsigned admitted(){return received_.load();}
  unsigned errors(){return decode_errors_.load();}
  unsigned displayed(){return presented_.load();}
  bool has_pending(){assert(xSemaphoreTake(mutex_,1000));bool pending=pending_surface_>=0;xSemaphoreGive(mutex_);return pending;}
};
void wait_decode(unsigned count) {
 std::unique_lock lock(decode_mutex);assert(decode_cv.wait_for(lock,std::chrono::seconds(3),[&]{return completed_decodes>=count;}));
}
void wait_pending(Renderer&r){for(unsigned n=0;n<1000&&!r.has_pending();n++)std::this_thread::sleep_for(std::chrono::milliseconds(1));assert(r.has_pending());}
int main(){
 Renderer r;r.set_width(8);r.set_height(8);r.set_max_jpeg_bytes(64);r.set_framerate(2);r.set_task_stack_in_psram(expected_external);r.set_buffer_in_psram(expected_external);r.setup();assert(!r.is_failed());assert(allocation_count==6);
 lv_display_t display;lv_obj_t container{&display};r.attach_video_container(&container);
 uint8_t a[]={0xff,0xd8,0xff,0xc0,0,11,8,0,8,0,8,1,1,0x11,0,
              0xff,0xda,0,8,1,1,0,0,63,0,11,0xff,0xd9};
 assert(!r.submit_jpeg(a,sizeof a)); assert(r.start());
 uint8_t oversized[65]={0xff,0xd8};assert(!r.submit_jpeg(oversized,sizeof oversized));
 // Borrowed data may be modified immediately after admission; decode owns a copy.
 {std::lock_guard lock(decode_mutex);hold_decode=true;entered_decode=false;}
 assert(r.submit_jpeg(a,sizeof a));
 {std::unique_lock lock(decode_mutex);assert(decode_cv.wait_for(lock,std::chrono::seconds(3),[]{return entered_decode;}));}
 a[25]=99;assert(!r.submit_jpeg(a,sizeof a));
 {std::lock_guard lock(decode_mutex);hold_decode=false;}decode_cv.notify_all();
 wait_decode(1);wait_pending(r);r.loop();assert(r.has_remote_frame());assert(image.src->data[0]==11);assert(r.displayed()==1);
 auto*first=image.src->data;
 // Decode can fill the other surface while the display still consumes its front.
 fake_ms=500;a[25]=22;assert(r.submit_jpeg(a,sizeof a));wait_decode(2);wait_pending(r);r.loop();
 assert(image.src->data==first&&first[0]==11&&r.displayed()==1);
 refresh_ready(display);r.loop();assert(image.src->data!=first&&image.src->data[0]==22&&r.displayed()==2);
 refresh_ready(display);
 // An old decode released after stop/start must not enter the new session.
 fake_ms=1000;a[25]=33;
 {std::lock_guard lock(decode_mutex);hold_decode=true;entered_decode=false;}
 assert(r.submit_jpeg(a,sizeof a));
 {std::unique_lock lock(decode_mutex);assert(decode_cv.wait_for(lock,std::chrono::seconds(3),[]{return entered_decode;}));}
 r.stop();r.loop();assert(!r.has_remote_frame());assert(image.flags&LV_OBJ_FLAG_HIDDEN);
 assert(r.start());
 {std::lock_guard lock(decode_mutex);hold_decode=false;}decode_cv.notify_all();wait_decode(3);
 std::this_thread::sleep_for(std::chrono::milliseconds(5));r.loop();assert(!r.has_remote_frame());assert(r.displayed()==2);
 fake_ms=1500;a[25]=44;assert(r.submit_jpeg(a,sizeof a));wait_decode(4);wait_pending(r);r.loop();assert(r.has_remote_frame());assert(image.src->data[0]==44);
 refresh_ready(display);
 // Power-of-two decode reduction keeps the same surfaces and LVGL uses the real output size.
 const auto reserved=allocation_count;
 fake_ms=2000;a[8]=16;a[10]=16;assert(r.submit_jpeg(a,sizeof a));
 wait_decode(5);wait_pending(r);r.loop();assert(image.src->header.w==8&&image.src->header.h==8);
 assert(image.src->header.stride==16&&image.src->data_size==128);refresh_ready(display);
 fake_ms=2500;a[8]=24;a[10]=32;assert(r.submit_jpeg(a,sizeof a));
 wait_decode(6);wait_pending(r);r.loop();assert(image.src->header.w==8&&image.src->header.h==6);
 assert(image.src->header.stride==16&&image.src->data_size==96);refresh_ready(display);
 assert(allocation_count==reserved);
 // Beyond the bounded native envelope, reject before vendor decoding or writing a surface.
 fake_ms=3000;a[10]=40;assert(r.submit_jpeg(a,sizeof a));
 for(unsigned n=0;n<1000&&!r.errors();n++)std::this_thread::sleep_for(std::chrono::milliseconds(1));
 assert(r.errors()==1);assert(image.src->data[0]==44);
 const auto stats=r.get_stats();assert(stats.decoded==6&&stats.presented==5&&stats.decode_errors==1);r.dump_state();
 r.stop();r.loop();assert(!r.has_remote_frame());r.on_shutdown();join_tasks();
 // Hardware shutdown retains display references; the harness releases only after joining everything.
 r.release_setup_resources_();
 // Allocation placement never silently falls back when the requested heap is exhausted.
 fail_allocations=true;Renderer failed;failed.set_task_stack_in_psram(expected_external);failed.set_buffer_in_psram(expected_external);failed.setup();assert(failed.is_failed());
 const auto allocated=allocation_count;Renderer invalid_core;invalid_core.set_task_core(2);invalid_core.setup();assert(invalid_core.is_failed()&&allocation_count==allocated);
}
'''.replace("EXPECTED_EXTERNAL", "true" if psram else "false"))
    binary = tmp_path / "renderer"
    subprocess.run([
        "g++", "-std=c++17", "-pthread", "-include", "cassert", "-Wall", "-Wextra",
        "-Werror", "-Wno-unused-variable", "-Wno-misleading-indentation",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I", str(tmp_path), "-I", str(SOURCE), str(SOURCE / "esp_video_renderer.cpp"),
        str(harness), "-o", str(binary),
    ], check=True, capture_output=True, text=True)
    subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=15)
