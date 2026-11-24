#pragma once
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_RENDERER
#include "video.h"
#include "esphome/components/video_renderer/video_renderer.h"
#include <algorithm>
#include <climits>
#include <limits>

namespace esphome::voip_stack {

// RTP clock extension belongs to this adapter, never to a display renderer.
// A missing/backwards timestamp stays explicitly invalid, not wall-clock time.
class RendererFrameClock {
 public:
  void reset() { this->seen_ = false; this->time_us_ = 0; this->remainder_ = 0; }
  video_renderer::FrameTime translate(uint32_t timestamp) {
    if (!this->seen_) {
      this->seen_ = true; this->last_ = timestamp;
      return {0, true};
    }
    const uint32_t elapsed = timestamp - this->last_;
    if (elapsed > uint32_t(INT32_MAX)) {
      this->last_ = timestamp; this->remainder_ = 0;
      return {this->time_us_, false};
    }
    const uint64_t scaled = uint64_t(elapsed) * 100U + this->remainder_;
    const uint64_t advance = scaled / 9U;
    if (advance > std::numeric_limits<uint64_t>::max() - this->time_us_)
      return {this->time_us_, false};
    this->last_ = timestamp; this->time_us_ += advance;
    this->remainder_ = scaled % 9U;
    return {this->time_us_, true};
  }
 protected:
  uint64_t time_us_{0};
  uint32_t last_{0};
  uint8_t remainder_{0};
  bool seen_{false};
};

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
inline const char *renderer_h264_profile_level_id(
    uint16_t width, uint16_t height, uint8_t max_fps) {
  const uint32_t macroblocks =
      ((static_cast<uint32_t>(width) + 15U) / 16U) *
      ((static_cast<uint32_t>(height) + 15U) / 16U);
  const uint32_t macroblocks_per_second =
      macroblocks * std::max<uint32_t>(1U, max_fps);
  struct LevelLimit {
    uint16_t max_macroblocks;
    uint32_t max_macroblocks_per_second;
    const char *profile_level_id;
  };
  // Smallest constrained-baseline level whose Annex A limits cover the
  // configured receive envelope. With bilateral level asymmetry this limits
  // browser -> P4 complexity without reducing the P4 hardware encoder's
  // independent outbound level.
  static constexpr LevelLimit LEVELS[] = {
      {99, 1485, "42c00a"},   {396, 3000, "42c00b"},
      {396, 6000, "42c00c"},  {396, 11880, "42c00d"},
      {792, 19800, "42c015"}, {1620, 20250, "42c016"},
      {1620, 40500, "42c01e"},
  };
  for (const auto &level : LEVELS) {
    if (macroblocks <= level.max_macroblocks &&
        macroblocks_per_second <= level.max_macroblocks_per_second) {
      return level.profile_level_id;
    }
  }
  return "42c01e";
}
#endif

class RendererVideoSink : public EncodedVideoSink {
 public:
  explicit RendererVideoSink(video_renderer::EncodedFrameRenderer *renderer) : renderer_(renderer) {}
  VideoCapability get_receive_video_capability() const override {
    const auto caps = this->renderer_->get_capabilities();
    VideoCapability capability{};
    capability.width = caps.preferred.width;
    capability.height = caps.preferred.height;
    capability.max_fps = caps.preferred.max_fps;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
    if (caps.preferred.codec == video_renderer::Codec::H264) {
      capability.payload_type = 103; capability.encoding = "H264";
      capability.profile_level_id = renderer_h264_profile_level_id(
          caps.max_decode_width, caps.max_decode_height, caps.preferred.max_fps);
      capability.packetization_mode = 1; capability.level_asymmetry_allowed = true;
      capability.max_bitrate_bps = 800000;
      capability.rtcp_feedback_pli = true; capability.rtcp_feedback_fir = true;
      return capability;
    }
#endif
    capability.payload_type = 26; capability.encoding = "JPEG";
    return capability;
  }
  size_t max_encoded_access_unit_bytes() const override {
    return this->renderer_->get_capabilities().max_encoded_frame_bytes;
  }
  bool start_video(const VideoCapability &capability) override {
    const auto caps = this->renderer_->get_capabilities();
    if (!capability.valid()) return false;
    if (caps.preferred.codec == video_renderer::Codec::JPEG) {
      if (!capability.is_jpeg()) return false;
    } else {
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
      if (!capability.is_h264() || !h264_level_fits(capability.profile_level_id,
          this->get_receive_video_capability().profile_level_id)) return false;
#else
      return false;
#endif
    }
    const video_renderer::StreamFormat format{caps.preferred.codec,
        capability.width, capability.height, capability.max_fps};
    if (!this->renderer_->prepare(format)) return false;
    this->clock_.reset();
    return true;
  }
  bool set_video_active(bool active) override { return this->renderer_->set_active(active); }
  void stop_video() override { this->renderer_->stop(); }
  bool consume_video_access_unit(const EncodedVideoAccessUnit &unit) override {
    video_renderer::EncodedFrame frame{};
    frame.data = unit.data; frame.size = unit.size;
    frame.presentation_time = this->clock_.translate(unit.timestamp_90khz);
    frame.key_frame = unit.key_frame;
    frame.release_callback = unit.release_callback; frame.release_ctx = unit.release_ctx;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
    frame.jpeg_info = {unit.jpeg_info.width, unit.jpeg_info.height,
        static_cast<video_renderer::JpegSampling>(unit.jpeg_info.sampling)};
#endif
    // The renderer owns an accepted lease; false leaves it with the RTP caller.
    return this->renderer_->submit_frame(frame);
  }
 protected:
  video_renderer::EncodedFrameRenderer *renderer_;
  RendererFrameClock clock_{};
};

class S3RendererVideoSink final : public RendererVideoSink {
 public:
  using RendererVideoSink::RendererVideoSink;
  VideoCapability get_receive_video_capability() const override {
    auto capability = RendererVideoSink::get_receive_video_capability();
    const auto caps = this->renderer_->get_capabilities();
    if (caps.preferred.codec == video_renderer::Codec::JPEG) {
      capability.width = caps.max_decode_width;
      capability.height = caps.max_decode_height;
    }
    return capability;
  }
  bool start_video(const VideoCapability &capability) override {
    const auto caps = this->renderer_->get_capabilities();
    if (caps.preferred.codec == video_renderer::Codec::JPEG)
      return capability.width <= caps.max_decode_width && capability.height <= caps.max_decode_height &&
             RendererVideoSink::start_video(capability);
    return capability.width <= caps.preferred.width && capability.height <= caps.preferred.height &&
           RendererVideoSink::start_video(capability);
  }
};

class P4RendererVideoSink final : public RendererVideoSink {
 public:
  using RendererVideoSink::RendererVideoSink;
  // Preserve the original P4 transport pool defaults. Renderer bounds still
  // apply independently when an access unit is submitted.
  size_t max_encoded_access_unit_bytes() const override { return 0; }
  bool start_video(const VideoCapability &capability) override {
    if (!RendererVideoSink::start_video(capability)) return false;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
    const auto caps = this->renderer_->get_capabilities();
    this->fps_ = std::max<uint8_t>(1, std::min(caps.preferred.max_fps, capability.max_fps));
    this->cadence_.reset(this->fps_);
#endif
    return true;
  }
  bool set_video_active(bool active) override {
    if (!RendererVideoSink::set_video_active(active)) return false;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
    if (active) this->cadence_.reset(this->fps_);
#endif
    return true;
  }
  bool consume_video_access_unit(const EncodedVideoAccessUnit &unit) override {
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
    if (!this->cadence_.accept(unit.timestamp_90khz)) return false;
#endif
    return RendererVideoSink::consume_video_access_unit(unit);
  }
 protected:
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  RtpFrameCadence90k cadence_{};
  uint8_t fps_{1};
#endif
};

}  // namespace esphome::voip_stack
#endif
