#include "voip_stack.h"

#ifdef USE_ESP32

#include <algorithm>
#include <cstring>

#include "esphome/core/application.h"
#include "esphome/components/network/util.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "audio_core_ring_buffer_caps.h"
#include "audio_core_task_utils.h"
#ifdef USE_ESPHOME_VOIP_SIP_TRANSPORT
#include "sip_transport.h"
#endif

#include "esp_event.h"
#include "esp_netif.h"

namespace esphome {
namespace voip_stack {

static const char *const TAG = "voip_stack";

void VoipStack::append_audio_format_(AudioFormatList *list, const AudioFormat &format) {
  if (list == nullptr || !format.is_valid()) return;
  for (uint8_t i = 0; i < list->count; i++) {
    if (list->formats[i] == format) return;
  }
  if (list->count >= VOIP_STACK_MAX_AUDIO_FORMATS) {
    ESP_LOGW(TAG, "Ignoring extra VoIP audio format: max supported format count is %u",
             (unsigned) VOIP_STACK_MAX_AUDIO_FORMATS);
    return;
  }
  list->formats[list->count++] = format;
}

bool VoipStack::ensure_mic_processing_buffer_() {
#ifdef USE_ESPHOME_VOIP_STACK_MIC
  if (this->tx_audio_format_.pcm_format != PcmFormat::S16LE) {
    ESP_LOGE(TAG, "mic_gain and dc_offset_removal require voip_stack.audio.tx.pcm_format: s16le");
    return false;
  }
  if (this->mic_converted_.load(std::memory_order_acquire) != nullptr)
    return true;

  RAMAllocator<int16_t> alloc = this->buffers_in_psram_
      ? RAMAllocator<int16_t>()
      : RAMAllocator<int16_t>(RAMAllocator<int16_t>::ALLOC_INTERNAL);
  int16_t *buf = alloc.allocate(this->mic_processing_samples_());
  if (buf == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate mic processing buffer");
    return false;
  }
  int16_t *expected = nullptr;
  if (!this->mic_converted_.compare_exchange_strong(
          expected, buf, std::memory_order_release, std::memory_order_acquire)) {
    alloc.deallocate(buf, this->mic_processing_samples_());
  }
  return true;
#else
  ESP_LOGW(TAG, "Ignoring mic processing request: this voip endpoint has no microphone");
  return false;
#endif
}

void VoipStack::cleanup_partial_setup_() {
  // Transactional setup cleanup. force_delete is safe here only because tasks
  // were just spawned and have not entered a blocking upstream call yet.
#ifdef USE_ESPHOME_VOIP_STACK_MIC
  audio_core::force_delete_pinned_task(&this->tx_task_handle_, &this->tx_task_stack_,
                                             VoipStack::kTxTaskStackBytes);

  RAMAllocator<int16_t> i16_alloc;
  if (int16_t *mic_converted = this->mic_converted_.exchange(nullptr, std::memory_order_acq_rel)) {
    i16_alloc.deallocate(mic_converted, this->mic_processing_samples_());
  }

  RAMAllocator<uint8_t> u8_alloc;
  if (this->tx_audio_chunk_ != nullptr) {
    u8_alloc.deallocate(this->tx_audio_chunk_, this->tx_audio_chunk_alloc_bytes_);
    this->tx_audio_chunk_ = nullptr;
  }
  this->tx_audio_chunk_alloc_bytes_ = 0;

  this->mic_buffer_.reset();
#endif
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
  audio_core::force_delete_pinned_task(&this->rx_task_handle_, &this->rx_task_stack_,
                                             VoipStack::kRxTaskStackBytes);
  this->rx_jitter_buffer_.reset();
  RAMAllocator<uint8_t> rx_u8_alloc;
  if (this->rx_audio_chunk_ != nullptr) {
    rx_u8_alloc.deallocate(this->rx_audio_chunk_, this->rx_audio_chunk_alloc_bytes_);
    this->rx_audio_chunk_ = nullptr;
  }
  if (this->rx_jitter_pcm_storage_ != nullptr) {
    rx_u8_alloc.deallocate(this->rx_jitter_pcm_storage_,
                           this->rx_audio_chunk_alloc_bytes_ * VoipStack::kRxQueuedFrames);
    this->rx_jitter_pcm_storage_ = nullptr;
  }
  if (this->rx_silence_chunk_ != nullptr) {
    rx_u8_alloc.deallocate(this->rx_silence_chunk_, this->rx_audio_chunk_alloc_bytes_);
    this->rx_silence_chunk_ = nullptr;
  }
  this->rx_audio_chunk_alloc_bytes_ = 0;
#endif
  this->transport_.reset();
}

bool VoipStack::allocate_setup_buffers_() {
#ifdef USE_ESPHOME_VOIP_STACK_MIC
  if (this->has_microphone_()) {
    const size_t tx_frame_bytes = this->tx_audio_chunk_bytes_();
    const size_t tx_buffer_bytes = std::max<size_t>(tx_frame_bytes * 16, tx_frame_bytes + 4096);
    this->mic_buffer_ = this->buffers_in_psram_
        ? audio_core::create_prefer_psram(tx_buffer_bytes, "voip.mic")
        : audio_core::create_internal(tx_buffer_bytes, "voip.mic");
    if (!this->mic_buffer_) {
      ESP_LOGE(TAG, "Failed to allocate mic ring buffer");
      return false;
    }
  }

  if (this->has_microphone_() && this->dc_offset_removal_) {
    if (this->tx_audio_format_.pcm_format != PcmFormat::S16LE) {
      ESP_LOGE(TAG, "dc_offset_removal requires voip_stack.audio.tx.pcm_format: s16le");
      return false;
    }
    if (!this->ensure_mic_processing_buffer_()) {
      return false;
    }
  }

  // Per-iteration drain buffers; same placement policy as above.
  RAMAllocator<uint8_t> psram_u8 = this->buffers_in_psram_
      ? RAMAllocator<uint8_t>()
      : RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_INTERNAL);
  if (this->has_microphone_()) {
    this->tx_audio_chunk_alloc_bytes_ = this->tx_audio_max_chunk_bytes_();
    this->tx_audio_chunk_ = psram_u8.allocate(this->tx_audio_chunk_alloc_bytes_);
    if (!this->tx_audio_chunk_) {
      ESP_LOGE(TAG, "Failed to allocate tx audio chunk buffer");
      this->tx_audio_chunk_alloc_bytes_ = 0;
      return false;
    }
  }
#endif

#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
  if (this->has_speaker_()) {
    size_t rx_frame_bytes = this->rx_audio_format_.nominal_frame_bytes();
    for (uint8_t i = 0; i < this->rx_audio_formats_.count; i++) {
      rx_frame_bytes = std::max(rx_frame_bytes, this->rx_audio_formats_.formats[i].nominal_frame_bytes());
    }
    this->rx_audio_chunk_alloc_bytes_ = rx_frame_bytes;
    RAMAllocator<uint8_t> psram_u8 = this->buffers_in_psram_
        ? RAMAllocator<uint8_t>()
        : RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_INTERNAL);
    this->rx_audio_chunk_ = psram_u8.allocate(this->rx_audio_chunk_alloc_bytes_);
    this->rx_jitter_pcm_storage_ =
        psram_u8.allocate(this->rx_audio_chunk_alloc_bytes_ * VoipStack::kRxQueuedFrames);
    this->rx_silence_chunk_ = psram_u8.allocate(this->rx_audio_chunk_alloc_bytes_);
    if (!this->rx_audio_chunk_ || !this->rx_jitter_pcm_storage_ || !this->rx_silence_chunk_) {
      ESP_LOGE(TAG, "Failed to allocate RX playout buffers");
      return false;
    }
    this->rx_jitter_buffer_ = std::make_unique<RtpJitterBuffer>(
        this->rx_jitter_pcm_storage_, this->rx_audio_chunk_alloc_bytes_,
        static_cast<uint8_t>(VoipStack::kRxQueuedFrames),
        static_cast<uint8_t>(VoipStack::kRxPrebufferFrames));
    memset(this->rx_silence_chunk_, 0, this->rx_audio_chunk_alloc_bytes_);
  }
#endif

