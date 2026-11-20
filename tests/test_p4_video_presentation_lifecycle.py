"""Execute renderer lifecycle methods with a contended presentation mutex."""

import os
from pathlib import Path
import resource
import subprocess

import pytest


RENDERER = (
    Path(os.environ.get("VIDEO_RENDERER_SOURCE", Path(__file__).resolve().parents[1].parent / "esphome-video-renderer"))
    / "esphome/components/p4_video_renderer"
)


@pytest.mark.parametrize("codec", ["JPEG", "H264"])
def test_stopped_video_cannot_reopen_on_following_audio_call(tmp_path, codec):
    source = (RENDERER / "p4_video_renderer.cpp").read_text()
    deactivate = source.split("bool P4VideoRenderer::set_active(bool active)", 1)[1]
    deactivate = deactivate.split("bool P4VideoRenderer::submit_frame", 1)[0]
    header = (RENDERER / "p4_video_renderer.h").read_text()
    query = header.split("  bool has_remote_frame() const {", 1)[1].split("\n  }", 1)[0]
    harness = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
constexpr int pdTRUE = 1;
bool mutex_available = false;
int xSemaphoreTake(int, int wait) { assert(wait == 0); return mutex_available; }
void xSemaphoreGive(int) {}
void xTaskNotifyGive(void*) {}
struct P4VideoRenderer {
  std::atomic<bool> rx_running_{true}, rx_session_prepared_{true}, rx_active_{true};
#ifdef USE_P4_VIDEO_RENDERER_JPEG
  std::atomic<unsigned> remote_frame_visible_{1};
#else
  std::atomic<bool> remote_frame_visible_{true};
#endif
  std::atomic<bool> video_ended_pending_{false},presentation_in_flight_{false};
  std::atomic<bool> waiting_for_key_frame_{false};
  std::atomic<unsigned> rx_session_generation_{1}, loss_generation_{0};
  std::atomic<int> pending_surface_{1};
  struct { void reset(uint8_t) {} } cadence_;
  struct { uint8_t max_fps{10}; } rx_format_;
  uint8_t framerate_{10};
  void* rx_task_handle_{nullptr};
  int presentation_mutex_{};
  bool loop_pending{false};
  void enable_loop_soon_any_context() { loop_pending = true; }
  bool set_active(bool active);
  bool has_remote_frame() const { QUERY }
};
bool P4VideoRenderer::set_active(bool active) BODY
int main() {
  P4VideoRenderer renderer;
  assert(renderer.has_remote_frame());
  // End a video call while the display worker still owns the last surface.
  assert(renderer.set_active(false));
  assert(!renderer.has_remote_frame());
  assert(!renderer.remote_frame_visible_.load());
  assert(renderer.loop_pending && renderer.video_ended_pending_.load());
  // An audio-only call has no new video activation: the page must stay hidden.
  assert(!renderer.has_remote_frame());
  // A later video call must wait for its own first image as well.
  assert(renderer.set_active(true));
  assert(!renderer.has_remote_frame());
#ifdef USE_P4_VIDEO_RENDERER_JPEG
  renderer.remote_frame_visible_ = renderer.rx_session_generation_.load();
#else
  renderer.remote_frame_visible_ = true;
#endif
  assert(renderer.has_remote_frame());
  mutex_available = true;
  assert(renderer.set_active(false));
  assert(renderer.pending_surface_.load() == -1);
  assert(!renderer.has_remote_frame());
  // A late publication from a stopped generation cannot advertise live video.
  renderer.remote_frame_visible_ = true;
  assert(!renderer.has_remote_frame());
}
'''.replace("QUERY", query + "\n").replace("BODY", deactivate)
    cpp = tmp_path / "renderer_lifecycle.cpp"
    cpp.write_text(harness)
    binary = tmp_path / "renderer_lifecycle"
    subprocess.run(
        ["g++", "-std=c++17", f"-DUSE_P4_VIDEO_RENDERER_{codec}", str(cpp), "-o", str(binary)],
        check=True, capture_output=True, text=True,
    )
    subprocess.run(
        [str(binary)], check=True, capture_output=True, text=True,
        preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)),
    )


def test_public_lvgl_debug_intervals_include_nested_flush(tmp_path):
    """Actual profiler method distinguishes total render and callback duration."""
    source = (RENDERER / "p4_presentation.cpp").read_text()
    start = source.index("void P4VideoRenderer::on_display_debug_event_(")
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    method = source[start:end]
    core = (RENDERER / "p4_video_renderer.cpp").read_text()
    maximum = core.index("void P4VideoRenderer::update_max(")
    update_max = core[maximum:core.index("\n}", maximum) + 2]
    header = (RENDERER / "p4_video_renderer.h").read_text()
    import re
    fields = "\n".join(re.findall(r"  std::atomic<uint32_t> debug_[^;]+;", header))
    cpp = tmp_path / "lvgl_debug_timing.cpp"
    cpp.write_text(r'''
