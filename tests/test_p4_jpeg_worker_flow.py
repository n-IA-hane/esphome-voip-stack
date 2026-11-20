"""Execute the actual JPEG worker and display-owned main-loop commit methods."""
from pathlib import Path
import os
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def method(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


@pytest.fixture(scope="module", params=[False, True], ids=["decoder-info", "producer-info"])
def jpeg_owner_binary(tmp_path_factory, request):
    folder = tmp_path_factory.mktemp("jpeg-owner")
    renderer = Path(os.environ.get("VIDEO_RENDERER_SOURCE", ROOT.parent / "esphome-video-renderer"))
    source_path = Path(os.environ.get("P4_JPEG_TEST_SOURCE", renderer / "esphome/components/p4_video_renderer/p4_video_renderer.cpp"))
    component = renderer / "esphome/components/p4_video_renderer"
    source = source_path.read_text()
    jpeg = (component / "p4_jpeg_decoder.cpp").read_text()
    presentation = (component / "p4_presentation.cpp").read_text()
    methods = "\n".join(method(source, name) for name in (
        "bool P4VideoRenderer::prepare(", "bool P4VideoRenderer::set_active(",
        "void P4VideoRenderer::stop()", "bool P4VideoRenderer::submit_frame(",
    ))
    methods += "\n" + "\n".join(method(jpeg, name) for name in (
        "bool P4VideoRenderer::decode_jpeg_access_unit_(",
        "uint16_t P4VideoRenderer::jpeg_storage_width_(", "uint16_t P4VideoRenderer::jpeg_storage_height_(",
        "bool P4VideoRenderer::read_jpeg_picture_info_(",
    ))
    methods += "\n" + "\n".join(method(presentation, name) for name in (
        "bool P4VideoRenderer::present_surface_direct_(",
        "bool P4VideoRenderer::refresh_layout_(", "void P4VideoRenderer::on_display_refresh_ready_(",
        "void P4VideoRenderer::loop()",
    ))
    worker = source.split("void P4VideoRenderer::rx_task_()", 1)[1].split("#else\n    uint8_t expected = 1;", 1)[1]
    worker = "uint8_t expected = 1;" + worker.split("\n#endif\n  }", 1)[0]
    code = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <future>
#include <mutex>
#include <vector>
#include "esphome/components/video_renderer/video_renderer.h"
#include "esphome/components/p4_video_renderer/p4_presentation_geometry.h"
using esphome::p4_video_renderer::fit_scale_units;
using esphome::p4_video_renderer::PPA_SCALE_UNITS;
using namespace esphome;
constexpr int ESP_OK=0,ESP_FAIL=1,ESP_ERR_INVALID_SIZE=2,ESP_ERR_INVALID_ARG=3,pdTRUE=1;
using esp_err_t=int;
enum {JPEG_DOWN_SAMPLING_YUV444,JPEG_DOWN_SAMPLING_YUV422,JPEG_DOWN_SAMPLING_YUV420,JPEG_DOWN_SAMPLING_GRAY};
struct jpeg_decode_picture_info_t{uint32_t width{},height{};int sample_method=JPEG_DOWN_SAMPLING_YUV420;};
struct PayloadFrame{uint16_t width,height,color;};
unsigned info_calls=0,decode_calls=0;
std::function<void()>decode_hook;
int jpeg_decoder_get_info(const uint8_t*p,uint32_t size,jpeg_decode_picture_info_t*out){assert(size==sizeof(PayloadFrame));PayloadFrame f;std::memcpy(&f,p,sizeof(f));*out={f.width,f.height,JPEG_DOWN_SAMPLING_YUV420};++info_calls;return ESP_OK;}
int jpeg_decoder_process(void*,int*,const uint8_t*p,uint32_t size,uint8_t*out,uint32_t capacity,uint32_t*written){
 assert(size==sizeof(PayloadFrame));PayloadFrame f;std::memcpy(&f,p,sizeof(f));++decode_calls;
 if(decode_hook)decode_hook();
 const uint32_t bytes=uint32_t(f.width)*f.height*2;if(bytes>capacity)return ESP_ERR_INVALID_ARG;
 std::fill(reinterpret_cast<uint16_t*>(out),reinterpret_cast<uint16_t*>(out)+bytes/2,f.color);*written=bytes;return ESP_OK;
}
struct lv_area_t{int x1{},y1{},x2{},y2{};};
struct lv_obj_t{int width=1280,height=720,x=0,y=0;};
lv_obj_t screen,other_screen;lv_obj_t*active_screen=&screen;
lv_obj_t*lv_obj_get_screen(lv_obj_t*){return &screen;}lv_obj_t*lv_screen_active(){return active_screen;}
int lv_obj_get_content_width(lv_obj_t*p){return p->width;}int lv_obj_get_content_height(lv_obj_t*p){return p->height;}
void lv_obj_get_coords(lv_obj_t*p,lv_area_t*a){*a={p->x,p->y,p->x+p->width-1,p->y+p->height-1};}
int lv_area_get_width(const lv_area_t*a){return a->x2-a->x1+1;}int lv_area_get_height(const lv_area_t*a){return a->y2-a->y1+1;}
unsigned invalidations=0;void lv_obj_invalidate(lv_obj_t*){++invalidations;}
enum{PPA_SRM_COLOR_MODE_RGB565=1,PPA_SRM_ROTATION_ANGLE_0=0,PPA_TRANS_MODE_BLOCKING=0};
struct ppa_srm_oper_config_t{
 struct{const void*buffer{};uint32_t pic_w{},pic_h{},block_w{},block_h{};int srm_cm{};}in;
 int rotation_angle{},mode{};float scale_x{},scale_y{};
};
namespace esphome::mipi_dsi{
struct MipiDsi{
 unsigned attempts=0,commits=0;int x{},y{},width{},height{};float scale{};uint16_t color{};bool failure=false;
 std::function<void()>before_admission,during_dma;
 bool supports_rgb565_transform()const{return true;}
 bool draw_rgb565_transformed(const ppa_srm_oper_config_t&op,size_t bytes,int xx,int yy,int w,int h,bool(*admit)(void*),void*ctx){
  ++attempts;if(before_admission)before_admission();if(!admit(ctx))return false;
  assert(op.rotation_angle==PPA_SRM_ROTATION_ANGLE_0&&op.mode==PPA_TRANS_MODE_BLOCKING);
  assert(op.in.srm_cm==PPA_SRM_COLOR_MODE_RGB565&&op.scale_x==op.scale_y);
  assert(bytes==size_t(op.in.pic_w)*op.in.pic_h*2);
  x=xx;y=yy;width=w;height=h;scale=op.scale_x;color=*static_cast<const uint16_t*>(op.in.buffer);
  const std::vector<uint8_t>snapshot(static_cast<const uint8_t*>(op.in.buffer),static_cast<const uint8_t*>(op.in.buffer)+bytes);
  if(during_dma)during_dma();
  assert(std::equal(snapshot.begin(),snapshot.end(),static_cast<const uint8_t*>(op.in.buffer)));
  if(failure){return false;}
  ++commits;return true;
 }
};}
const char*esp_err_to_name(int){return "injected";}
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
struct SlotState {
 std::atomic<uint8_t>value{0};std::function<void()>producer_before_cas;
 bool compare_exchange_strong(uint8_t&expected,uint8_t desired,std::memory_order order){if(expected==0&&producer_before_cas)producer_before_cas();return value.compare_exchange_strong(expected,desired,order);}
 void store(uint8_t v,std::memory_order order){value.store(v,order);}
};
bool xSemaphoreTake(std::mutex*m,int wait){assert(wait==0);return m->try_lock();}void xSemaphoreGive(std::mutex*m){m->unlock();}void xTaskNotifyGive(void*){}
struct P4VideoRenderer {
 lv_obj_t container;lv_obj_t*video_container_=&container;mipi_dsi::MipiDsi hardware;mipi_dsi::MipiDsi*display_=&hardware;
 std::atomic<uint32_t>layout_size_{(1280U<<16)|720U};
 static constexpr size_t surface_count_=1;size_t surface_capacity_bytes_=1280000;
 std::vector<uint8_t>storage=std::vector<uint8_t>(1280000);uint8_t*surfaces_[1]{storage.data()};
 size_t surface_data_size_[1]{};uint16_t surface_stride_bytes_[1]{},surface_content_width_[1]{},surface_content_height_[1]{};
 uint16_t max_decode_width_=800,max_decode_height_=800;void*jpeg_decoder_=this;int jpeg_decode_config_=0;
 video_renderer::JpegFrameInfo rx_au_jpeg_info_{};
 std::atomic<int>pending_surface_{-1};std::atomic<uint32_t>pending_surface_generation_{0},remote_frame_visible_{0};
 std::atomic<bool>rx_active_{false},rx_session_prepared_{false},rx_running_{true},video_ended_pending_{false},surface_ever_presented_{false},presentation_in_flight_{false};
 std::atomic<uint32_t>rx_session_generation_{0},rx_admitted_frames_{0},rx_rendered_frames_{0},rx_presented_frames_{0},rx_refresh_completed_{0};
 std::mutex presentation;std::mutex*presentation_mutex_=&presentation;void*rx_task_handle_=nullptr;video_renderer::StreamFormat rx_format_;uint8_t framerate_=10;
 struct{unsigned count=0;std::function<void()>callback;void trigger(){++count;if(callback)callback();}}first_frame_trigger_,video_ended_trigger_;
 std::vector<uint8_t>au=std::vector<uint8_t>(524288);uint8_t*rx_au_=au.data();size_t rx_au_capacity_=524288,rx_au_size_=0;uint32_t rx_au_generation_=0;uint64_t rx_timestamp_us_=0;bool rx_key_frame_=false;SlotState rx_slot_state_;
 bool is_failed(){return false;}void enable_loop_soon_any_context(){}void disable_loop(){}
 bool has_remote_frame()const{const auto g=remote_frame_visible_.load();return rx_active_&&g!=0&&g==rx_session_generation_;}
 bool prepare(const video_renderer::StreamFormat&);bool set_active(bool);void stop();void dump_state()const{};bool submit_frame(const video_renderer::EncodedFrame&);
 static uint16_t jpeg_storage_width_(const jpeg_decode_picture_info_t&);static uint16_t jpeg_storage_height_(const jpeg_decode_picture_info_t&);
 bool decode_jpeg_access_unit_(uint32_t);
 bool read_jpeg_picture_info_(const uint8_t*,size_t,const video_renderer::JpegFrameInfo&,jpeg_decode_picture_info_t*)const;
 bool present_surface_direct_(int);bool refresh_layout_();void loop();void on_display_refresh_ready_();
 void worker_one(){do{WORKER}while(false);}
 bool consume(const PayloadFrame&f){
  video_renderer::EncodedFrame frame{reinterpret_cast<const uint8_t*>(&f),sizeof(f),{0,true},true};
#ifdef FRAME_METADATA
  frame.jpeg_info={f.width,f.height,video_renderer::JpegSampling::YUV420};
#endif
  return submit_frame(frame);
 }
 void begin(){assert(prepare({video_renderer::Codec::JPEG,320,240,10})&&set_active(true));}
};
METHODS
int main(int argc,char**argv){
 assert(argc==2);const int mode=std::atoi(argv[1]);P4VideoRenderer r;PayloadFrame a{320,240,0xa1a1},b{640,480,0xb2b2};r.begin();
 if(mode==4||mode==5){
  active_screen=&other_screen;bool changed=false;r.first_frame_trigger_.callback=[&]{
   if(changed){return;}
   changed=true;r.stop();
   if(mode==5){r.begin();assert(r.consume(b));r.worker_one();active_screen=&screen;}
  };
  assert(r.consume(a));r.worker_one();r.loop();assert(changed);
  if(mode==4){assert(!r.rx_active_&&r.pending_surface_==-1);r.loop();assert(r.hardware.commits==0);}
  else{assert(r.pending_surface_==0);r.loop();assert(r.hardware.commits==1&&r.hardware.color==0xb2b2);}
  return 0;
 }
 if(mode==6){
  decode_hook=[&]{r.stop();};assert(r.consume(a));r.worker_one();decode_hook={};r.loop();
  assert(r.hardware.commits==0&&r.pending_surface_==-1&&!r.has_remote_frame());return 0;
 }
 if(mode==7){r.hardware.during_dma=[&]{assert(!r.consume(b));r.worker_one();r.stop();};}
 if(mode==8){r.hardware.failure=true;}
 if(mode==9){r.hardware.before_admission=[&]{r.stop();};}
 if(mode==3){active_screen=&other_screen;r.first_frame_trigger_.callback=[]{active_screen=&screen;};}
 if(mode==10){active_screen=&other_screen;r.remote_frame_visible_=r.rx_session_generation_.load();}
 assert(r.consume(a));r.worker_one();assert(r.pending_surface_==0&&r.rx_rendered_frames_==1);
 if(mode==2){const auto pixels=r.storage;assert(!r.consume(b));r.worker_one();assert(r.storage==pixels&&decode_calls==1);}
 r.loop();
 if(mode==3){assert(r.hardware.commits==0&&r.pending_surface_==0&&r.presentation_in_flight_);r.loop();assert(r.hardware.commits==0);r.on_display_refresh_ready_();r.loop();assert(r.hardware.commits==1);}
 if(mode==7||mode==8||mode==9||mode==10){assert(r.rx_presented_frames_==0&&r.pending_surface_==-1);if(mode==7||mode==9)assert(!r.has_remote_frame());return 0;}
 assert(r.hardware.commits==1&&r.rx_presented_frames_==1&&r.rx_refresh_completed_==1&&r.pending_surface_==-1);
 assert(r.hardware.x==180&&r.hardware.y==15&&r.hardware.width==920&&r.hardware.height==690&&r.hardware.scale==2.875f);
 assert(r.hardware.color==0xa1a1&&decode_calls==1);assert(r.first_frame_trigger_.count==1);
 if(mode==1){assert(r.consume(b));r.worker_one();r.loop();assert(r.hardware.commits==2&&r.hardware.color==0xb2b2);assert(r.surface_content_width_[0]==640&&r.surface_content_height_[0]==480);}
 if(mode==11){PayloadFrame portrait{480,640,0xc3c3};assert(r.consume(portrait));r.worker_one();r.loop();assert(r.hardware.commits==2&&r.hardware.color==0xc3c3);assert(r.hardware.width==540&&r.hardware.height==720);}
 r.stop();r.loop();assert(!r.has_remote_frame()&&r.pending_surface_==-1);assert(invalidations>0);
}
'''.replace("WORKER", worker).replace("METHODS", methods)
    cpp = folder / "jpeg_owner.cpp"
    cpp.write_text(code)
    binary = folder / "jpeg_owner"
    command = ["g++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
               "-DUSE_P4_VIDEO_RENDERER_JPEG", "-I", str(renderer)]
    if request.param:
        command.append("-DFRAME_METADATA")
    built = subprocess.run([*command, str(cpp), "-o", str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize("case", range(12), ids=[
    "one-SRM-legacy-fit", "resolution-growth", "pending-buffer-lease", "first-page-refresh",
    "callback-stop", "callback-redial", "stop-during-decode", "stop-during-DMA",
    "driver-rejection", "admission-after-wait", "inactive-page", "equal-area-portrait",
])
def test_actual_jpeg_worker_and_owned_main_loop(jpeg_owner_binary, case):
    result = subprocess.run([str(jpeg_owner_binary), str(case)], capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result.stderr
