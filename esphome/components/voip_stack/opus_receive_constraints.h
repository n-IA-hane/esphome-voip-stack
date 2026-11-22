#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace esphome::voip_stack {

// RFC 7587 receiver preferences for our selected transmitter. These values
// never describe our receiver or change the RTP clock/PCM geometry.
struct OpusReceiveConstraints {
  uint32_t max_average_bitrate{0};  // Unspecified: retain the local target.
  uint16_t max_playback_rate{48000};
  bool stereo{false};

  bool operator==(const OpusReceiveConstraints &other) const {
    return max_average_bitrate == other.max_average_bitrate &&
           max_playback_rate == other.max_playback_rate && stereo == other.stereo;
  }
};
static_assert(sizeof(OpusReceiveConstraints) <= 8, "Keep negotiated Opus facts bounded");

inline void read_opus_receive_constraints(std::string_view parameters, OpusReceiveConstraints &result) {
  const auto trim = [](std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    return value;
  };
  while (!parameters.empty()) {
    const auto separator = parameters.find(';');
    const auto item = trim(parameters.substr(0, separator));
    if (separator == std::string_view::npos) parameters = {};
    else parameters.remove_prefix(separator + 1);
    const auto equals = item.find('=');
    if (equals == std::string_view::npos) continue;
    const auto name = trim(item.substr(0, equals));
    const auto value = trim(item.substr(equals + 1));
    uint32_t number = 0;
    bool valid = !value.empty();
    for (const auto digit : value) {
      if (digit < '0' || digit > '9' || number > (UINT32_MAX - (digit - '0')) / 10) { valid = false; break; }
      number = number * 10 + digit - '0';
    }
    if (!valid) continue;
    if (name == "maxaveragebitrate" && number >= 6000 && number <= 510000)
      result.max_average_bitrate = result.max_average_bitrate == 0 ? number : std::min(result.max_average_bitrate, number);
    else if (name == "maxplaybackrate" && number >= 8000 && number <= 48000)
      result.max_playback_rate = std::min<uint32_t>(result.max_playback_rate, number);
    else if (name == "stereo" && number <= 1)
      result.stereo = number == 1;
  }
}

}  // namespace esphome::voip_stack
