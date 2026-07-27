#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO

#include <cstddef>
#include <cstdint>
#include <string>

namespace esphome {
namespace voip_stack {

/// One complete encoded video access unit.
///
/// Ownership stays with the producer for the duration of the callback. RTP
/// packetization must finish (or copy/drop the complete AU) before returning.
struct EncodedVideoAccessUnit {
  const uint8_t *data{nullptr};
  size_t size{0};
  uint32_t timestamp_90khz{0};
  bool key_frame{false};
};

struct VideoCapability {
  // Default construction is intentionally invalid. A source/sink must declare
  // its real codec contract (and H.264 must derive profile-level-id from the
  // encoder SPS) instead of inheriting a plausible but fabricated profile.
  uint8_t payload_type{0};
  uint32_t clock_rate{90000};
  uint16_t width{0};
  uint16_t height{0};
  uint8_t max_fps{0};
  uint8_t packetization_mode{0};
  bool level_asymmetry_allowed{false};
  std::string encoding;
  std::string profile_level_id;

  bool is_h264() const { return this->encoding == "H264"; }
  bool is_jpeg() const { return this->encoding == "JPEG"; }

  bool valid() const {
    if (this->payload_type > 127 || this->clock_rate != 90000 ||
        this->width == 0 || this->height == 0 || this->max_fps == 0) {
      return false;
    }
    if (this->is_jpeg()) {
      // JPEG has a static RTP/AVP assignment. Refuse private remappings so an
      // SDP answer can never silently disagree with RFC 3551 section 6.
      return this->payload_type == 26;
    }
    return this->is_h264() && this->payload_type >= 96 &&
           this->packetization_mode == 1 &&
           this->profile_level_id.size() == 6;
  }
};

using EncodedVideoAccessUnitCallback =
    void (*)(void *ctx, const EncodedVideoAccessUnit &access_unit);

/// Producer-side media adapter. It does not own SIP or call lifecycle.
class EncodedVideoSource {
 public:
  virtual ~EncodedVideoSource() = default;
  virtual VideoCapability get_video_capability() const = 0;
  /// Start the producer with the codec/dimensions/rate committed by SDP.
  /// Sources that can reconfigure an encoder do so here; adapters for a fixed
  /// source must at least enforce the negotiated maximum frame rate.
  virtual bool start_video(EncodedVideoAccessUnitCallback callback, void *ctx,
                           const VideoCapability &capability) = 0;
  virtual void stop_video() = 0;
  virtual void request_key_frame() {}
};

/// Consumer-side media adapter. It receives only complete access units.
class EncodedVideoSink {
 public:
  virtual ~EncodedVideoSink() = default;
  /// Local decoder contract advertised in SDP. This is intentionally
  /// independent from the encoder SPS used by EncodedVideoSource.
  virtual VideoCapability get_receive_video_capability() const = 0;
  virtual bool start_video(const VideoCapability &capability) = 0;
  virtual void stop_video() = 0;
  /// Returns false when the complete access unit could not be admitted. RTP
  /// then requests a fresh random-access point instead of feeding dependent
  /// pictures after a locally dropped reference frame.
  virtual bool consume_video_access_unit(
      const EncodedVideoAccessUnit &access_unit) = 0;
  virtual void request_key_frame() {}
};

/// Derive RFC 6184 profile-level-id from the first SPS in an Annex-B AU.
/// Returns false when the AU has no complete SPS or is malformed.
bool h264_profile_level_id_from_annex_b(const uint8_t *data, size_t size,
                                        std::string *profile_level_id);

/// We intentionally distinguish Baseline and Constrained Baseline. Comparing
/// only profile_idc=66 would negotiate streams that the constrained decoder
/// contract does not promise to accept.
bool h264_same_subprofile(const std::string &left, const std::string &right);

/// True when both IDs are the same RFC 6184 sub-profile and the source level
/// does not exceed the receiver's advertised level.
bool h264_level_fits(const std::string &source, const std::string &receiver);

}  // namespace voip_stack
}  // namespace esphome

#endif  // USE_ESPHOME_VOIP_STACK_VIDEO
