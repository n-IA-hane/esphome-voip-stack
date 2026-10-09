#pragma once

#include "sip_types.h"

namespace esphome::voip_stack {

inline bool pcm_receive_conversion_supported(const AudioFormat &source, const AudioFormat &target) {
    // Alternate S16 PCM rates are normalized upwards to the selected sink.
    // Other encodings keep their existing single-format negotiation.
    return source == target ||
           (source.codec == AudioCodec::PCM && target.codec == AudioCodec::PCM &&
            source.pcm_format == PcmFormat::S16LE && target.pcm_format == PcmFormat::S16LE &&
            source.channels == target.channels && source.frame_ms == target.frame_ms &&
            source.sample_rate <= target.sample_rate);
  }

// A payload number has meaning only inside the current offer/answer exchange.
// Keep the small negotiated set, rather than a permanent 128-entry RTP table.
struct RtpAudioPayloads {
  struct Entry {
    AudioFormat format;
    uint8_t payload_type{0};
  };
  Entry entries[VOIP_STACK_MAX_AUDIO_FORMATS]{};
  uint8_t count{0};

  bool add(uint8_t pt, const AudioFormat &format) {
    if (pt > 127 || !format.is_valid()) return false;
    for (uint8_t i = 0; i < this->count; ++i) {
      if (this->entries[i].payload_type == pt)
        return this->entries[i].format == format;
    }
    if (this->count == VOIP_STACK_MAX_AUDIO_FORMATS) return false;
    this->entries[this->count++] = Entry{format, pt};
    return true;
  }

  bool find(uint8_t pt, AudioFormat *format) const {
    for (uint8_t i = 0; i < this->count; ++i) {
      if (this->entries[i].payload_type == pt) {
        if (format != nullptr) *format = this->entries[i].format;
        return true;
      }
    }
    return false;
  }

  bool operator==(const RtpAudioPayloads &other) const {
    if (this->count != other.count) return false;
    for (uint8_t i = 0; i < this->count; ++i) {
      AudioFormat format;
      if (!other.find(this->entries[i].payload_type, &format) ||
          !(format == this->entries[i].format)) return false;
    }
    return true;
  }
};

}  // namespace esphome::voip_stack
