#pragma once

#include "sip_sdp_facts.h"

#include <algorithm>
#include <cstdint>
#include <string>

namespace esphome::voip_stack {

// Per-offer inputs. Configured and previously advertised mappings stay with
// the dialog and are read by reference throughout selection.
struct LocalAudioOffer {
  const AudioFormatList &tx;
  const AudioFormatList &rx;
  const AudioFormatList &offered_tx;
  const RtpAudioPayloads &offered_rx;
  size_t max_payload;
#ifdef USE_ESPHOME_VOIP_DTMF
  uint8_t offered_dtmf_clocks;
#endif
  size_t max_receive_payload{RTP_AUDIO_RECEIVE_PAYLOAD_BYTES};
};

struct AudioOfferAttributes {
  uint8_t ptime{0};
  uint32_t maxptime{0};
  bool has_maxptime{false};
  uint8_t flow{3};
  uint8_t payload_rank[128];
  uint16_t port{0};
  size_t section{std::string::npos};

  AudioOfferAttributes() { std::fill_n(payload_rank, 128, UINT8_MAX); }
  bool offered(uint8_t pt) const { return section != std::string::npos && payload_rank[pt] != UINT8_MAX; }
};

struct AudioMappingChanges {
  bool rx{false}, tx{false}, unsupported{false};
};

struct AudioMappingPolicy {
  const LocalAudioOffer &local;
  const AudioOfferAttributes &attributes;
  const AudioFormat &retained_tx_format;
  uint16_t transmit_ptime;
  bool remote_is_answer, retained_call;
};

enum class AudioSelectionResult { ACCEPTED, INCOMPATIBLE, INVALID };

struct AudioPayloadSelection {
  bool selected_tx{false}, selected_rx{false}, shared_format{false};
  RtpAudioPayloads received_payloads;
  AudioFormat selected_tx_format{disabled_audio_format()}, selected_rx_format{disabled_audio_format()};
  uint8_t selected_tx_payload_type{0}, selected_rx_payload_type{0}, selected_tx_rank{255};
  uint32_t seen_mapping[128]{};

  bool consider(AudioFormat fmt, uint8_t pt, const AudioMappingPolicy &policy, AudioMappingChanges &changes) {
    const auto &local = policy.local;
    const auto &attributes = policy.attributes;
    const auto fixed = static_rtp_audio_format(pt);
    if (fixed.is_valid() && !audio_formats_share_rtp_mapping(fmt, fixed)) return false;
    fmt.frame_ms = attributes.ptime;
    const uint32_t packed_mapping = pack_audio_format(fmt);
    if (seen_mapping[pt] != 0 && seen_mapping[pt] != packed_mapping) return false;
    seen_mapping[pt] = packed_mapping;
    AudioFormat local_rx;
    AudioFormat local_tx;
    // An inactive reoffer retains the admitted mappings.
    const uint8_t flow = policy.retained_call ? 0x03 : attributes.flow;
    const bool peer_can_send = (flow & 0x01) != 0;
    const bool peer_can_recv = (flow & 0x02) != 0;
    uint16_t preferred_tx_ptime = policy.transmit_ptime;
    // Omitting an optional preference in a direction-only reoffer must not
    // invent a TX quantum change. match_tx still enforces the declared
    // mapping, peer maxptime and UDP budget before retaining that duration.
    if (preferred_tx_ptime == 0 && policy.retained_tx_format.is_valid() &&
        audio_formats_share_rtp_mapping(policy.retained_tx_format, fmt))
      preferred_tx_ptime = policy.retained_tx_format.frame_ms;
    bool tx_ok =
        peer_can_recv && (match_tx(local.tx, fmt, preferred_tx_ptime, attributes, local.max_payload, &local_tx) ||
                          (preferred_tx_ptime != 0 &&
                           match_tx(local.tx, fmt, 0, attributes, local.max_payload, &local_tx)));
    // The peer's ptime is its receive preference, hence our TX duration.
    // An answer follows the executable receive preference captured from our
    // actual offer, rather than recomputing it from the configured catalog.
    uint16_t preferred_rx_ptime = 0;
    if (policy.remote_is_answer) {
      for (uint8_t i = 0; i < local.offered_rx.count; ++i) {
        const auto &entry = local.offered_rx.entries[i];
        if (audio_formats_share_rtp_mapping(entry.format, fmt)) {
          preferred_rx_ptime = entry.format.frame_ms;
          break;
        }
      }
    }
    bool rx_ok = peer_can_send &&
                 (audio_format_list_match_rtp_mapping(local.rx, fmt, preferred_rx_ptime, &local_rx, local.max_receive_payload) ||
                  (preferred_rx_ptime != 0 &&
                   audio_format_list_match_rtp_mapping(local.rx, fmt, 0, &local_rx, local.max_receive_payload)));
    uint8_t receive_pt = pt;
    if (policy.remote_is_answer) {
      // Configured duration and UDP budget were checked above. The answer
      // may request another ptime while retaining an offered RTP mapping.
      bool offered_tx = false;
      for (uint8_t i = 0; i < local.offered_tx.count; ++i) {
        if (audio_formats_share_rtp_mapping(local.offered_tx.formats[i], fmt)) {
          offered_tx = true;
          break;
        }
      }
      tx_ok = tx_ok && offered_tx;
      bool offered_rx = false;
      for (uint8_t i = 0; i < local.offered_rx.count; ++i) {
        const auto &entry = local.offered_rx.entries[i];
        if (audio_formats_share_rtp_mapping(entry.format, fmt)) {
          receive_pt = entry.payload_type;
          offered_rx = true;
          break;
        }
      }
      rx_ok = rx_ok && offered_rx;
    }
    shared_format |= tx_ok && rx_ok;
    const bool unidirectional = local.tx.count == 0 || local.rx.count == 0;
    const bool eligible = unidirectional || attributes.flow != 0x03 || (tx_ok && rx_ok);
    // RFC 3264 allows independent send preferences. Keep every accepted
    // receive mapping; select the first compatible local preference.
    // Accepted S16 rate alternatives use the existing audio-library resampler.
    if (rx_ok && eligible && !received_payloads.add(receive_pt, local_rx)) return false;
    if (rx_ok && eligible &&
        (!selected_rx || local_rank(local.rx, local_rx) < local_rank(local.rx, selected_rx_format))) {
      selected_rx_format = local_rx;
      selected_rx_payload_type = receive_pt;
      selected_rx = true;
      changes.rx = true;
    }
    if (tx_ok && eligible && (unidirectional || attributes.flow != 0x03 || rx_ok) &&
        (!selected_tx || local_rank(local.tx, local_tx) < local_rank(local.tx, selected_tx_format) ||
         (local_rank(local.tx, local_tx) == local_rank(local.tx, selected_tx_format) &&
          attributes.payload_rank[pt] < selected_tx_rank))) {
      selected_tx_format = local_tx;
      selected_tx_payload_type = pt;
      selected_tx_rank = attributes.payload_rank[pt];
      selected_tx = true;
      changes.tx = true;
    } else if (!selected_tx && !selected_rx) {
      changes.unsupported = true;
    }

    return true;
  }

