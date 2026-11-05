#include "voip_stack.h"

#ifdef USE_ESP32

#include <algorithm>
#include <cstdint>
#include <cstring>

#include <esp_timer.h>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include "audio_core_audio_utils.h"

namespace esphome {
namespace voip_stack {

static const char *const TAG = "voip_stack.audio";

#if defined(USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG) && defined(USE_ESPHOME_VOIP_STACK_MIC)
void VoipStack::record_tx_debug_(const uint8_t *pcm, size_t bytes, const AudioFormat &format) {
  const uint32_t frame_count = ++this->audio_debug_tx_frames_;
  const uint32_t now = millis();
  if (now - this->audio_debug_last_tx_log_ms_ < 1000) return;
  this->audio_debug_last_tx_log_ms_ = now;
  PcmDebugData data;
  data.producer_ms = now;
  data.frames = frame_count;
  data.bytes = bytes;
  data.samples = bytes / sizeof(int16_t);
  data.format = pack_audio_format(format);
  data.volume = this->volume_.load(std::memory_order_relaxed);
  data.state = static_cast<uint8_t>(this->call_state_.load(std::memory_order_acquire));
  if (pcm == nullptr || data.samples == 0) {
    data.status = 1;
  } else if (format.pcm_format != PcmFormat::S16LE) {
    data.status = 2;
  } else {
    const auto levels = compute_levels_dbfs_i16(reinterpret_cast<const int16_t *>(pcm), data.samples);
    data.peak = levels.peak;
    data.peak_dbfs = levels.peak_dbfs;
    data.rms_dbfs = levels.rms_dbfs;
  }
  if (this->audio_debug_pcm_.busy.test_and_set(std::memory_order_acquire)) {
    this->audio_debug_stats_quality_.fetch_or(1U, std::memory_order_relaxed);
    return;
  }
  if (this->audio_debug_pcm_.pending.load(std::memory_order_relaxed))
    this->audio_debug_stats_quality_.fetch_or(4U, std::memory_order_relaxed);
  this->audio_debug_pcm_.data = data;
  this->audio_debug_pcm_.pending.store(true, std::memory_order_release);
  this->audio_debug_pcm_.busy.clear(std::memory_order_release);
  this->enable_loop_soon_any_context();
}
#endif

#ifdef USE_ESPHOME_VOIP_STACK_MIC
namespace {
static uint32_t frames_for_bytes(size_t bytes, size_t frame_bytes) {
  if (bytes == 0 || frame_bytes == 0)
    return 0;
  return static_cast<uint32_t>((bytes + frame_bytes - 1) / frame_bytes);
}

static TickType_t ticks_until_deadline_us(int64_t deadline_us) {
  const int64_t remaining_us = deadline_us - esp_timer_get_time();
  if (remaining_us <= 0)
    return 0;
  constexpr uint64_t us_per_second = 1000000;
  const uint64_t ticks =
      (static_cast<uint64_t>(remaining_us) * configTICK_RATE_HZ + us_per_second - 1) / us_per_second;
  return static_cast<TickType_t>(std::max<uint64_t>(1, ticks));
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
         this->get_current_tx_audio_format_().is_valid() &&
         this->transport_ != nullptr && this->transport_->is_connected();
}

void VoipStack::send_chunk_(const uint8_t *data, size_t length) {
  if (!this->is_tx_stream_ready_())
    return;
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
  if (this->audio_debug_) {
    const AudioFormat tx_format = this->get_current_tx_audio_format_();
    this->record_tx_debug_(data, length, tx_format);
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
  int64_t next_send_at_us = 0;
  uint8_t paced_frame_ms = 0;

  while (true) {
    if (!this->is_tx_stream_ready_()) {
      next_send_at_us = 0;
      paced_frame_ms = 0;
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }

    if (this->mic_buffer_ == nullptr) {
      next_send_at_us = 0;
      paced_frame_ms = 0;
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }

    const AudioFormat tx_format = this->get_current_tx_audio_format_();
    const size_t frame_bytes = this->tx_audio_chunk_bytes_();
    if (frame_bytes == 0 || tx_format.frame_ms == 0) {
      next_send_at_us = 0;
      paced_frame_ms = 0;
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }

    if (paced_frame_ms != tx_format.frame_ms) {
      next_send_at_us = 0;
      paced_frame_ms = tx_format.frame_ms;
    }

    if (this->mic_buffer_->available() < frame_bytes) {
      this->media_tx_queue_depth_.store(0, std::memory_order_relaxed);
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }

    const int64_t now_us = esp_timer_get_time();
    if (next_send_at_us != 0 && now_us < next_send_at_us) {
      const TickType_t wait_ticks = ticks_until_deadline_us(next_send_at_us);
      if (wait_ticks > 0) {
        ulTaskNotifyTake(pdTRUE, wait_ticks);
        continue;
      }
    }

    if (!this->read_tx_chunk_(audio_chunk))
      continue;
    this->process_tx_chunk_(audio_chunk);

    const int64_t frame_interval_us = static_cast<int64_t>(tx_format.frame_ms) * 1000;
    // Preserve capture-clock debt after a late wakeup. The bounded microphone
    // queue limits catch-up while preventing scheduler stalls from becoming
    // permanent PCM loss.
    next_send_at_us = next_send_at_us == 0
                          ? esp_timer_get_time() + frame_interval_us
                          : next_send_at_us + frame_interval_us;

    this->media_tx_queue_depth_.store(
        static_cast<uint32_t>(this->mic_buffer_->available() / std::max<size_t>(1, frame_bytes)),
        std::memory_order_relaxed);
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
      MicDebugData snapshot;
      snapshot.producer_ms = now;
      snapshot.callbacks = this->audio_debug_mic_callbacks_;
      snapshot.bytes = len;
      snapshot.delta_ms = delta;
      snapshot.frame_bytes = this->tx_audio_chunk_bytes_();
      snapshot.available = this->mic_buffer_->available();
      snapshot.depth = this->media_tx_queue_depth_.load(std::memory_order_relaxed);
      snapshot.drops = this->media_tx_queue_drops_.load(std::memory_order_relaxed);
      snapshot.drop_bytes = this->media_tx_queue_drop_bytes_.load(std::memory_order_relaxed);
      snapshot.format = pack_audio_format(tx_format);
      snapshot.state = static_cast<uint8_t>(this->call_state_.load(std::memory_order_acquire));
      if (this->audio_debug_mic_.busy.test_and_set(std::memory_order_acquire)) {
        this->audio_debug_stats_quality_.fetch_or(2U, std::memory_order_relaxed);
      } else {
        if (this->audio_debug_mic_.pending.load(std::memory_order_relaxed))
          this->audio_debug_stats_quality_.fetch_or(8U, std::memory_order_relaxed);
        this->audio_debug_mic_.data = snapshot;
        this->audio_debug_mic_.pending.store(true, std::memory_order_release);
        this->audio_debug_mic_.busy.clear(std::memory_order_release);
        this->enable_loop_soon_any_context();
      }
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
        const uint8_t channels = this->get_current_tx_audio_format_().channels;
        for (size_t i = 0; i < chunk; i++) {
          mic_converted[i] = scale_sample(this->dc_blockers_[this->dc_channel_phase_].process(src[off + i]),
                                          effective_gain);
          if (++this->dc_channel_phase_ >= channels) this->dc_channel_phase_ = 0;
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

VoipStack::RxPlayoutPhase VoipStack::rx_playout_phase_(uint32_t revision) const {
  const uint32_t state = this->rx_playout_state_.load(std::memory_order_acquire);
  return (state >> 3U) == (revision & 0x1fffffffU) ? static_cast<RxPlayoutPhase>(state & 7U) : RxPlayoutPhase::STOPPED;
}

bool VoipStack::prepare_rx_playout_() {
  if (this->speaker_ == nullptr || !this->get_current_rx_audio_format_().is_valid()) return true;
  if (this->speaker_feedback_source_ != nullptr &&
      (this->speaker_feedback_source_->is_failed() || this->speaker_feedback_source_->status_has_error()))
    return false;
  const uint32_t revision = this->rx_audio_revision_.load(std::memory_order_acquire);
  const RxPlayoutPhase phase = this->rx_playout_phase_(revision);
  if (phase == RxPlayoutPhase::READY || phase == RxPlayoutPhase::ACTIVE) return true;
  if (phase == RxPlayoutPhase::STOPPED) {
    this->rx_prepare_started_us_.store(static_cast<uint32_t>(esp_timer_get_time()), std::memory_order_release);
    // Unrecognized legacy providers retain input-admission readiness. They
    // never wait for an output callback their public implementation omits.
    const auto next = this->speaker_feedback_source_ == nullptr ? RxPlayoutPhase::READY : RxPlayoutPhase::PREPARING;
    this->rx_playout_state_.store((revision << 3U) | static_cast<uint8_t>(next), std::memory_order_release);
    if (this->rx_task_handle_ != nullptr) xTaskNotifyGive(this->rx_task_handle_);
  }
  return this->rx_playout_phase_(revision) == RxPlayoutPhase::READY;
}

bool VoipStack::activate_rx_playout_() {
  if (this->speaker_ == nullptr || !this->get_current_rx_audio_format_().is_valid()) return true;
  const uint32_t revision = this->rx_audio_revision_.load(std::memory_order_acquire);
  uint32_t expected = (revision << 3U) | static_cast<uint8_t>(RxPlayoutPhase::READY);
  if (!this->rx_playout_state_.compare_exchange_strong(
          expected, (revision << 3U) | static_cast<uint8_t>(RxPlayoutPhase::ACTIVE), std::memory_order_acq_rel))
    return false;
  if (this->rx_task_handle_ != nullptr) xTaskNotifyGive(this->rx_task_handle_);
  return true;
}

void VoipStack::speaker_output_progress_(uint32_t frames, int64_t timestamp) {
  const uint32_t state = this->rx_playout_state_.load(std::memory_order_acquire);
  if ((state & 7U) != static_cast<uint8_t>(RxPlayoutPhase::WAITING_OUTPUT)) {
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
    if (this->audio_debug_ && ((state & 7U) == static_cast<uint8_t>(RxPlayoutPhase::READY) ||
                               (state & 7U) == static_cast<uint8_t>(RxPlayoutPhase::ACTIVE)))
      this->debug_output_last_us_.store(static_cast<uint32_t>(timestamp), std::memory_order_relaxed);
#endif
    return;
  }
  const uint32_t revision = this->rx_audio_revision_.load(std::memory_order_acquire);
  if (frames == 0 || this->speaker_ == nullptr || !this->speaker_->is_running() ||
      this->rx_playout_phase_(revision) == RxPlayoutPhase::STOPPED ||
      static_cast<int32_t>(static_cast<uint32_t>(timestamp) -
                           this->rx_prepare_started_us_.load(std::memory_order_acquire)) < 0)
    return;
  uint32_t expected = (revision << 3U) | static_cast<uint8_t>(RxPlayoutPhase::WAITING_OUTPUT);
  const bool prepared = this->rx_playout_state_.compare_exchange_strong(
      expected, (revision << 3U) | static_cast<uint8_t>(RxPlayoutPhase::READY), std::memory_order_release);
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
  if (this->audio_debug_) {
    this->debug_output_last_us_.store(static_cast<uint32_t>(timestamp), std::memory_order_relaxed);
    if (prepared) {
      uint32_t first = 0;
      this->debug_output_first_us_.compare_exchange_strong(first, static_cast<uint32_t>(timestamp),
                                                           std::memory_order_relaxed);
    }
  }
#else
  (void)prepared;
#endif
}

bool VoipStack::play_rx_preroll_(uint32_t revision, TickType_t frame_ticks) {
  const RxPlayoutPhase phase = this->rx_playout_phase_(revision);
  if (this->speaker_feedback_source_ == nullptr && phase == RxPlayoutPhase::ACTIVE) return false;
  const bool awaiting_first =
      phase == RxPlayoutPhase::ACTIVE && !this->first_audio_received_.load(std::memory_order_acquire);
  if (!awaiting_first && phase != RxPlayoutPhase::PREPARING && phase != RxPlayoutPhase::WAITING_OUTPUT &&
      phase != RxPlayoutPhase::READY)
    return false;
  if (!awaiting_first && this->speaker_feedback_source_ == nullptr) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return true;
  }
  // The ordinary receive clock owns the frame deadline, including wakeups.
  const TickType_t started = xTaskGetTickCount();
  TickType_t remaining = frame_ticks;
  while (remaining > 0 && this->rx_audio_revision_.load(std::memory_order_acquire) == revision) {
    ulTaskNotifyTake(pdTRUE, remaining);
    const auto current = this->rx_playout_phase_(revision);
    if (current == RxPlayoutPhase::STOPPED || (!awaiting_first && current == RxPlayoutPhase::ACTIVE) ||
        (awaiting_first &&
         (current != RxPlayoutPhase::ACTIVE || this->first_audio_received_.load(std::memory_order_acquire))))
      return true;
    const TickType_t elapsed = xTaskGetTickCount() - started;
    if (elapsed >= frame_ticks) break;
    remaining = frame_ticks - elapsed;
  }
  if (this->rx_audio_revision_.load(std::memory_order_acquire) == revision &&
      this->rx_playout_phase_(revision) != RxPlayoutPhase::STOPPED &&
      (awaiting_first || this->rx_playout_phase_(revision) != RxPlayoutPhase::ACTIVE)) {
    this->play_silence_frame_(SilenceReason::PREROLL, 0, revision);
  }
  return true;
}

#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
void VoipStack::debug_rx_startup_event_(RxStartupEvent event, uint32_t revision,
                                      const RtpJitterBuffer::Counters *jitter) {
  if (!this->audio_debug_ || this->rx_audio_revision_.load(std::memory_order_acquire) != revision) return;
  auto &record = this->audio_debug_rx_startup_[static_cast<uint8_t>(event)];
  uint32_t seen = record.revision.load(std::memory_order_relaxed);
  if (record.present.load(std::memory_order_acquire) &&
      (seen == revision || static_cast<int32_t>(revision - seen) <= 0)) return;
  const uint64_t at_us = esp_timer_get_time();
  if (record.busy.test_and_set(std::memory_order_acquire)) {
    if (this->rx_audio_revision_.load(std::memory_order_acquire) == revision)
      this->audio_debug_rx_startup_quality_.fetch_or(1U, std::memory_order_relaxed);
    return;
  }
  seen = record.revision.load(std::memory_order_relaxed);
  if ((!record.present.load(std::memory_order_relaxed) ||
       (seen != revision && static_cast<int32_t>(revision - seen) > 0)) &&
      this->rx_audio_revision_.load(std::memory_order_acquire) == revision) {
    RxStartupData data;
    data.us = at_us;
    data.output_us = this->debug_output_last_us_.load(std::memory_order_relaxed);
    data.state = static_cast<uint8_t>(this->call_state_.load(std::memory_order_acquire));
    data.flags = (this->speaker_ != nullptr && this->speaker_->is_running() ? 1U : 0U);
    if (jitter != nullptr) {
      data.flags |= 4U;
      data.depth = static_cast<uint16_t>(jitter->depth);
      data.drops = jitter->drops;
      data.discarded = jitter->discarded_frames;
    }
    if (this->rx_audio_revision_.load(std::memory_order_acquire) == revision) {
      if (event == RxStartupEvent::FIRST_ZERO_WRITE) this->debug_zero_output_us_ = data.output_us;
      if (event == RxStartupEvent::FIRST_JITTER_DROP) this->debug_drop_output_us_ = data.output_us;
      record.data = data;
      record.revision.store(revision, std::memory_order_release);
      record.present.store(true, std::memory_order_release);
    }
  }
  record.busy.clear(std::memory_order_release);
}
#endif

void VoipStack::enqueue_rx_frame_(const TransportAudioFrame &frame) {
  if (this->rx_jitter_buffer_ == nullptr || frame.pcm == nullptr || frame.bytes == 0)
    return;
  const AudioFormat rx_format = frame.format_packed != 0
                                    ? unpack_audio_format(frame.format_packed)
                                    : this->get_current_rx_audio_format_();
  const size_t expected = rx_format.nominal_frame_bytes();
  if ((rx_format.codec == AudioCodec::PCM && frame.bytes != expected) ||
      frame.bytes > this->rx_jitter_frame_alloc_bytes_)
    return;
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
  const uint32_t revision = this->rx_audio_revision_.load(std::memory_order_acquire);
#endif
  if (frame.source_changed) {
    this->rx_jitter_buffer_->reset();
  }

  const auto before = this->rx_jitter_buffer_->counters();
  RtpJitterBuffer::Frame jitter_frame;
  jitter_frame.pcm = frame.pcm;
  jitter_frame.bytes = frame.bytes;
  jitter_frame.sequence = frame.sequence;
  jitter_frame.timestamp = frame.timestamp;
  jitter_frame.has_metadata = frame.has_rtp_metadata;
  jitter_frame.source_changed = frame.source_changed;
  jitter_frame.format_packed = frame.format_packed;
  if (this->rx_jitter_buffer_->push(jitter_frame)) {
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
    this->debug_rx_startup_event_(RxStartupEvent::RX_ENQUEUE, revision);
#endif
    if (this->rx_task_handle_ != nullptr) {
      xTaskNotifyGive(this->rx_task_handle_);
    }
  }
  const auto after = this->rx_jitter_buffer_->counters();
  this->media_rx_queue_depth_.store(after.depth, std::memory_order_relaxed);
  if (after.drops > before.drops) {
    this->media_rx_queue_drops_.fetch_add(after.drops - before.drops, std::memory_order_relaxed);
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
    this->debug_rx_startup_event_(RxStartupEvent::FIRST_JITTER_DROP, revision, &after);
#endif
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

void VoipStack::play_rx_frame_(const uint8_t *pcm, size_t bytes, SilenceReason silence_reason, TickType_t ticks_to_wait,
                             uint32_t revision) {
  if (this->speaker_ == nullptr || pcm == nullptr || bytes == 0)
    return;
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
  // Trace the NONE PCM path before local muting, excluding generated silence.
  // The existing Opus PLC path also uses NONE; this does not prove RTP origin.
  const bool trace_pcm = silence_reason == SilenceReason::NONE;
#endif
  const bool receive_paused = this->transport_ != nullptr && this->transport_->audio_receive_paused();
  if (silence_reason == SilenceReason::NONE && this->rx_audio_chunk_ != nullptr &&
      bytes <= this->rx_audio_chunk_alloc_bytes_ &&
      (receive_paused || this->volume_.load(std::memory_order_relaxed) <= 0.001f)) {
    // The RX task owns decode and playout serially, including partial writes.
    // Zero PCM after decoding so codec prediction/PLC state still advances.
    memset(this->rx_audio_chunk_, 0, bytes);
    pcm = this->rx_audio_chunk_;
    silence_reason = receive_paused ? SilenceReason::NETWORK_GAP : SilenceReason::MUTED_SINK;
  }

  size_t offset = 0;
  while (offset < bytes && (this->audio_devices_active_.load(std::memory_order_acquire) ||
                           this->rx_playout_phase_(revision) != RxPlayoutPhase::ACTIVE) &&
         this->rx_playout_phase_(revision) != RxPlayoutPhase::STOPPED &&
         this->rx_audio_revision_.load(std::memory_order_acquire) == revision) {
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
    if (trace_pcm) this->debug_rx_startup_event_(RxStartupEvent::PLAY_ATTEMPT, revision);
#endif
    const TickType_t started = xTaskGetTickCount();
    const size_t written = this->speaker_->play(pcm + offset, bytes - offset, ticks_to_wait);
    if (written > 0 && silence_reason == SilenceReason::PREROLL) {
      uint32_t expected = (revision << 3U) | static_cast<uint8_t>(RxPlayoutPhase::PREPARING);
      this->rx_playout_state_.compare_exchange_strong(
          expected, (revision << 3U) | static_cast<uint8_t>(RxPlayoutPhase::WAITING_OUTPUT), std::memory_order_release);
    }
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
    if (trace_pcm) {
      if (written == 0) {
        this->debug_rx_startup_event_(RxStartupEvent::FIRST_ZERO_WRITE, revision);
      } else {
        this->debug_rx_startup_event_(RxStartupEvent::PCM_ACCEPTED, revision);
        if (this->audio_debug_rx_startup_[static_cast<uint8_t>(RxStartupEvent::FIRST_ZERO_WRITE)]
                .revision.load(std::memory_order_relaxed) == revision) {
          this->debug_rx_startup_event_(RxStartupEvent::RESUMED, revision);
        }
      }
    }
#endif
    if (written == 0) {
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
      this->audio_debug_speaker_short_writes_.fetch_add(1, std::memory_order_relaxed);
#endif
      // Silence/PLC callers already own their frame deadline. Real received
      // audio stays in the existing decode buffer until the sink accepts it.
      if (ticks_to_wait == 0) break;
      const TickType_t elapsed = xTaskGetTickCount() - started;
      if (elapsed < ticks_to_wait) {
        // A stopped or failed sink may return immediately instead of waiting.
        // Yield cooperatively; call termination wakes this same RX task.
        ulTaskNotifyTake(pdTRUE, ticks_to_wait - elapsed);
      }
      continue;
    }
    if (written < bytes - offset) {
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
      this->audio_debug_speaker_short_writes_.fetch_add(1, std::memory_order_relaxed);
#endif
    }
    offset += written;
  }
  if (silence_reason != SilenceReason::NONE) {
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
    this->audio_debug_rx_silence_frames_.fetch_add(1, std::memory_order_relaxed);
#endif
  }
}

void VoipStack::play_silence_frame_(SilenceReason reason, TickType_t ticks_to_wait, uint32_t revision,
                                  uint16_t frame_ms) {
  AudioFormat format = this->get_current_rx_audio_format_();
  if (frame_ms != 0) format.frame_ms = frame_ms;
  const size_t silence_bytes = format.nominal_frame_bytes();
  if (this->rx_audio_chunk_ != nullptr && silence_bytes <= this->rx_audio_chunk_alloc_bytes_) {
    memset(this->rx_audio_chunk_, 0, silence_bytes);
    this->play_rx_frame_(this->rx_audio_chunk_, silence_bytes, reason, ticks_to_wait, revision);
  }
}

void VoipStack::rx_task_() {
  ESP_LOGD(TAG, "RX playout task started");
  uint32_t previous_format_packed = 0;
  uint32_t previous_revision = UINT32_MAX;

  while (true) {
    const uint32_t revision = this->rx_audio_revision_.load(std::memory_order_acquire);
    if (this->rx_playout_phase_(revision) == RxPlayoutPhase::STOPPED ||
        (!this->audio_devices_active_.load(std::memory_order_acquire) &&
         this->rx_playout_phase_(revision) == RxPlayoutPhase::ACTIVE)) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }

    if (revision != previous_revision) {
      previous_format_packed = 0;
      previous_revision = revision;
    }
    AudioFormat rx_format = this->get_current_rx_audio_format_();
    if (previous_format_packed != 0)
      rx_format.frame_ms = unpack_audio_format(previous_format_packed).frame_ms;
    if (!rx_format.is_valid()) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }
    const uint64_t frame_tick_numerator =
        static_cast<uint64_t>(rx_format.frame_ms) * configTICK_RATE_HZ;
    const TickType_t frame_ticks = static_cast<TickType_t>(
        std::max<uint64_t>(1, (frame_tick_numerator + 999) / 1000));
    if (this->play_rx_preroll_(revision, frame_ticks)) continue;
    // A sink may go idle before the first RTP packet arrives. Deliver media
    // through play(), whose speaker contract starts it again as needed; waiting
    // for is_running() here would prevent that restart indefinitely.
    uint8_t *network_buffer = this->rx_network_chunk_;
    size_t network_bytes = 0;
    uint32_t format_packed = 0;
    bool source_changed = false;
    const auto read_result = this->rx_jitter_buffer_ != nullptr
                                 ? this->rx_jitter_buffer_->read(network_buffer, this->rx_jitter_frame_alloc_bytes_,
                                       nullptr, nullptr, nullptr, &network_bytes,
                                       &source_changed, &format_packed)
                                 : RtpJitterBuffer::ReadResult::BUFFERING;
    if (this->rx_audio_revision_.load(std::memory_order_acquire) != revision) continue;
    if (this->rx_jitter_buffer_ != nullptr) {
      this->media_rx_queue_depth_.store(this->rx_jitter_buffer_->depth(), std::memory_order_relaxed);
    }
    if (read_result == RtpJitterBuffer::ReadResult::FRAME) {
      this->rx_underrun_start_ms_.store(0, std::memory_order_relaxed);
      // The downstream speaker/mixer owns the hardware cadence. Blocking here
      // for up to one frame gives backpressure when that buffer is full; adding
      // an unconditional delay after a successful write double-paces playout
      // and lets the RTP queue grow until audible gaps appear.
      const AudioFormat frame_format = format_packed != 0 ? unpack_audio_format(format_packed) : rx_format;
      size_t frame_bytes = network_bytes;
      const uint8_t *playout = network_buffer;
      const uint32_t current_format_packed = pack_audio_format(frame_format);
      AudioFormat previous_geometry = previous_format_packed != 0
          ? unpack_audio_format(previous_format_packed) : frame_format;
      previous_geometry.frame_ms = frame_format.frame_ms;
      if (source_changed || previous_format_packed == 0 || !(previous_geometry == frame_format)) {
        this->transport_->reset_audio_decoder();
        if (frame_format.codec == AudioCodec::PCM)
          ESP_LOGD(TAG, "RX wire PCM=%uHz playout=%uHz frame=%ums",
                   (unsigned) frame_format.sample_rate, (unsigned) rx_format.sample_rate,
                   (unsigned) frame_format.frame_ms);
      }
      previous_format_packed = current_format_packed;
      const AudioFormat source_pcm = decoded_pcm_format(frame_format);
      AudioFormat target_pcm = decoded_pcm_format(rx_format);
      target_pcm.frame_ms = frame_format.frame_ms;
#if defined(USE_ESPHOME_VOIP_STACK_OPUS) || defined(USE_ESPHOME_VOIP_STACK_G722)
      if (frame_format.codec != AudioCodec::PCM) {
        const uint8_t *encoded = network_buffer;
        uint8_t *decoded = this->rx_audio_chunk_;
        size_t capacity = this->rx_audio_chunk_alloc_bytes_;
        if (!(source_pcm == target_pcm)) {
          // Retain the compressed input in the existing output buffer, then
          // decode into the input buffer. The resampler can expand into the
          // larger output buffer without doubling network/jitter storage.
          if (network_bytes > this->rx_audio_chunk_alloc_bytes_ ||
              source_pcm.nominal_frame_bytes() > this->rx_jitter_frame_alloc_bytes_) continue;
          memcpy(this->rx_audio_chunk_, network_buffer, network_bytes);
          encoded = this->rx_audio_chunk_;
          decoded = network_buffer;
          capacity = this->rx_jitter_frame_alloc_bytes_;
        }
        frame_bytes = this->transport_->decode_audio_payload(
            encoded, network_bytes, frame_format, decoded, capacity);
        if (frame_bytes != decoded_pcm_format(frame_format).nominal_frame_bytes()) continue;
        playout = decoded;
      }
#endif
      if (!(source_pcm == target_pcm)) {
        // Decoded codecs already occupy audio_chunk. Reuse the other setup
        // buffer as output so a resampler never aliases its input.
        uint8_t *converted = playout == this->rx_audio_chunk_ ? network_buffer : this->rx_audio_chunk_;
        frame_bytes = this->transport_->convert_received_pcm(
            source_pcm, playout, frame_bytes, converted, this->rx_audio_chunk_alloc_bytes_);
        playout = converted;
      }
      if (frame_bytes == 0) continue;
      const TickType_t packet_ticks = static_cast<TickType_t>(std::max<uint64_t>(1,
          (static_cast<uint64_t>(frame_format.frame_ms) * configTICK_RATE_HZ + 999) / 1000));
      this->play_rx_frame_(playout, frame_bytes, SilenceReason::NONE, packet_ticks, revision);
    } else {
      // Initial prebuffering must stay quiet, but BUFFERING is also returned
      // after an established stream drains completely. In that second case we
      // still have to clock silence into the sink or I2S can stall/pop until
      // enough RTP packets arrive to satisfy the prebuffer again.
      // Receiving a packet does not mean the initial prebuffer is ready. Retry
      // on notifications until this revision has consumed its first frame.
      if (read_result == RtpJitterBuffer::ReadResult::BUFFERING && previous_format_packed == 0) {
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
            this->rx_playout_phase_(revision) != RxPlayoutPhase::ACTIVE ||
            this->rx_audio_revision_.load(std::memory_order_acquire) != revision) {
          break;
        }
        const TickType_t elapsed = xTaskGetTickCount() - wait_started;
        if (elapsed >= frame_ticks) break;
        remaining = frame_ticks - elapsed;
      }
      if (!this->audio_devices_active_.load(std::memory_order_acquire) ||
          this->rx_playout_phase_(revision) != RxPlayoutPhase::ACTIVE ||
          this->rx_audio_revision_.load(std::memory_order_acquire) != revision) {
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
          this->rx_playout_phase_(revision) == RxPlayoutPhase::ACTIVE) {
        // The notification wait above already owns this frame's cadence.
        // Do not add a second blocking budget in the speaker sink.
#ifdef USE_ESPHOME_VOIP_STACK_OPUS
        const AudioFormat previous_format = previous_format_packed != 0
            ? unpack_audio_format(previous_format_packed) : rx_format;
        if (read_result == RtpJitterBuffer::ReadResult::MISSING && previous_format.codec == AudioCodec::OPUS) {
          const size_t concealed = this->transport_->decode_audio_payload(
              nullptr, 0, previous_format, this->rx_audio_chunk_, this->rx_audio_chunk_alloc_bytes_);
          if (concealed == rx_format.nominal_frame_bytes()) {
            this->play_rx_frame_(this->rx_audio_chunk_, concealed, SilenceReason::NONE, 0, revision);
            continue;
          }
        }
#endif
        this->play_silence_frame_(SilenceReason::NETWORK_GAP, 0, revision, rx_format.frame_ms);
      }
      continue;
    }
  }
}

void VoipStack::reset_rx_audio_(bool preserve_playout) {
  const uint32_t old_revision = this->rx_audio_revision_.load(std::memory_order_acquire);
  const bool active = preserve_playout && this->rx_playout_phase_(old_revision) == RxPlayoutPhase::ACTIVE;
  const uint32_t revision = this->rx_audio_revision_.fetch_add(1, std::memory_order_acq_rel) + 1;
  this->rx_playout_state_.store(
      (revision << 3U) | static_cast<uint8_t>(active ? RxPlayoutPhase::ACTIVE : RxPlayoutPhase::STOPPED),
      std::memory_order_release);
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
