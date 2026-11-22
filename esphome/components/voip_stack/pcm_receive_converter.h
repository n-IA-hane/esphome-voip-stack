#pragma once

#include "rtp_audio_payloads.h"

#if defined(USE_ESPHOME_VOIP_STACK_SPEAKER) || defined(USE_ESPHOME_VOIP_PCM_TX_CONVERSION)
#include <atomic>
#include <cstring>
#include <memory>
#include <new>
#include <resampler.h>
#ifdef USE_ESP32
#include "esphome/core/helpers.h"
#endif

namespace esphome::voip_stack {

// Streaming PCM conversion runs in the existing receive or transmit task.
// Resources are prepared before media admission, never allocated per packet.
class PcmReceiveConverter {
 public:
  bool prepare(const RtpAudioPayloads &payloads, const AudioFormat &target) {
    return this->prepare(payloads.formats(), target);
  }

  bool prepare(const AudioFormatList &payloads, const AudioFormat &target) {
    if (audio_formats_share_rtp_mapping(target, this->target_)) {
      bool covered = true;
      const uint8_t count = this->count_.load(std::memory_order_acquire);
      for (uint8_t i = 0; i < payloads.count && covered; ++i) {
        const auto &source = payloads.formats[i];
        AudioFormat packet_target = target;
        packet_target.frame_ms = source.frame_ms;
        if (source == packet_target) continue;
        bool found = false;
        for (uint8_t j = 0; j < count; ++j)
          found |= audio_formats_share_rtp_mapping(this->entries_[j].format, source) &&
                   this->entries_[j].format.frame_ms >= source.frame_ms;
        covered = found;
      }
      if (covered) {
        // RTP admission still owns the negotiated PT map. Reuse only the
        // existing converters, with fresh history for the new media session.
        this->reset();
        return true;
      }
    }
    this->clear_();
    this->target_ = target;
    return this->prepare_additional(payloads, target);
  }

  bool prepare_additional(const RtpAudioPayloads &payloads, const AudioFormat &target) {
    return this->prepare_additional(payloads.formats(), target);
  }

