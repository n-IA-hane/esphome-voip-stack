#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESPHOME_VOIP_STACK_VIDEO) && \
    defined(USE_ESPHOME_VOIP_STACK_VIDEO_JPEG)

#include <array>
#include <cstddef>
#include <cstdint>

namespace esphome {
namespace voip_stack {

/// The RFC 2435 subset has two standard colour layouts: 4:2:2 (type 0) and
/// 4:2:0 (type 1), optionally with restart markers (types 64/65).
struct RtpJpegFrameView {
  const uint8_t *scan{nullptr};
  size_t scan_size{0};
  std::array<uint8_t, 128> quantizers{};
  uint16_t width{0};
  uint16_t height{0};
  uint16_t restart_interval{0};
  uint8_t type{0};
};

enum class RtpJpegParseError : uint8_t {
  NONE = 0,
  MALFORMED,
  UNSUPPORTED_PRECISION,
  UNSUPPORTED_SAMPLING,
  NONSTANDARD_HUFFMAN,
  UNSUPPORTED_PROCESS,
};

const char *rtp_jpeg_parse_error_name(RtpJpegParseError error);

/// Parse one complete baseline JFIF frame without allocating or copying it.
/// Returns false for progressive/multi-scan/custom-layout JPEG frames that
/// RFC 2435 types 0/1 cannot describe.
bool parse_jpeg_for_rtp(const uint8_t *data, size_t size,
                        RtpJpegFrameView *frame,
                        RtpJpegParseError *error = nullptr);

/// Build the RFC 2435 headers for one fragment. The returned size excludes
/// entropy data, which the caller appends directly from RtpJpegFrameView.
size_t build_rtp_jpeg_fragment_header(const RtpJpegFrameView &frame,
                                      uint32_t fragment_offset,
                                      uint8_t *output, size_t capacity);

enum class RtpJpegPushResult : uint8_t {
  INCOMPLETE = 0,
  COMPLETE,
  DROPPED,
};

/// Allocation-free RFC 2435 reassembly state. The caller supplies the bounded
/// access-unit buffer already owned by VideoRtpSession.
class RtpJpegDepacketizer {
 public:
  static constexpr uint8_t kFirstCachedQuality = 128;
  static constexpr uint8_t kLastCachedQuality = 254;
  static constexpr size_t kQuantizationTableBytes = 128;
  static constexpr size_t kCachedQualityCount =
      kLastCachedQuality - kFirstCachedQuality + 1;
  static constexpr size_t kQuantizationCacheBytes =
      kCachedQualityCount * kQuantizationTableBytes;

  /// The caller owns this cache for the depacketizer lifetime. Keeping the
  /// 16-KiB table store external lets the ESP32 video session place it in
  /// PSRAM, while the small validity bitmap stays with the parser.
  void set_quantization_cache(uint8_t *storage, size_t capacity);

  RtpJpegPushResult push(const uint8_t *payload, size_t payload_size,
                         bool marker, uint32_t timestamp, uint8_t *output,
                         size_t output_capacity, size_t *output_size);
  /// Drop only the access unit currently being assembled.
  void reset();
  /// Start a new RTP session. RFC 2435 dynamic Q mappings are session-local.
  void reset_session();

 protected:
  bool start_frame_(uint8_t type_specific, uint8_t type, uint8_t quality,
                    uint8_t width_blocks, uint8_t height_blocks,
                    uint16_t restart_interval, const uint8_t *quantizers,
                    size_t quantizer_size, uint32_t timestamp,
                    uint8_t *output, size_t output_capacity);
  const uint8_t *find_cached_quantizers_(uint8_t quality) const;
  bool cache_or_validate_quantizers_(uint8_t quality,
                                     const uint8_t *quantizers);

  bool active_{false};
  uint32_t timestamp_{0};
  uint8_t type_specific_{0};
  uint8_t type_{0};
  uint8_t quality_{0};
  uint8_t width_blocks_{0};
  uint8_t height_blocks_{0};
  uint16_t restart_interval_{0};
  size_t header_size_{0};
  size_t scan_size_{0};
  uint8_t *quantization_cache_{nullptr};
  std::array<uint8_t, (kCachedQualityCount + 7) / 8>
      cached_quantizers_valid_{};
};

}  // namespace voip_stack
}  // namespace esphome

#endif
