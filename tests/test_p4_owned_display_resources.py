"""The actual renderer allocation owner keeps legacy provider interoperability."""
from pathlib import Path
import subprocess
import pytest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "esphome/components/p4_video_renderer"


def extract(text, signature):
    start = text.index(signature)
    return text[start:text.index("\n}", start) + 2]


@pytest.mark.parametrize("provider", ["old-provider", "owned", "runtime-unsupported", "other-provider", "no-direct"])
def test_renderer_reserves_one_surface_only_for_the_owned_provider(tmp_path, provider):
    source = (SOURCE / "p4_video_renderer.cpp").read_text()
    header = (SOURCE / "p4_video_renderer.h").read_text()
    setters = "  void set_direct_display(" + header.split("  void set_direct_display(", 1)[1].split("  void set_display_rotation", 1)[0]
    setup = extract(source, "void P4VideoRenderer::setup()")
    allocate = extract(source, "bool P4VideoRenderer::allocate_session_resources_()")
    code = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
constexpr int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
constexpr int JPEG=1;
std::map<void*,size_t>allocations;
void*heap_caps_aligned_alloc(size_t,size_t bytes,int){void*p=std::malloc(bytes);assert(p);allocations[p]=bytes;return p;}
void*alloc_surface(size_t bytes,size_t){return heap_caps_aligned_alloc(64,bytes,0);}
void heap_caps_free(void*p){if(p){assert(allocations.erase(p));std::free(p);}}
void*xSemaphoreCreateBinaryStatic(int*){return reinterpret_cast<void*>(1);}
void*xSemaphoreCreateMutexStatic(int*){return reinterpret_cast<void*>(1);}
#define ESP_LOGE(...) ((void)0)
namespace display {struct Display{virtual ~Display()=default;};}
namespace mipi_dsi {struct MipiDsi:display::Display {
 bool supported=true,ready=false;
#ifdef USE_MIPI_DSI_RGB565_OWNED_TRANSFORM
 bool supports_rgb565_transform(){assert(ready);return supported;}
#endif
};}
struct P4VideoRenderer {
 int rx_done_storage_{},presentation_mutex_storage_{};void*rx_done_{},*presentation_mutex_{};
 bool failed=false,worker=false,disabled=false;unsigned legacy_clients=0;
 std::atomic<bool>rx_running_{false};
 display::Display*direct_display_{};
#ifdef USE_MIPI_DSI_RGB565_OWNED_TRANSFORM
 mipi_dsi::MipiDsi*owned_display_{};
#endif
 uint16_t max_decode_width_=800,max_decode_height_=800;size_t surface_count_=2,surface_capacity_bytes_=0;
 static constexpr size_t kMaxAccessUnitBytes=512*1024;
 uint8_t*rx_au_{};size_t rx_au_capacity_{};uint8_t*surfaces_[2]{};
 size_t surface_data_size_[2]{};uint16_t surface_stride_bytes_[2]{},surface_content_width_[2]{},surface_content_height_[2]{};
 bool init_jpeg_decoder_(){return true;}bool init_direct_display_ppa_(){++legacy_clients;return true;}
 void mark_failed(){failed=true;}void disable_loop(){disabled=true;}bool start_rx_task_(){worker=true;return true;}
 void free_codec_resources_(){heap_caps_free(rx_au_);rx_au_=nullptr;}
 void free_unpublished_surfaces_(){for(auto*&p:surfaces_){heap_caps_free(p);p=nullptr;}}
 void setup();bool allocate_session_resources_();
''' + setters + "\n};\n" + allocate + "\n" + setup + r'''
int main(){
 P4VideoRenderer r;mipi_dsi::MipiDsi screen;screen.ready=true;
#ifdef RUNTIME_UNSUPPORTED
 screen.supported=false;
#endif
#ifdef OTHER_PROVIDER
 display::Display other;r.set_direct_display(&other);
#else
 r.set_direct_display(&screen);
#endif
 r.setup();assert(!r.failed&&r.worker&&r.disabled);
#ifdef EXPECT_OWNED
 assert(r.surface_count_==1&&r.surfaces_[0]&&r.surfaces_[1]==nullptr&&r.legacy_clients==0);
#else
 assert(r.surface_count_==2&&r.surfaces_[0]&&r.surfaces_[1]);
#ifdef USE_P4_VIDEO_RENDERER_DIRECT_DISPLAY
 assert(r.legacy_clients==1);
#endif
#endif
 size_t total=0;for(auto&a:allocations)total+=a.second;
 assert(total==512*1024+r.surface_count_*1280000);
 r.free_codec_resources_();r.free_unpublished_surfaces_();assert(allocations.empty());
}
'''
    cpp = tmp_path / "resources.cpp"
    cpp.write_text(code)
    binary = tmp_path / "resources"
    defines = ["-DUSE_P4_VIDEO_RENDERER_JPEG"]
    if provider != "no-direct":
        defines.append("-DUSE_P4_VIDEO_RENDERER_DIRECT_DISPLAY")
    if provider != "old-provider":
        defines.append("-DUSE_MIPI_DSI_RGB565_OWNED_TRANSFORM")
    if provider == "owned":
        defines.append("-DEXPECT_OWNED")
    if provider == "runtime-unsupported":
        defines.append("-DRUNTIME_UNSUPPORTED")
    if provider == "other-provider":
        defines.append("-DOTHER_PROVIDER")
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", *defines,
                             str(cpp), "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
