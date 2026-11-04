#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESPHOME_VOIP_STACK_G722

#include <cstddef>
#include <cstdint>

#include "sip_types.h"

namespace esphome::voip_stack {

// Espressif esp_audio_codec 2.5.0, direct G.722 APIs only. Codec state is
// allocated by open() before media admission, then reused without new tasks.
// PCM is 16 kHz mono S16LE; the transport owns the separate 8 kHz RTP clock.
class G722RtpCodec {
 public:
  G722RtpCodec() = default;
  ~G722RtpCodec();
  G722RtpCodec(const G722RtpCodec &) = delete;
  G722RtpCodec &operator=(const G722RtpCodec &) = delete;

  bool configure(const AudioFormat *tx, const AudioFormat *rx, size_t max_payload_bytes);
  bool ready_for(const AudioFormat &tx, const AudioFormat &rx) const;
  size_t encode(const uint8_t *pcm, size_t pcm_bytes, const AudioFormat &format,
                uint8_t *payload, size_t payload_capacity);
  size_t decode(const uint8_t *payload, size_t payload_bytes, const AudioFormat &format,
                uint8_t *pcm, size_t pcm_capacity);
  void reset();
  void reset_decoder();

 protected:
  static bool supported_(const AudioFormat &format);
  void clear_();

  void *encoder_{nullptr};
  void *decoder_{nullptr};
  size_t max_payload_bytes_{0};
};

}  // namespace esphome::voip_stack

#endif
