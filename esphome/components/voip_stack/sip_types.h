#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace esphome {
namespace voip_stack {

static constexpr uint16_t VOIP_STACK_PORT = 5060;
static constexpr size_t VOIP_STACK_MAX_CALL_ID_LEN = 64;
static constexpr size_t VOIP_STACK_MAX_ROUTE_ID_LEN = 64;
static constexpr size_t VOIP_STACK_MAX_NAME_LEN = 64;
static constexpr size_t VOIP_STACK_MAX_REASON_LEN = 160;

enum class PcmFormat : uint8_t {
  S16LE = 1,
  S24LE = 2,
  S24LE_IN_S32 = 3,
  S32LE = 4,
};

struct AudioFormat {
  uint32_t sample_rate{16000};
  PcmFormat pcm_format{PcmFormat::S16LE};
  uint8_t channels{1};
  uint16_t frame_ms{16};

  uint8_t container_bytes_per_sample() const {
    switch (this->pcm_format) {
      case PcmFormat::S16LE:
        return 2;
      case PcmFormat::S24LE:
        return 3;
      case PcmFormat::S24LE_IN_S32:
      case PcmFormat::S32LE:
        return 4;
      default:
        return 0;
    }
  }

  size_t nominal_frame_samples() const {
    return static_cast<size_t>((static_cast<uint64_t>(this->sample_rate) * this->frame_ms) / 1000u);
  }

  size_t nominal_frame_bytes() const {
    return this->nominal_frame_samples() * this->channels * this->container_bytes_per_sample();
  }

  uint8_t rtp_bytes_per_sample() const {
    if (this->pcm_format == PcmFormat::S24LE_IN_S32) return 3;
    return this->container_bytes_per_sample();
  }

  size_t nominal_rtp_payload_bytes() const {
    return this->nominal_frame_samples() * this->channels * this->rtp_bytes_per_sample();
  }

  bool is_valid() const {
    const bool valid_rate = this->sample_rate == 8000 || this->sample_rate == 12000 ||
                            this->sample_rate == 16000 || this->sample_rate == 24000 ||
                            this->sample_rate == 32000 || this->sample_rate == 44100 ||
                            this->sample_rate == 48000;
    const bool valid_channels = this->channels == 1 || this->channels == 2;
    const bool valid_frame = this->frame_ms == 10 || this->frame_ms == 16 ||
                             this->frame_ms == 20 || this->frame_ms == 32;
    const bool whole_frames = (static_cast<uint64_t>(this->sample_rate) * this->frame_ms) % 1000u == 0;
    return valid_rate && valid_channels && valid_frame && whole_frames && this->container_bytes_per_sample() != 0;
  }

