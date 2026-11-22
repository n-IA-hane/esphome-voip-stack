#pragma once

#include "rtp_audio_payloads.h"
#include "opus_receive_constraints.h"

#include <algorithm>
#include <cstring>

namespace esphome::voip_stack {

// Facts gathered from typed mappings before SDP serialization. The largest
// admitted receive packet for each mapping bounds the complete media line.
struct AudioSdpFacts {
  uint16_t maxptime{0};
  uint16_t required_minimum{0};

  void include_receive(const AudioFormat &mapping, const AudioFormatList &configured,
                       size_t max_payload) {
    const char *mapped_encoding = audio_format_rtp_encoding(mapping, SIZE_MAX);
    uint16_t maximum = 0, minimum = UINT16_MAX;
    for (uint8_t i = 0; i < configured.count; ++i) {
      const auto &format = configured.formats[i];
      const char *encoding = audio_format_rtp_encoding(format, max_payload);
      if (encoding != nullptr && mapped_encoding != nullptr &&
          std::strcmp(encoding, mapped_encoding) == 0 &&
          audio_format_rtp_clock_rate(format) == audio_format_rtp_clock_rate(mapping) &&
          audio_format_rtp_channels(format) == audio_format_rtp_channels(mapping)) {
        maximum = std::max(maximum, format.frame_ms);
        minimum = std::min(minimum, format.frame_ms);
      }
    }
    if (maximum != 0) {
      this->maxptime = this->maxptime == 0 ? maximum : std::min(this->maxptime, maximum);
      this->required_minimum = std::max(this->required_minimum, minimum);
    }
  }

  bool valid() const { return this->maxptime == 0 || this->required_minimum <= this->maxptime; }
};

// The audio part of a signaling proposal. RTP runtime state and task
// ownership stay with the transport that commits the negotiated values.
struct SdpAudioMedia {
  AudioFormat tx{DEFAULT_AUDIO_FORMAT}, rx{DEFAULT_AUDIO_FORMAT};
  uint8_t tx_pt{96}, rx_pt{96};
#ifdef USE_ESPHOME_VOIP_STACK_OPUS
  OpusReceiveConstraints opus_tx;
#endif
  RtpAudioPayloads rx_payloads;
  bool send{true}, receive{true}, zero_connection{false};
  uint32_t audio_ip{0};
  uint16_t audio_port{0};
};

struct AudioSdpEnvelope {
  uint64_t session_id;
  uint32_t version;
  uint16_t port;
};

}  // namespace esphome::voip_stack
