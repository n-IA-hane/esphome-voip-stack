#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESPHOME_VOIP_STACK_VIDEO) && \
    defined(USE_ESPHOME_VOIP_STACK_VIDEO_JPEG) && \
    defined(USE_ESPHOME_VOIP_STACK_VIDEO_CAMERA)

#include "video.h"

#include "esphome/components/camera/camera.h"
#include "esphome/core/helpers.h"

#include <cstdint>

namespace esphome {
namespace voip_stack {

/// Adapts the JPEG contract of ESPHome's standard camera platform to SIP
/// video. The camera keeps ownership of each image; VideoRtpSession copies the
/// complete frame into its single bounded TX slot during the callback.
class CameraJpegVideoSource : public EncodedVideoSource,
                              public camera::CameraListener {
 public:
  void set_camera(camera::Camera *camera) { this->camera_ = camera; }
  void set_dimensions(uint16_t width, uint16_t height) {
    this->width_ = width;
    this->height_ = height;
  }
  void set_max_fps(uint8_t max_fps) { this->max_fps_ = max_fps; }
  void register_listener();

  VideoCapability get_video_capability() const override;
  bool prepare_video(const VideoCapability &capability) override;
  bool start_video(EncodedVideoAccessUnitCallback callback, void *ctx,
                   const VideoCapability &capability) override;
  void stop_video() override;
  void on_camera_image(
      const std::shared_ptr<camera::CameraImage> &image) override;

 protected:
  void request_next_();

  camera::Camera *camera_{nullptr};
  uint16_t width_{640};
  uint16_t height_{480};
  uint8_t max_fps_{10};
  bool listener_registered_{false};
  Mutex callback_mutex_;
  bool active_{false};
  uint8_t negotiated_fps_{10};
  uint32_t last_emitted_timestamp_{0};
  EncodedVideoAccessUnitCallback callback_{nullptr};
  void *callback_ctx_{nullptr};
};

}  // namespace voip_stack
}  // namespace esphome

#endif
