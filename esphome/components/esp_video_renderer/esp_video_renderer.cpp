#include "esp_video_renderer.h"

#ifdef USE_ESP_IDF

#include "esp_heap_caps.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "jpeg_frame.h"
#include "sdkconfig.h"
#include <algorithm>
#include <cstring>

namespace esphome::esp_video_renderer {

static const char *const TAG = "esp_video_renderer";
#if !CONFIG_JD_USE_ROM && CONFIG_JD_FASTDECODE == 2
static constexpr size_t JPEG_WORKSPACE_BYTES = 65472;
#else
static constexpr size_t JPEG_WORKSPACE_BYTES = 3100;
#endif

static void update_max(std::atomic<uint32_t> &maximum, uint32_t value) {
  uint32_t previous = maximum.load(std::memory_order_relaxed);
  while (previous < value && !maximum.compare_exchange_weak(
                                 previous, value, std::memory_order_relaxed)) {
  }
}

void EspVideoRenderer::setup() {
  if (this->task_core_ >= 0 &&
      this->task_core_ >= static_cast<int>(portNUM_PROCESSORS)) {
    ESP_LOGE(TAG, "Configured worker core is unavailable in this firmware");
    this->mark_failed();
    return;
  }
  this->mutex_ = xSemaphoreCreateMutexStatic(&this->mutex_storage_);
  this->stopped_ = xSemaphoreCreateBinaryStatic(&this->stopped_storage_);
  this->surface_capacity_ =
      static_cast<size_t>(this->width_) * this->height_ * 2;
  this->worker_stack_ = static_cast<StackType_t *>(heap_caps_malloc(
      this->task_stack_size_,
      MALLOC_CAP_8BIT | (this->task_stack_in_psram_ ? MALLOC_CAP_SPIRAM
                                                    : MALLOC_CAP_INTERNAL)));
  bool allocated = this->mutex_ != nullptr && this->stopped_ != nullptr &&
                   this->worker_stack_ != nullptr;
  const uint32_t image_caps =
      MALLOC_CAP_8BIT |
      (this->buffer_in_psram_ ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL);
  for (auto &input : this->inputs_) {
    input.data = static_cast<uint8_t *>(
        heap_caps_aligned_alloc(16, this->max_jpeg_bytes_, image_caps));
    allocated = allocated && input.data != nullptr;
  }
  for (auto &surface : this->surfaces_) {
    surface.data = static_cast<uint8_t *>(
        heap_caps_aligned_alloc(16, this->surface_capacity_, image_caps));
    allocated = allocated && surface.data != nullptr;
  }
  this->decoder_workspace_ = static_cast<uint8_t *>(
      heap_caps_aligned_alloc(16, JPEG_WORKSPACE_BYTES, image_caps));
  if (!allocated || this->decoder_workspace_ == nullptr) {
    ESP_LOGE(TAG, "Cannot reserve JPEG renderer resources");
    this->release_setup_resources_();
    this->mark_failed();
    return;
  }
  // One dormant worker, created once. No per-call task/buffer churn.
  this->worker_handle_ = xTaskCreateStaticPinnedToCore(
      EspVideoRenderer::worker_entry_, "jpeg_render", this->task_stack_size_,
      this, this->task_priority_, this->worker_stack_, &this->worker_control_,
      this->task_core_ < 0 ? tskNO_AFFINITY : this->task_core_);
  if (this->worker_handle_ == nullptr) {
    ESP_LOGE(TAG, "Cannot create JPEG renderer worker");
    this->release_setup_resources_();
    this->mark_failed();
    return;
  }
  this->disable_loop();
}

void EspVideoRenderer::release_setup_resources_() {
  heap_caps_free(this->worker_stack_);
  this->worker_stack_ = nullptr;
  heap_caps_free(this->decoder_workspace_);
  this->decoder_workspace_ = nullptr;
  for (auto &input : this->inputs_) {
    heap_caps_free(input.data);
    input.data = nullptr;
  }
  for (auto &surface : this->surfaces_) {
    heap_caps_free(surface.data);
    surface.data = nullptr;
  }
}

void EspVideoRenderer::clear_pending_() {
  this->queued_input_ = -1;
  this->pending_surface_ = -1;
  for (auto &input : this->inputs_)
    if (input.state == InputState::QUEUED)
      input.state = InputState::FREE;
  this->admission_seen_ = false;
  this->admission_credit_ = 0;
}

bool EspVideoRenderer::prepare(uint8_t max_fps) {
  if (this->is_failed() || this->worker_handle_ == nullptr ||
      this->stopping_.load() || xSemaphoreTake(this->mutex_, 0) != pdTRUE)
    return false;
  this->active_.store(false, std::memory_order_release);
  this->generation_.fetch_add(1, std::memory_order_acq_rel);
  this->prepared_.store(true, std::memory_order_release);
  this->session_fps_ =
      std::min(this->framerate_, std::max<uint8_t>(1, max_fps));
  this->clear_pending_();
  xSemaphoreGive(this->mutex_);
  this->enable_loop_soon_any_context();
  return true;
}

bool EspVideoRenderer::start() {
  return this->prepare(this->framerate_) && this->set_active(true);
}

bool EspVideoRenderer::set_active(bool active) {
  if (active && (!this->prepared_.load(std::memory_order_acquire) ||
                 this->stopping_.load()))
    return false;
  const bool was_active =
      this->active_.exchange(active, std::memory_order_acq_rel);
  if (was_active == active)
    return true;
  this->generation_.fetch_add(1, std::memory_order_acq_rel);
  if (!active) {
    this->visible_.store(false, std::memory_order_release);
    this->ended_pending_.store(true, std::memory_order_release);
  }
  // Workers carry their own generation. Even with a contended mutex they can
  // neither publish old decoded data nor revive the previous session.
  if (this->mutex_ != nullptr && xSemaphoreTake(this->mutex_, 0) == pdTRUE) {
    this->clear_pending_();
    xSemaphoreGive(this->mutex_);
  }
  this->enable_loop_soon_any_context();
  return true;
}

void EspVideoRenderer::stop() {
  this->set_active(false);
  this->prepared_.store(false, std::memory_order_release);
  this->generation_.fetch_add(1, std::memory_order_acq_rel);
  if (this->mutex_ != nullptr && xSemaphoreTake(this->mutex_, 0) == pdTRUE) {
    this->clear_pending_();
    xSemaphoreGive(this->mutex_);
  }
  this->enable_loop_soon_any_context();
}

bool EspVideoRenderer::submit_jpeg(const uint8_t *data, size_t size) {
  if (!this->active_.load(std::memory_order_acquire) || data == nullptr ||
      size < 4 || size > this->max_jpeg_bytes_ || data[0] != 0xff ||
      data[1] != 0xd8 || this->mutex_ == nullptr ||
      xSemaphoreTake(this->mutex_, 0) != pdTRUE) {
    this->rejected_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const uint32_t now = millis();
  // Wall-clock token budget: preserve fractional cadence instead of resetting
  // the phase after each accepted frame. One slightly early arrival must not
  // halve the rate. Credit is bounded by the two existing input slots, so an
  // idle interval cannot accumulate unbounded decoder work. RTP PTS is unused.
  constexpr uint32_t FRAME_CREDIT = 1000;
  constexpr uint32_t MAX_CREDIT = 2 * FRAME_CREDIT;
  const uint32_t elapsed = now - this->last_admission_ms_;
  uint32_t credit = MAX_CREDIT;
  if (this->admission_seen_) {
    const uint32_t refill_ms =
        (MAX_CREDIT + this->session_fps_ - 1U) / this->session_fps_;
    credit = elapsed >= refill_ms
                 ? MAX_CREDIT
                 : std::min<uint32_t>(MAX_CREDIT, this->admission_credit_ +
                                                     elapsed * this->session_fps_);
  }
  if (!this->active_.load(std::memory_order_acquire) ||
      (this->admission_seen_ && (elapsed == 0 || credit < FRAME_CREDIT))) {
    xSemaphoreGive(this->mutex_);
    this->rejected_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  int slot = this->queued_input_;
  if (slot >= 0) {
    this->replaced_.fetch_add(1, std::memory_order_relaxed);
  } else {
    for (int i = 0; i < 2; i++)
      if (this->inputs_[i].state == InputState::FREE) {
        slot = i;
        break;
      }
  }
  if (slot < 0) {
    xSemaphoreGive(this->mutex_);
    this->rejected_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const uint32_t generation = this->generation_.load(std::memory_order_acquire);
  Input &input = this->inputs_[slot];
  std::memcpy(input.data, data, size);
  if (!this->active_.load(std::memory_order_acquire) ||
      generation != this->generation_.load(std::memory_order_acquire)) {
    input.state = InputState::FREE;
    if (this->queued_input_ == slot)
      this->queued_input_ = -1;
    xSemaphoreGive(this->mutex_);
    this->rejected_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  input.size = size;
  input.generation = generation;
  input.received_ms = now;
  input.state = InputState::QUEUED;
  this->queued_input_ = slot;
  this->last_admission_ms_ = now;
  this->admission_credit_ = static_cast<uint16_t>(credit - FRAME_CREDIT);
  this->admission_seen_ = true;
  this->received_.fetch_add(1, std::memory_order_relaxed);
  // A ready surface waits for the main loop; replacing its queued successor
  // needs no wakeup until that handoff releases the other RGB surface.
  const bool wake_worker = this->pending_surface_ < 0;
  xSemaphoreGive(this->mutex_);
  if (wake_worker)
    xTaskNotifyGive(this->worker_handle_);
  return true;
}

void EspVideoRenderer::worker_entry_(void *ctx) {
  static_cast<EspVideoRenderer *>(ctx)->worker_();
}

void EspVideoRenderer::worker_() {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (this->stopping_.load(std::memory_order_acquire))
      break;
    for (;;) {
      xSemaphoreTake(this->mutex_, portMAX_DELAY);
      const int input = this->queued_input_;
      if (input < 0 || !this->active_.load(std::memory_order_acquire) ||
          this->pending_surface_ >= 0) {
        xSemaphoreGive(this->mutex_);
        break;
      }
      this->queued_input_ = -1;
      this->inputs_[input].state = InputState::DECODING;
      const int surface = this->front_surface_ == 0 ? 1 : 0;
      // The front belongs to LVGL and a ready back belongs to the main loop.
      // With both occupied, park above and retain only the newest queued JPEG.
      // The main loop wakes us after consuming pending_surface_.
      xSemaphoreGive(this->mutex_);
      const bool decoded = this->decode_(input, surface);
      xSemaphoreTake(this->mutex_, portMAX_DELAY);
      const bool current =
          decoded && this->active_.load(std::memory_order_acquire) &&
          this->inputs_[input].generation ==
              this->generation_.load(std::memory_order_acquire);
      if (current) {
        this->surfaces_[surface].generation = this->inputs_[input].generation;
        this->surfaces_[surface].received_ms = this->inputs_[input].received_ms;
        this->pending_surface_ = surface;
      }
      this->inputs_[input].state = InputState::FREE;
      xSemaphoreGive(this->mutex_);
      if (current)
        this->enable_loop_soon_any_context();
      if (this->stopping_.load(std::memory_order_acquire))
        break;
    }
  }
  // The preallocated arena follows the same shutdown lifetime as the surfaces.
  xSemaphoreGive(this->stopped_);
  vTaskDelete(nullptr);
}

bool EspVideoRenderer::decode_(int input_index, int surface_index) {
  const auto &input = this->inputs_[input_index];
  auto &surface = this->surfaces_[surface_index];
  JpegDecodePlan plan;
  const uint32_t started = micros();
  if (!plan_baseline_jpeg(input.data, input.size, this->width_,
                          this->height_, plan)) {
    this->decode_errors_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  esp_jpeg_image_cfg_t config{};
  config.indata = input.data;
  config.indata_size = input.size;
  config.outbuf = surface.data;
  config.outbuf_size = this->surface_capacity_;
  config.out_format = JPEG_IMAGE_FORMAT_RGB565;
  config.out_scale = static_cast<esp_jpeg_image_scale_t>(plan.scale);
  config.flags.swap_color_bytes = false;
  config.advanced.working_buffer = this->decoder_workspace_;
  config.advanced.working_buffer_size = JPEG_WORKSPACE_BYTES;
  esp_jpeg_image_output_t info{};
  if (esp_jpeg_decode(&config, &info) != ESP_OK ||
      info.width != plan.output.width || info.height != plan.output.height ||
      info.output_len != size_t(plan.output.width) * plan.output.height * 2) {
    this->decode_errors_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  surface.width = info.width;
  surface.height = info.height;
  this->decoded_.fetch_add(1, std::memory_order_relaxed);
  update_max(this->decode_max_us_, micros() - started);
  return true;
}

void EspVideoRenderer::attach_video_container(lv_obj_t *container) {
  if (container == nullptr || this->image_ != nullptr || this->is_failed())
    return;
  this->image_ = lv_image_create(container);
  lv_obj_center(this->image_);
  lv_obj_add_flag(this->image_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(this->image_, LV_OBJ_FLAG_SCROLLABLE);
  this->display_ = lv_obj_get_display(this->image_);
  lv_display_add_event_cb(this->display_, EspVideoRenderer::refresh_ready_,
                          LV_EVENT_REFR_READY, this);
  this->enable_loop();
}

void EspVideoRenderer::refresh_ready_(lv_event_t *event) {
  auto *renderer =
      static_cast<EspVideoRenderer *>(lv_event_get_user_data(event));
  if (renderer->presentation_in_flight_.exchange(false,
                                                 std::memory_order_acq_rel))
    renderer->enable_loop_soon_any_context();
}

void EspVideoRenderer::loop() {
  this->disable_loop();
  if (this->image_ == nullptr)
    return;
  const uint32_t generation = this->generation_.load(std::memory_order_acquire);
  if (!this->active_.load(std::memory_order_acquire) ||
      this->visible_generation_ != generation) {
    lv_obj_add_flag(this->image_, LV_OBJ_FLAG_HIDDEN);
    this->visible_.store(false, std::memory_order_release);
  }
  if (this->ended_pending_.exchange(false, std::memory_order_acq_rel))
    this->video_ended_trigger_.trigger();
  if (xSemaphoreTake(this->mutex_, 0) != pdTRUE) {
    this->enable_loop_soon_any_context();
    return;
  }
  const int pending = this->pending_surface_;
  bool first = false;
  bool consumed = false;
  if (pending >= 0 && this->active_.load(std::memory_order_acquire) &&
      this->surfaces_[pending].generation ==
          this->generation_.load(std::memory_order_acquire) &&
      !this->presentation_in_flight_.load(std::memory_order_acquire)) {
    const auto &surface = this->surfaces_[pending];
    this->front_surface_ = pending;
    this->pending_surface_ = -1;
    this->image_descriptor_ = {};
    this->image_descriptor_.header.magic = LV_IMAGE_HEADER_MAGIC;
    this->image_descriptor_.header.cf = LV_COLOR_FORMAT_RGB565;
    this->image_descriptor_.header.w = surface.width;
    this->image_descriptor_.header.h = surface.height;
    this->image_descriptor_.header.stride = surface.width * 2;
    this->image_descriptor_.data_size =
        static_cast<size_t>(surface.width) * surface.height * 2;
    this->image_descriptor_.data = surface.data;
    lv_image_set_src(this->image_, &this->image_descriptor_);
    lv_obj_set_size(this->image_, surface.width, surface.height);
    lv_obj_center(this->image_);
    lv_obj_remove_flag(this->image_, LV_OBJ_FLAG_HIDDEN);
    this->presentation_in_flight_.store(true, std::memory_order_release);
    this->visible_generation_ = surface.generation;
    first = !this->visible_.exchange(true, std::memory_order_acq_rel);
    this->presented_.fetch_add(1, std::memory_order_relaxed);
    update_max(this->frame_age_max_ms_, millis() - surface.received_ms);
    lv_obj_invalidate(this->image_);
    consumed = true;
  }
  xSemaphoreGive(this->mutex_);
  if (first)
    this->first_frame_trigger_.trigger();
  if (consumed && this->worker_handle_ != nullptr &&
      !this->stopping_.load(std::memory_order_acquire))
    xTaskNotifyGive(this->worker_handle_);
}

EspVideoRenderer::Stats EspVideoRenderer::get_stats() const {
  return {this->received_.load(),      this->replaced_.load(),
          this->rejected_.load(),      this->decoded_.load(),
          this->decode_errors_.load(), this->presented_.load(),
          this->decode_max_us_.load(), this->frame_age_max_ms_.load()};
}

void EspVideoRenderer::dump_state() const {
  [[maybe_unused]] const auto stats = this->get_stats();
  ESP_LOGI(TAG,
           "Frames admitted/replaced/rejected: %u/%u/%u, decoded/errors/LVGL: "
           "%u/%u/%u",
           static_cast<unsigned>(stats.admitted),
           static_cast<unsigned>(stats.replaced),
           static_cast<unsigned>(stats.rejected),
           static_cast<unsigned>(stats.decoded),
           static_cast<unsigned>(stats.decode_errors),
           static_cast<unsigned>(stats.presented));
  ESP_LOGI(TAG, "Maximum decode elapsed: %u us, frame age at LVGL: %u ms",
           static_cast<unsigned>(stats.decode_max_us),
           static_cast<unsigned>(stats.frame_age_max_ms));
}

void EspVideoRenderer::dump_config() {
  ESP_LOGCONFIG(TAG, "Software JPEG renderer: %ux%u, up to %u fps",
                this->width_, this->height_, this->framerate_);
  ESP_LOGCONFIG(TAG, "  Worker priority/core: %u/%d, stack: %u bytes (%s)",
                this->task_priority_, this->task_core_,
                static_cast<unsigned>(this->task_stack_size_),
                this->task_stack_in_psram_ ? "PSRAM" : "internal");
  ESP_LOGCONFIG(TAG, "  Image buffers: %s",
                this->buffer_in_psram_ ? "PSRAM" : "internal");
  ESP_LOGCONFIG(TAG, "  Reserved image buffers: %u bytes",
                static_cast<unsigned>(2 * this->max_jpeg_bytes_ +
                                      2 * this->surface_capacity_));
  ESP_LOGCONFIG(TAG, "  Frames admitted/replaced/rejected: %u/%u/%u",
                static_cast<unsigned>(this->received_.load()),
                static_cast<unsigned>(this->replaced_.load()),
                static_cast<unsigned>(this->rejected_.load()));
  ESP_LOGCONFIG(TAG, "  Decode errors / submitted to LVGL: %u/%u",
                static_cast<unsigned>(this->decode_errors_.load()),
                static_cast<unsigned>(this->presented_.load()));
  ESP_LOGCONFIG(TAG,
                "  Maximum decode: %u us, frame age at presentation: %u ms",
                static_cast<unsigned>(this->decode_max_us_.load()),
                static_cast<unsigned>(this->frame_age_max_ms_.load()));
}

void EspVideoRenderer::on_shutdown() {
  this->stop();
  this->stopping_.store(true, std::memory_order_release);
  if (this->worker_handle_ != nullptr) {
    xTaskNotifyGive(this->worker_handle_);
    const TickType_t timeout = std::max<TickType_t>(1, pdMS_TO_TICKS(500));
    if (xSemaphoreTake(this->stopped_, timeout) != pdTRUE) {
      ESP_LOGW(
          TAG,
          "JPEG worker is still active; retaining its resources until reset");
    }
    // Buffers also remain potentially referenced by LVGL/DSI. Shutdown does
    // not free them, even after the worker acknowledges stopping.
  }
}

} // namespace esphome::esp_video_renderer
#endif // USE_ESP_IDF