  bool operator==(const AudioFormat &other) const {
    return this->sample_rate == other.sample_rate &&
           this->pcm_format == other.pcm_format &&
           this->channels == other.channels &&
           this->frame_ms == other.frame_ms;
  }
};

static constexpr AudioFormat DEFAULT_AUDIO_FORMAT{};
static constexpr size_t VOIP_STACK_MAX_AUDIO_FORMATS = 8;

inline constexpr uint32_t pack_audio_format(const AudioFormat &format) {
  return (format.sample_rate & 0xFFFFU) | (static_cast<uint32_t>(format.pcm_format) << 16) |
         (static_cast<uint32_t>(format.channels & 0x03U) << 24) |
         (static_cast<uint32_t>(format.frame_ms & 0x3FU) << 26);
}

inline constexpr AudioFormat unpack_audio_format(uint32_t packed) {
  return AudioFormat{
      packed & 0xFFFFU,
      static_cast<PcmFormat>((packed >> 16) & 0xFFU),
      static_cast<uint8_t>((packed >> 24) & 0x03U),
      static_cast<uint16_t>((packed >> 26) & 0x3FU),
  };
}

struct AudioFormatList {
  AudioFormat formats[VOIP_STACK_MAX_AUDIO_FORMATS]{};
  uint8_t count{1};
};

inline void audio_format_list_default(AudioFormatList *out) {
  out->formats[0] = DEFAULT_AUDIO_FORMAT;
  out->count = 1;
}

enum class SipSignalType : uint8_t {
  INVITE,
  STATUS_180_RINGING,
  STATUS_200_OK,
  CANCEL,
  BYE,
  FINAL_RESPONSE,
  OPTIONS,
  AUTH_REQUIRED,
  PROXY_AUTH_REQUIRED,
  MEDIA_INCOMPATIBLE,
  PROTOCOL_ERROR,
};

inline const char *sip_signal_type_name(SipSignalType type) {
  switch (type) {
    case SipSignalType::INVITE: return "INVITE";
    case SipSignalType::STATUS_180_RINGING: return "180";
    case SipSignalType::STATUS_200_OK: return "200";
    case SipSignalType::CANCEL: return "CANCEL";
    case SipSignalType::BYE: return "BYE";
    case SipSignalType::FINAL_RESPONSE: return "FINAL_RESPONSE";
    case SipSignalType::OPTIONS: return "OPTIONS";
    case SipSignalType::AUTH_REQUIRED: return "401";
    case SipSignalType::PROXY_AUTH_REQUIRED: return "407";
    case SipSignalType::MEDIA_INCOMPATIBLE: return "488";
    case SipSignalType::PROTOCOL_ERROR: return "PROTOCOL_ERROR";
    default: return "UNKNOWN";
  }
}

struct SipSignal {
  SipSignalType type{SipSignalType::PROTOCOL_ERROR};
  uint16_t status_code{0};
  std::string call_id;
  std::string caller_route;
  std::string caller_name;
  std::string dest_route;
  std::string dest_name;
  std::string reason;
  AudioFormatList caller_tx_formats{};
  AudioFormatList caller_rx_formats{};
  AudioFormat selected_tx_format{DEFAULT_AUDIO_FORMAT};
  AudioFormat selected_rx_format{DEFAULT_AUDIO_FORMAT};
};

static constexpr uint32_t SAMPLE_RATE = DEFAULT_AUDIO_FORMAT.sample_rate;
static constexpr size_t AUDIO_CHUNK_BYTES = 1024;
static constexpr size_t MAX_AUDIO_CHUNK = 16 * 1024;
static constexpr size_t UDP_SAFE_AUDIO_PAYLOAD_BYTES = 1200;

inline uint8_t audio_format_bits_per_sample(const AudioFormat &format) {
  switch (format.pcm_format) {
    case PcmFormat::S16LE:
      return 16;
    case PcmFormat::S24LE:
      return 24;
    case PcmFormat::S24LE_IN_S32:
    case PcmFormat::S32LE:
      return 32;
    default:
      return 16;
  }
}

inline const char *audio_format_rtp_encoding(const AudioFormat &format,
                                             size_t max_payload = UDP_SAFE_AUDIO_PAYLOAD_BYTES) {
  if (format.nominal_rtp_payload_bytes() > max_payload) return nullptr;
  if (format.pcm_format == PcmFormat::S16LE) return "L16";
  if (format.pcm_format == PcmFormat::S24LE || format.pcm_format == PcmFormat::S24LE_IN_S32) return "L24";
  return nullptr;
}

inline bool audio_format_list_contains(const AudioFormatList &list, const AudioFormat &format) {
  for (uint8_t i = 0; i < list.count; i++) {
    if (list.formats[i] == format) return true;
  }
  return false;
}

inline bool choose_common_audio_format(const AudioFormatList &preferred, const AudioFormatList &supported,
                                       AudioFormat *out) {
  for (uint8_t i = 0; i < preferred.count; i++) {
    if (audio_format_list_contains(supported, preferred.formats[i])) {
      if (out != nullptr) *out = preferred.formats[i];
      return true;
    }
  }
  return false;
}

inline bool audio_format_list_supports_rtp_ptime(const AudioFormatList &list, uint8_t ptime,
                                                 size_t max_payload = UDP_SAFE_AUDIO_PAYLOAD_BYTES) {
  for (uint8_t i = 0; i < list.count; i++) {
    const AudioFormat &candidate = list.formats[i];
    if (candidate.frame_ms == ptime && audio_format_rtp_encoding(candidate, max_payload) != nullptr) return true;
  }
  return false;
}

inline uint8_t choose_common_audio_ptime(const AudioFormatList &tx, const AudioFormatList &rx,
                                         size_t max_payload = UDP_SAFE_AUDIO_PAYLOAD_BYTES) {
  static constexpr uint8_t kPreference[] = {10, 16, 20, 32};
  for (uint8_t ptime : kPreference) {
    if (audio_format_list_supports_rtp_ptime(tx, ptime, max_payload) &&
        audio_format_list_supports_rtp_ptime(rx, ptime, max_payload)) {
      return ptime;
    }
  }
  return 0;
}

inline bool audio_format_list_match_udp_safe(const AudioFormatList &list, const AudioFormat &remote,
                                             AudioFormat *local,
                                             size_t max_payload = UDP_SAFE_AUDIO_PAYLOAD_BYTES) {
  if (remote.nominal_rtp_payload_bytes() > max_payload) return false;
  const char *remote_encoding = audio_format_rtp_encoding(remote, max_payload);
  if (remote_encoding == nullptr) return false;
  for (uint8_t i = 0; i < list.count; i++) {
    const AudioFormat &candidate = list.formats[i];
    const char *candidate_encoding = audio_format_rtp_encoding(candidate, max_payload);
    const bool same_wire_format = candidate.sample_rate == remote.sample_rate &&
                                  candidate.channels == remote.channels &&
                                  candidate.frame_ms == remote.frame_ms &&
                                  candidate_encoding != nullptr && remote_encoding != nullptr &&
                                  std::strcmp(candidate_encoding, remote_encoding) == 0;
    if (same_wire_format) {
      if (local != nullptr) *local = candidate;
      return true;
    }
  }
  return false;
}

}  // namespace voip_stack
}  // namespace esphome
