#pragma once

#include "esphome/core/defines.h"
#include "sip_types.h"

#ifdef USE_ESPHOME_VOIP_STACK_OPUS

#include <cstddef>
#include <cstdint>

#include "opus.h"

namespace esphome {
namespace voip_stack {

class OpusRtpCodec {
 public:
  ~OpusRtpCodec();

  bool configure(const AudioFormat *tx, const AudioFormat *rx,
                 size_t max_payload_bytes);
  bool ready_for(const AudioFormat &tx, const AudioFormat &rx) const;
  size_t encode(const uint8_t *pcm, size_t pcm_bytes, const AudioFormat &format,
                uint8_t *payload, size_t payload_capacity);
  size_t decode(const uint8_t *payload, size_t payload_bytes,
                const AudioFormat &format, uint8_t *pcm,
                size_t pcm_capacity);
  void reset();
  void reset_decoder();

 private:
  void clear_();

  OpusEncoder *encoder_{nullptr};
  OpusDecoder *decoder_{nullptr};
  AudioFormat tx_format_{};
  AudioFormat rx_format_{};
  size_t max_payload_bytes_{0};
};

}  // namespace voip_stack
}  // namespace esphome

#endif
