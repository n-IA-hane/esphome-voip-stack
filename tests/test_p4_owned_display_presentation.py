"""Production presentation contracts at the blocking display/PPA boundary."""
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def extract(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


@pytest.mark.parametrize("provider", ["owned", "old-provider", "runtime-unsupported"])
@pytest.mark.parametrize("codec", ["JPEG", "H264"])
def test_native_blit_geometry_and_jpeg_source_lifetime(tmp_path, provider, codec):
    source = (ROOT / "esphome/components/p4_video_renderer/p4_video_renderer.cpp").read_text()
    signatures = ["bool P4VideoRenderer::present_native_rgb565_(", "bool P4VideoRenderer::present_surface_direct_("]
    if codec == "JPEG":
        signatures += ["bool P4VideoRenderer::start_video(", "bool P4VideoRenderer::set_video_active(",
                       "void P4VideoRenderer::stop_video()", "bool P4VideoRenderer::consume_video_access_unit("]
    methods = "\n".join(extract(source, s) for s in signatures)
    code = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <future>
#include <functional>
#include <mutex>
#include <vector>
using namespace std::chrono_literals;
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=1,pdTRUE=1,PPA_TRANS_MODE_BLOCKING=1,PPA_SRM_COLOR_MODE_RGB565=16;
enum ppa_srm_rotation_angle_t {PPA_SRM_ROTATION_ANGLE_0,PPA_SRM_ROTATION_ANGLE_90,PPA_SRM_ROTATION_ANGLE_180,PPA_SRM_ROTATION_ANGLE_270};
struct Pic {const void*buffer{};size_t buffer_size{};int pic_w{},pic_h{},block_w{},block_h{},block_offset_x{},block_offset_y{},srm_cm{};};
struct ppa_srm_oper_config_t {Pic in,out;ppa_srm_rotation_angle_t rotation_angle{};float scale_x{},scale_y{};int mode{};};
struct lv_area_t{int x1=2,y1=3,x2=7,y2=6;};
void lv_obj_get_coords(void*p,lv_area_t*out){*out=*static_cast<lv_area_t*>(p);}
int lv_area_get_width(const lv_area_t*p){return p->x2-p->x1+1;}int lv_area_get_height(const lv_area_t*p){return p->y2-p->y1+1;}
int ppa_do_scale_rotate_mirror(void*,const ppa_srm_oper_config_t*c){
 assert(c->mode==PPA_TRANS_MODE_BLOCKING);auto*src=static_cast<const uint16_t*>(c->in.buffer);auto*out=const_cast<uint16_t*>(static_cast<const uint16_t*>(c->out.buffer));
 int sw=int(c->in.block_w*c->scale_x),sh=int(c->in.block_h*c->scale_y);
 bool swap=c->rotation_angle==PPA_SRM_ROTATION_ANGLE_90||c->rotation_angle==PPA_SRM_ROTATION_ANGLE_270;
 int w=swap?sh:sw,h=swap?sw:sh;
 for(int y=0;y<h;y++)for(int x=0;x<w;x++){int ix=x,iy=y;switch(c->rotation_angle){case PPA_SRM_ROTATION_ANGLE_90:ix=sw-1-y;iy=x;break;case PPA_SRM_ROTATION_ANGLE_180:ix=sw-1-x;iy=sh-1-y;break;case PPA_SRM_ROTATION_ANGLE_270:ix=y;iy=sh-1-x;break;default:break;}ix=int(ix/c->scale_x);iy=int(iy/c->scale_y);out[y*w+x]=src[iy*c->in.pic_w+ix];}
 return 0;
}
const char*esp_err_to_name(int){return "injected";}
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
namespace display {constexpr int COLOR_ORDER_RGB=0,COLOR_BITNESS_565=1;struct Display {
 int rotation=0;unsigned generic_blits=0,native_blits=0;std::vector<uint16_t>pixels=std::vector<uint16_t>(19*17,0xaaaa);
 virtual ~Display()=default;int get_native_width(){return 19;}int get_native_height(){return 17;}
 void blit(int x,int y,int w,int h,const uint8_t*p){assert(x>=0&&y>=0&&x+w<=19&&y+h<=17);for(int row=0;row<h;row++)std::memcpy(pixels.data()+(y+row)*19+x,p+row*w*2,w*2);}
 void draw_pixels_at(int x,int y,int w,int h,const uint8_t*p,int,int,bool){++generic_blits;blit(x,y,w,h,p);}
};}
struct OwnedDisplay:display::Display {
 bool capable=true,reject=false;std::mutex framebuffer;std::atomic<unsigned>transforms{0};std::promise<void>*entered=nullptr;std::shared_future<void>permit;
 bool supports_native_rgb565(){return true;}bool supports_rgb565_transform(){return capable;}
 bool draw_native_rgb565(int x,int y,int w,int h,const uint8_t*p,size_t bytes){assert(bytes>=size_t(w*h*2));std::lock_guard owner(framebuffer);++native_blits;blit(x,y,w,h,p);return true;}
 bool draw_rgb565_transformed(const ppa_srm_oper_config_t&in,size_t bytes,int x,int y,int w,int h,bool(*admit)(void*),void*ctx){
  std::lock_guard owner(framebuffer);if(!admit(ctx)||reject)return false;assert(bytes>=size_t(in.in.pic_w*in.in.pic_h*2));++transforms;
  std::vector<uint16_t>immutable(static_cast<const uint16_t*>(in.in.buffer),static_cast<const uint16_t*>(in.in.buffer)+bytes/2);
  if(entered){entered->set_value();permit.wait();}assert(std::equal(immutable.begin(),immutable.end(),static_cast<const uint16_t*>(in.in.buffer)));
  auto c=in;std::vector<uint16_t>rotated(w*h);c.out.buffer=rotated.data();ppa_do_scale_rotate_mirror(this,&c);blit(x,y,w,h,reinterpret_cast<const uint8_t*>(rotated.data()));return admit(ctx);
 }
};
namespace voip_stack {struct VideoCapability {uint8_t max_fps=10;bool valid()const{return true;}bool is_jpeg()const{return true;}};
struct EncodedVideoAccessUnit{const uint8_t*data;size_t size;uint32_t timestamp_90khz=0;bool key_frame=true;};}
bool xSemaphoreTake(std::mutex*m,int wait){assert(wait==0);return m->try_lock();}void xSemaphoreGive(std::mutex*m){m->unlock();}void xTaskNotifyGive(void*){}
struct SlotState {
 std::atomic<uint8_t>value{0};std::function<void()>before_exchange;
 bool compare_exchange_strong(uint8_t&expected,uint8_t desired,std::memory_order order){if(before_exchange)before_exchange();return value.compare_exchange_strong(expected,desired,order);}
 void store(uint8_t v,std::memory_order order){value.store(v,order);}
};
struct P4VideoRenderer {
 display::Display*direct_display_;OwnedDisplay*owned_display_;void*direct_display_ppa_=this;lv_area_t container;void*video_container_=&container;
 uint16_t display_rotation_=0;size_t surface_count_=2,surface_capacity_bytes_=128;int decoded_storage_width_=8,decoded_storage_height_=6;
 std::vector<uint16_t>storage[2]={std::vector<uint16_t>(64),std::vector<uint16_t>(64)};uint8_t*surfaces_[2]{};
 uint16_t surface_content_width_[2]{6,6},surface_content_height_[2]{4,4},surface_stride_bytes_[2]{16,16};size_t surface_data_size_[2]{96,96};
 int surface_native_x_[2]{2,2},surface_native_y_[2]{3,3};uint64_t surface_layout_area_[2]{1,1};uint32_t surface_native_size_[2]{(19<<16)|17,(19<<16)|17};
 std::atomic<uint64_t>direct_layout_area_{1};std::atomic<uint32_t>direct_layout_native_size_{(19<<16)|17};
 std::atomic<int>pending_surface_{0},front_surface_{1};std::atomic<bool>rx_active_{true},rx_session_prepared_{true},rx_running_{true},remote_frame_visible_{true},video_ended_pending_{false};
 std::atomic<uint32_t>rx_session_generation_{1},pending_surface_generation_{1};uint32_t rx_au_generation_=1;
 std::atomic<uint32_t>rx_admitted_frames_{0},rx_rendered_frames_{0},rx_presented_frames_{0},rx_refresh_completed_{0};
 std::mutex presentation;std::mutex*presentation_mutex_=&presentation;void*rx_task_handle_=nullptr;
 voip_stack::VideoCapability rx_capability_;uint8_t framerate_=10;struct{void reset(uint8_t){}bool accept(uint32_t){return true;}}cadence_;
 uint8_t au[128]{};uint8_t*rx_au_=au;size_t rx_au_capacity_=128,rx_au_size_=0;uint32_t rx_timestamp_=0;bool rx_key_frame_=false;SlotState rx_slot_state_;
 explicit P4VideoRenderer(OwnedDisplay&d):direct_display_(&d),owned_display_(&d){for(int b=0;b<2;b++){surfaces_[b]=reinterpret_cast<uint8_t*>(storage[b].data());for(size_t i=0;i<storage[b].size();i++)storage[b][i]=100+i;}}
 bool is_failed(){return false;}void enable_loop_soon_any_context(){}
 bool present_native_rgb565_(int,int,int,int,const uint8_t*,size_t);bool present_surface_direct_(int);
 bool start_video(const voip_stack::VideoCapability&);bool set_video_active(bool);void stop_video();bool consume_video_access_unit(const voip_stack::EncodedVideoAccessUnit&);
};
''' + methods + r'''
int main(){
 // Already-oriented H.264 and legacy JPEG output must go to native coordinates
 // even when the logical LVGL display has a quarter-turn rotation.
 for(int rotation:{0,90,180,270}){
  OwnedDisplay d;d.rotation=rotation;P4VideoRenderer r(d);r.display_rotation_=rotation;
#ifdef OWNED
  r.surface_count_=1;
#endif
#ifdef USE_P4_VIDEO_RENDERER_H264
  r.surface_content_width_[0]=6;r.surface_content_height_[0]=4;r.surface_stride_bytes_[0]=12;r.surface_data_size_[0]=48;
  assert(r.present_surface_direct_(0));
  auto expected=std::vector<uint16_t>(19*17,0xaaaa);for(int y=0;y<4;y++)for(int x=0;x<6;x++)expected[(y+3)*19+x+2]=r.storage[0][y*6+x];
#else
  assert(r.present_surface_direct_(0));auto expected=std::vector<uint16_t>(19*17,0xaaaa);
  for(int y=0;y<4;y++)for(int x=0;x<6;x++){int nx=x+2,ny=y+3;if(rotation==90){nx=19-(y+3)-1;ny=x+2;}else if(rotation==180){nx=19-(x+2)-1;ny=17-(y+3)-1;}else if(rotation==270){nx=y+3;ny=17-(x+2)-1;}expected[ny*19+nx]=r.storage[0][y*8+x];}
#endif
  assert(d.pixels==expected&&r.pending_surface_==-1);
#ifdef USE_MIPI_DSI_RGB565_OWNED_TRANSFORM
  assert(d.generic_blits==0);
#else
  assert(d.generic_blits==1);
#endif
 }
#if defined(OWNED) && defined(USE_P4_VIDEO_RENDERER_JPEG)
 // A consume may pass pending<0, then win the AU slot only after the decoder
 // publishes its old surface and releases that slot. Stop/redial before the
 // presentation admission must not relabel that surface with reused AU data.
 {
  OwnedDisplay reused;P4VideoRenderer old(reused);old.surface_count_=1;old.pending_surface_=-1;
  std::promise<void>before_cas,resume_cas;auto permit=resume_cas.get_future().share();
  old.rx_slot_state_.before_exchange=[&]{before_cas.set_value();permit.wait();};
  uint8_t packet[4]{8,7,6,5};auto consume=std::async(std::launch::async,[&]{return old.consume_video_access_unit({packet,sizeof(packet)});});
  before_cas.get_future().wait();old.pending_surface_.store(0,std::memory_order_release);
  std::unique_lock presentation_owner(old.presentation);old.stop_video();assert(old.pending_surface_==0);
  assert(old.start_video({})&&old.set_video_active(true));resume_cas.set_value();assert(consume.get());
  assert(old.rx_au_generation_==old.rx_session_generation_);presentation_owner.unlock();
  assert(!old.present_surface_direct_(0)&&reused.transforms==0);
 }
 // Delay an accepted display transaction. SIP stop/redial is nonblocking,
 // but the actual admission owner rejects a replacement AU until completion.
 OwnedDisplay d;P4VideoRenderer r(d);r.surface_count_=1;
 std::promise<void>entered,release;d.entered=&entered;d.permit=release.get_future().share();
 auto render=std::async(std::launch::async,[&]{std::lock_guard owner(r.presentation);bool result=r.present_surface_direct_(0);int expected=0;r.pending_surface_.compare_exchange_strong(expected,-1);return result;});
 entered.get_future().wait();assert(render.wait_for(20ms)==std::future_status::timeout&&r.pending_surface_==0);
 const auto original=r.storage[0];r.stop_video();assert(!r.rx_active_&&!r.rx_session_prepared_&&r.pending_surface_==0);
 assert(r.start_video({})&&r.set_video_active(true));
 uint8_t packet[4]{1,2,3,4};assert(!r.consume_video_access_unit({packet,sizeof(packet)}));assert(r.storage[0]==original&&r.pending_surface_==0);
 auto repaint=std::async(std::launch::async,[&]{std::lock_guard owner(d.framebuffer);std::fill(d.pixels.begin(),d.pixels.end(),0xbeef);});
 assert(repaint.wait_for(20ms)==std::future_status::timeout);release.set_value();assert(!render.get());repaint.get();
 assert(r.pending_surface_==-1&&d.transforms==1);for(auto pixel:d.pixels)assert(pixel==0xbeef);
 assert(r.consume_video_access_unit({packet,sizeof(packet)}));assert(r.rx_au_generation_==r.rx_session_generation_);
 // A stale pending surface that reaches the framebuffer queue after stop or
 // replacement is rejected before any visible write or callback.
 for(int replacement:{0,1}){OwnedDisplay stale;P4VideoRenderer old(stale);old.surface_count_=1;if(replacement)old.pending_surface_=1;else old.rx_session_generation_++;assert(!old.present_surface_direct_(0));assert(stale.transforms==0);for(auto pixel:stale.pixels)assert(pixel==0xaaaa);}
 OwnedDisplay error;error.reject=true;P4VideoRenderer rejected(error);rejected.surface_count_=1;assert(!rejected.present_surface_direct_(0));assert(rejected.pending_surface_==0&&error.transforms==0);
#endif
}
'''
    cpp = tmp_path / "presentation.cpp"
    cpp.write_text(code)
    binary = tmp_path / "presentation"
    defines = [f"-DUSE_P4_VIDEO_RENDERER_{codec}", "-DUSE_P4_VIDEO_RENDERER_DIRECT_DISPLAY"]
    if provider != "old-provider":
        defines.append("-DUSE_MIPI_DSI_RGB565_OWNED_TRANSFORM")
    if provider == "owned":
        defines.append("-DOWNED")
    result = subprocess.run(["g++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                             "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                             *defines, str(cpp), "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result.stderr
