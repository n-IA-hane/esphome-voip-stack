#pragma once

#include "media_negotiator.h"

namespace esphome::voip_stack {

// Preserve the existing logging adapter while the decision has one compiled
// owner. The callback context lives for this synchronous negotiation only.
template <typename MappingTrace, typename IncompatibleTrace>
bool negotiate_remote_audio(const std::string &sdp, uint32_t default_ip, bool remote_is_answer, bool retained_call,
                            const LocalAudioOffer &local, const SdpAudioMedia &retained, SdpAudioMedia &media,
#ifdef USE_ESPHOME_VOIP_DTMF
                            DtmfConfig &dtmf_result,
#endif
                            MappingTrace report_mapping, IncompatibleTrace report_incompatible) {
  struct Observers {
    MappingTrace &mapping;
    IncompatibleTrace &incompatible;
  } observers{report_mapping, report_incompatible};
  const MediaNegotiationTrace trace{
      &observers,
      [](void *context, uint8_t pt, const AudioFormat &format, const AudioPayloadSelection &selection,
         const AudioMappingChanges &changes) {
        static_cast<Observers *>(context)->mapping(pt, format, selection, changes);
      },
      [](void *context, size_t size, uint16_t port, uint32_t ip, bool tx, bool rx) {
        static_cast<Observers *>(context)->incompatible(size, port, ip, tx, rx);
      }};
  return negotiate_audio_contract(sdp, default_ip, remote_is_answer, retained_call, local, retained, media,
#ifdef USE_ESPHOME_VOIP_DTMF
                                  dtmf_result,
#endif
                                  trace);
}

}  // namespace esphome::voip_stack
