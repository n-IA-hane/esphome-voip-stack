#pragma once

#include "esphome/core/defines.h"
#ifdef USE_ESPHOME_VOIP_REGISTRATION
#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string_view>

namespace esphome::voip_stack {
// Shared by the native adapter and the maintained host registration qualifier.
inline bool sip_sdp_supports_directional_audio_v1(std::string_view body) {
  // RFC 3264 section 9 uses m=audio 0 for capabilities. Read only an explicit
  // D1 attribute on an advertised audio payload, never an account or UA name.
  if (body.substr(0, 5) != "v=0\r\n") return false;
  bool audio = false, payloads[128]{};
  auto number = [](std::string_view value, uint32_t maximum, uint32_t *out) {
    if (value.empty()) return false;
    uint32_t parsed = 0;
    for (const char ch : value) {
      if (ch < '0' || ch > '9' || parsed > (maximum - (ch - '0')) / 10) return false;
      parsed = parsed * 10 + ch - '0';
    }
    *out = parsed; return true;
  };
  for (size_t pos = 0; pos < body.size();) {
    const auto end = body.find("\r\n", pos);
    const auto line = body.substr(pos, end == std::string_view::npos ? end : end - pos);
    if (line.substr(0, 2) == "m=") {
      audio = false;
      std::fill(std::begin(payloads), std::end(payloads), false);
      if (line.substr(0, 8) == "m=audio ") {
        const auto port_end = line.find(' ', 8);
        uint32_t port;
        if (port_end != std::string_view::npos && number(line.substr(8, port_end - 8), 65535, &port) &&
            line.substr(port_end + 1, 8) == "RTP/AVP ") {
          audio = true;
          for (size_t at = port_end + 9; at < line.size();) {
            const auto next = line.find(' ', at); uint32_t payload;
            if (!number(line.substr(at, next == std::string_view::npos ? next : next - at), 127, &payload)) {
              audio = false; break;
            }
            payloads[payload] = true;
            if (next == std::string_view::npos) break;
            at = next + 1;
          }
        }
      }
    } else if (audio && line.substr(0, 20) == "a=x-voip-stack-flow:") {
      const auto split = line.find(' ', 20); uint32_t payload;
      if (split != std::string_view::npos && number(line.substr(20, split - 20), 127, &payload) && payloads[payload]) {
        const auto flow = line.substr(split + 1);
        if (flow == "sendrecv" || flow == "send" || flow == "recv") return true;
      }
    }
    if (end == std::string_view::npos) break;
    pos = end + 2;
  }
  return false;
}
}  // namespace esphome::voip_stack
#endif
