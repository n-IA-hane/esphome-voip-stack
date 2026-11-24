#pragma once

#include "sip_sdp_audio_attributes.h"
#include "rtp_dtmf.h"

namespace esphome::voip_stack {

// The SDP on the wire owns the offered mappings. The configured lists supply
// their executable packet durations; the currently selected TX is not a catalog.
inline bool capture_audio_offer(const std::string &sdp, const AudioFormatList &tx,
                                const AudioFormatList &rx, size_t send_budget,
                                AudioFormatList *offered_tx, RtpAudioPayloads *offered_rx,
                                uint8_t *offered_dtmf_clocks = nullptr) {
  if (offered_tx == nullptr || offered_rx == nullptr) return false;
  AudioOfferAttributes attributes;
  if (!read_audio_offer_attributes(sdp, attributes) || attributes.section == std::string::npos ||
      attributes.port == 0) return false;
  RtpAudioPayloads mappings;
  uint8_t clocks = 0;
  SdpLines lines(sdp);
  std::string line;
  bool audio = false;
  while (lines.next(line)) {
    if (line.rfind("m=", 0) == 0) audio = lines.offset() == attributes.section;
    if (!audio || line.rfind("a=rtpmap:", 0) != 0) continue;
    const auto separator = line.find_first_of(" \t", 9);
    uint32_t declared = 0;
    if (!parse_decimal_u32(line.substr(9, separator - 9), 127, &declared)) return false;
    if (!attributes.offered(static_cast<uint8_t>(declared))) continue;
    DtmfFormat event;
    if (dtmf_rtpmap(line, &event)) {
      clocks |= 1U << dtmf_clock_index(event.clock);
      continue;
    }
    AudioFormat mapping;
    uint8_t payload = 0;
    if (!parse_rtpmap_format(line, &mapping, &payload) || payload != declared) return false;
    AudioFormat previous;
    if (mappings.find(payload, &previous)) {
      if (!audio_formats_share_rtp_mapping(previous, mapping)) return false;
    } else if (!mappings.add(payload, mapping)) return false;
  }
  for (uint8_t payload = 0; payload < 96; ++payload) {
    const auto mapping = static_rtp_audio_format(payload);
    if (!attributes.offered(payload) || !mapping.is_valid()) continue;
    AudioFormat explicit_mapping;
    if (mappings.find(payload, &explicit_mapping)) {
      if (!audio_formats_share_rtp_mapping(explicit_mapping, mapping)) return false;
    } else if (!mappings.add(payload, mapping)) return false;
  }
  AudioFormatList sends;
  sends.count = 0;
  AudioFormatList bounded_rx;
  bounded_rx.count = 0;
  for (uint8_t i = 0; i < rx.count; ++i) {
    const auto &candidate = rx.formats[i];
    // This maximum is from our own offer, so it bounds our receiver rather
    // than the encoder whose policy comes from the remote answer.
    if (!attributes.has_maxptime || candidate.frame_ms <= attributes.maxptime)
      bounded_rx.formats[bounded_rx.count++] = candidate;
  }
  RtpAudioPayloads receives;
  for (uint8_t i = 0; i < tx.count; ++i) {
    const auto &candidate = tx.formats[i];
    if (audio_format_rtp_encoding(candidate, send_budget) == nullptr) continue;
    for (uint8_t j = 0; j < mappings.count; ++j)
      if (audio_formats_share_rtp_mapping(candidate, mappings.entries[j].format)) {
        if (!audio_format_list_contains(sends, candidate)) sends.formats[sends.count++] = candidate;
        break;
      }
  }
  for (uint8_t i = 0; i < mappings.count; ++i) {
    const auto &mapping = mappings.entries[i];
    AudioFormat candidate;
    // This is our own serialized SDP: ptime describes the packets we prefer
    // to receive. Keep its executable preference with the offered mapping.
    const bool admitted = audio_format_list_match_rtp_mapping(
        bounded_rx, mapping.format, attributes.ptime, &candidate, RTP_AUDIO_RECEIVE_PAYLOAD_BYTES) ||
        (attributes.ptime != 0 && audio_format_list_match_rtp_mapping(
            bounded_rx, mapping.format, 0, &candidate, RTP_AUDIO_RECEIVE_PAYLOAD_BYTES));
    if (admitted &&
        !receives.add(mapping.payload_type, candidate)) return false;
  }
  if ((tx.count != 0 && sends.count == 0) || (rx.count != 0 && receives.count == 0)) return false;
  *offered_tx = sends;
  *offered_rx = receives;
  if (offered_dtmf_clocks != nullptr) *offered_dtmf_clocks = clocks;
  return true;
}

}  // namespace esphome::voip_stack
