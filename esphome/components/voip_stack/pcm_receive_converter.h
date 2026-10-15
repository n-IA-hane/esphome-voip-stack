#pragma once

#include "rtp_audio_payloads.h"

#if defined(USE_ESPHOME_VOIP_STACK_SPEAKER) && !defined(USE_ESPHOME_VOIP_STACK_OPUS)
#include <atomic>
#include <memory>
#include <new>
#include <resampler.h>
#ifdef USE_ESP32
#include "esphome/core/helpers.h"
#endif

namespace esphome::voip_stack {

// Conversion runs in the existing playout task after RTP reordering. Resources
// are prepared before media admission, never allocated for individual packets.
class PcmReceiveConverter {
 public:
  bool prepare(const RtpAudioPayloads &payloads, const AudioFormat &target) {
    this->clear_();
    this->target_ = target;
    return this->prepare_additional(payloads, target);
  }

  bool prepare_additional(const RtpAudioPayloads &payloads, const AudioFormat &target) {
    if (!(target == this->target_)) return false;
    const uint8_t previous = this->count_.load(std::memory_order_acquire);
    uint8_t count = previous;
    auto rollback = [&]() {
      for (uint8_t i = previous; i < count; ++i) this->entries_[i].converter.reset();
      return false;
    };
    for (uint8_t i = 0; i < payloads.count; ++i) {
      const auto &source = payloads.entries[i].format;
      if (source == target) continue;
      if (!pcm_receive_conversion_supported(source, target)) return rollback();
      bool found = false;
      for (uint8_t j = 0; j < count; ++j) found |= this->entries_[j].format == source;
      if (found) continue;
      if (count == VOIP_STACK_MAX_AUDIO_FORMATS) return rollback();
      auto &entry = this->entries_[count];
      entry.format = source;
      entry.converter.reset(new (std::nothrow) Converter(
          source.nominal_frame_samples() * source.channels,
          target.nominal_frame_samples() * target.channels));
      if (entry.converter == nullptr) return rollback();
      esp_audio_libs::resampler::ResamplerConfiguration config{
          static_cast<float>(source.sample_rate), static_cast<float>(target.sample_rate),
          16, 16, source.channels, false, false, 16, 16};
      if (!entry.converter->initialize(config)) {
        entry.converter.reset();
        return rollback();
      }
      ++count;
    }
    // Publish only fully initialized additions. The playout task keeps using
    // its existing converters while signaling prepares another wire format.
    this->count_.store(count, std::memory_order_release);
    return true;
  }


  void reset() {
    for (uint8_t i = 0, count = this->count_.load(std::memory_order_acquire); i < count; ++i) this->entries_[i].converter->reset();
  }

  size_t convert(const AudioFormat &source, const uint8_t *pcm, size_t bytes,
                 uint8_t *output, size_t capacity) {
    if (pcm == nullptr || output == nullptr || bytes != source.nominal_frame_bytes() ||
        capacity < this->target_.nominal_frame_bytes()) return 0;
    for (uint8_t i = 0, count = this->count_.load(std::memory_order_acquire); i < count; ++i) {
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
    this->count_.store(0, std::memory_order_release);
  }
  class Converter : public esp_audio_libs::resampler::Resampler {
   public:
    using Resampler::Resampler;
#ifdef USE_ESP32
    static void *operator new(size_t size, const std::nothrow_t &) noexcept {
      return RAMAllocator<uint8_t>().allocate(size);
    }
    static void operator delete(void *value) noexcept {
      RAMAllocator<uint8_t>().deallocate(static_cast<uint8_t *>(value), 0);
    }
#endif
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
  std::atomic<uint8_t> count_{0};
  AudioFormat target_;
};

}  // namespace esphome::voip_stack
#endif
