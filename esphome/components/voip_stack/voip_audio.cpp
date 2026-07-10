#include "voip_stack.h"

#ifdef USE_ESP32

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include "audio_core_audio_utils.h"

namespace esphome {
namespace voip_stack {

static const char *const TAG = "voip_stack.audio";

#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
void VoipStack::debug_log_pcm_level_(const char *label, const uint8_t *pcm, size_t bytes,
                                       const AudioFormat &format,
                                       uint32_t &last_log_ms, uint32_t &frame_count) {
  frame_count++;
  const uint32_t now = millis();
  if (now - last_log_ms < 1000)
    return;
  last_log_ms = now;

  const size_t samples = bytes / sizeof(int16_t);
  if (pcm == nullptr || samples == 0) {
    ESP_LOGI(TAG, "AudioDebug[%s]: frames=%u bytes=%u empty state=%s",
             label, frame_count, (unsigned) bytes, this->get_call_state_str());
    return;
  }

  if (format.pcm_format != PcmFormat::S16LE) {
    ESP_LOGI(TAG,
             "AudioDebug[%s]: frames=%u bytes=%u format=%u:%u:%u:%u "
             "levels=skipped_non_s16 "
             "voip_volume=%.3f state=%s",
             label, frame_count, (unsigned) bytes, (unsigned) format.sample_rate, (unsigned) format.pcm_format,
             (unsigned) format.channels, (unsigned) format.frame_ms, this->volume_.load(std::memory_order_relaxed),
             this->get_call_state_str());
    return;
  }

  const auto levels = compute_levels_dbfs_i16(reinterpret_cast<const int16_t *>(pcm), samples);
  const char *path = "direct";
  ESP_LOGI(TAG,
           "AudioDebug[%s]: frames=%u bytes=%u samples=%u peak=%u "
           "peak_dbfs=%.1f rms_dbfs=%.1f "
           "voip_volume=%.3f state=%s path=%s",
           label, frame_count, (unsigned) bytes, (unsigned) samples, (unsigned) levels.peak, levels.peak_dbfs,
           levels.rms_dbfs, this->volume_.load(std::memory_order_relaxed), this->get_call_state_str(), path);
}
#endif

#ifdef USE_ESPHOME_VOIP_STACK_MIC
namespace {
static uint32_t frames_for_bytes(size_t bytes, size_t frame_bytes) {
  if (bytes == 0 || frame_bytes == 0)
    return 0;
  return static_cast<uint32_t>((bytes + frame_bytes - 1) / frame_bytes);
}
}  // namespace

// === TX Task (Core 0) - Mic to Network ===

size_t VoipStack::tx_audio_buffer_bytes_() const {
  const size_t frame_bytes = this->tx_audio_max_chunk_bytes_();
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_STACK_MIC
  // esp_audio_stack already owns the realtime I2S/AFE buffering and emits
  // regular processed frames. Keep only enough VoIP queue to bridge scheduler
  // jitter into the network task.
  return std::max<size_t>(frame_bytes * 6, frame_bytes + 1024);
#else
  return std::max<size_t>(frame_bytes * 16, frame_bytes + 4096);
#endif
}

void VoipStack::tx_task(void *param) {
  static_cast<VoipStack *>(param)->tx_task_();
}

bool VoipStack::is_tx_stream_ready_() const {
  return this->audio_devices_active_.load(std::memory_order_acquire) &&
         this->call_state_.load(std::memory_order_acquire) == CallState::IN_CALL &&
         this->transport_ != nullptr && this->transport_->is_connected();
}

void VoipStack::send_chunk_(const uint8_t *data, size_t length) {
  if (!this->is_tx_stream_ready_())
    return;
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
  if (this->audio_debug_) {
    const AudioFormat tx_format = this->get_current_tx_audio_format_();
    this->debug_log_pcm_level_("tx_network", data, length, tx_format, this->audio_debug_last_tx_log_ms_,
                               this->audio_debug_tx_frames_);
  }
#endif
  this->transport_->send_audio_frame(data, length);
}

void VoipStack::process_tx_chunk_(const uint8_t *audio_chunk) {
  this->send_chunk_(audio_chunk, this->tx_audio_chunk_bytes_());
}

bool VoipStack::read_tx_chunk_(uint8_t *audio_chunk) {
  const size_t frame_bytes = this->tx_audio_chunk_bytes_();
  return this->mic_buffer_ != nullptr &&
         this->mic_buffer_->read(audio_chunk, frame_bytes, 0) == frame_bytes;
}

bool VoipStack::write_mic_buffer_(const uint8_t *data, size_t len) {
  if (this->mic_buffer_ == nullptr || data == nullptr || len == 0) {
    return false;
  }
  const size_t frame_bytes = this->tx_audio_chunk_bytes_();
  const size_t capacity = this->tx_audio_buffer_bytes_();
  size_t skipped = 0;
  if (len > capacity) {
    // RingBuffer::write() cannot accept one item larger than its capacity.
    // Keep the newest PCM instead of losing the whole callback or preserving
    // audio that is already stale by the time the network task can send it.
    skipped = len - capacity;
    data += skipped;
    len = capacity;
  }
  const size_t free_before = this->mic_buffer_->free();
  const size_t replaced = free_before < len ? len - free_before : 0;
  const size_t written = this->mic_buffer_->write(data, len);
  const size_t dropped = skipped + replaced + (written < len ? len - written : 0);
  if (dropped > 0) {
    this->media_tx_queue_drops_.fetch_add(frames_for_bytes(dropped, frame_bytes), std::memory_order_relaxed);
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
    this->media_tx_queue_drop_bytes_.fetch_add(static_cast<uint32_t>(dropped), std::memory_order_relaxed);
#endif
  }
  if (written > 0 && this->tx_task_handle_ != nullptr) {
    xTaskNotifyGive(this->tx_task_handle_);
  }
  return skipped == 0 && written == len;
}

void VoipStack::tx_task_() {
  ESP_LOGD(TAG, "TX task started");

  uint8_t *const audio_chunk = this->tx_audio_chunk_;

  while (true) {
    if (!this->is_tx_stream_ready_()) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }

    if (this->mic_buffer_ == nullptr) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }

    // Capture-clocked TX: the microphone callback is the only pacing source.
    // Drain every complete frame immediately; RTP timestamps are sample-based,
    // so cadence jitter belongs in the receiver jitter buffer, not a TX timer.
    const size_t frame_bytes = this->tx_audio_chunk_bytes_();
    while (this->mic_buffer_->available() >= frame_bytes && this->read_tx_chunk_(audio_chunk)) {
      this->process_tx_chunk_(audio_chunk);
      if (!this->is_tx_stream_ready_())
        break;
    }
    this->media_tx_queue_depth_.store(
        static_cast<uint32_t>(this->mic_buffer_->available() / std::max<size_t>(1, frame_bytes)),
        std::memory_order_relaxed);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  }
}

// === Microphone Callback ===

void VoipStack::on_microphone_data_(const uint8_t *data, size_t len) {
  if (!this->is_tx_stream_ready_()) {
    return;
  }
  if (this->mic_buffer_ == nullptr || data == nullptr || len == 0) {
    return;
  }
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
  if (this->audio_debug_) {
    const uint32_t now = millis();
    const uint32_t delta = this->audio_debug_last_mic_callback_ms_ == 0
                               ? 0
                               : now - this->audio_debug_last_mic_callback_ms_;
    this->audio_debug_last_mic_callback_ms_ = now;
    this->audio_debug_mic_callbacks_++;
    if (now - this->audio_debug_last_mic_log_ms_ >= 1000) {
      const AudioFormat tx_format = this->get_current_tx_audio_format_();
      this->audio_debug_last_mic_log_ms_ = now;
      ESP_LOGI(TAG,
               "AudioDebug[mic_callback]: callbacks=%u len=%u delta_ms=%u "
               "tx_frame_bytes=%u "
               "accum_available=%u tx_queue_depth=%u tx_drops=%u tx_drop_bytes=%u "
               "tx_format=%u:%u:%u:%u state=%s",
               (unsigned) this->audio_debug_mic_callbacks_, (unsigned) len, (unsigned) delta,
               (unsigned) this->tx_audio_chunk_bytes_(), (unsigned) this->mic_buffer_->available(),
               (unsigned) this->media_tx_queue_depth_.load(std::memory_order_relaxed),
               (unsigned) this->media_tx_queue_drops_.load(std::memory_order_relaxed),
               (unsigned) this->media_tx_queue_drop_bytes_.load(std::memory_order_relaxed),
               (unsigned) tx_format.sample_rate, (unsigned) tx_format.pcm_format, (unsigned) tx_format.channels,
               (unsigned) tx_format.frame_ms, this->get_call_state_str());
    }
  }
#endif

  // Skip our gain when esp_audio_stack owns the mic_gain entity (already
  // applied upstream).
  int16_t *mic_converted = this->mic_converted_.load(std::memory_order_acquire);
  const float effective_gain = mic_converted != nullptr
      ? this->mic_gain_.load(std::memory_order_relaxed)
      : 1.0f;
  const bool needs_processing =
      mic_converted != nullptr && (effective_gain != 1.0f || this->dc_offset_removal_);

  if (needs_processing) {
    const int16_t *src = reinterpret_cast<const int16_t *>(data);
    const size_t total_samples = len / sizeof(int16_t);
    // Chunk by MIC_CONVERTED_SAMPLES so a long mic frame doesn't overflow
    // the staging buffer when gain/DC processing is on.
    size_t off = 0;
    while (off < total_samples) {
      const size_t chunk = std::min(total_samples - off, this->mic_processing_samples_());
      if (this->dc_offset_removal_) {
        for (size_t i = 0; i < chunk; i++) {
          mic_converted[i] = scale_sample(this->dc_blocker_.process(src[off + i]), effective_gain);
        }
      } else {
        scale_block_i16(src + off, mic_converted, chunk, effective_gain);
      }
      const size_t bytes = chunk * sizeof(int16_t);
      this->write_mic_buffer_(reinterpret_cast<const uint8_t *>(mic_converted), bytes);
      off += chunk;
    }
  } else {
    this->write_mic_buffer_(data, len);
  }
}
#endif  // USE_ESPHOME_VOIP_STACK_MIC