  return true;
}

bool VoipStack::setup_audio_helpers_() {
#ifdef USE_ESPHOME_VOIP_STACK_MIC
  if (this->microphone_ != nullptr) {
    this->microphone_->add_data_callback([this](const std::vector<uint8_t> &data) {
      this->on_microphone_data_(data.data(), data.size());
    });
  }
  if (this->microphone_source_ != nullptr) {
    this->microphone_source_->add_data_callback([this](const std::vector<uint8_t> &data) {
      this->on_microphone_data_(data.data(), data.size());
    });
  }
#endif
  return true;
}

bool VoipStack::setup_transport_() {
#ifdef USE_ESPHOME_VOIP_SIP_TRANSPORT
  this->transport_ = std::make_unique<SipTransport>(
      this->sip_port_, this->rtp_port_, this->udp_max_payload_, "",
      this->task_stacks_in_psram_);
  this->transport_->set_sip_signaling_transport(this->protocol_ == TransportType::TCP);
#else
  ESP_LOGE(TAG, "SIP transport was not compiled into this firmware");
  return false;
#endif
  if (!this->transport_) {
    ESP_LOGE(TAG, "Failed to allocate transport");
    return false;
  }

  this->transport_->set_audio_formats(this->tx_audio_formats_, this->rx_audio_formats_);

  // Wire callbacks before start() so the transport task never fires into null.
  this->transport_->set_audio_callback(VoipStack::transport_audio_callback_, this);
  this->transport_->set_sip_signal_callback(VoipStack::transport_sip_signal_callback_, this);
  this->transport_->set_connection_callback(VoipStack::transport_connection_callback_, this);
  this->transport_->set_accept_callback(VoipStack::transport_accept_callback_, this);
  this->transport_->set_dialog_active_callback(VoipStack::transport_dialog_active_callback_, this);

  if (!this->transport_->start()) {
    ESP_LOGE(TAG, "Transport failed to start");
    return false;
  }
  return true;
}

