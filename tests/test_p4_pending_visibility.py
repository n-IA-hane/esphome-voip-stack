"""Run the production loop across stale JPEG publication and call transitions."""

from pathlib import Path
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[1]


def method(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


@pytest.mark.parametrize("codec", ["JPEG", "H264"])
def test_published_frame_generation_controls_page_visibility(tmp_path, codec):
    component = ROOT / "esphome/components/p4_video_renderer"
    source = (component / "p4_video_renderer.cpp").read_text()
    header = (component / "p4_video_renderer.h").read_text()
    query = header.split("  bool has_remote_frame() const {", 1)[1].split("\n  }", 1)[0]
    signatures = ["void P4VideoRenderer::loop()", "bool P4VideoRenderer::set_video_active("]
    if codec == "JPEG":
        signatures += ["bool P4VideoRenderer::start_video(", "void P4VideoRenderer::stop_video()"]
    methods = "\n".join(method(source, signature) for signature in signatures)
    code = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <utility>
constexpr int pdTRUE=1;
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
struct Semaphore {bool held=false;};
bool xSemaphoreTake(Semaphore*s,int wait){assert(wait==0);if(s->held)return false;s->held=true;return true;}
void xSemaphoreGive(Semaphore*s){assert(s->held);s->held=false;}
void xTaskNotifyGive(void*){}
int active_screen=0,other_screen=1;
struct Container {int*screen=&other_screen;};
int*lv_obj_get_screen(Container*c){return c->screen;}
int*lv_disp_get_scr_act(void*){return &active_screen;}
int*lv_screen_active(){return &active_screen;}
void lv_obj_invalidate(void*){}
struct Trigger {unsigned count=0;void trigger(){++count;}};
// Inject a real stop/start immediately before the visibility publication.
// It exercises the check/publication window without relying on scheduling.
struct Visibility {
 std::atomic<uint32_t>value{0};std::function<void()>before_exchange;
 uint32_t load(std::memory_order order)const{return value.load(order);}
 void store(uint32_t v,std::memory_order order){value.store(v,order);}
 uint32_t exchange(uint32_t v,std::memory_order order){auto hook=std::move(before_exchange);before_exchange={};if(hook)hook();return value.exchange(v,order);}
};
namespace voip_stack {struct VideoCapability {uint8_t max_fps=10;bool valid()const{return true;}bool is_jpeg()const{return true;}};}
struct P4VideoRenderer {
 Semaphore presentation;Semaphore*presentation_mutex_=&presentation;void*rx_task_handle_=nullptr;
 std::atomic<bool>rx_running_{true},rx_active_{false},rx_session_prepared_{false},presentation_in_flight_{false},video_ended_pending_{false};
 std::atomic<bool>direct_page_active_{false},waiting_for_key_frame_{false};
 std::atomic<uint32_t>rx_session_generation_{0},loss_generation_{0},pending_surface_generation_{0};
 std::atomic<int>pending_surface_{-1};Visibility remote_frame_visible_;
 std::atomic<uint32_t>rx_admitted_frames_{0},rx_rendered_frames_{0},rx_presented_frames_{0},rx_refresh_completed_{0};
 uint8_t pixels[2]{};uint8_t*surfaces_[2]{pixels,pixels+1};
 Container container;Container*video_container_=&container;
 voip_stack::VideoCapability rx_capability_;uint8_t framerate_=10;struct{void reset(uint8_t){}}cadence_;
 Trigger first_frame_trigger_,video_ended_trigger_;unsigned commits=0,wakeups=0;
 bool is_failed()const{return false;}void disable_loop(){}void enable_loop_soon_any_context(){++wakeups;}
 void refresh_direct_display_layout_(){}
 bool commit_direct_surface_(int index){
  assert(index==0);++commits;
#ifdef USE_P4_VIDEO_RENDERER_JPEG
  if(pending_surface_generation_.load()!=rx_session_generation_.load())return false;
#endif
  pending_surface_.store(-1,std::memory_order_release);return true;
 }
 bool has_remote_frame()const {QUERY}
 bool set_video_active(bool);bool start_video(const voip_stack::VideoCapability&);void stop_video();void loop();
 void begin(){
#ifdef USE_P4_VIDEO_RENDERER_JPEG
  assert(start_video({}));
#else
  rx_session_prepared_.store(true);rx_session_generation_.fetch_add(1);
#endif
  assert(set_video_active(true));
 }
 void publish(uint32_t generation){pending_surface_generation_.store(generation,std::memory_order_relaxed);pending_surface_.store(0,std::memory_order_release);}
};
METHODS
int main(){
 // A current frame opens the hidden page before any display submission.
 // The following visible-page frame must not repeat that first-frame edge.
 {
  P4VideoRenderer r;r.begin();r.publish(r.rx_session_generation_.load());r.loop();
  assert(r.has_remote_frame()&&r.first_frame_trigger_.count==1&&r.commits==0);
  r.container.screen=&active_screen;r.publish(r.rx_session_generation_.load());r.loop();
  assert(r.has_remote_frame()&&r.first_frame_trigger_.count==1&&r.commits==1);
  assert(r.set_video_active(false));assert(!r.has_remote_frame());
  assert(r.set_video_active(true));assert(!r.has_remote_frame());
 }
#ifdef USE_P4_VIDEO_RENDERER_JPEG
 for(bool page_active:{false,true}){
  P4VideoRenderer r;r.begin();const auto old=r.rx_session_generation_.load();
  r.stop_video();r.begin();if(page_active)r.container.screen=&active_screen;
  // A persistent decoder may pass its final active/generation check, then
  // publish after stop invalidates that session. Model that exact publication.
  r.publish(old);r.loop();
  assert(!r.has_remote_frame()&&r.first_frame_trigger_.count==0&&r.commits==0);
  assert(r.pending_surface_.load()==-1);
  r.publish(r.rx_session_generation_.load());r.loop();
  assert(r.has_remote_frame()&&r.first_frame_trigger_.count==1);
 }
 {
  P4VideoRenderer r;r.begin();r.publish(r.rx_session_generation_.load());
  r.remote_frame_visible_.before_exchange=[&]{r.stop_video();r.begin();};
  r.loop();
  assert(!r.has_remote_frame()&&r.first_frame_trigger_.count==0);
  // A late old visibility write must not consume the new call's first edge.
  r.publish(r.rx_session_generation_.load());r.loop();
  assert(r.has_remote_frame()&&r.first_frame_trigger_.count==1);
 }
#endif
}
'''.replace("QUERY", query + "\n").replace("METHODS", methods)
    cpp = tmp_path / "pending_visibility.cpp"
    cpp.write_text(code)
    binary = tmp_path / "pending_visibility"
    result = subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
        f"-DUSE_P4_VIDEO_RENDERER_{codec}", "-DUSE_P4_VIDEO_RENDERER_DIRECT_DISPLAY",
        str(cpp), "-o", str(binary),
    ], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result.stderr