#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
void VoipStack::rx_task(void *param) {
  static_cast<VoipStack *>(param)->rx_task_();
}

void VoipStack::enqueue_rx_frame_(const TransportAudioFrame &frame) {
  if (this->rx_jitter_buffer_ == nullptr || frame.pcm == nullptr || frame.bytes == 0)
    return;
  const size_t expected = this->get_current_rx_audio_format_().nominal_frame_bytes();
  if (frame.bytes != expected || frame.bytes > this->rx_audio_chunk_alloc_bytes_)
    return;

  const auto before = this->rx_jitter_buffer_->counters();
  RtpJitterBuffer::Frame jitter_frame;
  jitter_frame.pcm = frame.pcm;
  jitter_frame.bytes = frame.bytes;
  jitter_frame.sequence = frame.sequence;
  jitter_frame.timestamp = frame.timestamp;
  jitter_frame.has_metadata = frame.has_rtp_metadata;
  if (this->rx_jitter_buffer_->push(jitter_frame)) {
    if (this->rx_task_handle_ != nullptr) {
      xTaskNotifyGive(this->rx_task_handle_);
    }
  }
  const auto after = this->rx_jitter_buffer_->counters();
  this->media_rx_queue_depth_.store(after.depth, std::memory_order_relaxed);
  if (after.drops > before.drops) {
    this->media_rx_queue_drops_.fetch_add(after.drops - before.drops, std::memory_order_relaxed);
  }
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
  if (after.late > before.late) {
    this->audio_debug_rx_late_frames_.fetch_add(after.late - before.late, std::memory_order_relaxed);
  }
  if (after.missing > before.missing) {
    this->audio_debug_rx_missing_frames_.fetch_add(after.missing - before.missing, std::memory_order_relaxed);
  }
  if (after.duplicates > before.duplicates) {
    this->audio_debug_rx_duplicate_frames_.fetch_add(after.duplicates - before.duplicates, std::memory_order_relaxed);
  }
#endif
}