void VoipStack::transport_audio_callback_(void *ctx, const TransportAudioFrame &frame) {
  static_cast<VoipStack *>(ctx)->on_audio_received_(frame);
}

void VoipStack::transport_sip_signal_callback_(void *ctx, const SipSignal &signal) {
  static_cast<VoipStack *>(ctx)->on_sip_signal_received_(signal);
}

void VoipStack::transport_connection_callback_(void *ctx, bool connected) {
  static_cast<VoipStack *>(ctx)->on_connection_change_(connected);
}

bool VoipStack::transport_accept_callback_(void *ctx) {
  return static_cast<VoipStack *>(ctx)->can_accept_session_();
}

bool VoipStack::transport_dialog_active_callback_(void *ctx) {
  return static_cast<VoipStack *>(ctx)->is_active();
}

bool VoipStack::start_runtime_tasks_() {
#ifdef USE_ESPHOME_VOIP_STACK_MIC
  // TX task exists only when a microphone is configured. Speaker-only peers
  // still accept calls and play incoming audio through the transport recv task.
  if (this->has_microphone_()) {
    if (!audio_core::start_pinned_task(VoipStack::tx_task, "voip_tx",
                                             VoipStack::kTxTaskStackBytes, this, VoipStack::kMediaTaskPriority, 0,
                                             this->task_stacks_in_psram_, TAG,
                                             &this->tx_task_handle_, &this->tx_task_tcb_,
                                             &this->tx_task_stack_)) {
      return false;
    }
  }
#endif
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
  if (this->has_speaker_()) {
    if (!audio_core::start_pinned_task(VoipStack::rx_task, "voip_rx",
                                             VoipStack::kRxTaskStackBytes, this, VoipStack::kMediaTaskPriority, 0,
                                             this->task_stacks_in_psram_, TAG,
                                             &this->rx_task_handle_, &this->rx_task_tcb_,
                                             &this->rx_task_stack_)) {
      return false;
    }
  }
#endif
  return true;
}

void VoipStack::publish_initial_state_later_() {
  // Deferred so sensors are fully wired before the first publish.
  this->set_timeout(SCHED_PUBLISH_INITIAL_STATE, 250, [this]() {
    this->publish_state_();
    this->publish_destination_();
    this->publish_transport_();
    this->publish_endpoint_();
    this->publish_sip_snapshot_();
  });
}

void VoipStack::fail_setup_() {
  this->cleanup_partial_setup_();
  this->mark_failed();
}