  bool prepare_additional(const AudioFormatList &payloads, const AudioFormat &target) {
    if (!audio_formats_share_rtp_mapping(target, this->target_)) return false;
    const uint8_t previous = this->count_.load(std::memory_order_acquire);
    uint8_t count = previous;
    auto rollback = [&]() {
      for (uint8_t i = previous; i < count; ++i) this->entries_[i].converter.reset();
      return false;
    };
    for (uint8_t i = 0; i < payloads.count; ++i) {
      const auto &source = payloads.formats[i];
      AudioFormat packet_target = target;
      packet_target.frame_ms = source.frame_ms;
      if (source == packet_target) continue;
      if (!pcm_receive_conversion_supported(source, packet_target)) return rollback();
      bool found = false;
      for (uint8_t j = 0; j < count; ++j) {
        if (!audio_formats_share_rtp_mapping(this->entries_[j].format, source)) continue;
        if (this->entries_[j].format.frame_ms < source.frame_ms) return rollback();
        found = true;
      }
      if (found) continue;
      if (count == VOIP_STACK_MAX_AUDIO_FORMATS) return rollback();
      auto &entry = this->entries_[count];
      entry.format = source;
      entry.target = target;
      // One DSP history per PCM geometry. Use the descriptor's duration only
      // as its prepared maximum capacity, not as part of the stream identity.
      for (uint8_t j = 0; j < payloads.count; ++j)
        if (audio_formats_share_rtp_mapping(payloads.formats[j], source))
          entry.format.frame_ms = std::max(entry.format.frame_ms, payloads.formats[j].frame_ms);
      packet_target.frame_ms = entry.format.frame_ms;
      entry.converter.reset(new (std::nothrow) Converter(
          entry.format.nominal_frame_samples() * source.channels,
          packet_target.nominal_frame_samples() * packet_target.channels));
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


  bool prepare_transmit(const AudioFormat &native, const AudioFormatList &alternatives) {
    this->clear_();
    for (uint8_t i = 0; i < alternatives.count; ++i) {
      AudioFormat source = decoded_pcm_format(native);
      AudioFormat target = decoded_pcm_format(alternatives.formats[i]);
      source.frame_ms = target.frame_ms;
      if (source == target) continue;
      if (!pcm_receive_conversion_supported(source, target)) return false;
      uint8_t count = this->count_.load(std::memory_order_relaxed);
      uint8_t slot = 0;
      while (slot < count && !audio_formats_share_rtp_mapping(this->entries_[slot].target, target)) ++slot;
      if (slot != count) {
        // Prepare the largest declared packet once for each target geometry.
        if (this->entries_[slot].format.frame_ms >= source.frame_ms) continue;
        this->entries_[slot].converter.reset();
      } else {
        if (count == VOIP_STACK_MAX_AUDIO_FORMATS) return false;
        this->count_.store(++count, std::memory_order_relaxed);
      }
      auto &entry = this->entries_[slot];
      entry.format = source;
      entry.target = target;
      entry.converter.reset(new (std::nothrow) Converter(source.nominal_frame_samples() * source.channels,
                                                        target.nominal_frame_samples() * target.channels));
      if (entry.converter == nullptr) return false;
      esp_audio_libs::resampler::ResamplerConfiguration config{
          static_cast<float>(source.sample_rate), static_cast<float>(target.sample_rate),
          16, 16, source.channels, false, false, 16, 16};
      if (!entry.converter->initialize(config)) return false;
    }
    return true;
  }

  size_t convert_to(const AudioFormat &source, const AudioFormat &target,
                    const uint8_t *pcm, size_t bytes, uint8_t *output, size_t capacity) {
    if (pcm == nullptr || output == nullptr || bytes != source.nominal_frame_bytes() ||
        capacity < target.nominal_frame_bytes()) return 0;
    if (source == target) {
      std::memcpy(output, pcm, bytes);
      return bytes;
    }
    for (uint8_t i = 0, count = this->count_.load(std::memory_order_acquire); i < count; ++i) {
      auto &entry = this->entries_[i];
      if (!audio_formats_share_rtp_mapping(source, entry.format) ||
          !audio_formats_share_rtp_mapping(target, entry.target) || source.frame_ms > entry.format.frame_ms) continue;
      const auto result = entry.converter->resample(
          pcm, output, source.nominal_frame_samples(), target.nominal_frame_samples(), 0.0f);
      if (result.frames_used != source.nominal_frame_samples()) return 0;
      return result.frames_generated * target.channels * sizeof(int16_t);
    }
    return 0;
  }


  void release() { this->clear_(); this->target_ = disabled_audio_format(); }

  void reset() {
    for (uint8_t i = 0, count = this->count_.load(std::memory_order_acquire); i < count; ++i) this->entries_[i].converter->reset();
  }

  size_t convert(const AudioFormat &source, const uint8_t *pcm, size_t bytes,
                 uint8_t *output, size_t capacity) {
    if (pcm == nullptr || output == nullptr || bytes != source.nominal_frame_bytes()) return 0;
    for (uint8_t i = 0, count = this->count_.load(std::memory_order_acquire); i < count; ++i) {
      auto &entry = this->entries_[i];
      if (!audio_formats_share_rtp_mapping(source, entry.format) || source.frame_ms > entry.format.frame_ms) continue;
      AudioFormat packet_target = this->target_;
      packet_target.frame_ms = source.frame_ms;
      if (capacity < packet_target.nominal_frame_bytes()) return 0;
      const auto result = entry.converter->resample(
          pcm, output, source.nominal_frame_samples(), packet_target.nominal_frame_samples(), 0.0f);
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
    AudioFormat target;
    std::unique_ptr<Converter> converter;
  };
  Entry entries_[VOIP_STACK_MAX_AUDIO_FORMATS]{};
  std::atomic<uint8_t> count_{0};
  AudioFormat target_;
};

}  // namespace esphome::voip_stack
#endif