void VoipStack::play_rx_frame_(const uint8_t *pcm, size_t bytes, SilenceReason silence_reason, TickType_t ticks_to_wait) {
  if (this->speaker_ == nullptr || pcm == nullptr || bytes == 0)
    return;
  if (silence_reason == SilenceReason::NONE && this->rx_silence_chunk_ != nullptr &&
      this->volume_.load(std::memory_order_relaxed) <= 0.001f) {
    pcm = this->rx_silence_chunk_;
    silence_reason = SilenceReason::MUTED_SINK;
  }

  size_t offset = 0;
  uint8_t stalls = 0;
  TickType_t wait_budget = ticks_to_wait;
  while (offset < bytes && this->call_state_.load(std::memory_order_acquire) == CallState::IN_CALL) {
    const size_t written = this->speaker_->play(pcm + offset, bytes - offset, wait_budget);
    wait_budget = 0;
    if (written == 0) {
      if (++stalls >= 4) {
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
        this->audio_debug_speaker_short_writes_.fetch_add(1, std::memory_order_relaxed);
#endif
        break;
      }
      continue;
    }
    if (written < bytes - offset) {
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
      this->audio_debug_speaker_short_writes_.fetch_add(1, std::memory_order_relaxed);
#endif
    }
    stalls = 0;
    offset += written;
  }
  if (silence_reason != SilenceReason::NONE) {
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
    this->audio_debug_rx_silence_frames_.fetch_add(1, std::memory_order_relaxed);
#endif
  }
}

void VoipStack::play_silence_frame_(SilenceReason reason, TickType_t ticks_to_wait) {
  const size_t silence_bytes = this->get_current_rx_audio_format_().nominal_frame_bytes();
  if (this->rx_silence_chunk_ != nullptr && silence_bytes <= this->rx_audio_chunk_alloc_bytes_) {
    this->play_rx_frame_(this->rx_silence_chunk_, silence_bytes, reason, ticks_to_wait);
  }
}

