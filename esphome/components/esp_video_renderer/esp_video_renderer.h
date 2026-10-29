#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP_IDF

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "jpeg_decoder.h"
#include "lvgl.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace esphome::esp_video_renderer {

// Owns decoded images and their LVGL lifetime, without requiring SIP or RTP.
class EspVideoRenderer : public Component {
public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  struct Stats {
    uint32_t admitted, replaced, rejected, decoded, decode_errors, presented;
    uint32_t decode_max_us, frame_age_max_ms;
  };
  Stats get_stats() const;
  void dump_state() const;
  float get_setup_priority() const override { return setup_priority::DATA; }
  void set_width(uint16_t value) { this->width_ = value; }
  void set_height(uint16_t value) { this->height_ = value; }
  void set_framerate(uint8_t value) { this->framerate_ = value; }
  void set_max_jpeg_bytes(size_t value) { this->max_jpeg_bytes_ = value; }
  void set_task_core(int8_t value) { this->task_core_ = value; }
  void set_task_priority(uint8_t value) { this->task_priority_ = value; }
  void set_task_stack_size(uint32_t value) { this->task_stack_size_ = value; }
  void set_task_stack_in_psram(bool value) {
    this->task_stack_in_psram_ = value;
  }
  void set_buffer_in_psram(bool value) { this->buffer_in_psram_ = value; }
  uint16_t get_width() const { return this->width_; }
  uint16_t get_height() const { return this->height_; }
  uint8_t get_framerate() const { return this->framerate_; }
  size_t get_max_jpeg_bytes() const { return this->max_jpeg_bytes_; }

  // Attach from lvgl.on_boot. All LVGL operations remain in the main loop.
  void attach_video_container(lv_obj_t *container);
  // prepare() reserves a new, initially hidden session. start() also activates
  // it.
  bool prepare(uint8_t max_fps);
  bool start();
  bool set_active(bool active);
  void stop();
  // Data is borrowed only until this function returns, and is copied on
  // admission.
  bool submit_jpeg(const uint8_t *data, size_t size);
  bool has_remote_frame() const {
    return this->active_.load() && this->visible_.load();
  }
  Trigger<> *get_first_frame_trigger() { return &this->first_frame_trigger_; }
  Trigger<> *get_video_ended_trigger() { return &this->video_ended_trigger_; }

protected:
  enum class InputState : uint8_t { FREE, QUEUED, DECODING };
  struct Input {
    uint8_t *data{nullptr};
    size_t size{0};
    uint32_t generation{0};
    uint32_t received_ms{0};
    InputState state{InputState::FREE};
  };
  struct Surface {
    uint8_t *data{nullptr};
    uint16_t width{0}, height{0};
    uint32_t generation{0}, received_ms{0};
  };
  uint32_t task_stack_size_{8192};
  uint8_t task_priority_{5};
  int8_t task_core_{-1};
  bool task_stack_in_psram_{true}, buffer_in_psram_{true};
  static void worker_entry_(void *ctx);
  void worker_();
  bool decode_(int input, int surface);
  void clear_pending_(); // mutex held; an in-flight decoder retains its input
  void release_setup_resources_(); // only before the worker has been started
  static void refresh_ready_(lv_event_t *event);

  uint16_t width_{320}, height_{240};
  uint8_t framerate_{2}, session_fps_{2};
  size_t max_jpeg_bytes_{131072}, surface_capacity_{0};
  Input inputs_[2]{};
  Surface surfaces_[2]{};
  int queued_input_{-1}, pending_surface_{-1}, front_surface_{-1};
  uint32_t last_admission_ms_{0};
  bool admission_seen_{false};
  uint16_t admission_credit_{0}; // milli-frames, fits the existing alignment gap
  SemaphoreHandle_t mutex_{nullptr}, stopped_{nullptr};
  StaticSemaphore_t mutex_storage_{}, stopped_storage_{};
  TaskHandle_t worker_handle_{nullptr};
  StackType_t *worker_stack_{nullptr};
  StaticTask_t worker_control_{};
  uint8_t *decoder_workspace_{nullptr};
  std::atomic<bool> prepared_{false}, active_{false}, stopping_{false};
  std::atomic<bool> visible_{false}, presentation_in_flight_{false},
      ended_pending_{false};
  std::atomic<uint32_t> generation_{0};
  lv_obj_t *image_{nullptr};
  lv_display_t *display_{nullptr};
  lv_image_dsc_t image_descriptor_{};
  uint32_t visible_generation_{0};
  Trigger<> first_frame_trigger_{}, video_ended_trigger_{};
  std::atomic<uint32_t> received_{0}, replaced_{0}, rejected_{0},
      decode_errors_{0};
  std::atomic<uint32_t> decoded_{0}, presented_{0}, decode_max_us_{0},
      frame_age_max_ms_{0};
};

template <typename... Ts> class StartAction : public Action<Ts...> {
public:
  explicit StartAction(EspVideoRenderer *parent) : parent_(parent) {}
  void play(const Ts &...args) override { this->parent_->start(); }

protected:
  EspVideoRenderer *parent_;
};
template <typename... Ts> class StopAction : public Action<Ts...> {
public:
  explicit StopAction(EspVideoRenderer *parent) : parent_(parent) {}
  void play(const Ts &...args) override { this->parent_->stop(); }

protected:
  EspVideoRenderer *parent_;
};

} // namespace esphome::esp_video_renderer

#ifdef USE_ESP_VIDEO_RENDERER_VOIP
#include "voip_adapter.h"
#endif

#endif // USE_ESP_IDF