#include <atomic>
#include <cassert>
#include <cstdint>
using lv_event_code_t = int;
constexpr int LV_EVENT_RENDER_START=1, LV_EVENT_RENDER_READY=2;
constexpr int LV_EVENT_FLUSH_START=3, LV_EVENT_FLUSH_FINISH=4;
struct lv_area_t {int32_t x1,y1,x2,y2;};
uint32_t clock_us=1;uint32_t micros(){return clock_us;}
struct P4VideoRenderer {
 static void update_max(std::atomic<uint32_t>&, uint32_t);
 std::atomic<bool> rx_active_{true},rx_session_prepared_{true};
FIELDS
 void on_display_debug_event_(lv_event_code_t,const lv_area_t*);
};
UPDATE_MAX
METHOD
int main(){
 P4VideoRenderer renderer;lv_area_t area{2,4,321,243};
 renderer.on_display_debug_event_(LV_EVENT_RENDER_START,nullptr);
 clock_us=11;renderer.on_display_debug_event_(LV_EVENT_FLUSH_START,&area);
 clock_us=51;renderer.on_display_debug_event_(LV_EVENT_FLUSH_FINISH,&area);
 clock_us=101;renderer.on_display_debug_event_(LV_EVENT_RENDER_READY,nullptr);
 assert(renderer.debug_render_count_==1&&renderer.debug_render_total_us_==100);
 assert(renderer.debug_render_max_us_==100);
 assert(renderer.debug_flush_count_==1&&renderer.debug_flush_total_us_==40);
 assert(renderer.debug_flush_max_us_==40);
 assert(renderer.debug_flush_width_==320&&renderer.debug_flush_height_==240);
 renderer.on_display_debug_event_(LV_EVENT_RENDER_READY,nullptr);
 renderer.on_display_debug_event_(LV_EVENT_FLUSH_FINISH,nullptr);
 assert(renderer.debug_render_count_==1&&renderer.debug_flush_count_==1);
 renderer.rx_active_=false;clock_us=201;
 renderer.on_display_debug_event_(LV_EVENT_RENDER_START,nullptr);
 renderer.on_display_debug_event_(LV_EVENT_FLUSH_START,&area);
 clock_us=301;renderer.on_display_debug_event_(LV_EVENT_RENDER_READY,nullptr);
 renderer.on_display_debug_event_(LV_EVENT_FLUSH_FINISH,&area);
 assert(renderer.debug_render_count_==1&&renderer.debug_flush_count_==1);
}
'''.replace("FIELDS", fields).replace("UPDATE_MAX", update_max).replace("METHOD", method))
    binary = tmp_path / "lvgl_debug_timing"
    compiled = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                               str(cpp), "-o", str(binary)], capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True,
                            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    assert result.returncode == 0, result.stderr


@pytest.fixture(scope="module")
def h264_owner_binary(tmp_path_factory):
    """Exercise real conversion/publication and the shared owned pixel commit."""
    folder = tmp_path_factory.mktemp("h264-owner")
    source = (RENDERER / "p4_h264_decoder.cpp").read_text() + (RENDERER / "p4_presentation.cpp").read_text()
    signatures = ["bool P4VideoRenderer::h264_resolution_fits_(", "bool P4VideoRenderer::configure_i420_converter_(",
                  "bool P4VideoRenderer::render_i420_(", "bool P4VideoRenderer::present_surface_direct_("]
    methods = []
    for signature in signatures:
        start = source.index(signature)
        methods.append(source[start:source.index("\n}", start) + 2])
    cpp = folder / "h264-owner.cpp"
    cpp.write_text(r'''
