#include "opus_rtp_codec.h"

#ifdef USE_ESPHOME_VOIP_STACK_OPUS

#include <algorithm>

#include "esphome/core/log.h"

namespace esphome {
namespace voip_stack {

static const char *const TAG = "voip_stack.opus";

static bool codec_state_matches_format(const AudioFormat &state_format,
                                       const AudioFormat &format) {
  return state_format.codec == format.codec &&
         state_format.sample_rate == format.sample_rate &&
         state_format.pcm_format == format.pcm_format &&
         state_format.channels == format.channels;
}

OpusRtpCodec::~OpusRtpCodec() { this->clear_(); }

void OpusRtpCodec::clear_() {
  if (this->encoder_ != nullptr) opus_encoder_destroy(this->encoder_);
  if (this->decoder_ != nullptr) opus_decoder_destroy(this->decoder_);
  this->encoder_ = nullptr;
  this->decoder_ = nullptr;
}

bool OpusRtpCodec::configure(const AudioFormat *tx, const AudioFormat *rx,
                             size_t max_payload_bytes) {
  this->clear_();
  this->max_payload_bytes_ = std::min<size_t>(max_payload_bytes, 1275U);
  int error = OPUS_OK;
  if (tx != nullptr) {
    this->tx_format_ = *tx;
    this->encoder_ = opus_encoder_create(
        static_cast<opus_int32>(tx->sample_rate), tx->channels,
        OPUS_APPLICATION_VOIP, &error);
    if (this->encoder_ == nullptr || error != OPUS_OK) {
      ESP_LOGE(TAG, "Encoder allocation failed: %s", opus_strerror(error));
      this->clear_();
      return false;
    }
    if (opus_encoder_ctl(this->encoder_, OPUS_SET_BITRATE(24000)) != OPUS_OK ||
        opus_encoder_ctl(this->encoder_, OPUS_SET_COMPLEXITY(0)) != OPUS_OK ||
        opus_encoder_ctl(this->encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE)) != OPUS_OK) {
      ESP_LOGE(TAG, "Encoder configuration failed");
      this->clear_();
      return false;
    }
  }
  if (rx != nullptr) {
    this->rx_format_ = *rx;
    this->decoder_ = opus_decoder_create(
        static_cast<opus_int32>(rx->sample_rate), rx->channels, &error);
    if (this->decoder_ == nullptr || error != OPUS_OK) {
      ESP_LOGE(TAG, "Decoder allocation failed: %s", opus_strerror(error));
      this->clear_();
      return false;
    }
  }
  ESP_LOGI(TAG, "Opus ready: tx=%uHz rx=%uHz mono frame=%ums max=%u",
           tx == nullptr ? 0U : (unsigned) tx->sample_rate,
           rx == nullptr ? 0U : (unsigned) rx->sample_rate,
           tx != nullptr ? (unsigned) tx->frame_ms : (unsigned) rx->frame_ms,
           (unsigned) this->max_payload_bytes_);
  return true;
}

bool OpusRtpCodec::ready_for(const AudioFormat &tx,
                             const AudioFormat &rx) const {
  return (tx.codec != AudioCodec::OPUS ||
          (this->encoder_ != nullptr &&
           codec_state_matches_format(this->tx_format_, tx))) &&
         (rx.codec != AudioCodec::OPUS ||
          (this->decoder_ != nullptr &&
           codec_state_matches_format(this->rx_format_, rx)));
}

size_t OpusRtpCodec::encode(const uint8_t *pcm, size_t pcm_bytes,
                            const AudioFormat &format, uint8_t *payload,
                            size_t payload_capacity) {
  if (this->encoder_ == nullptr ||
      !codec_state_matches_format(this->tx_format_, format) ||
      pcm_bytes != format.nominal_frame_bytes()) return 0;
  const size_t limit = std::min(payload_capacity, this->max_payload_bytes_);
  const int encoded = opus_encode(
      this->encoder_, reinterpret_cast<const opus_int16 *>(pcm),
      static_cast<int>(format.nominal_frame_samples()), payload,
      static_cast<opus_int32>(limit));
  if (encoded < 0) {
    ESP_LOGW(TAG, "Encode failed: %s", opus_strerror(encoded));
    return 0;
  }
  return static_cast<size_t>(encoded);
}

size_t OpusRtpCodec::decode(const uint8_t *payload, size_t payload_bytes,
                            const AudioFormat &format, uint8_t *pcm,
                            size_t pcm_capacity) {
  if (this->decoder_ == nullptr ||
      !codec_state_matches_format(this->rx_format_, format) ||
      pcm_capacity < format.nominal_frame_bytes() || payload_bytes > 1275U)
    return 0;
  if (payload == nullptr && payload_bytes != 0) return 0;
  const int decoded = opus_decode(
      this->decoder_, payload, static_cast<opus_int32>(payload_bytes),
      reinterpret_cast<opus_int16 *>(pcm),
      static_cast<int>(format.nominal_frame_samples()), 0);
  if (decoded < 0) {
    ESP_LOGW(TAG, "Decode failed: %s", opus_strerror(decoded));
    return 0;
  }
  if (static_cast<size_t>(decoded) != format.nominal_frame_samples()) return 0;
  return static_cast<size_t>(decoded) * format.channels * sizeof(opus_int16);
}

void OpusRtpCodec::reset() {
  if (this->encoder_ != nullptr)
    opus_encoder_ctl(this->encoder_, OPUS_RESET_STATE);
  if (this->decoder_ != nullptr)
    opus_decoder_ctl(this->decoder_, OPUS_RESET_STATE);
}

void OpusRtpCodec::reset_decoder() {
  if (this->decoder_ != nullptr)
    opus_decoder_ctl(this->decoder_, OPUS_RESET_STATE);
}

}  // namespace voip_stack
}  // namespace esphome

#endif