void VoipStack::setup() {
  ESP_LOGI(TAG, "Setting up VoIP Stack...");

  ESP_LOGI(TAG, "Audio capability: %s (SIP/%s, tasks: %s)",
           this->audio_capability_(),
           this->protocol_ == TransportType::TCP ? "TCP" : "UDP",
           this->has_microphone_() ? "tx+rx/control" : "rx/control");

  if (!this->allocate_setup_buffers_()) {
    this->fail_setup_();
    return;
  }
  if (!this->setup_audio_helpers_()) {
    this->fail_setup_();
    return;
  }
  if (!this->setup_transport_()) {
    this->fail_setup_();
    return;
  }
  if (!this->start_runtime_tasks_()) {
    this->fail_setup_();
    return;
  }

  this->load_settings_();
  esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                      &VoipStack::ip_event_handler_,
                                      this, nullptr);
  this->publish_initial_state_later_();

  ESP_LOGI(TAG, "VoIP Stack ready as SIP phone on %s/%u RTP UDP/%u",
           this->protocol_ == TransportType::TCP ? "TCP" : "UDP",
           (unsigned) this->sip_port_, (unsigned) this->rtp_port_);
}

void VoipStack::handle_call_timeouts_(uint32_t now_ms, uint32_t calling_timeout_ms) {
  const CallState state = this->call_state_.load(std::memory_order_acquire);
  if (this->ringing_timeout_ms_ > 0 && state == CallState::RINGING &&
      now_ms - this->ringing_start_time_ >= this->ringing_timeout_ms_) {
    const std::string cid = this->get_current_call_id_();
    ESP_LOGI(TAG, "Ringing timeout after %u ms - declining caller (call_id=%s)",
             this->ringing_timeout_ms_, cid.c_str());
    this->fire_timeout_decline_();
    return;
  }

  if (state == CallState::CALLING &&
      now_ms - this->calling_start_time_ >= INVITE_NO_RESPONSE_TIMEOUT_MS) {
    bool saw_sip_response = false;
    if (this->transport_ != nullptr) {
      saw_sip_response = this->transport_->snapshot().last_sip_status_code != 0;
    }
    if (!saw_sip_response) {
      const std::string cid = this->get_current_call_id_();
      ESP_LOGI(TAG, "SIP INVITE timeout after %u ms without response - ending call (call_id=%s)",
               (unsigned) INVITE_NO_RESPONSE_TIMEOUT_MS, cid.c_str());
      this->fire_timeout_decline_();
      return;
    }
  }

  if (calling_timeout_ms > 0 && (state == CallState::CALLING || state == CallState::REMOTE_RINGING) &&
      now_ms - this->calling_start_time_ >= calling_timeout_ms) {
    const std::string cid = this->get_current_call_id_();
    ESP_LOGI(TAG, "Calling timeout after %u ms - sending CANCEL (call_id=%s)",
             calling_timeout_ms, cid.c_str());
    this->fire_timeout_decline_();
    return;
  }

  if (state == CallState::IN_CALL && this->transport_ != nullptr) {
    const uint32_t rx_packets = this->transport_->snapshot().rtp_rx_packets;
    const uint32_t last_seen = this->media_timeout_rtp_rx_packets_.load(std::memory_order_acquire);
    if (rx_packets != last_seen) {
      this->media_timeout_rtp_rx_packets_.store(rx_packets, std::memory_order_release);
      this->last_peer_audio_ms_.store(now_ms, std::memory_order_release);
      if (rx_packets != 0) {
        this->first_audio_received_.store(true, std::memory_order_release);
      }
    }
    const uint32_t last_audio = this->last_peer_audio_ms_.load(std::memory_order_acquire);
    const uint32_t elapsed_since_audio = now_ms - last_audio;
    if (last_audio != 0 && elapsed_since_audio < 0x80000000UL && elapsed_since_audio >= MEDIA_TIMEOUT_MS) {
      const std::string cid = this->get_current_call_id_();
      ESP_LOGW(TAG, "Media timeout after %u ms without RTP - ending call (call_id=%s)",
               (unsigned) MEDIA_TIMEOUT_MS, cid.c_str());
      this->set_terminal_response_(cid, kReasonMediaTimeout);
      this->end_call_(CallEndReason::MEDIA_TIMEOUT, kReasonMediaTimeout);
      bool waiting_for_bye_response = false;
      if (this->transport_ && this->transport_->is_connected() && !cid.empty()) {
        waiting_for_bye_response = this->send_sip_bye_(cid);
      }
      this->set_in_call_(false);
      this->set_audio_devices_active_(false);
      if (this->transport_ && !waiting_for_bye_response) this->transport_->disconnect();
    }
  }
}

