"""Run the production JPEG worker and display admission with SDK-only stubs."""
from pathlib import Path
import os
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def method(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


@pytest.mark.parametrize("surfaces", [1, 2])
@pytest.mark.parametrize("resolution_case", ["pending", "growth", "equal-area"])
@pytest.mark.parametrize("metadata", [False, True])
def test_actual_jpeg_worker_accepts_owned_and_legacy_surfaces(tmp_path, surfaces, resolution_case, metadata):
    source_path = Path(os.environ.get("P4_JPEG_TEST_SOURCE", ROOT / "esphome/components/p4_video_renderer/p4_video_renderer.cpp"))
    source = source_path.read_text()
    per_frame = "read_jpeg_picture_info_(" in source
    methods = "\n".join(method(source, name) for name in (
        "bool P4VideoRenderer::start_video(",
        "bool P4VideoRenderer::set_video_active(",
        "bool P4VideoRenderer::consume_video_access_unit(",
        "uint16_t P4VideoRenderer::jpeg_storage_width_(",
        "uint16_t P4VideoRenderer::jpeg_storage_height_(",
        "bool P4VideoRenderer::read_jpeg_picture_info_(" if per_frame else "bool P4VideoRenderer::update_jpeg_picture_info_(",
        "bool P4VideoRenderer::present_native_rgb565_(",
        "bool P4VideoRenderer::present_surface_direct_(",
        "bool P4VideoRenderer::commit_direct_surface_(",
    ))
    worker = source.split("void P4VideoRenderer::rx_task_()", 1)[1]
    worker = worker.split("#else\n    uint8_t expected = 1;", 1)[1]
    worker = "uint8_t expected = 1;" + worker.split("\n#endif\n  }", 1)[0]
    code = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <future>
#include <mutex>
#include <vector>
constexpr int ESP_OK=0,ESP_FAIL=1,ESP_ERR_INVALID_SIZE=2,ESP_ERR_INVALID_ARG=3,pdTRUE=1;
constexpr int PPA_TRANS_MODE_BLOCKING=1,PPA_SRM_COLOR_MODE_RGB565=16;
using esp_err_t=int;
enum ppa_srm_rotation_angle_t {PPA_SRM_ROTATION_ANGLE_0,PPA_SRM_ROTATION_ANGLE_90,PPA_SRM_ROTATION_ANGLE_180,PPA_SRM_ROTATION_ANGLE_270};
struct Pic {const void*buffer{};size_t buffer_size{};int pic_w{},pic_h{},block_w{},block_h{},srm_cm{};};
struct ppa_srm_oper_config_t {Pic in,out;ppa_srm_rotation_angle_t rotation_angle{};float scale_x{},scale_y{};int mode{};};
enum {JPEG_DOWN_SAMPLING_YUV444,JPEG_DOWN_SAMPLING_YUV422,JPEG_DOWN_SAMPLING_YUV420,JPEG_DOWN_SAMPLING_GRAY};
struct jpeg_decode_picture_info_t{uint32_t width{},height{};int sample_method=JPEG_DOWN_SAMPLING_YUV420;};
struct EncodedFrame{uint16_t width,height,color;};
unsigned info_calls=0,decode_calls=0;
std::function<void()>decode_hook;
int jpeg_decoder_get_info(const uint8_t*p,uint32_t size,jpeg_decode_picture_info_t*out){assert(size==sizeof(EncodedFrame));EncodedFrame f;std::memcpy(&f,p,sizeof(f));*out={f.width,f.height,JPEG_DOWN_SAMPLING_YUV420};++info_calls;return ESP_OK;}
int jpeg_decoder_process(void*,int*,const uint8_t*p,uint32_t size,uint8_t*out,uint32_t capacity,uint32_t*written){
 assert(size==sizeof(EncodedFrame));EncodedFrame f;std::memcpy(&f,p,sizeof(f));++decode_calls;
 if(decode_hook)decode_hook();
 const uint32_t bytes=uint32_t(f.width)*f.height*2;if(bytes>capacity)return ESP_ERR_INVALID_ARG;
 std::fill(reinterpret_cast<uint16_t*>(out),reinterpret_cast<uint16_t*>(out)+bytes/2,f.color);*written=bytes;return ESP_OK;
}
struct lv_area_t{int x1=2,y1=3,x2=641,y2=482;};
void lv_obj_get_coords(void*p,lv_area_t*out){*out=*static_cast<lv_area_t*>(p);}
int lv_area_get_width(const lv_area_t*p){return p->x2-p->x1+1;}int lv_area_get_height(const lv_area_t*p){return p->y2-p->y1+1;}
const char*esp_err_to_name(int){return "injected";}
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
int ppa_do_scale_rotate_mirror(void*,const ppa_srm_oper_config_t*c){auto value=*static_cast<const uint16_t*>(c->in.buffer);std::fill(const_cast<uint16_t*>(static_cast<const uint16_t*>(c->out.buffer)),const_cast<uint16_t*>(static_cast<const uint16_t*>(c->out.buffer))+c->out.pic_w*c->out.pic_h,value);return ESP_OK;}
namespace display {constexpr int COLOR_ORDER_RGB=0,COLOR_BITNESS_565=1;struct Display {
 unsigned submissions=0;uint16_t visible=0;
 int get_native_width(){return 800;}int get_native_height(){return 1280;}
 void draw_pixels_at(int,int,int,int,const uint8_t*p,int,int,bool){visible=*reinterpret_cast<const uint16_t*>(p);++submissions;}
};}
struct OwnedDisplay:display::Display {
 int input_width=0,input_height=0;
 bool supports_native_rgb565(){return true;}
 bool draw_native_rgb565(int x,int y,int w,int h,const uint8_t*p,size_t){draw_pixels_at(x,y,w,h,p,0,1,false);return true;}
 bool draw_rgb565_transformed(const ppa_srm_oper_config_t&c,size_t bytes,int,int,int,int,bool(*admit)(void*),void*ctx){
  assert(bytes>=size_t(c.in.pic_w*c.in.pic_h*2));if(!admit(ctx))return false;input_width=c.in.pic_w;input_height=c.in.pic_h;visible=*static_cast<const uint16_t*>(c.in.buffer);++submissions;return true;
 }
};
namespace voip_stack {
enum class JpegSampling:uint8_t{UNKNOWN,YUV444,YUV422,YUV420,GRAY};
struct JpegFrameInfo{uint16_t width{},height{};JpegSampling sampling=JpegSampling::UNKNOWN;bool is_set()const{return width||height||sampling!=JpegSampling::UNKNOWN;}};
struct VideoCapability{uint8_t max_fps=10;bool valid()const{return true;}bool is_jpeg()const{return true;}};
struct EncodedVideoAccessUnit{const uint8_t*data;size_t size;uint32_t timestamp_90khz;bool key_frame=true;void(*release_callback)(void*)=nullptr;void*release_ctx=nullptr;JpegFrameInfo jpeg_info{};};}
struct SlotState {
 std::atomic<uint8_t>value{0};std::function<void()>producer_before_cas;
 bool compare_exchange_strong(uint8_t&expected,uint8_t desired,std::memory_order order){if(expected==0&&producer_before_cas)producer_before_cas();return value.compare_exchange_strong(expected,desired,order);}
 void store(uint8_t v,std::memory_order order){value.store(v,order);}
};
bool xSemaphoreTake(std::mutex*m,int wait){assert(wait==0);return m->try_lock();}void xSemaphoreGive(std::mutex*m){m->unlock();}void xTaskNotifyGive(void*){}
struct P4VideoRenderer {
 OwnedDisplay display;display::Display*direct_display_=&display;OwnedDisplay*owned_display_=&display;void*direct_display_ppa_=this;
 lv_area_t container;void*video_container_=&container;uint16_t display_rotation_=0;
 size_t surface_count_=SURFACES,surface_capacity_bytes_=1280000;
 std::vector<uint8_t>storage[2]={std::vector<uint8_t>(1280000),std::vector<uint8_t>(SURFACES==2?1280000:0)};uint8_t*surfaces_[2]{storage[0].data(),SURFACES==2?storage[1].data():nullptr};
 size_t surface_data_size_[2]{};uint16_t surface_stride_bytes_[2]{},surface_content_width_[2]{},surface_content_height_[2]{};
 uint16_t max_decode_width_=800,max_decode_height_=800,decoded_storage_width_=0,decoded_storage_height_=0;
 jpeg_decode_picture_info_t jpeg_picture_info_;bool jpeg_picture_info_valid_=false;uint32_t jpeg_picture_info_generation_=0;void*jpeg_decoder_=this;int jpeg_decode_config_=0;
 voip_stack::JpegFrameInfo rx_au_jpeg_info_{};
 std::atomic<int>pending_surface_{-1},front_surface_{0};std::atomic<uint32_t>pending_surface_generation_{0};
 std::atomic<bool>rx_active_{false},rx_session_prepared_{false},rx_running_{true},remote_frame_visible_{false},video_ended_pending_{false},surface_ever_presented_{false};
 std::atomic<uint32_t>rx_session_generation_{0},rx_admitted_frames_{0},rx_rendered_frames_{0},rx_presented_frames_{0},rx_refresh_completed_{0};
 std::mutex presentation;std::mutex*presentation_mutex_=&presentation;void*rx_task_handle_=nullptr;voip_stack::VideoCapability rx_capability_;uint8_t framerate_=10;
 struct{void reset(uint8_t){}bool accept(uint32_t){return true;}}cadence_;
 std::vector<uint8_t>au=std::vector<uint8_t>(524288);uint8_t*rx_au_=au.data();size_t rx_au_capacity_=524288,rx_au_size_=0;uint32_t rx_au_generation_=0,rx_timestamp_=0;bool rx_key_frame_=false;SlotState rx_slot_state_;
 bool is_failed(){return false;}void enable_loop_soon_any_context(){}
 bool start_video(const voip_stack::VideoCapability&);bool set_video_active(bool);bool consume_video_access_unit(const voip_stack::EncodedVideoAccessUnit&);
 static uint16_t jpeg_storage_width_(const jpeg_decode_picture_info_t&);static uint16_t jpeg_storage_height_(const jpeg_decode_picture_info_t&);bool update_jpeg_picture_info_(const uint8_t*,size_t);
 bool read_jpeg_picture_info_(const uint8_t*,size_t,const voip_stack::JpegFrameInfo&,jpeg_decode_picture_info_t*)const;
 bool present_native_rgb565_(int,int,int,int,const uint8_t*,size_t);bool present_surface_direct_(int);bool commit_direct_surface_(int);
 void worker_one(){do{WORKER}while(false);}
 bool consume(const EncodedFrame&f,uint32_t ts){
  voip_stack::EncodedVideoAccessUnit au{reinterpret_cast<const uint8_t*>(&f),sizeof(f),ts,true};
#ifdef FRAME_METADATA
  au.jpeg_info={f.width,f.height,voip_stack::JpegSampling::YUV420};
#endif
  return consume_video_access_unit(au);
 }
};
METHODS
int main(){
 {
  P4VideoRenderer initial;EncodedFrame frame{32,32,0xa1a1};
  assert(initial.start_video({})&&initial.set_video_active(true)&&initial.consume(frame,0));
  initial.worker_one();assert(initial.rx_rendered_frames_==1&&initial.pending_surface_>=0);
  assert(initial.commit_direct_surface_(initial.pending_surface_.load(std::memory_order_acquire)));
  assert(initial.display.visible==0xa1a1);
 }
 info_calls=decode_calls=0;
 if(RESOLUTION_CASE!=0){
  P4VideoRenderer changed;assert(changed.start_video({})&&changed.set_video_active(true));
  EncodedFrame first=RESOLUTION_CASE==1?EncodedFrame{320,240,0xaaaa}:EncodedFrame{640,480,0xaaaa};
  EncodedFrame second=RESOLUTION_CASE==1?EncodedFrame{640,480,0xbbbb}:EncodedFrame{480,640,0xbbbb};
  assert(changed.consume(first,0));changed.worker_one();
  int pending=changed.pending_surface_.load(std::memory_order_acquire);assert(pending>=0&&changed.commit_direct_surface_(pending));
  assert(changed.consume(second,9000));changed.worker_one();
  pending=changed.pending_surface_.load(std::memory_order_acquire);
  assert(changed.rx_rendered_frames_==2&&pending>=0);
  assert(changed.surface_stride_bytes_[pending]==second.width*2);
  assert(changed.surface_data_size_[pending]==size_t(second.width)*second.height*2);
  assert(changed.commit_direct_surface_(pending)&&changed.display.visible==0xbbbb);
  if(SURFACES==1)assert(changed.display.input_width==second.width&&changed.display.input_height==second.height);
  return 0;
 }
 P4VideoRenderer r;assert(r.start_video({})&&r.set_video_active(true));EncodedFrame a{32,32,0xa1a1},b{16,16,0xb2b2};
 assert(r.consume(a,0));
 std::promise<void>decoding,finish_decode,producer_checked,finish_consume;auto decode_permit=finish_decode.get_future().share();auto producer_permit=finish_consume.get_future().share();
 decode_hook=[&]{decoding.set_value();decode_permit.wait();};auto worker=std::async(std::launch::async,[&]{r.worker_one();});decoding.get_future().wait();
 r.rx_slot_state_.producer_before_cas=[&]{producer_checked.set_value();producer_permit.wait();};auto producer=std::async(std::launch::async,[&]{return r.consume(b,9000);});producer_checked.get_future().wait();
 finish_decode.set_value();worker.get();
 // Real prepare/consume/update-info/decode/publication must work with one
 // source buffer. The old unconditional second-surface guard fails here.
 constexpr unsigned expected_info_calls=
#if defined(PER_FRAME) && defined(FRAME_METADATA)
  0;
#else
  1;
#endif
 assert(r.rx_rendered_frames_==1&&r.pending_surface_>=0&&info_calls==expected_info_calls&&decode_calls==1);
 int pending=r.pending_surface_.load(std::memory_order_acquire);const auto immutable=r.storage[pending];
 finish_consume.set_value();assert(producer.get());r.rx_slot_state_.producer_before_cas={};decode_hook={};
 assert(r.rx_session_generation_==r.rx_au_generation_);
 r.worker_one();
 // B cannot parse/redecode while A owns the published source, including a
 // consume that passed its pending precheck before A's publication.
 assert(info_calls==expected_info_calls&&decode_calls==1&&r.surface_stride_bytes_[pending]==64&&r.surface_data_size_[pending]==2048);
 assert(r.pending_surface_==pending&&r.storage[pending]==immutable&&r.rx_rendered_frames_==1);
 assert(r.commit_direct_surface_(pending)&&r.display.visible==0xa1a1&&r.rx_presented_frames_==1&&r.pending_surface_==-1);
 // B is legal after the pending source is released. Its smaller native
 // output triggers the actual worker reparse and per-surface metadata update.
 assert(r.consume(b,18000));r.worker_one();assert(info_calls==expected_info_calls*2&&decode_calls==2);
 pending=r.pending_surface_.load(std::memory_order_acquire);assert(pending>=0&&r.surface_stride_bytes_[pending]==32&&r.surface_data_size_[pending]==512);
 assert(r.commit_direct_surface_(pending)&&r.display.visible==0xb2b2&&r.rx_presented_frames_==2);
#ifdef PER_FRAME
 // Invalid metadata is rejected before decode. A valid but wrong size cannot
 // publish a surface when the hardware's actual output disagrees.
 for(int kind=0;kind<6;kind++){
  P4VideoRenderer invalid;assert(invalid.start_video({})&&invalid.set_video_active(true));
  EncodedFrame encoded{16,16,0xdddd};
  voip_stack::EncodedVideoAccessUnit au{reinterpret_cast<const uint8_t*>(&encoded),sizeof(encoded),0,true};
  au.jpeg_info={16,16,voip_stack::JpegSampling::YUV420};
  if(kind==0)au.jpeg_info.sampling=voip_stack::JpegSampling::GRAY;
  if(kind==1)au.jpeg_info.sampling=static_cast<voip_stack::JpegSampling>(255);
  if(kind==2)au.jpeg_info.width=816;
  if(kind==3)au.jpeg_info.height=0;
  if(kind==4)au.jpeg_info={32,32,voip_stack::JpegSampling::YUV420};
  if(kind==5){encoded={32,32,0xdddd};au.jpeg_info={16,16,voip_stack::JpegSampling::YUV420};}
  const auto before_info=info_calls,before_decode=decode_calls;
  assert(invalid.consume_video_access_unit(au));invalid.worker_one();
  assert(info_calls==before_info&&decode_calls==before_decode+(kind>=4));
  assert(invalid.pending_surface_==-1&&invalid.rx_rendered_frames_==0&&invalid.display.submissions==0);
 }
#endif
 // A declared two-surface path still requires its legacy scratch target.
 if(SURFACES==2){r.surfaces_[1]=nullptr;
#ifdef PER_FRAME
  jpeg_decode_picture_info_t picture{};assert(!r.read_jpeg_picture_info_(reinterpret_cast<const uint8_t*>(&a),sizeof(a),{},&picture));
#else
  assert(!r.update_jpeg_picture_info_(reinterpret_cast<const uint8_t*>(&a),sizeof(a)));
#endif
 }
}
'''.replace("SURFACES", str(surfaces)).replace("RESOLUTION_CASE", str(["pending", "growth", "equal-area"].index(resolution_case))).replace("WORKER", worker).replace("METHODS", methods)
    cpp = tmp_path / "jpeg_worker_flow.cpp"
    cpp.write_text(code)
    binary = tmp_path / "jpeg_worker_flow"
    defines = (["-DPER_FRAME"] if per_frame else []) + (["-DFRAME_METADATA"] if metadata else [])
    result = subprocess.run([
        "g++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
        "-DUSE_P4_VIDEO_RENDERER_JPEG", "-DUSE_P4_VIDEO_RENDERER_DIRECT_DISPLAY",
        "-DUSE_MIPI_DSI_RGB565_OWNED_TRANSFORM", *defines, str(cpp), "-o", str(binary),
    ], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result.stderr