  AudioSelectionResult finish(const LocalAudioOffer &local, const AudioOfferAttributes &attributes,
                              const SdpAudioMedia &retained, bool retained_call, uint32_t media_ip,
                              bool zero_connection, SdpAudioMedia &media, bool remote_is_answer = false) {
    const bool want_tx = local.tx.count != 0 && !zero_connection && (attributes.flow & 0x02) != 0;
    const bool want_rx = local.rx.count != 0 && !zero_connection && (attributes.flow & 0x01) != 0;
    if (retained_call) {
      // Check every offered meaning before filtering unsupported mappings.
      // A retired dynamic PT cannot be reassigned within the same session.
      for (uint8_t pt = 0; pt < 128; ++pt) {
        if (seen_mapping[pt] == 0) continue;
        const auto offered = unpack_audio_format(seen_mapping[pt]);
        AudioFormat previous;
        if ((retained.rx_payloads.find_known(pt, &previous) &&
             !audio_formats_share_rtp_mapping(previous, offered)) ||
            (pt == retained.tx_pt && retained.tx.is_valid() &&
             !audio_formats_share_rtp_mapping(retained.tx, offered))) return AudioSelectionResult::INVALID;
      }
      const auto retained_mapping_offered = [&](const AudioFormat &format, uint8_t pt) {
        return format.is_valid() && seen_mapping[pt] != 0 &&
               audio_formats_share_rtp_mapping(format, unpack_audio_format(seen_mapping[pt]));
      };
      if (want_rx && !remote_is_answer && retained_mapping_offered(retained.rx, retained.rx_pt) &&
          audio_format_list_contains(local.rx, retained.rx) &&
          audio_format_rtp_encoding(retained.rx, local.max_receive_payload) != nullptr) {
        AudioFormat prepared, offered;
        // A remote reoffer changes its own receive preference, hence our TX.
        // Keep our complete admitted RX contract when that exact PT/mapping
        // is still eligible. The prepared table and resource owner below
        // continue to reject new meanings, aliases or DSP geometry changes.
        if (retained.rx_payloads.find(retained.rx_pt, &prepared) && prepared == retained.rx &&
            received_payloads.find(retained.rx_pt, &offered) &&
            audio_formats_share_rtp_mapping(offered, retained.rx)) {
          selected_rx_format = retained.rx;
          selected_rx_payload_type = retained.rx_pt;
          selected_rx = true;
        }
      }
      // A suspended direction need not appear in a one-way offer. Keep exactly
      // its admitted geometry/PT instead of inventing or migrating a mapping.
      if (!want_tx && !want_rx && !retained_mapping_offered(retained.tx, retained.tx_pt) &&
          !retained_mapping_offered(retained.rx, retained.rx_pt))
        return AudioSelectionResult::INVALID;
      if (!want_tx) {
        selected_tx_format = retained.tx;
        selected_tx_payload_type = retained.tx_pt;
        selected_tx = retained.tx.is_valid();
      }
      if (!want_rx) {
        selected_rx_format = retained.rx;
        selected_rx_payload_type = retained.rx_pt;
        selected_rx = retained.rx.is_valid();
      }
    }
    if (attributes.port == 0 || media_ip == 0 || (!want_tx && !want_rx && !retained_call) ||
        (!selected_tx && !selected_rx) || (want_tx && !selected_tx) || (want_rx && !selected_rx) ||
        (want_tx && want_rx && attributes.flow == 0x03 && !shared_format)) {
      return AudioSelectionResult::INCOMPATIBLE;
    }
    // Initial setup has no retained media owner to pause. Reject the legacy
    // address rather than acknowledging it while transmitting to the fallback.
    if (zero_connection && !retained_call) return AudioSelectionResult::INVALID;
    media.zero_connection = zero_connection;
    media.send = !zero_connection && selected_tx && (attributes.flow & 0x02) != 0;
    media.receive = !zero_connection && selected_rx && (attributes.flow & 0x01) != 0;
    RtpAudioPayloads accepted_rx;
    for (uint8_t i = 0; i < received_payloads.count; ++i) {
      const auto &entry = received_payloads.entries[i];
      // One audio m-line advertises the selected receiver's packet duration.
      // Keep each alternative at that duration when it is locally declared.
      AudioFormat receive_format = entry.format;
      audio_format_list_match_rtp_mapping(local.rx, entry.format, selected_rx_format.frame_ms,
                                         &receive_format, local.max_receive_payload);
      bool usable = receive_format == selected_rx_format;
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
      AudioFormat target_pcm = decoded_pcm_format(selected_rx_format);
      target_pcm.frame_ms = receive_format.frame_ms;
      usable |= pcm_receive_conversion_supported(decoded_pcm_format(receive_format), target_pcm);
#endif
#ifdef USE_ESPHOME_VOIP_STACK_OPUS
      usable |= same_decoded_audio_geometry(receive_format, selected_rx_format);
#endif
      if (usable && !accepted_rx.add(entry.payload_type, receive_format)) return AudioSelectionResult::INVALID;
    }
    if (retained_call && !media.receive) accepted_rx = retained.rx_payloads;
    if (retained_call) {
      // Preserve protocol history through unchanged and direction-only offers.
      // Resource admission remains with the committed transport, not this parser.
      auto known = retained.rx_payloads;
      if (known.activate_known_set(accepted_rx)) accepted_rx = known;
    }
    media.tx = selected_tx_format;
    media.rx = selected_rx_format;
    media.tx_pt = selected_tx_payload_type;
    media.rx_pt = selected_rx_payload_type;
    media.rx_payloads = accepted_rx;
    media.audio_ip = zero_connection ? retained.audio_ip : media_ip;
    media.audio_port = attributes.port;
    return AudioSelectionResult::ACCEPTED;
  }

 private:
  static bool match_tx(const AudioFormatList &formats, const AudioFormat &fmt, uint16_t preferred_time,
                       const AudioOfferAttributes &attributes, size_t max_payload, AudioFormat *matched) {
    for (uint8_t i = 0; i < formats.count; ++i) {
      const auto &candidate = formats.formats[i];
      if ((preferred_time == 0 || candidate.frame_ms == preferred_time) &&
          (!attributes.has_maxptime || candidate.frame_ms <= attributes.maxptime) &&
          audio_formats_share_rtp_mapping(candidate, fmt) &&
          audio_format_rtp_encoding(candidate, max_payload) != nullptr) {
        *matched = candidate;
        return true;
      }
    }
    return false;
  }

  static uint8_t local_rank(const AudioFormatList &list, const AudioFormat &format) {
    for (uint8_t i = 0; i < list.count; ++i)
      if (list.formats[i] == format) return i;
    return static_cast<uint8_t>(VOIP_STACK_MAX_AUDIO_FORMATS);
  }
};

}  // namespace esphome::voip_stack