void VoipStack::loop() {
  uint32_t now = millis();
  if (this->endpoint_publish_requested_.exchange(false, std::memory_order_acq_rel)) {
    this->publish_endpoint_();
  }

  // Phonebook cycle timeout safeguard: a stuck on_phonebook_update chain (e.g.
  // an external update source never completes) would otherwise leave the cycle open forever
  // and block subsequent counter advances. CYCLE_TIMEOUT_MS commits forcibly.
  if (this->cycle_active_ &&
      (millis() - this->cycle_started_at_) > CYCLE_TIMEOUT_MS) {
    ESP_LOGD("voip_stack", "Phonebook update cycle auto-commit after %u ms",
             (unsigned) CYCLE_TIMEOUT_MS);
    this->commit_cycle_();
  }

  // Auto-decline timeouts (0 = disabled). CALLING falls back to
  // ringing_timeout when calling_timeout is unset.
  const uint32_t calling_to = this->calling_timeout_ms_ > 0
                                ? this->calling_timeout_ms_
                                : this->ringing_timeout_ms_;

  this->handle_call_timeouts_(now, calling_to);
  const CallState state = this->call_state_.load(std::memory_order_acquire);
  const uint32_t snapshot_refresh_ms = this->audio_debug_ ? 500 : 2000;
  if (state != CallState::IDLE && now - this->last_sip_snapshot_refresh_ms_ >= snapshot_refresh_ms) {
    this->last_sip_snapshot_refresh_ms_ = now;
    this->publish_sip_snapshot_();
  }
  bool keep_loop = this->cycle_active_ || state != CallState::IDLE;
  keep_loop = keep_loop || this->endpoint_publish_requested_.load(std::memory_order_acquire);
  if (!keep_loop) {
    this->disable_loop();
  }
}

void VoipStack::fire_timeout_decline_() {
  // Timeout sends CANCEL for pending outbound INVITE or a SIP final response for inbound ringing.
  const std::string call_id = this->get_current_call_id_();
  if (this->transport_ && this->transport_->is_connected() && !call_id.empty()) {
    this->send_sip_final_response_(call_id, kReasonTimeout);
  }
  this->set_terminal_response_(call_id, kReasonTimeout);
  this->set_audio_devices_active_(false);
  this->end_call_(CallEndReason::TIMEOUT, kReasonTimeout);
  if (this->transport_) this->transport_->disconnect();
}

void VoipStack::dump_config() {
  ESP_LOGCONFIG(TAG, "VoIP Stack:");
  if (this->transport_) {
    ESP_LOGCONFIG(TAG, "  Transport: %s", this->transport_->transport_name());
  } else {
    ESP_LOGCONFIG(TAG, "  Transport: (not initialised)");
  }
  ESP_LOGCONFIG(TAG, "  SIP listen port: %u", (unsigned) this->sip_port_);
  ESP_LOGCONFIG(TAG, "  RTP port: %u", (unsigned) this->rtp_port_);
  ESP_LOGCONFIG(TAG, "  Routing: SIP dial plan");
  ESP_LOGCONFIG(TAG, "  HA peer name: %s", this->ha_peer_name_.c_str());
  ESP_LOGCONFIG(TAG, "  Audio capability: %s", this->audio_capability_());
  ESP_LOGCONFIG(TAG, "  HA as first contact: %s", YESNO(this->use_ha_as_first_contact_));
  ESP_LOGCONFIG(TAG, "  Phonebook source: HA SIP phonebook");
#ifdef USE_ESPHOME_VOIP_STACK_MIC
  ESP_LOGCONFIG(TAG, "  Microphone: %s", this->microphone_ ? "direct" : (this->microphone_source_ ? "source" : "none"));
#endif
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
  ESP_LOGCONFIG(TAG, "  Speaker: %s", this->speaker_ ? "configured" : "none");
#endif
  ESP_LOGCONFIG(TAG, "  Tasks: %s", this->has_microphone_() ? "tx+rx/control" : "rx/control only");
  ESP_LOGCONFIG(TAG, "  Device Name: %s",
                this->device_name_.empty() ? "(unset)" : this->device_name_.c_str());
  if (this->ringing_timeout_ms_ > 0) {
    ESP_LOGCONFIG(TAG, "  Ringing Timeout: %u ms", this->ringing_timeout_ms_);
  } else {
    ESP_LOGCONFIG(TAG, "  Ringing Timeout: disabled");
  }
  if (this->calling_timeout_ms_ > 0) {
    ESP_LOGCONFIG(TAG, "  Calling Timeout: %u ms", this->calling_timeout_ms_);
  } else {
    ESP_LOGCONFIG(TAG, "  Calling Timeout: disabled");
  }
  ESP_LOGCONFIG(TAG, "  Contacts: %zu configured", this->phonebook_.size());
}

