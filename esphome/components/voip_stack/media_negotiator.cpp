#include "media_negotiator.h"

#ifdef ESP_PLATFORM
#include <lwip/sockets.h>
#else
#include <arpa/inet.h>
#endif

namespace esphome::voip_stack {

bool negotiate_audio_contract(const std::string &sdp, uint32_t default_ip, bool remote_is_answer, bool retained_call,
                              const LocalAudioOffer &local, const SdpAudioMedia &retained, SdpAudioMedia &media,
#ifdef USE_ESPHOME_VOIP_DTMF
                              DtmfConfig &dtmf_result,
#endif
                              const MediaNegotiationTrace &trace) {
  media.directional_audio = false;
  AudioOfferAttributes attributes;
  if (!read_audio_offer_attributes(sdp, attributes)) return false;
  uint32_t media_ip = default_ip;
  bool zero_connection = false;
  AudioPayloadSelection selection;
  for (uint8_t pt = 0; pt < 128; ++pt)
    if (attributes.offered(pt) && attributes.payload_flow[pt] != 0 && (!remote_is_answer || local.offered_directional))
      media.directional_audio = true;
  // Only the earlier private D1 contract coupled both directions. Ordinary
  // SIP and peers explicitly declaring this version use RFC 3264 semantics.
  const bool legacy_directional_ptime = media.directional_audio && !attributes.independent_ptime;
  const uint16_t transmit_ptime = legacy_directional_ptime && attributes.ptime == 0
                                      ? choose_common_audio_ptime(local.tx, local.rx, local.max_payload)
                                      : attributes.ptime;
  if (legacy_directional_ptime && transmit_ptime == 0) return false;
  AudioFormat retained_tx_format = disabled_audio_format();
  if (retained_call && !remote_is_answer && attributes.ptime == 0) retained_tx_format = retained.tx;
  const AudioMappingPolicy policy{local,
                                  attributes,
                                  retained_tx_format,
                                  transmit_ptime,
                                  media.directional_audio,
                                  legacy_directional_ptime,
                                  remote_is_answer,
                                  retained_call};
  auto consider_mapping = [&](AudioFormat fmt, uint8_t pt) {
    AudioMappingChanges changes;
    if (!selection.consider(fmt, pt, policy, changes)) return false;
    if (trace.mapping != nullptr) trace.mapping(trace.context, pt, fmt, selection, changes);
    return true;
  };
  bool seen_media = false;
  bool in_audio = false;
  SdpLines lines(sdp);
  std::string line;
  while (lines.next(line)) {
    if (line.rfind("m=", 0) == 0) {
      seen_media = true;
      in_audio = lines.offset() == attributes.section;
    } else if (line.rfind("c=IN IP4 ", 0) == 0 && (!seen_media || in_audio)) {
      struct in_addr a{};
      if (inet_aton(line.substr(9).c_str(), &a) != 0) {
        zero_connection = a.s_addr == 0;
        if (!zero_connection) media_ip = ntohl(a.s_addr);
      }
    } else if (in_audio && line.rfind("a=rtpmap:", 0) == 0) {
      AudioFormat fmt;
      uint8_t pt = 0;
      const bool valid_mapping = parse_rtpmap_format(line, &fmt, &pt);
      uint32_t declared_pt = 0;
      const size_t separator = line.find_first_of(" \t", 9);
      if (attributes.offered(9) && parse_decimal_u32(line.substr(9, separator - 9), 127, &declared_pt) &&
          declared_pt == 9 && !valid_mapping)
        return false;
      if (valid_mapping && attributes.offered(pt)) {
        if (!consider_mapping(fmt, pt)) return false;
      }
    }
  }
  if (attributes.offered(9) && selection.seen_mapping[9] == 0) {
    const AudioFormat g722{16000, PcmFormat::S16LE, 1, 20, AudioCodec::G722};
    if (!consider_mapping(g722, 9)) return false;
  }
  const auto result = selection.finish(local, attributes, retained, retained_call, media_ip, zero_connection, media);
  if (result == AudioSelectionResult::INCOMPATIBLE) {
    if (trace.incompatible != nullptr)
      trace.incompatible(trace.context, sdp.size(), attributes.port, media_ip, selection.selected_tx, selection.selected_rx);
  }
  if (result != AudioSelectionResult::ACCEPTED) return false;
#ifdef USE_ESPHOME_VOIP_DTMF
  const auto next_media = sdp.find("\r\nm=", attributes.section);
  const std::string_view audio_section(
      sdp.data() + attributes.section,
      (next_media == std::string::npos ? sdp.size() : next_media + 2) - attributes.section);
  auto dtmf = negotiate_dtmf(
      audio_section, attributes.payload_rank,
      audio_format_rtp_clock_rate(selection.selected_tx ? selection.selected_tx_format : selection.selected_rx_format),
      remote_is_answer, local.offered_dtmf_clocks);
  if (dtmf.valid() &&
      (selection.seen_mapping[dtmf.payload_type] != 0 || selection.received_payloads.find(dtmf.payload_type, nullptr) ||
       (selection.selected_tx && selection.selected_tx_payload_type == dtmf.payload_type)))
    dtmf = {};
  DtmfConfig dtmf_config{selection.selected_tx ? dtmf : DtmfFormat{}, selection.selected_rx ? dtmf : DtmfFormat{}};
  if (remote_is_answer && dtmf_config.rx.valid()) dtmf_config.rx.payload_type = 120 + dtmf_clock_index(dtmf.clock);
  dtmf_result = dtmf_config;
#endif
  return true;
}


}  // namespace esphome::voip_stack
