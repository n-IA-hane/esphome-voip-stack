#pragma once

#include "sip_types.h"

#include <string>
#include <string_view>

namespace esphome::voip_stack {

// Read CRLF records without copying the body. Consumers may use the byte
// offset to bind later attributes to the same media section.
class SdpLines {
 public:
  explicit SdpLines(const std::string &body) : body_(body) {}
  bool next(std::string &line) {
    if (next_ >= body_.size()) return false;
    offset_ = next_;
    const auto end = body_.find("\r\n", offset_);
    line.assign(body_, offset_, end == std::string::npos ? end : end - offset_);
    next_ = end == std::string::npos ? body_.size() : end + 2;
    return true;
  }
  size_t offset() const { return offset_; }

 private:
  const std::string &body_;
  size_t next_{0}, offset_{0};
};

// The video reader has historically accepted a direction before a suffix;
// audio requires the complete attribute. Preserve both on existing peers.
inline int sdp_direction(std::string_view line, bool allow_suffix = false) {
  constexpr std::string_view directions[] = {"a=inactive", "a=sendonly", "a=recvonly", "a=sendrecv"};
  if (line.size() < 9 || line[0] != 'a') return -1;
  for (int direction = 0; direction < 4; ++direction) {
    const auto token = directions[direction];
    if (line == token || (allow_suffix && line.substr(0, token.size()) == token)) return direction;
  }
  return -1;
}

std::string trim_copy(const std::string &value);
bool parse_decimal_u32(const std::string &raw, uint32_t max_value, uint32_t *out);
bool parse_rtpmap_format(const std::string &line, AudioFormat *format, uint8_t *payload_type);
bool parse_audio_media_line(const std::string &line, uint16_t *port, bool payload_types[128],
                            uint8_t *preference_rank = nullptr);

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
bool parse_video_media_line(const std::string &line, uint16_t *port, bool payload_types[128],
                            uint8_t *payload_order = nullptr, size_t *payload_count = nullptr);
bool parse_video_rtpmap(const std::string &line, uint8_t *payload_type, std::string *encoding);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
bool parse_video_rtcp_feedback(const std::string &line, bool payloads[128], bool *pli, bool *fir);
std::string fmtp_parameter(const std::string &line, const char *name, uint8_t *payload_type);
#endif
bool parse_rtcp_attribute(const std::string &line, uint16_t *port, uint32_t *ip_v4, bool *has_address);
#endif

}  // namespace esphome::voip_stack