void VoipStack::set_remote_endpoint(const std::string &ip, uint16_t port, uint16_t rtp_port) {
  if (this->transport_ != nullptr) {
    this->transport_->set_remote(ip, port, rtp_port);
  }
  ESP_LOGI(TAG, "Remote endpoint updated to SIP %s:%u RTP %u", ip.c_str(), (unsigned) port,
           (unsigned) (rtp_port != 0 ? rtp_port : this->rtp_port_));
}

void VoipStack::set_remote_sip_transport_tcp(bool tcp) {
  if (this->transport_ != nullptr) {
    this->transport_->set_sip_signaling_transport(tcp);
  }
  ESP_LOGI(TAG, "Remote SIP signaling transport set to %s", tcp ? "TCP" : "UDP");
}

void VoipStack::publish_transport_() {
  if (this->transport_sensor_ != nullptr) {
    this->transport_sensor_->publish_state(this->protocol_ == TransportType::TCP ? "tcp" : "udp");
  }
}

std::string VoipStack::audio_format_token_(const AudioFormat &fmt) {
  const char *pcm = "s16le";
  switch (fmt.pcm_format) {
    case PcmFormat::S16LE:
      pcm = "s16le";
      break;
    case PcmFormat::S24LE:
      pcm = "s24le";
      break;
    case PcmFormat::S24LE_IN_S32:
      pcm = "s24le_in_s32";
      break;
    case PcmFormat::S32LE:
      pcm = "s32le";
      break;
    default:
      pcm = "s16le";
      break;
  }
  char token[48];
  snprintf(token, sizeof(token), "%u:%s:%u:%u",
           (unsigned) fmt.sample_rate, pcm,
           (unsigned) fmt.channels, (unsigned) fmt.frame_ms);
  return token;
}

std::string VoipStack::local_ip_string_() const {
  char ip[network::IP_ADDRESS_BUFFER_SIZE];
  for (auto &address : network::get_ip_addresses()) {
    if (!address.is_ip4()) continue;
    address.str_to(ip);
    if (strcmp(ip, "0.0.0.0") != 0) {
      return ip;
    }
  }
  return "";
}

std::string VoipStack::build_endpoint_string_() const {
  const std::string name = !this->device_name_.empty()
                               ? this->device_name_
                               : App.get_friendly_name().str();
  const std::string ip = this->local_ip_string_();
  if (name.empty() || ip.empty()) {
    return "";
  }

  auto format_list_token = [&](const AudioFormatList &list) -> std::string {
    std::string out;
    for (uint8_t i = 0; i < list.count; i++) {
      if (!out.empty()) out += "; ";
      out += VoipStack::audio_format_token_(list.formats[i]);
    }
    return out;
  };
  const std::string tx = format_list_token(this->tx_audio_formats_);
  const std::string rx = format_list_token(this->rx_audio_formats_);
  char buf[896];
  snprintf(buf, sizeof(buf), "%s | %s | %u | %u | %s | %s | %s | %s | %s", name.c_str(), ip.c_str(),
           (unsigned) this->sip_port_, (unsigned) this->rtp_port_,
           this->audio_capability_(), tx.c_str(), rx.c_str(),
           this->protocol_ == TransportType::TCP ? "sip_tcp" : "sip_udp",
           this->extension_.c_str());
  if (!this->conference_group_.empty() || !this->ring_group_.empty()) {
    std::string out = buf;
    out += " | ";
    out += this->conference_group_;
    out += " | ";
    out += this->ring_group_;
    return out;
  }
  return buf;
}

