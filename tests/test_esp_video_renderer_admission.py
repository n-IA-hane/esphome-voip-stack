"""Production JPEG admission keeps wall-clock rate without jitter phase lockout."""
from pathlib import Path
import json
import os
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "esphome/components/esp_video_renderer"


def method(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        depth += (source[index] == "{") - (source[index] == "}")
        if depth == 0:
            return source[start:index + 1]
    raise AssertionError(signature)


def admission_source():
    source = (COMPONENT / "esp_video_renderer.cpp").read_text()
    header = (COMPONENT / "esp_video_renderer.h").read_text()
    a = header.index("  uint32_t last_admission_ms_")
    b = header.index("  SemaphoreHandle_t mutex_", a)
    fields = header[a:b]
    return r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>
constexpr int pdTRUE=1;
uint32_t fake_ms=0;uint32_t millis(){return fake_ms;}
bool mutex_busy=false;unsigned notifications=0;
int xSemaphoreTake(void*,unsigned){return !mutex_busy;}void xSemaphoreGive(void*){}
void xTaskNotifyGive(void*){++notifications;}
struct EspVideoRenderer {
 enum class InputState:uint8_t {FREE,QUEUED,DECODING};
 struct Input {uint8_t *data=nullptr;size_t size=0;uint32_t generation=0,received_ms=0;InputState state=InputState::FREE;};
 uint8_t storage[2][64]{};Input inputs_[2]{};
 int queued_input_=-1,pending_surface_=-1;
 std::atomic<bool>active_{true};std::atomic<uint32_t>generation_{1};
 std::atomic<uint32_t>rejected_{0},replaced_{0},received_{0};
 size_t max_jpeg_bytes_=64;uint8_t session_fps_=2;
 void *mutex_=this,*worker_handle_=this;
''' + fields + r'''
 EspVideoRenderer(){for(unsigned i=0;i<2;++i)inputs_[i].data=storage[i];}
 bool submit_jpeg(const uint8_t*,size_t);void clear_pending_();
};
''' + method(source, "void EspVideoRenderer::clear_pending_()") + "\n" + method(source, "bool EspVideoRenderer::submit_jpeg(") + r'''
int main(int argc,char**argv){
 assert(argc>=3);const unsigned fps=std::atoi(argv[1]);const unsigned scenario=std::atoi(argv[2]);
 EspVideoRenderer renderer;renderer.session_fps_=fps;uint8_t jpeg[]={0xff,0xd8,0xff,0xd9};
 std::vector<uint64_t> accepted;unsigned attempts=0;
 auto offer=[&](uint64_t now){++attempts;fake_ms=static_cast<uint32_t>(now);if(renderer.submit_jpeg(jpeg,sizeof jpeg))accepted.push_back(now);};
 if(scenario<=2){ // Target boundary: constant499/500/501ms at2fps.
  for(unsigned i=0;i<40;++i)offer(i*(499U+scenario));
 }else if(scenario==3){ // Exact rational cadence with bounded arrival jitter.
  constexpr int jitter[]={0,-1,1,-2,2};
  for(unsigned i=0;i<fps*30+1;++i)offer((i*1000ULL+fps/2)/fps+jitter[i%5]);
 }else if(scenario==4){ // Overload including repeated arrivals in each millis tick.
  for(unsigned ms=0;ms<=10000;++ms)for(unsigned repeat=0;repeat<3;++repeat)offer(ms);
 }else if(scenario==5){
  offer(0);offer(0);offer(0);assert(accepted.size()==1);
  renderer.clear_pending_();offer(0);assert(accepted.size()==2);
  renderer.active_=false;offer(1000000);assert(accepted.size()==2);
  renderer.active_=true;renderer.clear_pending_();offer(1000000);assert(accepted.size()==3);
  mutex_busy=true;offer(1001000);mutex_busy=false;
  renderer.clear_pending_();renderer.inputs_[0].state=EspVideoRenderer::InputState::DECODING;
  renderer.inputs_[1].state=EspVideoRenderer::InputState::DECODING;offer(1001000);assert(accepted.size()==3);
  renderer.inputs_[1].state=EspVideoRenderer::InputState::FREE;offer(1001000);assert(accepted.size()==4);
 }else if(scenario==6){ // The same rational cadence across uint32 millis wrap.
  constexpr int jitter[]={0,-1,1,-2,2};
  for(unsigned i=0;i<fps*30+1;++i)offer(UINT32_MAX-1000ULL+(i*1000ULL+fps/2)/fps+jitter[i%5]);
 }else if(scenario==7){ // Rejections must neither consume budget nor shift phase.
  offer(0);offer(0);offer(499);offer(500);offer(501);offer(1000);
 }else if(scenario==9){ // More than ten minutes of slightly fast499ms arrivals.
  for(unsigned now=0;now<=700000;now+=499)offer(now);
 }else if(scenario==8){ // Long gaps refill a bounded amount, not elapsed*fps overflow.
  offer(1);offer(0xfffffffeULL);offer(0xfffffffeULL);offer(0xffffffffULL);
  offer(0xffffffffULL);offer(0x100000000ULL);offer(0x100000001ULL);
 }
 std::cout<<"{\"fps\":"<<fps<<",\"scenario\":"<<scenario<<",\"attempts\":"<<attempts<<",\"admitted\":"<<accepted.size()<<",\"times\":[";
 for(size_t i=0;i<accepted.size();++i){if(i)std::cout<<',';std::cout<<accepted[i];}
 std::cout<<"]}";
}
'''


@pytest.fixture(scope="module")
def admission_binary(tmp_path_factory):
    path = tmp_path_factory.mktemp("video-admission")
    cpp = path / "admission.cpp"
    cpp.write_text(admission_source())
    binary = path / "admission"
    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if os.getenv("VIDEO_ADMISSION_SANITIZE") else []
    result = subprocess.run(["g++", "-std=c++17", "-O1", *flags, str(cpp), "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


def run_case(binary, fps, scenario):
    result = subprocess.run([str(binary), str(fps), str(scenario)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


@pytest.mark.parametrize("scenario", [0, 1, 2], ids=["499ms", "500ms", "501ms"])
def test_slightly_early_two_fps_input_does_not_halve_output(admission_binary, scenario):
    result = run_case(admission_binary, 2, scenario)
    assert result["admitted"] == result["attempts"]


@pytest.mark.parametrize("fps", [1, 2, 5, 10, 15])
@pytest.mark.parametrize("scenario", [3, 6], ids=["ordinary-clock", "millis-wrap"])
def test_nominal_cadence_with_two_ms_jitter_is_admitted(admission_binary, fps, scenario):
    result = run_case(admission_binary, fps, scenario)
    assert result["admitted"] == result["attempts"]


@pytest.mark.parametrize("fps", [1, 2, 5, 10, 15])
def test_every_overload_window_respects_rate_and_two_frame_burst(admission_binary, fps):
    result = run_case(admission_binary, fps, 4)
    accepted = result["times"]
    assert len(accepted) >= fps * 10
    assert len(accepted) == len(set(accepted)), "same-millisecond duplicates must remain rejected"
    for first in range(len(accepted)):
        for last in range(first, len(accepted)):
            assert (last - first + 1) * 1000 <= 2000 + (accepted[last] - accepted[first]) * fps


def test_stop_reset_busy_slots_and_duplicate_zero_time(admission_binary):
    run_case(admission_binary, 2, 5)


def test_rejected_arrivals_do_not_spend_or_shift_the_budget(admission_binary):
    assert run_case(admission_binary, 2, 7)["times"] == [0, 499, 500, 1000]


def test_long_idle_refill_saturates_without_overflow(admission_binary):
    result = run_case(admission_binary, 15, 8)
    assert result["times"] == [1, 0xfffffffe, 0xffffffff]


def test_long_slightly_fast_input_keeps_negotiated_average_without_halving(admission_binary):
    result = run_case(admission_binary, 2, 9)
    times = result["times"]
    elapsed = times[-1] - times[0]
    assert (len(times) - 2) * 1000 <= elapsed * 2
    assert len(times) >= elapsed * 2 // 1000
    assert len(times) > result["attempts"] * 0.99
