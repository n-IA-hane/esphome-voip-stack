#pragma once

#include "video.h"
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO

#include "sip_sdp_fields.h"
#include "video_payload_contract.h"
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
#include "sdp_imageattr.h"
#endif

#include <algorithm>
#include <arpa/inet.h>
#include <cstring>

namespace esphome::voip_stack {

struct VideoOfferAttributes {
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  ImageAttributeMatch imageattr_specific, imageattr_wildcard;
#endif
  bool offered_payloads[128]{};
  VideoPayloadMeaning payload_meanings[128]{};
  uint8_t offered_payload_order[128]{};
  size_t offered_payload_count = 0;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
  bool h264_payloads[128]{};
#endif
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  bool jpeg_mapping_seen{false}, jpeg_mapping_matches{false};
#endif
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
  char profiles[128][7]{};
  bool profile_valid[128]{};
  uint8_t packetization_modes[128]{};
  bool level_asymmetry_allowed[128]{};
  bool rtcp_feedback_pli[128]{};
  bool rtcp_feedback_fir[128]{};
#endif
  uint8_t media_max_fps = 0;
  uint32_t media_max_bitrate_bps = 0;
  uint16_t media_port = 0;
  uint32_t media_ip{0};
  bool media_zero_connection = false;
  uint16_t media_rtcp_port = 0;
  uint32_t media_rtcp_ip{0};
  bool media_rtcp_attribute_invalid = false;
  bool rtcp_mux_only = false;
  uint8_t media_direction = 0x03;  // peer send + receive
  bool offered{false};
};

inline void read_video_offer_attributes(const std::string &sdp, uint32_t default_ip,
                                        [[maybe_unused]] const VideoCapability &local_send,
                                        [[maybe_unused]] const VideoCapability &local_receive,
                                        VideoOfferAttributes &offer) {
  uint32_t session_ip = default_ip;
  bool session_zero_connection = false;
  uint8_t session_direction = 0x03;
  bool seen_media = false;
  bool in_video = false;

  bool media_rtcp_address_explicit = false;
  offer.media_ip = offer.media_rtcp_ip = default_ip;
  SdpLines lines(sdp);
  std::string line;
  while (lines.next(line)) {
    if (line.rfind("m=", 0) == 0) {
      seen_media = true;
      in_video = false;
      if (!offer.offered && line.rfind("m=video ", 0) == 0) {
        // A rejected m-line can leave partial payload flags. The order and
        // count are consumed only after a complete line succeeds.
        std::fill_n(offer.offered_payloads, 128, false);
        if (parse_video_media_line(line, &offer.media_port, offer.offered_payloads, offer.offered_payload_order,
                                   &offer.offered_payload_count)) {
          offer.offered = true;
          offer.media_ip = session_ip;
          offer.media_zero_connection = session_zero_connection;
          offer.media_rtcp_ip = session_ip;
          offer.media_rtcp_port = offer.media_port == UINT16_MAX ? 0 : static_cast<uint16_t>(offer.media_port + 1);
          media_rtcp_address_explicit = false;
          offer.media_direction = session_direction;
          in_video = true;
        }
      }
    } else if (!seen_media && line.rfind("c=IN IP4 ", 0) == 0) {
      struct in_addr address{};
      if (inet_aton(line.substr(9).c_str(), &address) != 0) {
        session_zero_connection = address.s_addr == 0;
        if (!session_zero_connection) {
          session_ip = ntohl(address.s_addr);
          offer.media_ip = session_ip;
          offer.media_rtcp_ip = session_ip;
        }
      }
    } else if (in_video && line.rfind("c=IN IP4 ", 0) == 0) {
      struct in_addr address{};
      if (inet_aton(line.substr(9).c_str(), &address) != 0) {
        offer.media_zero_connection = address.s_addr == 0;
        if (!offer.media_zero_connection) {
          offer.media_ip = ntohl(address.s_addr);
          if (!media_rtcp_address_explicit) offer.media_rtcp_ip = offer.media_ip;
        }
      }
    } else if (in_video && line.rfind("a=rtcp:", 0) == 0) {
      uint16_t parsed_port = 0;
      uint32_t parsed_ip = offer.media_ip;
      bool has_address = false;
      if (parse_rtcp_attribute(line, &parsed_port, &parsed_ip, &has_address)) {
        offer.media_rtcp_port = parsed_port;
        offer.media_rtcp_ip = has_address ? parsed_ip : offer.media_ip;
        media_rtcp_address_explicit = has_address;
      } else {
        offer.media_rtcp_attribute_invalid = true;
      }
    } else if (in_video && line == "a=rtcp-mux-only") {
      offer.rtcp_mux_only = true;
    } else if (in_video && line.rfind("a=rtpmap:", 0) == 0) {
      uint8_t payload_type = 0;
      std::string encoding;
      if (parse_video_rtpmap(line, &payload_type, &encoding) && offer.offered_payloads[payload_type]) {
        offer.payload_meanings[payload_type] = encoding == "H264/90000" ? VideoPayloadMeaning::H264
            : encoding == "JPEG/90000" ? VideoPayloadMeaning::JPEG : VideoPayloadMeaning::OTHER;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
        offer.h264_payloads[payload_type] = encoding == "H264/90000";
#endif
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
        if (payload_type == 26) {
          offer.jpeg_mapping_seen = true;
          offer.jpeg_mapping_matches = encoding == "JPEG/90000";
        }
#endif
      }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
    } else if (in_video && line.rfind("a=fmtp:", 0) == 0) {
      uint8_t payload_type = 0;
      const std::string profile = fmtp_parameter(line, "profile-level-id", &payload_type);
      if (offer.offered_payloads[payload_type] && profile.size() == 6) {
        memcpy(offer.profiles[payload_type], profile.data(), 6);
        offer.profile_valid[payload_type] = true;
      }
      uint8_t mode_payload = 0;
      const std::string mode = fmtp_parameter(line, "packetization-mode", &mode_payload);
      if (offer.offered_payloads[mode_payload] && mode == "1") offer.packetization_modes[mode_payload] = 1;
      uint8_t asymmetry_payload = 0;
      const std::string asymmetry = fmtp_parameter(line, "level-asymmetry-allowed", &asymmetry_payload);
      if (offer.offered_payloads[asymmetry_payload] && asymmetry == "1")
        offer.level_asymmetry_allowed[asymmetry_payload] = true;
    } else if (in_video && line.rfind("a=rtcp-fb:", 0) == 0) {
      parse_video_rtcp_feedback(line, offer.offered_payloads, offer.rtcp_feedback_pli, offer.rtcp_feedback_fir);
#endif
    } else if (in_video && line.rfind("b=TIAS:", 0) == 0) {
      uint32_t parsed_bitrate = 0;
      if (parse_decimal_u32(trim_copy(line.substr(7)), UINT32_MAX, &parsed_bitrate) && parsed_bitrate > 0) {
        offer.media_max_bitrate_bps = parsed_bitrate;
      }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
    } else if (in_video && line.rfind("a=imageattr:", 0) == 0) {
      const auto parsed = ImageAttributeParser::parse(std::string_view(line).substr(12), local_receive.width,
                                                      local_receive.height, local_send.width, local_send.height);
      if (parsed.valid) {
        // RFC6236 permits send/recv on separate attribute lines. Merge each
        // direction independently; payload-specific entries override wildcard
        // entries only for the direction they actually declare.
        auto &target = parsed.explicit_payload ? offer.imageattr_specific : offer.imageattr_wildcard;
        target.valid = true;
        if (parsed.send_present) {
          target.send_compatible = (!target.send_present || target.send_compatible) && parsed.send_compatible;
          target.send_present = true;
        }
        if (parsed.recv_present) {
          target.recv_compatible = (!target.recv_present || target.recv_compatible) && parsed.recv_compatible;
          target.recv_present = true;
        }
      }
#endif
    } else if (in_video && line.rfind("a=framerate:", 0) == 0) {
      uint32_t parsed_fps = 0;
      if (parse_decimal_u32(trim_copy(line.substr(12)), UINT8_MAX, &parsed_fps) && parsed_fps > 0) {
        offer.media_max_fps = static_cast<uint8_t>(parsed_fps);
      }
    }
    if (!seen_media || in_video) {
      const int direction = sdp_direction(line, true);
      if (direction >= 0) {
        if (in_video)
          offer.media_direction = direction;
        else
          session_direction = direction;
      }
    }
  }
}

}  // namespace esphome::voip_stack
#endif
