#include "voip_stack.h"

#ifdef USE_ESP32
#include <cstdio>
#include <new>

namespace esphome::voip_stack {
static const char *const TAG = "voip_stack.diagnostics";

void VoipStack::dump_diagnostics() {
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
      ESP_LOGI(TAG, "configured: transport=%s sip_port=%u rtp_port=%u capability=%s registration=unsupported",
               this->configured_sip_transport_name(), this->sip_port_, this->rtp_port_, this->audio_capability_());
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
      ESP_LOGI(TAG, "rtp: running=%s remote_sip_port=%u remote_rtp_port=%u tx_packets=%u rx_packets=%u tx_bytes=%u rx_bytes=%u",
               YESNO(transport.rtp_running), transport.remote_sip_port, transport.remote_rtp_port,
               (unsigned) transport.rtp_tx_packets, (unsigned) transport.rtp_rx_packets,
               (unsigned) transport.rtp_tx_bytes, (unsigned) transport.rtp_rx_bytes);
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
        ESP_LOGI(TAG, "rx_jitter: depth=%u capacity=%u missing=%u late=%u duplicates=%u drops=%u",
                 (unsigned) dump.jitter.depth, (unsigned) kRxQueuedFrames, (unsigned) dump.jitter.missing,
                 (unsigned) dump.jitter.late, (unsigned) dump.jitter.duplicates, (unsigned) dump.jitter.drops);
      }
#endif
      break;
    case 9:
      ESP_LOGI(TAG, "audio: active=%s buffers_psram=%s task_stacks_psram=%s audio_stacks_psram=%s route=%s",
               YESNO(dump.audio_active), YESNO(this->buffers_in_psram_), YESNO(this->task_stacks_in_psram_),
               YESNO(this->audio_task_stacks_in_psram_), dump.route);
      break;
    case 10: ESP_LOGI(TAG, "last_termination=%s", dump.last_reason); break;
    default:
      ESP_LOGI(TAG, "END v=1 uptime_ms=%u untracked_metrics=not_available", (unsigned) dump.requested_ms);
      this->diagnostic_dump_.reset();
      return;
  }
  this->defer("voip-diagnostics", [this]() { this->emit_diagnostic_section_(); });
}
}  // namespace esphome::voip_stack
#endif
