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
  uint8_t payload_type{103};
  uint32_t clock_rate{90000};
  uint16_t width{640};
  uint16_t height{480};
  uint8_t max_fps{10};
  uint8_t packetization_mode{1};
  bool level_asymmetry_allowed{true};
  std::string encoding{"H264"};
  std::string profile_level_id{"42c01e"};

  bool valid() const {
    return this->payload_type <= 127 && this->clock_rate == 90000 &&
           this->width > 0 && this->height > 0 && this->max_fps > 0 &&
           this->encoding == "H264" && this->packetization_mode == 1 &&
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
  virtual bool start_video(EncodedVideoAccessUnitCallback callback, void *ctx) = 0;
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