void VoipStack::rx_task_() {
  ESP_LOGD(TAG, "RX playout task started");

  while (true) {
    if (!this->audio_devices_active_.load(std::memory_order_acquire) ||
        this->call_state_.load(std::memory_order_acquire) != CallState::IN_CALL) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }

    const AudioFormat rx_format = this->get_current_rx_audio_format_();
    const TickType_t frame_ticks = std::max<TickType_t>(1, pdMS_TO_TICKS(rx_format.frame_ms));
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
    if (this->speaker_ != nullptr && !this->speaker_->is_running()) {
      ulTaskNotifyTake(pdTRUE, frame_ticks);
      continue;
    }
#endif
    const auto read_result = this->rx_jitter_buffer_ != nullptr
                                 ? this->rx_jitter_buffer_->read(this->rx_audio_chunk_, rx_format.nominal_frame_bytes())
                                 : RtpJitterBuffer::ReadResult::BUFFERING;
    if (this->rx_jitter_buffer_ != nullptr) {
      this->media_rx_queue_depth_.store(this->rx_jitter_buffer_->depth(), std::memory_order_relaxed);
    }
    if (read_result == RtpJitterBuffer::ReadResult::FRAME) {
      this->rx_underrun_start_ms_.store(0, std::memory_order_relaxed);
      // The downstream speaker/mixer owns the hardware cadence. Blocking here
      // for up to one frame gives backpressure when that buffer is full; adding
      // an unconditional delay after a successful write double-paces playout
      // and lets the RTP queue grow until audible gaps appear.
      const size_t frame_bytes = rx_format.nominal_frame_bytes();
      this->play_rx_frame_(this->rx_audio_chunk_, frame_bytes, SilenceReason::NONE, frame_ticks);
    } else {
      // Initial prebuffering must stay quiet, but BUFFERING is also returned
      // after an established stream drains completely. In that second case we
      // still have to clock silence into the sink or I2S can stall/pop until
      // enough RTP packets arrive to satisfy the prebuffer again.
      if (read_result == RtpJitterBuffer::ReadResult::BUFFERING &&
          !this->first_audio_received_.load(std::memory_order_acquire)) {
        ulTaskNotifyTake(pdTRUE, frame_ticks);
        continue;
      }
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
      if (read_result == RtpJitterBuffer::ReadResult::MISSING) {
        this->audio_debug_rx_missing_frames_.fetch_add(1, std::memory_order_relaxed);
      }
#endif
      // RTP notifications must not shorten the missing frame itself. Wait to
      // the one-frame deadline while still consuming wakeups; this preserves
      // playout cadence without adding a second speaker wait or task delay.
      const TickType_t wait_started = xTaskGetTickCount();
      TickType_t remaining = frame_ticks;
      while (remaining > 0) {
        ulTaskNotifyTake(pdTRUE, remaining);
        if (!this->audio_devices_active_.load(std::memory_order_acquire) ||
            this->call_state_.load(std::memory_order_acquire) != CallState::IN_CALL) {
          break;
        }
        const TickType_t elapsed = xTaskGetTickCount() - wait_started;
        if (elapsed >= frame_ticks) break;
        remaining = frame_ticks - elapsed;
      }
      if (!this->audio_devices_active_.load(std::memory_order_acquire) ||
          this->call_state_.load(std::memory_order_acquire) != CallState::IN_CALL) {
        continue;
      }
      const uint32_t now = millis();
      uint32_t underrun_start = this->rx_underrun_start_ms_.load(std::memory_order_relaxed);
      if (underrun_start == 0) {
        underrun_start = read_result == RtpJitterBuffer::ReadResult::MISSING ? now - VoipStack::kRxSilenceAfterMs : now;
        this->rx_underrun_start_ms_.store(underrun_start, std::memory_order_relaxed);
      }
      if (now - underrun_start >= VoipStack::kRxSilenceAfterMs &&
          this->first_audio_received_.load(std::memory_order_acquire) &&
          this->audio_devices_active_.load(std::memory_order_acquire) &&
          this->call_state_.load(std::memory_order_acquire) == CallState::IN_CALL) {
        // The notification wait above already owns this frame's cadence.
        // Do not add a second blocking budget in the speaker sink.
        this->play_silence_frame_(SilenceReason::NETWORK_GAP, 0);
      }
      continue;
    }
  }
}

void VoipStack::reset_rx_audio_() {
  if (this->rx_jitter_buffer_ != nullptr) {
    this->rx_jitter_buffer_->reset();
  }
  this->media_rx_queue_depth_.store(0, std::memory_order_relaxed);
  this->media_rx_queue_drops_.store(0, std::memory_order_relaxed);
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
  this->media_tx_queue_drop_bytes_.store(0, std::memory_order_relaxed);
  this->audio_debug_rx_late_frames_.store(0, std::memory_order_relaxed);
  this->audio_debug_rx_missing_frames_.store(0, std::memory_order_relaxed);
  this->audio_debug_rx_duplicate_frames_.store(0, std::memory_order_relaxed);
  this->audio_debug_rx_silence_frames_.store(0, std::memory_order_relaxed);
  this->audio_debug_speaker_short_writes_.store(0, std::memory_order_relaxed);
#endif
  this->rx_underrun_start_ms_.store(0, std::memory_order_relaxed);
}
#endif  // USE_ESPHOME_VOIP_STACK_SPEAKER

}  // namespace voip_stack
}  // namespace esphome

#endif  // USE_ESP32
