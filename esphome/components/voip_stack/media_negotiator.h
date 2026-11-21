#pragma once

#ifdef ESP_PLATFORM
#include "esphome/core/defines.h"
#endif
#include "sip_sdp_audio_attributes.h"
#ifdef USE_ESPHOME_VOIP_DTMF
#include "rtp_dtmf.h"
#endif

namespace esphome::voip_stack {

// This owner prepares a media value without owning a dialog, logger or RTP task.
// Optional observers do not allocate and cannot change the selected contract.
struct MediaNegotiationTrace {
  void *context{nullptr};
  void (*mapping)(void *, uint8_t, const AudioFormat &, const AudioPayloadSelection &,
                  const AudioMappingChanges &){nullptr};
  void (*incompatible)(void *, size_t, uint16_t, uint32_t, bool, bool){nullptr};
};

bool negotiate_audio_contract(const std::string &sdp, uint32_t default_ip, bool remote_is_answer, bool retained_call,
                              const LocalAudioOffer &local, const SdpAudioMedia &retained, SdpAudioMedia &media,
#ifdef USE_ESPHOME_VOIP_DTMF
                              DtmfConfig &dtmf_result,
#endif
                              const MediaNegotiationTrace &trace = {});

}  // namespace esphome::voip_stack
