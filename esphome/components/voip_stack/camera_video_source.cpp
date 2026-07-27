#include "camera_video_source.h"

#if defined(USE_ESPHOME_VOIP_STACK_VIDEO) && \
    defined(USE_ESPHOME_VOIP_STACK_VIDEO_CAMERA)

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <algorithm>

namespace esphome {
namespace voip_stack {

static const char *const TAG = "voip_stack.camera_video";

void CameraJpegVideoSource::register_listener() {
  if (this->camera_ == nullptr || this->listener_registered_) return;
  this->camera_->add_listener(this);
  this->listener_registered_ = true;
}

VideoCapability CameraJpegVideoSource::get_video_capability() const {
  VideoCapability capability;
  capability.payload_type = 26;
  capability.encoding = "JPEG";
  capability.profile_level_id.clear();
  capability.packetization_mode = 0;
  capability.level_asymmetry_allowed = false;
  capability.width = this->width_;
  capability.height = this->height_;
  capability.max_fps = this->max_fps_;
  return capability;
}

bool CameraJpegVideoSource::prepare_video(
    const VideoCapability &capability) {
  LockGuard lock(this->callback_mutex_);
  return this->camera_ != nullptr && this->listener_registered_ &&
         !this->active_ && capability.valid() && capability.is_jpeg() &&
         capability.width <= this->width_ &&
         capability.height <= this->height_;
}

bool CameraJpegVideoSource::start_video(
    EncodedVideoAccessUnitCallback callback, void *ctx,
    const VideoCapability &capability) {
  {
    LockGuard lock(this->callback_mutex_);
    if (this->camera_ == nullptr || callback == nullptr ||
        !this->listener_registered_ || this->active_ ||
        !capability.valid() || !capability.is_jpeg()) {
      return false;
    }
    // Publish one coherent callback record. stop_video() takes the same mutex,
    // so it cannot return while an image callback still references its owner.
    this->callback_ = callback;
    this->callback_ctx_ = ctx;
    this->negotiated_fps_ =
        std::max<uint8_t>(1, std::min(this->max_fps_, capability.max_fps));
    this->last_emitted_timestamp_ = 0;
    this->active_ = true;
  }
  this->request_next_();
  ESP_LOGI(TAG, "Standard ESPHome camera JPEG source started");
  return true;
}

void CameraJpegVideoSource::stop_video() {
  LockGuard lock(this->callback_mutex_);
  this->active_ = false;
  this->callback_ = nullptr;
  this->callback_ctx_ = nullptr;
}

void CameraJpegVideoSource::request_next_() {
  camera::Camera *camera_component = nullptr;
  {
    LockGuard lock(this->callback_mutex_);
    if (this->active_) camera_component = this->camera_;
  }
  if (camera_component != nullptr) {
    // A one-shot WEB_REQUESTER request deliberately avoids borrowing either
    // persistent stream bit. stop_video() therefore cannot stop an API/web
    // stream owned by another consumer of the same standard camera entity.
    camera_component->request_image(camera::WEB_REQUESTER);
  }
}

void CameraJpegVideoSource::on_camera_image(
    const std::shared_ptr<camera::CameraImage> &image) {
  if (image == nullptr ||
      !image->was_requested_by(camera::WEB_REQUESTER)) {
    return;
  }
  {
    LockGuard lock(this->callback_mutex_);
    if (!this->active_ || this->callback_ == nullptr) return;
    uint8_t *data = image->get_data_buffer();
    const size_t size = image->get_data_length();
    const uint32_t timestamp = static_cast<uint32_t>(millis() * 90U);
    const uint32_t minimum_delta =
        (90000U + this->negotiated_fps_ - 1U) / this->negotiated_fps_;
    const bool rate_ready =
        this->last_emitted_timestamp_ == 0 ||
        timestamp - this->last_emitted_timestamp_ >= minimum_delta;
    if (data != nullptr && size > 0 && rate_ready) {
      const EncodedVideoAccessUnit access_unit{
          data, size, timestamp, true};
      // VideoRtpSession performs only a bounded copy here. Keeping the record
      // locked makes callback context lifetime deterministic during teardown.
      this->callback_(this->callback_ctx_, access_unit);
      this->last_emitted_timestamp_ = timestamp;
    }
  }
  this->request_next_();
}

}  // namespace voip_stack
}  // namespace esphome

#endif
