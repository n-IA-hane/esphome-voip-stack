#pragma once

#include "sip_sdp_audio_selection.h"
#include "sip_sdp_fields.h"

namespace esphome::voip_stack {

inline bool read_audio_offer_attributes(const std::string &sdp, AudioOfferAttributes &attributes) {
  uint8_t session_flow = 0x03;
  bool seen_any_media = false;
  bool in_audio = false;
  SdpLines lines(sdp);
  std::string line;
  while (lines.next(line)) {
    if (line.rfind("m=", 0) == 0) {
      seen_any_media = true;
      in_audio = false;
      if (attributes.section == std::string::npos && line.rfind("m=audio ", 0) == 0) {
        // An invalid m-line can have a valid prefix. Clear any tentative ranks
        // before considering the next line. The first rank wins on duplicates.
        std::fill_n(attributes.payload_rank, 128, UINT8_MAX);
        if (parse_audio_media_line(line, &attributes.port, nullptr, attributes.payload_rank)) {
          attributes.section = lines.offset();
          attributes.flow = session_flow;
          in_audio = true;
        }
      }
    } else if (in_audio && line == "a=x-voip-stack-independent-ptime:1") {
      attributes.independent_ptime = true;
    } else if (in_audio && line.rfind("a=rtpmap:", 0) == 0) {
      uint32_t declared = 0;
      const auto separator = line.find_first_of(" \t", 9);
      if (parse_decimal_u32(line.substr(9, separator - 9), 127, &declared) && (declared == 10 || declared == 11)) {
        AudioFormat mapped; uint8_t payload = 0;
        // CC-7/CC-12: an invalid explicit static mapping removes that payload;
        // it must not become a valid bare static mapping in the second pass.
        if (!parse_rtpmap_format(line, &mapped, &payload)) attributes.payload_rank[declared] = UINT8_MAX;
      }
    } else if (in_audio && line.rfind("a=ptime:", 0) == 0) {
      uint32_t parsed = 0;
      if (parse_decimal_u32(line.substr(8), UINT8_MAX, &parsed) &&
          (parsed == 10 || parsed == 16 || parsed == 20 || parsed == 32)) {
        attributes.ptime = static_cast<uint8_t>(parsed);
      }
    } else if (in_audio && line.rfind("a=maxptime:", 0) == 0) {
      // RFC 8866 allows a nonzero integer or decimal. Durations in our
      // explicit format list are integral, so flooring the bound is exact.
      const auto value = line.substr(11);
      const auto dot = value.find('.');
      uint32_t whole = 0;
      if (!parse_decimal_u32(value.substr(0, dot), UINT32_MAX, &whole)) return false;
      bool fraction_nonzero = false;
      if (dot != std::string::npos) {
        if (dot + 1 == value.size()) return false;
        for (size_t i = dot + 1; i < value.size(); ++i) {
          if (value[i] < '0' || value[i] > '9') return false;
          fraction_nonzero |= value[i] != '0';
        }
      }
      if (whole == 0 && !fraction_nonzero) return false;
      attributes.maxptime = attributes.has_maxptime ? std::min(attributes.maxptime, whole) : whole;
      attributes.has_maxptime = true;
    } else if (in_audio && line.rfind("a=x-voip-stack-flow:", 0) == 0) {
      const size_t value_start = sizeof("a=x-voip-stack-flow:") - 1;
      const size_t space = line.find(' ', value_start);
      if (space != std::string::npos) {
        uint32_t parsed_pt = 0;
        const std::string flow = trim_copy(line.substr(space + 1));
        if (parse_decimal_u32(line.substr(value_start, space - value_start), 127, &parsed_pt)) {
          uint8_t flags = 0;
          if (flow == "send" || flow == "sendrecv") flags |= 0x01;
          if (flow == "recv" || flow == "sendrecv") flags |= 0x02;
          attributes.payload_flow[parsed_pt] = flags;
        }
      }
    } else if (!seen_any_media || in_audio) {
      const int direction = sdp_direction(line);
      if (direction >= 0) {
        if (in_audio)
          attributes.flow = direction;
        else
          session_flow = direction;
      }
    }
  }
  return true;
}

}  // namespace esphome::voip_stack