std::string VoipStack::build_sip_snapshot_string_() const {
  auto field_escape = [](const std::string &in, size_t max_len = 32) -> std::string {
    std::string out;
    for (char ch : in) {
      if (ch == '\r' || ch == '\n' || ch == ';' || ch == '|') {
        out.push_back(' ');
      } else {
        out.push_back(ch);
      }
      if (out.size() >= max_len) break;
    }
    return out;
  };
  auto compact_audio_format = [](const AudioFormat &fmt) -> std::string {
    char buf[18];
    const uint32_t khz = fmt.sample_rate / 1000;
    snprintf(buf, sizeof(buf), "%uk/%u", (unsigned) khz, (unsigned) fmt.frame_ms);
    return buf;
  };
  CallSnapshot call = this->snapshot_call_identity_();
  const std::string state = this->get_call_state_str();
  std::string direction;
  if (!call.caller_name.empty() && call.caller_name == this->device_name_) {
    direction = "outgoing";
  } else if (!call.dest_name.empty() && call.dest_name == this->device_name_) {
    direction = "incoming";
  } else if (this->call_state_.load(std::memory_order_acquire) == CallState::CALLING) {
    direction = "outgoing";
  } else if (this->call_state_.load(std::memory_order_acquire) == CallState::RINGING) {
    direction = "incoming";
  }
  if (call.call_id.empty() && !this->last_terminal_call_id_.empty() && !this->last_reason_.empty()) {
    call.call_id = this->last_terminal_call_id_;
    call.caller_name = this->last_terminal_caller_name_;
    call.dest_name = this->last_terminal_dest_name_;
    direction = this->last_terminal_direction_;
  }
  uint32_t rtp_tx_packets = 0;
  uint32_t rtp_rx_packets = 0;
  uint16_t sip_status = 0;
  const char *last_event = "";
  AudioFormat selected_tx_format = this->current_tx_audio_format_;
  AudioFormat selected_rx_format = this->current_rx_audio_format_;
  if (!this->last_terminal_call_id_.empty() && !this->last_reason_.empty() &&
      this->call_state_.load(std::memory_order_acquire) == CallState::IDLE) {
    selected_tx_format = this->last_terminal_tx_audio_format_;
    selected_rx_format = this->last_terminal_rx_audio_format_;
  }
  if (this->transport_ != nullptr) {
    const SipTransportSnapshot snap = this->transport_->snapshot();
    rtp_tx_packets = snap.rtp_tx_packets;
    rtp_rx_packets = snap.rtp_rx_packets;
    sip_status = snap.last_sip_status_code;
    last_event = snap.last_sip_event;
    if (this->call_state_.load(std::memory_order_acquire) != CallState::IDLE || this->last_reason_.empty()) {
      selected_tx_format = snap.selected_tx_format;
      selected_rx_format = snap.selected_rx_format;
    }
  }
  std::string contact = this->phonebook_.current_name();
  char out[512];
  snprintf(out, sizeof(out),
           "st=%s; id=%s; dir=%s; from=%s; to=%s; ct=%s; tr=%s; sc=%u; "
           "tx=%s; rx=%s; pt=%u; pr=%u; "
           "tqd=%u; tqdrop=%u; rqd=%u; rqdrop=%u; rs=%s; ev=%s",
           field_escape(state, 18).c_str(),
           field_escape(call.call_id, 12).c_str(),
           field_escape(direction, 3).c_str(),
           field_escape(call.caller_name, 14).c_str(),
           field_escape(call.dest_name, 14).c_str(),
           field_escape(contact, 14).c_str(),
           this->protocol_ == TransportType::TCP ? "tcp" : "udp",
           (unsigned) sip_status,
           compact_audio_format(selected_tx_format).c_str(),
           compact_audio_format(selected_rx_format).c_str(),
           (unsigned) rtp_tx_packets,
           (unsigned) rtp_rx_packets,
           (unsigned) this->media_tx_queue_depth_.load(std::memory_order_relaxed),
           (unsigned) this->media_tx_queue_drops_.load(std::memory_order_relaxed),
           (unsigned) this->media_rx_queue_depth_.load(std::memory_order_relaxed),
           (unsigned) this->media_rx_queue_drops_.load(std::memory_order_relaxed),
           field_escape(this->last_reason_, 22).c_str(),
           field_escape(last_event, 22).c_str());
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
  if (this->audio_debug_) {
    char debug[48];
    snprintf(debug, sizeof(debug), "; rsil=%u; spkshort=%u",
             (unsigned) this->audio_debug_rx_silence_frames_.load(std::memory_order_relaxed),
             (unsigned) this->audio_debug_speaker_short_writes_.load(std::memory_order_relaxed));
    return std::string(out) + debug;
  }
#endif
  return out;
}

