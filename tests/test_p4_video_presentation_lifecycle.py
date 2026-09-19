"""Execute renderer lifecycle methods with a contended presentation mutex."""

import os
from pathlib import Path
import resource
import subprocess

import pytest


RENDERER = (
    Path(os.environ.get("VOIP_STACK_REPO", Path(__file__).resolve().parents[1]))
    / "esphome/components/p4_video_renderer"
)


@pytest.mark.parametrize("codec", ["JPEG", "H264"])
def test_stopped_video_cannot_reopen_on_following_audio_call(tmp_path, codec):
    source = (RENDERER / "p4_video_renderer.cpp").read_text()
    deactivate = source.split("bool P4VideoRenderer::set_video_active(bool active)", 1)[1]
    deactivate = deactivate.split("bool P4VideoRenderer::consume_video_access_unit", 1)[0]
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
  std::atomic<bool> remote_frame_visible_{true}, video_ended_pending_{false};
  std::atomic<bool> waiting_for_key_frame_{false};
  std::atomic<unsigned> rx_session_generation_{0}, loss_generation_{0};
  std::atomic<int> pending_surface_{1};
  struct { void reset(uint8_t) {} } cadence_;
  struct { uint8_t max_fps{10}; } rx_capability_;
  uint8_t framerate_{10};
  void* rx_task_handle_{nullptr};
  int presentation_mutex_{};
  bool loop_pending{false};
  void enable_loop_soon_any_context() { loop_pending = true; }
  bool set_video_active(bool active);
  bool has_remote_frame() const { QUERY }
};
bool P4VideoRenderer::set_video_active(bool active) BODY
int main() {
  P4VideoRenderer renderer;
  assert(renderer.has_remote_frame());
  // End a video call while the display worker still owns the last surface.
  assert(renderer.set_video_active(false));
  assert(!renderer.has_remote_frame());
  assert(!renderer.remote_frame_visible_.load());
  assert(renderer.loop_pending && renderer.video_ended_pending_.load());
  // An audio-only call has no new video activation: the page must stay hidden.
  assert(!renderer.has_remote_frame());
  // A later video call must wait for its own first image as well.
  assert(renderer.set_video_active(true));
  assert(!renderer.has_remote_frame());
  renderer.remote_frame_visible_ = true;
  assert(renderer.has_remote_frame());
  mutex_available = true;
  assert(renderer.set_video_active(false));
  assert(renderer.pending_surface_.load() == -1);
  assert(!renderer.has_remote_frame());
  // A late publication from a stopped generation cannot advertise live video.
  renderer.remote_frame_visible_ = true;
  assert(!renderer.has_remote_frame());
}
'''.replace("QUERY", query).replace("BODY", deactivate)
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
