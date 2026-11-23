#pragma once

#include "sip_types.h"

namespace esphome::voip_stack {

inline size_t pcm_receive_buffer_capacity(const AudioFormatList &configured) {
  size_t capacity = 0;
  for (uint8_t i = 0; i < configured.count; ++i) {
    const size_t bytes = configured.formats[i].nominal_frame_bytes();
    if (bytes > capacity) capacity = bytes;
  }
  // Every declared packet duration must also fit at the configured PCM sink
  // clock. Input/jitter storage stays sized for the actual wire PCM geometry.
  if (configured.count != 0 && configured.formats[0].codec == AudioCodec::PCM) {
    for (uint8_t i = 0; i < configured.count; ++i) {
      if (configured.formats[i].pcm_format != PcmFormat::S16LE ||
          configured.formats[i].channels != configured.formats[0].channels ||
          configured.formats[i].sample_rate > configured.formats[0].sample_rate) continue;
      AudioFormat target = configured.formats[0];
      target.frame_ms = configured.formats[i].frame_ms;
      if (target.nominal_frame_bytes() > capacity) capacity = target.nominal_frame_bytes();
    }
  }
  return capacity;
}

inline bool pcm_receive_conversion_supported(const AudioFormat &source, const AudioFormat &target) {
    // The shared streaming resampler handles both receive normalization and
    // transmit alternatives with identical sample/channel accounting.
    return source == target ||
           (source.codec == AudioCodec::PCM && target.codec == AudioCodec::PCM &&
            source.pcm_format == PcmFormat::S16LE && target.pcm_format == PcmFormat::S16LE &&
            source.channels == target.channels && source.frame_ms == target.frame_ms);
  }

// Keep the configured PCM sink clock independent of an accepted wire rate.
// Packet duration still belongs to the negotiated stream. The setup-time RX
// buffers already cover the configured formats; do not exceed that capacity.
inline AudioFormat pcm_playout_format(const AudioFormat &wire, const AudioFormatList &configured) {
  if (!wire.is_valid() || configured.count == 0 ||
      (wire.codec != AudioCodec::PCM && wire.codec != AudioCodec::G722) ||
      wire.pcm_format != PcmFormat::S16LE)
    return wire;
  AudioFormat target = configured.formats[0];
  target.frame_ms = wire.frame_ms;
  const AudioFormat decoded = decoded_pcm_format(wire);
  if (decoded == target) return wire;
  if (!target.is_valid() || target.codec != AudioCodec::PCM || target.pcm_format != PcmFormat::S16LE ||
      !pcm_receive_conversion_supported(decoded, target))
    return wire;
  const size_t capacity = pcm_receive_buffer_capacity(configured);
  for (uint8_t i = 0; i < configured.count; ++i) {
    const auto &format = configured.formats[i];
    // Opus and other mixed codecs retain their established geometry contract.
    if (format.codec != AudioCodec::PCM && format.codec != AudioCodec::G722) return wire;
    if (format.pcm_format != PcmFormat::S16LE) return wire;
  }
  return target.nominal_frame_bytes() <= capacity ? target : wire;
}

// RFC 3264 keeps a dynamic payload's meaning stable for the whole session.
// Active mappings occupy the prefix; retired mappings share the same bounded
// slots so a direction-only reoffer can restore an already admitted alias.
struct RtpAudioPayloads {
  struct Entry {
    AudioFormat format;
    uint8_t payload_type{0};
  };
  Entry entries[VOIP_STACK_MAX_AUDIO_FORMATS]{};
  uint8_t count{0};
  uint8_t known_count{0};

  bool add(uint8_t pt, const AudioFormat &format) {
    if (pt > 127 || !format.is_valid()) return false;
    for (uint8_t i = 0; i < this->count; ++i) {
      if (this->entries[i].payload_type == pt)
        return this->entries[i].format == format;
    }
    // A retained table changes its active set through activate_known_set().
    // Do not overwrite a retired mapping with an unrelated new payload.
    if (this->known_count > this->count || this->count == VOIP_STACK_MAX_AUDIO_FORMATS) return false;
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

  bool find_known(uint8_t pt, AudioFormat *format) const {
    const uint8_t limit = this->known_count > this->count ? this->known_count : this->count;
    if (limit > VOIP_STACK_MAX_AUDIO_FORMATS) return false;
    for (uint8_t i = 0; i < limit; ++i) {
      if (this->entries[i].payload_type == pt) {
        if (format != nullptr) *format = this->entries[i].format;
        return true;
      }
    }
    return false;
  }

  // Protocol bookkeeping only: this does not allocate or admit media resources.
  // The transport must validate geometry against its old committed media owner.
  bool activate_known_set(const RtpAudioPayloads &active) {
    const uint8_t limit = this->known_count > this->count ? this->known_count : this->count;
    if (limit > VOIP_STACK_MAX_AUDIO_FORMATS || active.count > limit) return false;
    RtpAudioPayloads next = *this;
    for (uint8_t i = 0; i < active.count; ++i) {
      uint8_t found = 0;
      while (found < limit && next.entries[found].payload_type != active.entries[i].payload_type) ++found;
      if (found < i || found == limit ||
          !audio_formats_share_rtp_mapping(next.entries[found].format, active.entries[i].format)) return false;
      const Entry previous = next.entries[i];
      next.entries[i] = active.entries[i];
      if (found != i) next.entries[found] = previous;
    }
    next.count = active.count;
    next.known_count = limit;
    *this = next;
    return true;
  }

  AudioFormatList formats() const {
    AudioFormatList result;
    result.count = 0;
    for (uint8_t i = 0; i < this->count; ++i) result.formats[result.count++] = this->entries[i].format;
    return result;
  }

  RtpAudioPayloads pcm_only() const {
    RtpAudioPayloads pcm;
    for (uint8_t i = 0; i < this->count; ++i) {
      const auto &entry = this->entries[i];
      if (entry.format.codec == AudioCodec::PCM) pcm.add(entry.payload_type, entry.format);
    }
    return pcm;
  }

  RtpAudioPayloads decoded_pcm_payloads() const {
    RtpAudioPayloads pcm;
    for (uint8_t i = 0; i < this->count; ++i)
      pcm.add(this->entries[i].payload_type, decoded_pcm_format(this->entries[i].format));
    return pcm;
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

inline AudioFormatList configured_decoded_receive_formats(const RtpAudioPayloads &negotiated,
                                                          const AudioFormatList &configured,
                                                          size_t max_payload) {
  AudioFormatList result;
  result.count = 0;
  for (uint8_t i = 0; i < configured.count; ++i) {
    const auto &candidate = configured.formats[i];
    if (!candidate.is_valid() || audio_format_rtp_encoding(candidate, max_payload) == nullptr) continue;
    for (uint8_t j = 0; j < negotiated.count; ++j) {
      if (!audio_formats_share_rtp_mapping(candidate, negotiated.entries[j].format)) continue;
      const AudioFormat decoded = decoded_pcm_format(candidate);
      if (!audio_format_list_contains(result, decoded) && result.count < VOIP_STACK_MAX_AUDIO_FORMATS)
        result.formats[result.count++] = decoded;
      break;
    }
  }
  return result;
}

}  // namespace esphome::voip_stack