void VoipStack::publish_sip_snapshot_() {
  if (this->sip_snapshot_sensor_ == nullptr) return;
  const std::string snapshot = this->build_sip_snapshot_string_();
  if (snapshot == this->last_sip_snapshot_) return;
  this->last_sip_snapshot_ = snapshot;
  this->sip_snapshot_sensor_->publish_state(snapshot);
}

void VoipStack::publish_endpoint_() {
  std::string endpoint = this->build_endpoint_string_();
  if (endpoint.empty()) {
    ESP_LOGW(TAG, "VoIP endpoint waiting for IPv4 address from ESPHome network");
    return;
  }
  if (this->endpoint_sensor_ != nullptr && endpoint != this->last_endpoint_) {
    this->last_endpoint_ = endpoint;
    this->endpoint_sensor_->publish_state(endpoint);
  }
  this->publish_sip_snapshot_();
}

void VoipStack::request_endpoint_publish_() {
  this->endpoint_publish_requested_.store(true, std::memory_order_release);
  this->enable_loop_soon_any_context();
}

void VoipStack::ip_event_handler_(void *arg, esp_event_base_t event_base,
                                    int32_t event_id, void *event_data) {
  if (event_base != IP_EVENT) return;
  if (event_id != IP_EVENT_STA_GOT_IP && event_id != IP_EVENT_ETH_GOT_IP) return;
  static_cast<VoipStack *>(arg)->request_endpoint_publish_();
}

// Wired from YAML via `api.on_client_connected:`.
void VoipStack::publish_entity_states() {
  // Re-publish on every HA reconnect so voip_stack sees it without
  // depending on HA restart timing. Restore-backed switches are applied only
  // once; a reconnect must not roll runtime state back to the boot preference.
  const bool apply_restore = !this->entity_restore_applied_;
  this->entity_restore_applied_ = true;

  this->publish_transport_();
  this->publish_endpoint_();
  this->publish_sip_snapshot_();
  if (this->last_reason_sensor_ != nullptr) {
    this->last_reason_sensor_->publish_state(this->last_reason_);
  }

  if (this->auto_answer_switch_ != nullptr) {
    if (apply_restore) {
      auto initial = this->auto_answer_switch_->get_initial_state_with_restore_mode();
      if (initial.has_value()) {
        this->auto_answer_ = *initial;
      }
    }
    this->auto_answer_switch_->publish_state(this->auto_answer_);
  }

  if (this->dnd_switch_ != nullptr) {
    if (apply_restore) {
      auto initial = this->dnd_switch_->get_initial_state_with_restore_mode();
      if (initial.has_value()) {
        this->do_not_disturb_ = *initial;
      }
    }
    this->dnd_switch_->publish_state(this->do_not_disturb_);
  }

  ESP_LOGD(TAG, "Entity states synced (vol=%.0f%%, mic=%.1fdB, auto=%s, dnd=%s)",
           this->volume_.load(std::memory_order_relaxed) * 100.0f, this->mic_gain_db_,
           this->auto_answer_ ? "ON" : "OFF", this->do_not_disturb_ ? "ON" : "OFF");

  if (this->volume_number_ != nullptr) {
    this->volume_number_->publish_state(this->volume_.load(std::memory_order_relaxed) * 100.0f);
  }
  if (this->mic_gain_number_ != nullptr) {
    this->mic_gain_number_->publish_state(this->mic_gain_db_);
  }
}

}  // namespace voip_stack
}  // namespace esphome

#endif  // USE_ESP32
