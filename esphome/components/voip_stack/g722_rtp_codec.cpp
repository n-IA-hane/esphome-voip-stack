#include "g722_rtp_codec.h"

#ifdef USE_ESPHOME_VOIP_STACK_G722

#include "esp_g722_enc.h"
#include "esp_g722_dec.h"
#include "esphome/core/log.h"

namespace esphome::voip_stack {

static const char *const TAG = "voip_stack.g722";

G722RtpCodec::~G722RtpCodec() { this->clear_(); }

void G722RtpCodec::clear_() {
  if (this->encoder_ != nullptr) esp_g722_enc_close(this->encoder_);
  if (this->decoder_ != nullptr) esp_g722_dec_close(this->decoder_);
  this->encoder_ = nullptr;
  this->decoder_ = nullptr;
  this->max_payload_bytes_ = 0;
}

bool G722RtpCodec::supported_(const AudioFormat &format) {
  return format.codec == AudioCodec::G722 && format.sample_rate == 16000 &&
         format.pcm_format == PcmFormat::S16LE && format.channels == 1 &&
         (format.frame_ms == 10 || format.frame_ms == 20);
}

bool G722RtpCodec::configure(const AudioFormat *tx, const AudioFormat *rx, size_t max_payload_bytes) {
  this->clear_();
  if ((tx != nullptr && (!supported_(*tx) || max_payload_bytes < tx->nominal_rtp_payload_bytes())) ||
      (rx != nullptr && (!supported_(*rx) || max_payload_bytes < rx->nominal_rtp_payload_bytes()))) return false;
  if (tx != nullptr) {
    esp_g722_enc_config_t config{};
    config.sample_rate = 16000;
    config.channel = 1;
    config.bits_per_sample = 16;
    config.bitrate = 64000;
    config.packed = false;  // At 64 kbit/s every code already occupies one byte.
    // A 10 ms processing quantum accepts either negotiated packet duration:
    // the direct process API also accepts integer multiples of this quantum.
    config.frame_duration = 10;
    if (esp_g722_enc_open(&config, sizeof(config), &this->encoder_) != ESP_AUDIO_ERR_OK) {
      this->clear_();
      return false;
    }
    int input_bytes = 0;
    int output_bytes = 0;
    if (this->encoder_ == nullptr ||
        esp_g722_enc_get_frame_size(this->encoder_, &input_bytes, &output_bytes) != ESP_AUDIO_ERR_OK ||
        input_bytes != 320 || output_bytes != 80) {
      this->clear_();
      return false;
    }
  }
  if (rx != nullptr) {
    esp_g722_dec_cfg_t config{};
    config.sample_rate = 16000;
    config.bitrate = 64000;
    config.packed = false;
    if (esp_g722_dec_open(&config, sizeof(config), &this->decoder_) != ESP_AUDIO_ERR_OK ||
        this->decoder_ == nullptr) {
      this->clear_();
      return false;
    }
  }
  this->max_payload_bytes_ = max_payload_bytes;
  return true;
}

bool G722RtpCodec::ready_for(const AudioFormat &tx, const AudioFormat &rx) const {
  return (tx.codec != AudioCodec::G722 || (this->encoder_ != nullptr && supported_(tx) &&
          tx.nominal_rtp_payload_bytes() <= this->max_payload_bytes_)) &&
         (rx.codec != AudioCodec::G722 || (this->decoder_ != nullptr && supported_(rx) &&
          rx.nominal_rtp_payload_bytes() <= this->max_payload_bytes_));
}

size_t G722RtpCodec::encode(const uint8_t *pcm, size_t pcm_bytes, const AudioFormat &format,
                           uint8_t *payload, size_t payload_capacity) {
  if (this->encoder_ == nullptr || !supported_(format) || pcm == nullptr || payload == nullptr ||
      pcm_bytes != format.nominal_frame_bytes()) return 0;
  const size_t payload_bytes = format.nominal_rtp_payload_bytes();
  if (payload_capacity < payload_bytes || this->max_payload_bytes_ < payload_bytes) return 0;
  esp_audio_enc_in_frame_t input{};
  input.buffer = const_cast<uint8_t *>(pcm);
  input.len = static_cast<uint32_t>(pcm_bytes);
  esp_audio_enc_out_frame_t output{};
  output.buffer = payload;
  output.len = static_cast<uint32_t>(payload_bytes);
  if (esp_g722_enc_process(this->encoder_, &input, &output) != ESP_AUDIO_ERR_OK ||
      output.encoded_bytes != payload_bytes) return 0;
  return output.encoded_bytes;
}

size_t G722RtpCodec::decode(const uint8_t *payload, size_t payload_bytes, const AudioFormat &format,
                           uint8_t *pcm, size_t pcm_capacity) {
  if (this->decoder_ == nullptr || !supported_(format) || payload == nullptr || pcm == nullptr ||
      payload_bytes != format.nominal_rtp_payload_bytes() || payload_bytes > this->max_payload_bytes_ ||
      pcm_capacity < format.nominal_frame_bytes()) return 0;
  esp_audio_dec_in_raw_t input{};
  input.buffer = const_cast<uint8_t *>(payload);
  input.len = static_cast<uint32_t>(payload_bytes);
  esp_audio_dec_out_frame_t output{};
  output.buffer = pcm;
  output.len = static_cast<uint32_t>(format.nominal_frame_bytes());
  esp_audio_dec_info_t info{};
  if (esp_g722_dec_decode(this->decoder_, &input, &output, &info) != ESP_AUDIO_ERR_OK ||
      input.consumed != payload_bytes || output.decoded_size != format.nominal_frame_bytes()) return 0;
  if (info.sample_rate != 0 && (info.sample_rate != 16000 || info.channel != 1 || info.bits_per_sample != 16))
    return 0;
  return output.decoded_size;
}

void G722RtpCodec::reset() {
  if (this->encoder_ != nullptr && esp_g722_enc_reset(this->encoder_) != ESP_AUDIO_ERR_OK)
    ESP_LOGW(TAG, "Encoder reset failed");
  this->reset_decoder();
}

void G722RtpCodec::reset_decoder() {
  if (this->decoder_ != nullptr && esp_g722_dec_reset(this->decoder_) != ESP_AUDIO_ERR_OK)
    ESP_LOGW(TAG, "Decoder reset failed");
}

}  // namespace esphome::voip_stack

#endif
