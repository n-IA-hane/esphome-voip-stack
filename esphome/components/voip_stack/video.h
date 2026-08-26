#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO

#if defined(USE_ESPHOME_VOIP_STACK_VIDEO_JPEG) == \
    defined(USE_ESPHOME_VOIP_STACK_VIDEO_H264)
#error "voip_stack video builds require exactly one codec backend"
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace esphome {
namespace voip_stack {

enum class VideoCodec : uint8_t {
  JPEG = 0,
  H264,
};

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

/// Wrap-safe 90 kHz token bucket with one frame of bounded recovery credit.
class RtpFrameCadence90k {
 public:
  void reset(uint8_t frames_per_second) {
    const uint32_t fps = frames_per_second == 0 ? 1U : frames_per_second;
    this->interval_.store((90000U + fps - 1U) / fps,
                          std::memory_order_relaxed);
    this->seen_.store(false, std::memory_order_release);
    this->credit_.store(0, std::memory_order_relaxed);
  }

  bool accept(uint32_t timestamp) {
    const uint32_t interval = this->interval_.load(std::memory_order_relaxed);
    if (!this->seen_.exchange(true, std::memory_order_acq_rel)) {
      this->last_.store(timestamp, std::memory_order_relaxed);
      return true;
    }
    const uint32_t previous = this->last_.exchange(
        timestamp, std::memory_order_acq_rel);
    const uint32_t elapsed = timestamp - previous;
    uint32_t credit = this->credit_.load(std::memory_order_relaxed);
    // Preserve fractional phase during normal source cadence. After a long
    // stall, reset to one due frame instead of retaining catch-up credit that
    // would burst expensive codec/PPA work back-to-back.
    if (elapsed >= interval * 2U) {
      credit = interval;
    } else {
      credit = elapsed >= UINT32_MAX - credit ? UINT32_MAX : credit + elapsed;
    }
    if (credit < interval) {
      this->credit_.store(credit, std::memory_order_relaxed);
      return false;
    }
    this->credit_.store(credit - interval, std::memory_order_relaxed);
    return true;
  }

 protected:
  std::atomic<uint32_t> interval_{9000};
  std::atomic<uint32_t> last_{0};
  std::atomic<uint32_t> credit_{0};
  std::atomic<bool> seen_{false};
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
  uint32_t max_bitrate_bps{0};
  bool rtcp_feedback_pli{false};
  bool rtcp_feedback_fir{false};
  std::string encoding;
  std::string profile_level_id;

  bool is_h264() const { return this->encoding == "H264"; }
  bool is_jpeg() const { return this->encoding == "JPEG"; }

  bool valid() const {
    if (this->payload_type > 127 || this->clock_rate != 90000 ||
        this->width == 0 || this->height == 0 || this->max_fps == 0) {
      return false;
    }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
    if (this->is_jpeg()) {
      // JPEG has a static RTP/AVP assignment. Refuse private remappings so an
      // SDP answer can never silently disagree with RFC 3551 section 6.
      return this->payload_type == 26;
    }
#endif
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
    if (this->is_h264()) {
      return this->payload_type >= 96 && this->packetization_mode == 1 &&
             this->profile_level_id.size() == 6;
    }
#endif
    return false;
  }
};

using EncodedVideoAccessUnitCallback =
    void (*)(void *ctx, const EncodedVideoAccessUnit &access_unit);

/// Producer-side media adapter. It does not own SIP or call lifecycle.
class EncodedVideoSource {
 public:
  virtual ~EncodedVideoSource() = default;
  virtual VideoCapability get_video_capability() const = 0;
  /// Validate and reserve any source-side state needed by the negotiated
  /// format without producing media. Offer/answer can therefore admit a
  /// stream before its final response without leaking RTP before commit.
  virtual bool prepare_video(const VideoCapability &capability) = 0;
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
  /// Allocate/start dormant decoder resources. This must not make a stale
  /// frame visible before the SDP transaction commits.
  virtual bool start_video(const VideoCapability &capability) = 0;
  /// Commit or suspend receive presentation without joining/freeing the
  /// prepared decoder worker. Direction-only re-INVITEs stay lightweight.
  virtual bool set_video_active(bool active) = 0;
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
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
bool h264_profile_level_id_from_annex_b(const uint8_t *data, size_t size,
                                        std::string *profile_level_id);

/// We intentionally distinguish Baseline and Constrained Baseline. Comparing
/// only profile_idc=66 would negotiate streams that the constrained decoder
/// contract does not promise to accept.
bool h264_same_subprofile(const std::string &left, const std::string &right);

/// True when both IDs are the same RFC 6184 sub-profile and the source level
/// does not exceed the receiver's advertised level.
bool h264_level_fits(const std::string &source, const std::string &receiver);
#endif

}  // namespace voip_stack
}  // namespace esphome

#endif  // USE_ESPHOME_VOIP_STACK_VIDEO
