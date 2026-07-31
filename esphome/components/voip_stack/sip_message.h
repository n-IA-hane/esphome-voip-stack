#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_ESPHOME_VOIP_SIP_TRANSPORT)

#include "transport.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace esphome {
namespace voip_stack {

static constexpr size_t MAX_SIP_BODY_BYTES = 4096;
static constexpr size_t MAX_SIP_TCP_RX_BUFFER = 8192;

std::string trim_copy(const std::string &value);
std::string sip_uri_user_encode(const std::string &raw);
std::string sip_uri_user_decode(const std::string &raw);
std::string header_value(const std::string &message, const char *name);
std::string header_values(const std::string &message, const char *name);
bool parse_decimal_u32(const std::string &raw, uint32_t max_value,
                       uint32_t *out);
bool time_reached(uint32_t now, uint32_t deadline);
std::string message_body(const std::string &message);
bool sip_content_length(const std::string &message, size_t *out);
std::string sip_header_token(
    const std::string &raw,
    size_t max_bytes = VOIP_STACK_MAX_REASON_LEN);
std::string sip_quoted(const std::string &raw);
std::string reason_text_from_header(const std::string &value);
std::string cseq_method(const std::string &cseq);
uint32_t cseq_number(const std::string &cseq);
std::string via_branch(const std::string &via);
bool sip_method_known_(const std::string &method);
std::string sip_failure_reason_(int status);
std::string tag_from_header(const std::string &value);
std::string strip_angle_uri(const std::string &value);
bool sip_uri_ipv4_target(const std::string &value, uint32_t *ip_v4,
                         uint16_t *port);
std::string sip_user_from_header(const std::string &value);
std::string response_via_with_rport(const std::string &via,
                                    uint32_t source_ip,
                                    uint16_t source_port);
std::string make_token(const char *prefix);
bool parse_rtpmap_format(const std::string &line, AudioFormat *format,
                         uint8_t *payload_type);
bool parse_audio_media_line(const std::string &line, uint16_t *port,
                            bool payload_types[128]);

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
bool parse_video_media_line(const std::string &line, uint16_t *port,
                            bool payload_types[128],
                            uint8_t *payload_order = nullptr,
                            size_t *payload_count = nullptr);
bool parse_video_rtpmap(const std::string &line, uint8_t *payload_type,
                        std::string *encoding);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
bool parse_video_rtcp_feedback(const std::string &line, bool payloads[128],
                               bool *pli, bool *fir);
std::string fmtp_parameter(const std::string &line, const char *name,
                           uint8_t *payload_type);
#endif
bool parse_rtcp_attribute(const std::string &line, uint16_t *port,
                          uint32_t *ip_v4, bool *has_address);
#endif
size_t pcm_to_rtp_payload(const uint8_t *pcm, size_t bytes,
                          const AudioFormat &format, uint8_t *dst,
                          size_t dst_cap);
size_t rtp_payload_to_pcm(const uint8_t *payload, size_t payload_len,
                          const AudioFormat &format, uint8_t *pcm,
                          size_t pcm_cap);

}  // namespace voip_stack
}  // namespace esphome

#endif
