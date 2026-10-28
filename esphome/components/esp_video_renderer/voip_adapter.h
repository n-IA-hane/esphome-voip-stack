#pragma once

#ifdef USE_ESP_VIDEO_RENDERER_VOIP
#include "esphome/components/voip_stack/video.h"

namespace esphome::esp_video_renderer {

// Only this adapter knows the call contract. The renderer remains reusable.
class EspVideoRendererSink : public voip_stack::EncodedVideoSink {
public:
  explicit EspVideoRendererSink(EspVideoRenderer *renderer)
      : renderer_(renderer) {}
  voip_stack::VideoCapability get_receive_video_capability() const override {
    voip_stack::VideoCapability capability{};
    capability.encoding = "JPEG";
    capability.payload_type = 26;
    capability.width = this->renderer_->get_width();
    capability.height = this->renderer_->get_height();
    capability.max_fps = this->renderer_->get_framerate();
    return capability;
  }
  bool start_video(const voip_stack::VideoCapability &capability) override {
    return capability.valid() && capability.is_jpeg() &&
           capability.width <= this->renderer_->get_width() &&
           capability.height <= this->renderer_->get_height() &&
           this->renderer_->prepare(capability.max_fps);
  }
  bool set_video_active(bool active) override {
    return this->renderer_->set_active(active);
  }
  void stop_video() override { this->renderer_->stop(); }
  bool consume_video_access_unit(
      const voip_stack::EncodedVideoAccessUnit &frame) override {
    const bool accepted = this->renderer_->submit_jpeg(frame.data, frame.size);
    if (accepted)
      frame.release();
    return accepted;
  }

protected:
  EspVideoRenderer *renderer_;
};

} // namespace esphome::esp_video_renderer
#endif
