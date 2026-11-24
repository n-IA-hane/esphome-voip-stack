#include "media_negotiator.h"
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
#include "sip_sdp_video_attributes.h"
#endif

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
  AudioOfferAttributes attributes;
  if (!read_audio_offer_attributes(sdp, attributes)) return false;
  uint32_t media_ip = default_ip;
  bool zero_connection = false;
  AudioPayloadSelection selection;
  // RFC 3264: stream direction gates activity; ptime is a receive preference.
  const uint16_t transmit_ptime = attributes.ptime;
  AudioFormat retained_tx_format = disabled_audio_format();
  if (retained_call && !remote_is_answer && attributes.ptime == 0) retained_tx_format = retained.tx;
  const AudioMappingPolicy policy{local,
                                  attributes,
                                  retained_tx_format,
                                  transmit_ptime,
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
  for (uint8_t pt = 9; pt <= 11; ++pt)
    if (attributes.offered(pt) && selection.seen_mapping[pt] == 0 &&
        !consider_mapping(static_rtp_audio_format(pt), pt)) return false;
  const auto result = selection.finish(local, attributes, retained, retained_call, media_ip, zero_connection, media,
                                       remote_is_answer);
  if (result == AudioSelectionResult::INCOMPATIBLE) {
    if (trace.incompatible != nullptr)
      trace.incompatible(trace.context, sdp.size(), attributes.port, media_ip, selection.selected_tx, selection.selected_rx);
  }
  if (result != AudioSelectionResult::ACCEPTED) return false;
#ifdef USE_ESPHOME_VOIP_STACK_OPUS
  media.opus_tx = {};
  if (media.tx.codec == AudioCodec::OPUS) {
    // Parameters can precede their rtpmap. Read only the selected transmit PT
    // after selection, without an additional per-payload table or allocation.
    SdpLines parameters(sdp);
    bool selected_section = false;
    while (parameters.next(line)) {
      if (line.rfind("m=", 0) == 0) selected_section = parameters.offset() == attributes.section;
      if (!selected_section || line.rfind("a=fmtp:", 0) != 0) continue;
      const auto space = line.find_first_of(" \t", 7);
      uint32_t pt = 0;
      if (space != std::string::npos && parse_decimal_u32(line.substr(7, space - 7), 127, &pt) && pt == media.tx_pt)
        read_opus_receive_constraints(std::string_view(line).substr(space + 1), media.opus_tx);
    }
  }

#endif
#ifdef USE_ESPHOME_VOIP_DTMF
  const auto next_media = sdp.find("\r\nm=", attributes.section);
  const std::string_view audio_section(
      sdp.data() + attributes.section,
      (next_media == std::string::npos ? sdp.size() : next_media + 2) - attributes.section);
  const auto events_for = [&](const AudioFormat &audio, bool receive) {
    auto events = negotiate_dtmf(audio_section, attributes.payload_rank, audio_format_rtp_clock_rate(audio),
                                 remote_is_answer, local.offered_dtmf_clocks);
    if (events.valid() &&
        (selection.seen_mapping[events.payload_type] != 0 ||
         selection.received_payloads.find(events.payload_type, nullptr) ||
         (selection.selected_tx && selection.selected_tx_payload_type == events.payload_type))) return DtmfFormat{};
    if (remote_is_answer && events.valid()) {
      if (local.offered_dtmf == nullptr) return DtmfFormat{};
      auto offered = local.offered_dtmf->select(events.clock, events.events);
      if (receive && retained_call && local.retained_dtmf_rx != nullptr && local.retained_dtmf_rx->valid()) {
        const auto retained_offer = local.offered_dtmf->find_payload(local.retained_dtmf_rx->payload_type);
        if (retained_offer == *local.retained_dtmf_rx && retained_offer.clock == events.clock &&
            (retained_offer.events & events.events) != 0) offered = retained_offer;
      }
      if (!offered.valid()) return DtmfFormat{};
      // The answer owns our transmitter's PT. Our exact serialized offer owns
      // the receive PT. Preserve the existing conservative event intersection.
      events.events &= offered.events;
      if (!events.valid()) return DtmfFormat{};
      if (receive) events.payload_type = offered.payload_type;
      if (receive && local.offered_rx.find(events.payload_type, nullptr)) return DtmfFormat{};
    }
    return events;
  };
  // Each direction follows its actual RTP clock. G.722
  // decodes at 16 kHz but its audio and telephone-event timestamps use 8 kHz.
  const DtmfConfig dtmf_config{
      selection.selected_tx ? events_for(selection.selected_tx_format, false) : DtmfFormat{},
      selection.selected_rx ? events_for(selection.selected_rx_format, true) : DtmfFormat{}};
  dtmf_result = dtmf_config;
#endif
  return true;
}


#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
bool video_payload_history_allows(const VideoOfferAttributes &offer, bool remote_is_answer,
                                  const VideoPayloadContract &retained) {
  for (uint8_t pt = 0; pt < 128; ++pt) {
    if (!offer.offered_payloads[pt]) continue;
    const auto next = offer.payload_meanings[pt];
    if (next == VideoPayloadMeaning::UNDECLARED) continue;
    if (!VideoPayloadContract::permits(retained.tx_meanings, pt, next) ||
        (!remote_is_answer && !VideoPayloadContract::permits(retained.rx_meanings, pt, next))) return false;
  }
  return true;
}

bool negotiate_video_payload_contract(const VideoOfferAttributes &offer, bool remote_is_answer,
                                      uint8_t offered_pt, uint8_t selected_pt,
                                      const VideoPayloadContract &retained, VideoPayloadContract *result) {
  if (result == nullptr || selected_pt > 127 || !offer.offered_payloads[selected_pt] ||
      !video_payload_history_allows(offer, remote_is_answer, retained)) return false;
  auto next = retained;
  auto codec = offer.payload_meanings[selected_pt];
  if (selected_pt == 26 && codec == VideoPayloadMeaning::UNDECLARED) codec = VideoPayloadMeaning::JPEG;
  if (!next.remember(selected_pt, remote_is_answer ? offered_pt : selected_pt, codec)) return false;
  next.conflict = false;
  *result = next;
  return true;
}
#endif

}  // namespace esphome::voip_stack
