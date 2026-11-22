#include "sip_transport_internal.h"

#if defined(USE_ESP32) && defined(USE_ESPHOME_VOIP_SIP_TRANSPORT)

namespace esphome {
namespace voip_stack {

void *SipTransport::operator new(size_t size) noexcept {
  return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void SipTransport::operator delete(void *ptr) noexcept { heap_caps_free(ptr); }

SipTransport::SipTransport(uint16_t sip_port, uint16_t rtp_port, size_t udp_max_payload,
                           const std::string &remote_host,
                           bool task_stacks_in_psram)
    : sip_port_(sip_port), rtp_port_(rtp_port), udp_max_payload_(udp_max_payload),
      task_stacks_in_psram_(task_stacks_in_psram) {
  audio_format_list_default(&this->offer_tx_formats_);
  audio_format_list_default(&this->offer_rx_formats_);
  this->rtp_ssrc_ = esp_random();
  this->parse_remote_(remote_host);
}

SipTransport::~SipTransport() { this->stop(); }

const char *SipTransport::sip_event_name_(SipEvent event) {
  switch (event) {
    case SipEvent::INVITE: return "INVITE";
    case SipEvent::ACK: return "ACK";
    case SipEvent::CANCEL: return "CANCEL";
    case SipEvent::BYE: return "BYE";
    case SipEvent::OPTIONS: return "OPTIONS";
    case SipEvent::UPDATE: return "UPDATE";
    case SipEvent::RESPONSE: return "SIP_RESPONSE";
    case SipEvent::NONE:
    default: return "";
  }
}

SipTransport::SipEvent SipTransport::sip_event_from_method_(const std::string &method) {
  if (method == "INVITE") return SipEvent::INVITE;
  if (method == "ACK") return SipEvent::ACK;
  if (method == "CANCEL") return SipEvent::CANCEL;
  if (method == "BYE") return SipEvent::BYE;
  if (method == "OPTIONS") return SipEvent::OPTIONS;
  if (method == "UPDATE") return SipEvent::UPDATE;
  return SipEvent::NONE;
}

void SipTransport::mark_sip_event_(SipEvent event, uint16_t status) {
  this->last_sip_event_.store(static_cast<uint8_t>(event), std::memory_order_release);
  if (status != 0) {
    this->last_sip_status_code_.store(status, std::memory_order_release);
  }
}

SipTransportSnapshot SipTransport::snapshot() const {
  SipTransportSnapshot out;
  out.running = this->running_.load(std::memory_order_acquire);
  out.rtp_running = this->rtp_running_.load(std::memory_order_acquire);
  {
    LockGuard lock(this->dialog_mutex_);
    out.terminal_transaction_pending =
        this->terminal_transaction_pending_locked_();
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
    out.video_running =
        this->video_session_ != nullptr && this->video_session_->is_running() &&
        this->video_negotiated_ &&
        (this->video_send_enabled_ || this->video_receive_enabled_);
    out.video_send_enabled = this->video_send_enabled_;
    out.video_send_requested =
        this->video_send_requested_.load(std::memory_order_acquire);
    out.video_send_change_pending =
        this->pending_video_direction_invite_.pending();
    if (this->video_session_ != nullptr) {
      out.video_tx_packets = this->video_session_->tx_packets();
      out.video_rx_packets = this->video_session_->rx_packets();
      out.video_tx_access_units =
          this->video_session_->tx_access_units();
      out.video_rx_access_units =
          this->video_session_->rx_access_units();
    }
#endif
  }
  out.media_lifecycle_phase = static_cast<uint8_t>(
      this->media_lifecycle_phase_.load(std::memory_order_acquire));
  out.call_active = this->media_active_.load(std::memory_order_acquire);
  out.pending_invite = this->outgoing_invite_pending_.load(std::memory_order_acquire);
  out.sip_tcp = this->remote_sip_tcp_.load(std::memory_order_acquire);
  out.remote_ip_v4 = this->remote_ip_v4_.load(std::memory_order_acquire);
  out.remote_sip_port = this->remote_sip_port_.load(std::memory_order_acquire);
  out.remote_rtp_port = this->remote_rtp_port_.load(std::memory_order_acquire);
  this->get_media_config_(&out.selected_tx_format, &out.selected_rx_format, nullptr, nullptr);
  out.rtp_tx_packets = this->rtp_tx_packets_.load(std::memory_order_acquire);
  out.rtp_rx_packets = this->rtp_rx_packets_.load(std::memory_order_acquire);
  out.rtp_rx_datagrams = this->rtp_rx_datagrams_.load(std::memory_order_relaxed);
  out.rtp_tx_bytes = this->rtp_tx_bytes_.load(std::memory_order_acquire);
  out.rtp_rx_bytes = this->rtp_rx_bytes_.load(std::memory_order_acquire);
  out.last_sip_status_code = this->last_sip_status_code_.load(std::memory_order_acquire);
  out.last_sip_event = SipTransport::sip_event_name_(
      static_cast<SipEvent>(this->last_sip_event_.load(std::memory_order_acquire)));
  return out;
}

bool SipTransport::parse_remote_(const std::string &host) {
  if (host.empty()) {
    this->remote_ip_v4_.store(0, std::memory_order_release);
    this->remote_rtp_ip_v4_.store(0, std::memory_order_release);
    return false;
  }
  struct in_addr a{};
  if (inet_aton(host.c_str(), &a) == 0 || a.s_addr == 0) {
    this->remote_ip_v4_.store(0, std::memory_order_release);
    this->remote_rtp_ip_v4_.store(0, std::memory_order_release);
    return false;
  }
  const uint32_t ip_v4 = ntohl(a.s_addr);
  this->remote_ip_v4_.store(ip_v4, std::memory_order_release);
  this->remote_rtp_ip_v4_.store(ip_v4, std::memory_order_release);
  return true;
}

#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
void SipTransport::configure_startup_trace(bool enabled, bool prefer_psram) {
  if (!enabled || this->startup_trace_) return;
  RAMAllocator<StartupTrace> allocator = prefer_psram
                                             ? RAMAllocator<StartupTrace>()
                                             : RAMAllocator<StartupTrace>(RAMAllocator<StartupTrace>::ALLOC_INTERNAL);
  auto* memory = allocator.allocate(1);
  if (memory != nullptr)
    this->startup_trace_.reset(new (memory) StartupTrace());
  else
    ESP_LOGW(TAG, "Startup RTP diagnostics allocation failed");
}

void SipTransport::reset_startup_trace_() {
  if (!this->startup_trace_ || !this->rtp_task_quiesced_.load(std::memory_order_acquire)) return;
  for (auto& event : this->startup_trace_->events) event.store(0, std::memory_order_relaxed);
  this->startup_trace_->count = 0;
}

void SipTransport::record_startup_(StartupEvent event) {
  if (!this->startup_trace_) return;
  uint32_t expected = 0;
  this->startup_trace_->events[static_cast<uint8_t>(event)].compare_exchange_strong(
      expected, static_cast<uint32_t>(esp_timer_get_time()), std::memory_order_relaxed);
}

uint8_t SipTransport::record_startup_packet_(const uint8_t* data, size_t bytes) {
  if (!this->startup_trace_ || bytes < 12 || this->startup_trace_->count == 64) return 255;
  this->record_startup_(StartupEvent::RECV);
  auto read32 = [](const uint8_t* p) {
    return uint32_t(p[0]) << 24U | uint32_t(p[1]) << 16U | uint32_t(p[2]) << 8U | p[3];
  };
  const uint8_t index = this->startup_trace_->count++;
  auto& packet = this->startup_trace_->packets[index];
  packet = {static_cast<uint32_t>(esp_timer_get_time()),
            0,
            read32(data + 4),
            read32(data + 8),
            static_cast<uint16_t>(uint16_t(data[2]) << 8U | data[3]),
            static_cast<uint8_t>(data[1] & 127U),
            static_cast<uint8_t>(this->media_lifecycle_phase_.load(std::memory_order_acquire))};
  return index;
}

void SipTransport::dump_startup_trace() {
  if (!this->startup_trace_ || this->rtp_running_.load(std::memory_order_acquire) ||
      !this->rtp_task_quiesced_.load(std::memory_order_acquire))
    return;
  LockGuard lock(this->dialog_mutex_);
  if (this->rtp_running_.load(std::memory_order_acquire) || !this->rtp_task_quiesced_.load(std::memory_order_acquire))
    return;
  for (uint8_t i = 0; i < static_cast<uint8_t>(StartupEvent::COUNT); ++i) {
    const auto stamp = this->startup_trace_->events[i].load(std::memory_order_relaxed);
    if (stamp != 0) ESP_LOGI(TAG, "MediaStart stage=%u us=%u", i, (unsigned)stamp);
  }
  for (uint8_t i = 0; i < this->startup_trace_->count; ++i) {
    const auto& p = this->startup_trace_->packets[i];
    ESP_LOGI(TAG, "RTPStart n=%u us=%u done=%u seq=%u ts=%u ssrc=%08x pt=%u phase=%u delivered=%u", i, (unsigned)p.us,
             (unsigned)p.done_us, p.sequence, (unsigned)p.timestamp, (unsigned)p.ssrc, p.pt, p.phase & 127U,
             p.phase >> 7U);
  }
}
#endif


bool SipTransport::start() {
  if (!this->initialized()) {
    ESP_LOGE(TAG, "SIP signaling storage is unavailable");
    return false;
  }
  if (this->running_.load(std::memory_order_acquire)) return true;
#ifdef USE_ESPHOME_VOIP_STACK_OPUS
  if (!this->opus_codec_ready_) {
    ESP_LOGE(TAG, "Opus codec resources are unavailable");
    return false;
  }
#endif
#ifdef USE_ESPHOME_VOIP_STACK_G722
  if (!this->g722_codec_ready_) {
    ESP_LOGE(TAG, "G722 codec resources are unavailable");
    return false;
  }
#endif
  if (this->sip_task_handle_ != nullptr || this->rtp_task_handle_ != nullptr) {
    ESP_LOGE(TAG, "Cannot start SIP transport while a previous task is still owned");
    return false;
  }
  if (this->sip_task_done_ == nullptr) {
    this->sip_task_done_ = xSemaphoreCreateBinaryStatic(&this->sip_task_done_storage_);
  }
  if (this->sip_task_done_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create SIP task completion signal");
    return false;
  }
  xSemaphoreTake(this->sip_task_done_, 0);
  this->transport_stopping_.store(false, std::memory_order_release);
  this->media_lifecycle_phase_.store(MediaLifecyclePhase::IDLE,
                                      std::memory_order_release);
  if (!this->bind_udp_(&this->sip_socket_, this->sip_port_, "SIP")) return false;
  if (!this->bind_tcp_(&this->sip_tcp_listener_socket_, this->sip_port_, "SIP")) {
    close(this->sip_socket_);
    this->sip_socket_ = -1;
    return false;
  }
  if (!this->bind_udp_(&this->sip_wake_socket_, 0, "SIP wake")) {
    close(this->sip_socket_);
    this->sip_socket_ = -1;
    close(this->sip_tcp_listener_socket_);
    this->sip_tcp_listener_socket_ = -1;
    return false;
  }
  struct sockaddr_in wake_addr {};
  socklen_t wake_addr_len = sizeof(wake_addr);
  if (getsockname(this->sip_wake_socket_,
                  reinterpret_cast<struct sockaddr *>(&wake_addr),
                  &wake_addr_len) < 0 ||
      wake_addr.sin_port == 0) {
    const int err = errno;
    ESP_LOGE(TAG, "Failed to resolve SIP wake socket: %s (%d: %s)",
             socket_errno_name(err), err, socket_errno_text(err));
    close(this->sip_wake_socket_);
    this->sip_wake_socket_ = -1;
    close(this->sip_socket_);
    this->sip_socket_ = -1;
    close(this->sip_tcp_listener_socket_);
    this->sip_tcp_listener_socket_ = -1;
    return false;
  }
  this->sip_wake_port_ = ntohs(wake_addr.sin_port);
  this->running_.store(true, std::memory_order_release);
  if (!voip_audio_core::start_pinned_task(SipTransport::sip_task_trampoline_, "voip_sip",
                                          kSipTaskStackBytes, this, kSipTaskPriority, 1,
                                          this->task_stacks_in_psram_, TAG,
                                          &this->sip_task_handle_, &this->sip_task_tcb_,
                                          &this->sip_task_stack_)) {
    this->running_.store(false, std::memory_order_release);
    close(this->sip_socket_);
    this->sip_socket_ = -1;
    close(this->sip_tcp_listener_socket_);
    this->sip_tcp_listener_socket_ = -1;
    close(this->sip_wake_socket_);
    this->sip_wake_socket_ = -1;
    this->sip_wake_port_ = 0;
    return false;
  }
  if (this->rtp_task_done_ == nullptr) {
    this->rtp_task_done_ = xSemaphoreCreateBinaryStatic(&this->rtp_task_done_storage_);
  }
  if (this->rtp_cleanup_done_ == nullptr) {
    this->rtp_cleanup_done_ =
        xSemaphoreCreateBinaryStatic(&this->rtp_cleanup_done_storage_);
  }
  if (this->rtp_task_done_ == nullptr || this->rtp_cleanup_done_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create RTP lifecycle signals");
    this->stop();
    return false;
  }
#ifdef USE_ESPHOME_VOIP_DTMF
  if (this->dtmf_queue_ == nullptr)
    this->dtmf_queue_ = xQueueCreateStatic(kDtmfQueueDepth, sizeof(DtmfRequest), this->dtmf_queue_buffer_, &this->dtmf_queue_storage_);
  if (this->dtmf_queue_ == nullptr) { this->stop(); return false; }
  xQueueReset(this->dtmf_queue_);
#endif
  xSemaphoreTake(this->rtp_task_done_, 0);
  xSemaphoreTake(this->rtp_cleanup_done_, 0);
  this->rtp_task_quiesced_.store(true, std::memory_order_release);
  this->rtp_task_terminate_.store(false, std::memory_order_release);
  if (!voip_audio_core::start_pinned_task(SipTransport::rtp_task_trampoline_, "voip_rtp",
                                          kRtpTaskStackBytes, this, kRtpTaskPriority, 1,
                                          this->task_stacks_in_psram_, TAG,
                                          &this->rtp_task_handle_, &this->rtp_task_tcb_,
                                          &this->rtp_task_stack_)) {
    this->stop();
    return false;
  }
  ESP_LOGI(TAG, "SIP listening on UDP+TCP/%u, RTP base UDP/%u", (unsigned) this->sip_port_, (unsigned) this->rtp_port_);
  this->emit_connection_change_(true);
  return true;
}

void SipTransport::stop() {
  // An incomplete constructor has never published sockets, tasks or borrowers.
  if (!this->initialized()) return;
  this->transport_stopping_.store(true, std::memory_order_release);
  const bool was_running =
      this->running_.exchange(false, std::memory_order_acq_rel);
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  this->cancel_registration_dns_();
#endif

  // Stop new SIP work first. A strong wait is required here because this
  // method is also the destructor boundary: returning with a worker that can
  // still dereference SipTransport would be a use-after-free, not a safe leak.
  this->tcp_flow_.connect_requested.store(false, std::memory_order_release);
  {
    LockGuard lock(this->tcp_tx_pending_mutex_);
    reset_string(this->tcp_flow_.pending);
  }
  this->tcp_flow_.close_requested.store(true,
                                               std::memory_order_release);
  if (this->sip_task_handle_ != nullptr) {
    xSemaphoreTake(this->sip_task_done_, 0);
    this->wake_sip_task_();
    xSemaphoreTake(this->sip_task_done_, portMAX_DELAY);
    voip_audio_core::cleanup_pinned_task(&this->sip_task_handle_,
                                          &this->sip_task_stack_,
                                          kSipTaskStackBytes);
  }
  if (this->sip_socket_ >= 0) {
    close(this->sip_socket_);
    this->sip_socket_ = -1;
  }
  if (this->sip_tcp_listener_socket_ >= 0) {
    close(this->sip_tcp_listener_socket_);
    this->sip_tcp_listener_socket_ = -1;
  }
  if (this->sip_wake_socket_ >= 0) {
    close(this->sip_wake_socket_);
    this->sip_wake_socket_ = -1;
    this->sip_wake_port_ = 0;
  }

  this->stop_audio_path();
  if (this->rtp_task_handle_ != nullptr) {
    if (!this->rtp_task_quiesced_.load(std::memory_order_acquire)) {
      xSemaphoreTake(this->rtp_cleanup_done_, portMAX_DELAY);
    }
    {
      LockGuard media_lock(this->media_lifecycle_mutex_);
      this->media_lifecycle_phase_.store(
          MediaLifecyclePhase::SHUTTING_DOWN, std::memory_order_release);
    }
    xSemaphoreTake(this->rtp_task_done_, 0);
    this->rtp_task_terminate_.store(true, std::memory_order_release);
    xTaskNotifyGive(this->rtp_task_handle_);
    xSemaphoreTake(this->rtp_task_done_, portMAX_DELAY);
    voip_audio_core::cleanup_pinned_task(&this->rtp_task_handle_,
                                          &this->rtp_task_stack_,
                                          kRtpTaskStackBytes);
  }
  this->release_refer_dialog_();
  for (auto &completed : this->completed_refers_) completed.reset();
  this->refer_signal_pending_ = false;
  this->logical_call_id_.clear();
  this->logical_wire_id_.clear();
  if (was_running) this->emit_connection_change_(false);
}

bool SipTransport::is_connected() const {
  return this->running_.load(std::memory_order_acquire);
}

void SipTransport::disconnect() {
  LockGuard lock(this->dialog_mutex_);
  if (this->terminal_transaction_pending_locked_()) {
    ESP_LOGD(TAG,
             "SIP dialog reset deferred while terminal transaction is pending");
    return;
  }
  this->reset_dialog_();
}

bool SipTransport::originate(const std::string &host, uint16_t port) {
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  if (this->next_account_ >= 0) {
    if (!this->parse_remote_(host)) return false;
    this->remote_sip_port_.store(port ? port : 5060, std::memory_order_release);
    return true;  // The persistent account flow already owns this next hop.
  }
#endif
  if (!this->parse_remote_(host)) return false;
  const uint16_t sip_port = port ? port : 5060;
  this->remote_sip_port_.store(sip_port, std::memory_order_release);
  if (!this->remote_sip_tcp_.load(std::memory_order_acquire)) {
    ESP_LOGI(TAG, "SIP UDP originate target set to %s:%u", host.c_str(), (unsigned) sip_port);
    return true;
  }

  this->request_tcp_client_close_();
  const uint32_t ip_v4 = this->remote_ip_v4_.load(std::memory_order_acquire);
  if (ip_v4 == 0) return false;
  {
    LockGuard lock(this->tcp_tx_pending_mutex_);
    reset_string(this->tcp_flow_.pending);
  }
  this->tcp_flow_.target_ip.store(ip_v4, std::memory_order_release);
  this->tcp_flow_.target_port.store(sip_port, std::memory_order_release);
  this->tcp_flow_.connect_requested.store(true, std::memory_order_release);
  this->wake_sip_task_();
  return true;
}

void SipTransport::set_remote(const std::string &ip, uint16_t port, uint16_t rtp_port) {
  this->parse_remote_(ip);
  if (port) this->remote_sip_port_.store(port, std::memory_order_release);
  if (rtp_port) this->remote_rtp_port_.store(rtp_port, std::memory_order_release);
}

void SipTransport::set_sip_signaling_transport(bool tcp) {
  const bool was_tcp = this->remote_sip_tcp_.exchange(tcp, std::memory_order_acq_rel);
  if (!tcp && was_tcp) this->request_tcp_client_close_();
}

}  // namespace voip_stack
}  // namespace esphome

#endif