#include "p4_presentation_geometry.h"
using esphome::p4_video_renderer::fit_scale_units;
using esphome::p4_video_renderer::PPA_SCALE_UNITS;
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <vector>
constexpr int pdTRUE=1;
constexpr int PPA_SRM_COLOR_MODE_YUV420=2,PPA_SRM_ROTATION_ANGLE_0=0,PPA_TRANS_MODE_BLOCKING=0;
constexpr int PPA_COLOR_RANGE_LIMIT=1,PPA_COLOR_CONV_STD_RGB_YUV_BT601=1;
struct ppa_srm_oper_config_t{struct{const void*buffer{};uint32_t pic_w{},pic_h{},block_w{},block_h{};int srm_cm{},yuv_range{},yuv_std{};}in;int rotation_angle{},mode{};float scale_x{},scale_y{};};
constexpr int ESP_IMGFX_ERR_OK=0,ESP_IMGFX_PIXEL_FMT_I420=1,ESP_IMGFX_PIXEL_FMT_O_UYY_E_VYY=2,ESP_IMGFX_COLOR_SPACE_STD_BT601=3;
using esp_imgfx_err_t=int;
struct esp_imgfx_color_convert_cfg_t{struct{int16_t width{},height{};}in_res;int in_pixel_fmt{},out_pixel_fmt{},color_space_std{};};
struct esp_imgfx_data_t{uint8_t*data;uint32_t data_len;};
unsigned conversions=0,opens=0;std::function<void()>convert_hook;
int esp_imgfx_color_convert_open(const esp_imgfx_color_convert_cfg_t*c,void**p){assert(c->in_pixel_fmt==ESP_IMGFX_PIXEL_FMT_I420&&c->out_pixel_fmt==ESP_IMGFX_PIXEL_FMT_O_UYY_E_VYY);*p=reinterpret_cast<void*>(1);++opens;return 0;}
int esp_imgfx_color_convert_close(void*){return 0;}
int esp_imgfx_color_convert_process(void*,const esp_imgfx_data_t*i,esp_imgfx_data_t*o){++conversions;std::memcpy(o->data,i->data,i->data_len);if(convert_hook)convert_hook();return 0;}
struct lv_area_t{int x1,y1,x2,y2;};struct lv_obj_t{int width=1280,height=720;};
void lv_obj_get_coords(lv_obj_t*p,lv_area_t*a){*a={0,0,p->width-1,p->height-1};}int lv_area_get_width(const lv_area_t*a){return a->x2-a->x1+1;}int lv_area_get_height(const lv_area_t*a){return a->y2-a->y1+1;}
bool xSemaphoreTake(std::mutex*m,int wait){assert(wait==0);return m->try_lock();}void xSemaphoreGive(std::mutex*m){m->unlock();}
#define ESP_LOGE(...) ((void)0)
namespace esphome::mipi_dsi{struct MipiDsi{
 unsigned writes=0;bool supports_rgb565_transform()const{return true;}
 bool draw_rgb565_transformed(const ppa_srm_oper_config_t&c,size_t bytes,int x,int y,int w,int h,bool(*admit)(void*),void*ctx){
  assert(c.in.srm_cm==PPA_SRM_COLOR_MODE_YUV420&&c.rotation_angle==0&&c.mode==PPA_TRANS_MODE_BLOCKING);
  assert(bytes==size_t(c.in.pic_w)*c.in.pic_h*3/2);assert(c.scale_x==2.875f&&c.scale_y==2.875f);
  assert(x==180&&y==15&&w==920&&h==690);assert(admit(ctx));++writes;return true;
 }
};}
using namespace esphome;
struct P4VideoRenderer{
 uint16_t max_decode_width_=800,max_decode_height_=800;static constexpr size_t kH264Level30MaxMacroblocks=1620;
 void*i420_converter_=nullptr;uint16_t i420_converter_width_=0,i420_converter_height_=0;bool i420_converter_failure_logged_=false;
 std::vector<uint8_t>storage=std::vector<uint8_t>(960000);uint8_t*optimized_yuv420_=storage.data();size_t optimized_yuv420_capacity_=960000;
 uint8_t*surfaces_[1]{storage.data()};size_t surface_capacity_bytes_=1280000,surface_data_size_[1]{};uint16_t surface_stride_bytes_[1]{},surface_content_width_[1]{},surface_content_height_[1]{};uint32_t surface_layout_size_[1]{},surface_generation_[1]{};
 lv_obj_t container;lv_obj_t*video_container_=&container;mipi_dsi::MipiDsi driver;mipi_dsi::MipiDsi*display_=&driver;
 std::atomic<uint32_t>rx_session_generation_{1},layout_size_{(1280U<<16)|720U};std::atomic<bool>rx_active_{true},rx_session_prepared_{true};std::atomic<int>pending_surface_{-1};std::mutex lock;std::mutex*presentation_mutex_=&lock;
 void enable_loop_soon_any_context(){}
 bool h264_resolution_fits_(uint16_t,uint16_t)const;bool configure_i420_converter_(uint16_t,uint16_t);bool render_i420_(const uint8_t*,size_t,uint16_t,uint16_t,uint32_t);bool present_surface_direct_(int);
};
METHODS
int main(int argc,char**argv){
 assert(argc==2);const int mode=std::atoi(argv[1]);P4VideoRenderer r;std::vector<uint8_t>input(320*240*3/2,0xa5);
 if(mode==1)convert_hook=[&]{r.rx_active_=false;++r.rx_session_generation_;};
 const bool published=r.render_i420_(input.data(),input.size(),320,240,1);
 assert(r.driver.writes==0&&conversions==1&&opens==1);
 if(mode==1){assert(!published&&r.pending_surface_==-1);return 0;}
 assert(published&&r.pending_surface_==0&&r.surface_data_size_[0]==input.size());
 // A pending frame owns the same YUV pixels until the main-loop transaction.
 const auto pixels=r.storage;assert(!r.render_i420_(input.data(),input.size(),320,240,1));assert(conversions==1&&r.storage==pixels);
 if(mode==2){++r.rx_session_generation_;assert(!r.present_surface_direct_(0));assert(r.driver.writes==0);return 0;}
 assert(r.present_surface_direct_(0));assert(r.driver.writes==1&&conversions==1);
}
'''.replace("METHODS", "\n".join(methods)))
    binary = folder / "h264-owner"
    built = subprocess.run(["g++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                            "-DUSE_P4_VIDEO_RENDERER_H264", "-I", str(RENDERER), str(cpp), "-o", str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize("case", [0, 1, 2], ids=["one-owned-YUV-SRM", "stop-during-convert", "stale-pending"])
def test_h264_conversion_hands_pixels_to_main_loop_only(h264_owner_binary, case):
    result = subprocess.run([str(h264_owner_binary), str(case)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("codec", ["JPEG", "H264"])
def test_setup_reserves_one_source_pixel_buffer_without_scaled_rgb(tmp_path, codec):
    source = (RENDERER / "p4_video_renderer.cpp").read_text()
    start = source.index("bool P4VideoRenderer::allocate_session_resources_()")
    body = source[start:source.index("\n}", start) + 2]
    cpp = tmp_path / "source-allocation.cpp"
    cpp.write_text(r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <vector>
constexpr int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
std::vector<size_t>encoded,rgb,yuv;
void*heap_caps_aligned_alloc(size_t alignment,size_t size,int){encoded.push_back(size);return std::aligned_alloc(alignment,size);}
void*alloc_surface(size_t size,size_t reserve){assert(reserve==0);rgb.push_back(size);return std::aligned_alloc(64,size);}
void*alloc_psram_dma(size_t size){yuv.push_back(size);return std::aligned_alloc(64,size);}
struct P4VideoRenderer{
 static constexpr size_t kMaxAccessUnitBytes=512*1024;uint16_t max_decode_width_=800,max_decode_height_=800;
 uint8_t*rx_au_=nullptr;size_t rx_au_capacity_=0;uint8_t*optimized_yuv420_=nullptr;size_t optimized_yuv420_capacity_=0;
 uint8_t*surfaces_[1]{};size_t surface_capacity_bytes_=0,surface_data_size_[1]{};uint16_t surface_stride_bytes_[1]{},surface_content_width_[1]{},surface_content_height_[1]{};
 size_t h264_optimized_yuv_bytes_()const{return size_t(max_decode_width_)*max_decode_height_*3/2;}
 void free_codec_resources_(){}void free_unpublished_surfaces_(){}
 bool allocate_session_resources_();
};
METHOD
int main(){
 P4VideoRenderer r;assert(r.allocate_session_resources_());auto*pixels=r.surfaces_[0];assert(pixels&&r.surface_capacity_bytes_==1280000);
 assert(r.allocate_session_resources_()&&r.surfaces_[0]==pixels);
#ifdef USE_P4_VIDEO_RENDERER_JPEG
 assert(encoded==std::vector<size_t>{524288});assert(rgb==std::vector<size_t>{1280000});assert(yuv.empty());std::free(r.rx_au_);std::free(r.surfaces_[0]);
#else
 assert(encoded.empty()&&rgb.empty());assert(yuv==std::vector<size_t>{960000});assert(r.surfaces_[0]==r.optimized_yuv420_);std::free(r.optimized_yuv420_);
#endif
}
'''.replace("METHOD", body))
    binary = tmp_path / "source-allocation"
    built = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                            f"-DUSE_P4_VIDEO_RENDERER_{codec}", str(cpp), "-o", str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
