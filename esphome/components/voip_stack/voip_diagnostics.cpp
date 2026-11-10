#include "voip_stack.h"

#ifdef USE_ESP32
#include <cstdio>
#include <new>

namespace esphome::voip_stack {
static const char *const TAG = "voip_stack.diagnostics";
#if defined(USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG) && \
    (defined(USE_ESPHOME_VOIP_STACK_MIC) || defined(USE_ESPHOME_VOIP_STACK_SPEAKER))
static const char *const AUDIO_TAG = "voip_stack.audio";
#endif

#if defined(USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG) && defined(USE_ESPHOME_VOIP_STACK_MIC)
void VoipStack::emit_audio_debug_pending_() {
  if (!this->audio_debug_) return;
  if (this->audio_debug_pcm_.pending.load(std::memory_order_acquire)) {
    if (!this->audio_debug_pcm_.busy.test_and_set(std::memory_order_acquire)) {
      const PcmDebugData data = this->audio_debug_pcm_.data;
      this->audio_debug_pcm_.pending.store(false, std::memory_order_relaxed);
      this->audio_debug_pcm_.busy.clear(std::memory_order_release);
      const auto quality = this->audio_debug_stats_quality_.load(std::memory_order_relaxed);
      const auto *state = call_state_to_str(static_cast<CallState>(data.state));
      const auto format = unpack_audio_format(data.format);
      if (data.status == 1) {
        ESP_LOGI(AUDIO_TAG, "AudioDebug[tx_network]: frames=%u bytes=%u empty state=%s producer_ms=%u quality=%u quality_scope=since_boot",
            (unsigned) data.frames, (unsigned) data.bytes, state, (unsigned) data.producer_ms, (unsigned) quality);
      } else if (data.status == 2) {
        ESP_LOGI(AUDIO_TAG, "AudioDebug[tx_network]: frames=%u bytes=%u format=%u:%u:%u:%u levels=skipped_non_s16 voip_volume=%.3f state=%s producer_ms=%u quality=%u quality_scope=since_boot",
            (unsigned) data.frames, (unsigned) data.bytes, (unsigned) format.sample_rate,
            (unsigned) format.pcm_format, (unsigned) format.channels, (unsigned) format.frame_ms, data.volume, state,
            (unsigned) data.producer_ms, (unsigned) quality);
      } else {
        ESP_LOGI(AUDIO_TAG, "AudioDebug[tx_network]: frames=%u bytes=%u samples=%u peak=%u peak_dbfs=%.1f rms_dbfs=%.1f voip_volume=%.3f state=%s path=direct producer_ms=%u quality=%u quality_scope=since_boot",
            (unsigned) data.frames, (unsigned) data.bytes, (unsigned) data.samples, (unsigned) data.peak,
            data.peak_dbfs, data.rms_dbfs, data.volume, state, (unsigned) data.producer_ms, (unsigned) quality);
      }
    } else {
      this->audio_debug_stats_quality_.fetch_or(16U, std::memory_order_relaxed);
    }
  }
  if (this->audio_debug_mic_.pending.load(std::memory_order_acquire)) {
    if (!this->audio_debug_mic_.busy.test_and_set(std::memory_order_acquire)) {
      const MicDebugData data = this->audio_debug_mic_.data;
      this->audio_debug_mic_.pending.store(false, std::memory_order_relaxed);
      this->audio_debug_mic_.busy.clear(std::memory_order_release);
      const auto format = unpack_audio_format(data.format);
      ESP_LOGI(AUDIO_TAG, "AudioDebug[mic_callback]: callbacks=%u len=%u delta_ms=%u tx_frame_bytes=%u accum_available=%u tx_queue_depth=%u tx_drops=%u tx_drop_bytes=%u tx_format=%u:%u:%u:%u state=%s producer_ms=%u quality=%u quality_scope=since_boot",
          (unsigned) data.callbacks, (unsigned) data.bytes, (unsigned) data.delta_ms,
          (unsigned) data.frame_bytes, (unsigned) data.available, (unsigned) data.depth,
          (unsigned) data.drops, (unsigned) data.drop_bytes, (unsigned) format.sample_rate,
          (unsigned) format.pcm_format, (unsigned) format.channels, (unsigned) format.frame_ms,
          call_state_to_str(static_cast<CallState>(data.state)), (unsigned) data.producer_ms,
          (unsigned) this->audio_debug_stats_quality_.load(std::memory_order_relaxed));
    } else {
      this->audio_debug_stats_quality_.fetch_or(32U, std::memory_order_relaxed);
    }
  }
}
#endif

#if defined(USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG) && defined(USE_ESPHOME_VOIP_STACK_SPEAKER)
bool VoipStack::debug_rx_startup_snapshot_(RxStartupEvent event, uint32_t &revision, RxStartupData &data) {
  auto &record = this->audio_debug_rx_startup_[static_cast<uint8_t>(event)];
  if (record.busy.test_and_set(std::memory_order_acquire)) {
    this->audio_debug_rx_startup_quality_.fetch_or(2U, std::memory_order_relaxed);
    return false;
  }
  revision = record.revision.load(std::memory_order_relaxed);
  data = record.data;
  if (record.present.load(std::memory_order_relaxed)) data.flags |= 2U;
  record.busy.clear(std::memory_order_release);
  return true;
}
#endif

void VoipStack::dump_diagnostics() {
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  this->dump_registration_memory_();
#endif
  const uint32_t now = millis();
  if (this->diagnostic_dump_ || (this->last_diagnostics_ms_ != 0 && now - this->last_diagnostics_ms_ < 1000)) return;
  if (this->is_failed()) {
    ESP_LOGI(TAG, "BEGIN v=1 uptime_ms=%u failed=YES runtime=unavailable", (unsigned) now);
    ESP_LOGI(TAG, "END v=1 uptime_ms=%u see_boot_error_log", (unsigned) now);
    return;
  }
  auto dump = std::unique_ptr<DiagnosticDump>(new (std::nothrow) DiagnosticDump());
  if (!dump) { ESP_LOGW(TAG, "Diagnostic snapshot allocation failed"); return; }
  this->last_diagnostics_ms_ = now;
  dump->requested_ms = now;
  dump->state = this->call_state_.load(std::memory_order_acquire);
  auto call_hash = [this]() {
    uint32_t hash = 2166136261U;
    LockGuard lock(this->call_state_mutex_);
    for (unsigned char c : this->current_call_id_) hash = (hash ^ c) * 16777619U;
    return this->current_call_id_.empty() ? 0U : hash;
  };
  dump->call_hash = call_hash();
  const auto formats = this->snapshot_current_media_formats_();
  dump->tx = formats.tx;
  dump->rx = formats.rx;
  dump->has_transport = this->transport_ != nullptr;
  if (dump->has_transport) dump->transport = this->transport_->snapshot();
  dump->changed = dump->call_hash != call_hash() || dump->state != this->call_state_.load(std::memory_order_acquire);
  dump->tx_depth = this->media_tx_queue_depth_.load(std::memory_order_relaxed);
  dump->tx_drops = this->media_tx_queue_drops_.load(std::memory_order_relaxed);
  dump->rx_depth = this->media_rx_queue_depth_.load(std::memory_order_relaxed);
  dump->rx_drops = this->media_rx_queue_drops_.load(std::memory_order_relaxed);
  dump->receive_volume = this->get_volume();
  dump->transmit_gain = this->mic_gain_.load(std::memory_order_relaxed);
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
  if (this->rx_jitter_buffer_ != nullptr) {
    dump->jitter = this->rx_jitter_buffer_->counters();
    dump->has_jitter = true;
  }
#endif
  dump->audio_active = this->audio_devices_active_.load(std::memory_order_acquire);
  dump->route = this->get_media_route_str();
  std::snprintf(dump->last_reason, sizeof(dump->last_reason), "%.47s",
                this->last_reason_.empty() ? "none" : this->last_reason_.c_str());
  this->diagnostic_dump_ = std::move(dump);
  this->emit_diagnostic_section_();
}

void VoipStack::emit_diagnostic_section_() {
  if (!this->diagnostic_dump_) return;
  auto &dump = *this->diagnostic_dump_;
  const auto &transport = dump.transport;
  auto format = [](const char *name, const AudioFormat &value) {
    ESP_LOGI(TAG, "%s: codec=%s rate=%u channels=%u ptime_ms=%u",
             name, value.codec == AudioCodec::OPUS ? "opus" : "pcm", (unsigned) value.sample_rate,
             value.channels, value.frame_ms);
  };
  switch (dump.step++) {
    case 0:
      ESP_LOGI(TAG, "BEGIN v=1 uptime_ms=%u state=%s call_hash=%08x transition=%s failed=NO",
               (unsigned) dump.requested_ms, call_state_to_str(dump.state), (unsigned) dump.call_hash, YESNO(dump.changed));
      break;
    case 1:
      ESP_LOGI(TAG, "configured: transport=%s sip_port=%u rtp_port=%u capability=%s registration=%s",
               this->configured_sip_transport_name(), this->sip_port_, this->rtp_port_, this->audio_capability_(),
#ifdef USE_ESPHOME_VOIP_REGISTRATION
               "enabled"
#else
               "disabled"
#endif
      );
      break;
    case 2: format("local TX", dump.tx); format("local RX", dump.rx); break;
    case 3:
      if (dump.has_transport) {
        ESP_LOGI(TAG, "sip: running=%s peer_transport=%s invite_pending=%s terminal_pending=%s status=%u event=%s",
                 YESNO(transport.running), transport.sip_tcp ? "tcp" : "udp", YESNO(transport.pending_invite),
                 YESNO(transport.terminal_transaction_pending), transport.last_sip_status_code,
                 transport.last_sip_event != nullptr ? transport.last_sip_event : "");
      } else { ESP_LOGI(TAG, "transport: not available"); }
      break;
    case 4:
      ESP_LOGI(TAG, "rtp: running=%s remote_sip_port=%u remote_rtp_port=%u tx_packets=%u rx_packets=%u tx_bytes=%u rx_bytes=%u udp_datagrams=%u",
               YESNO(transport.rtp_running), transport.remote_sip_port, transport.remote_rtp_port,
               (unsigned) transport.rtp_tx_packets, (unsigned) transport.rtp_rx_packets,
               (unsigned) transport.rtp_tx_bytes, (unsigned) transport.rtp_rx_bytes,
               (unsigned) transport.rtp_rx_datagrams);
      break;
    case 5:
      if (!dump.changed && transport.call_active) {
        format("negotiated TX", transport.selected_tx_format);
        format("negotiated RX", transport.selected_rx_format);
      } else { ESP_LOGI(TAG, "negotiated formats: not available (inactive or changing call)"); }
      break;
    case 6:
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
      ESP_LOGI(TAG, "video: active=%s send=%s pending=%s tx_packets=%u rx_packets=%u tx_frames=%u rx_frames=%u",
               YESNO(transport.video_running), YESNO(transport.video_send_enabled), YESNO(transport.video_send_change_pending),
               (unsigned) transport.video_tx_packets, (unsigned) transport.video_rx_packets,
               (unsigned) transport.video_tx_access_units, (unsigned) transport.video_rx_access_units);
#endif
      break;
    case 7:
      ESP_LOGI(TAG, "queues: tx_depth=%u tx_drops=%u rx_depth=%u rx_drops=%u", (unsigned) dump.tx_depth,
               (unsigned) dump.tx_drops, (unsigned) dump.rx_depth, (unsigned) dump.rx_drops);
      break;
    case 8:
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
      if (dump.has_jitter) {
        ESP_LOGI(TAG, "rx_jitter: depth=%u capacity=%u missing=%u late=%u duplicates=%u drops=%u discarded_frames=%u",
                 (unsigned) dump.jitter.depth, (unsigned) kRxQueuedFrames, (unsigned) dump.jitter.missing,
                 (unsigned) dump.jitter.late, (unsigned) dump.jitter.duplicates, (unsigned) dump.jitter.drops,
                 (unsigned) dump.jitter.discarded_frames);
        ESP_LOGI(TAG, "rx_storage: frame_bytes=%u exchange_aligned=%s", (unsigned) this->rx_jitter_frame_alloc_bytes_,
                 YESNO(this->rx_jitter_buffer_ != nullptr &&
                       this->rx_jitter_buffer_->has_exchange_alignment(this->rx_network_chunk_)));
      }
#endif
      break;
    case 9: {
      ESP_LOGI(TAG, "audio: active=%s buffers_psram=%s task_stacks_psram=%s audio_stacks_psram=%s route=%s rx_volume=%.3f tx_gain=%.3f",
               YESNO(dump.audio_active), YESNO(this->buffers_in_psram_), YESNO(this->task_stacks_in_psram_),
               YESNO(this->audio_task_stacks_in_psram_), dump.route, dump.receive_volume, dump.transmit_gain);
      unsigned rx_bytes = 0, rx_free = 0, tx_bytes = 0, tx_free = 0;
      bool rx_present = false, tx_present = false;
      // ESP-IDF reports bytes, unlike upstream FreeRTOS stack words. Scan only
      // on this requested dump, never in an audio worker or per-packet path.
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
      rx_bytes = this->rx_task_stack_bytes_;
      rx_present = this->rx_task_handle_ != nullptr;
      if (rx_present) rx_free = uxTaskGetStackHighWaterMark(this->rx_task_handle_);
#endif
#ifdef USE_ESPHOME_VOIP_STACK_MIC
      tx_bytes = this->tx_task_stack_bytes_;
      tx_present = this->tx_task_handle_ != nullptr;
      if (tx_present) tx_free = uxTaskGetStackHighWaterMark(this->tx_task_handle_);
#endif
      ESP_LOGI(TAG, "audio_stacks: rx_present=%s rx_stack_bytes=%u rx_min_free_bytes=%u tx_present=%s tx_stack_bytes=%u tx_min_free_bytes=%u",
               YESNO(rx_present), rx_bytes, rx_free, YESNO(tx_present), tx_bytes, tx_free);
      break;
    }
    case 10:
      ESP_LOGI(TAG, "last_termination=%s", dump.last_reason);
#if defined(USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG) && defined(USE_ESPHOME_VOIP_STACK_MIC)
      if (this->audio_debug_ && this->audio_debug_stats_quality_.load(std::memory_order_relaxed) != 0)
        ESP_LOGI(AUDIO_TAG, "AudioDebugQuality flags=%u quality_scope=since_boot",
                 (unsigned) this->audio_debug_stats_quality_.load(std::memory_order_relaxed));
#endif
      break;
#if defined(USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG) && defined(USE_ESPHOME_VOIP_STACK_SPEAKER)
    case 11: {
      if (!this->audio_debug_ || this->call_state_.load(std::memory_order_acquire) != CallState::IDLE ||
          this->audio_devices_active_.load(std::memory_order_acquire)) { dump.step = 21; break; }
      bool present = false;
      bool retry = false;
      uint32_t latest = 0;
      for (uint8_t i = 0; i < 8; ++i) {
        uint32_t revision;
        RxStartupData data;
        if (!this->debug_rx_startup_snapshot_(static_cast<RxStartupEvent>(i), revision, data)) {
          retry = true; break;
        }
        if ((data.flags & 2U) && (!present || static_cast<int32_t>(revision - latest) > 0)) {
          latest = revision; present = true;
        }
      }
      if (retry) { --dump.step; break; }  // Keep the existing deferred owner pending.
      dump.startup_revision = latest;
      dump.startup_present = present;
      break;
    }
    case 12: case 13: case 14: case 15: case 16: case 17: case 18: case 19: {
      if (!dump.startup_present) break;
      if (this->call_state_.load(std::memory_order_acquire) != CallState::IDLE ||
          this->audio_devices_active_.load(std::memory_order_acquire)) {
        this->audio_debug_rx_startup_quality_.fetch_or(4U, std::memory_order_relaxed);
        dump.step = 21; break;
      }
      const uint8_t index = dump.step - 13;
      uint32_t revision;
      RxStartupData data;
      if (!this->debug_rx_startup_snapshot_(static_cast<RxStartupEvent>(index), revision, data)) {
        --dump.step; break;
      }
      if (!(data.flags & 2U)) break;
      if (revision != dump.startup_revision) {
        if (static_cast<int32_t>(revision - dump.startup_revision) > 0)
          this->audio_debug_rx_startup_quality_.fetch_or(4U, std::memory_order_relaxed);
        break;
      }
      static const char *const names[]{"speaker_start_request", "in_call_gate", "rx_enqueue", "play_attempt",
          "pcm_accepted", "first_zero_write", "resumed", "first_jitter_drop"};
      if (data.flags & 4U) {
        ESP_LOGI(AUDIO_TAG, "AudioStartup rev=%u event=%s us=%llu sink_running=%u state=%u output_us=%u jitter_depth=%u drops=%u discarded=%u",
            (unsigned) revision, names[index], (unsigned long long) data.us, data.flags & 1U, (unsigned) data.state,
            (unsigned) data.output_us, (unsigned) data.depth, (unsigned) data.drops, (unsigned) data.discarded);
      } else {
        ESP_LOGI(AUDIO_TAG, "AudioStartup rev=%u event=%s us=%llu sink_running=%u state=%u output_us=%u jitter_snapshot=unavailable",
            (unsigned) revision, names[index], (unsigned long long) data.us, data.flags & 1U, (unsigned) data.state,
            (unsigned) data.output_us);
      }
      break;
    }
    case 20: {
      bool retry = false;
      for (uint8_t i = 0; i < 8; ++i) {
        uint32_t revision;
        RxStartupData data;
        if (!this->debug_rx_startup_snapshot_(static_cast<RxStartupEvent>(i), revision, data)) {
          retry = true; break;
        }
        if (dump.startup_present && (data.flags & 2U) &&
            static_cast<int32_t>(revision - dump.startup_revision) > 0)
          this->audio_debug_rx_startup_quality_.fetch_or(4U, std::memory_order_relaxed);
      }
      if (retry) { --dump.step; break; }
      {
        const auto quality = this->audio_debug_rx_startup_quality_.load(std::memory_order_relaxed);
        if (dump.startup_present || quality != 0)
          ESP_LOGI(AUDIO_TAG, "AudioStartupTrace rev=%u present=%s complete=%s quality=%u deferred_read=%s quality_scope=since_boot",
              (unsigned) dump.startup_revision, YESNO(dump.startup_present),
              YESNO(dump.startup_present && (quality & 5U) == 0), (unsigned) quality, YESNO(quality & 2U));
      }
      break;
    }
#endif
    default:
      ESP_LOGI(TAG, "END v=1 uptime_ms=%u untracked_metrics=not_available", (unsigned) dump.requested_ms);
      this->diagnostic_dump_.reset();
      return;
  }
  this->defer("voip-diagnostics", [this]() { this->emit_diagnostic_section_(); });
}
}  // namespace esphome::voip_stack
#endif
