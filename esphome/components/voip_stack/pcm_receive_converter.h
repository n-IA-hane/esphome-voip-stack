#pragma once

#include "rtp_audio_payloads.h"

#if defined(USE_ESPHOME_VOIP_STACK_SPEAKER) && !defined(USE_ESPHOME_VOIP_STACK_OPUS)
#include <memory>
#include <new>
#include <resampler.h>

namespace esphome::voip_stack {

// Conversion runs in the existing playout task after RTP reordering. Resources
// are prepared before media admission, never allocated for individual packets.
class PcmReceiveConverter {
 public:
  bool prepare(const RtpAudioPayloads &payloads, const AudioFormat &target) {
    this->clear_();
    this->target_ = target;
    for (uint8_t i = 0; i < payloads.count; ++i) {
      const auto &source = payloads.entries[i].format;
      if (source == target) continue;
      if (!pcm_receive_conversion_supported(source, target)) { this->clear_(); return false; }
      bool found = false;
      for (uint8_t j = 0; j < this->count_; ++j)
        found |= this->entries_[j].format == source;
      if (found) continue;
      auto &entry = this->entries_[this->count_];
      entry.format = source;
      entry.converter.reset(new (std::nothrow) Converter(
          source.nominal_frame_samples() * source.channels,
          target.nominal_frame_samples() * target.channels));
      if (entry.converter == nullptr) { this->clear_(); return false; }
      esp_audio_libs::resampler::ResamplerConfiguration config{
          static_cast<float>(source.sample_rate), static_cast<float>(target.sample_rate),
          16, 16, source.channels, false, false, 16, 16};
      if (!entry.converter->initialize(config)) { this->clear_(); return false; }
      ++this->count_;
    }
    return true;
  }


  void reset() {
    for (uint8_t i = 0; i < this->count_; ++i) this->entries_[i].converter->reset();
  }

  size_t convert(const AudioFormat &source, const uint8_t *pcm, size_t bytes,
                 uint8_t *output, size_t capacity) {
    if (pcm == nullptr || output == nullptr || bytes != source.nominal_frame_bytes() ||
        capacity < this->target_.nominal_frame_bytes()) return 0;
    for (uint8_t i = 0; i < this->count_; ++i) {
      auto &entry = this->entries_[i];
      if (!(source == entry.format)) continue;
      const auto result = entry.converter->resample(
          pcm, output, source.nominal_frame_samples(), this->target_.nominal_frame_samples(), 0.0f);
      if (result.frames_used != source.nominal_frame_samples()) return 0;
      return result.frames_generated * this->target_.channels * sizeof(int16_t);
    }
    return 0;
  }

 protected:
  void clear_() {
    for (auto &entry : this->entries_) entry.converter.reset();
    this->count_ = 0;
  }
  class Converter : public esp_audio_libs::resampler::Resampler {
   public:
    using Resampler::Resampler;
    void reset() {
      if (this->resampler_ != nullptr) {
        esp_audio_libs::art_resampler::resampleReset(this->resampler_);
        esp_audio_libs::art_resampler::resampleAdvancePosition(this->resampler_, this->number_of_taps_ / 2.0f);
      }
    }
  };
  struct Entry {
    AudioFormat format;
    std::unique_ptr<Converter> converter;
  };
  Entry entries_[VOIP_STACK_MAX_AUDIO_FORMATS]{};
  uint8_t count_{0};
  AudioFormat target_;
};

}  // namespace esphome::voip_stack
#endif
