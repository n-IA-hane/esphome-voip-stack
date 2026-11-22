#include "sip_transport_internal.h"

#if defined(USE_ESP32) && defined(USE_ESPHOME_VOIP_SIP_TRANSPORT)

namespace esphome {
namespace voip_stack {

void SipTransport::sip_task_trampoline_(void *param) {
  static_cast<SipTransport *>(param)->sip_task_();
}

void SipTransport::rtp_task_trampoline_(void *param) {
  static_cast<SipTransport *>(param)->rtp_task_();
}

void SipTransport::sip_task_() {
  char *const buf = this->sip_rx_scratch_.data();
  while (this->running_.load(std::memory_order_acquire)) {
    this->pump_udp_retransmits_();
#ifdef USE_ESPHOME_VOIP_REGISTRATION
    {
      LockGuard lock(this->dialog_mutex_); this->pump_registration_dns_(); this->registration_.tick(millis());
#if defined(USE_ESPHOME_VOIP_MWI) || defined(USE_ESPHOME_VOIP_BLF)
      this->pump_registration_mwi_(millis());
#endif
#ifdef USE_VOIP_HA_AUTO_REGISTER
      this->notify_managed_account_main_loop_();
#endif
    }
    for (auto &flow : this->registration_flows_) this->prepare_tcp_flow_(flow);
#endif
    this->prepare_tcp_flow_(this->tcp_flow_);
    if (this->refer_dialog_) this->prepare_tcp_flow_(this->refer_dialog_->tcp_flow_);
    fd_set readfds;
    fd_set writefds;
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    int max_fd = -1;
    if (this->sip_wake_socket_ >= 0) {
      FD_SET(this->sip_wake_socket_, &readfds);
      max_fd = std::max(max_fd, this->sip_wake_socket_);
    }
    if (this->sip_socket_ >= 0) {
      FD_SET(this->sip_socket_, &readfds);
      max_fd = std::max(max_fd, this->sip_socket_);
    }
    if (this->sip_tcp_listener_socket_ >= 0) {
      FD_SET(this->sip_tcp_listener_socket_, &readfds);
      max_fd = std::max(max_fd, this->sip_tcp_listener_socket_);
    }
        for (auto *flow :
#ifdef USE_ESPHOME_VOIP_REGISTRATION
         this->signaling_flows_()
#else
         std::array<SipTcpFlow *, 2>{&this->tcp_flow_, this->refer_dialog_ ? &this->refer_dialog_->tcp_flow_ : nullptr}
#endif
         ) {
      if (flow == nullptr) continue;
      const int socket = flow->socket.load(std::memory_order_acquire);
      if (socket >= 0) { FD_SET(socket, &readfds); max_fd = std::max(max_fd, socket); }
      if (flow->connecting_socket >= 0) {
        FD_SET(flow->connecting_socket, &writefds);
        max_fd = std::max(max_fd, flow->connecting_socket);
      }
    }
    if (max_fd < 0) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }
    struct timeval timeout{};
    struct timeval *timeout_ptr = nullptr;
    uint32_t timeout_ms = 0;
    bool have_timeout = false;
        for (auto *flow :
#ifdef USE_ESPHOME_VOIP_REGISTRATION
         this->signaling_flows_()
#else
         std::array<SipTcpFlow *, 2>{&this->tcp_flow_, this->refer_dialog_ ? &this->refer_dialog_->tcp_flow_ : nullptr}
#endif
         ) {
      if (flow == nullptr || flow->connecting_socket < 0) continue;
      const uint32_t now = millis();
      const uint32_t remaining = time_reached(now, flow->connect_deadline) ? 0 : flow->connect_deadline - now;
      if (!have_timeout || remaining < timeout_ms) timeout_ms = remaining;
      have_timeout = true;
      timeout_ptr = &timeout;
    }
    {
      const uint32_t now = millis();
      uint32_t next_ms = 0;
      bool have_sip_timeout = false;
      auto include_at = [now, &next_ms, &have_sip_timeout](uint32_t deadline) {
        const uint32_t delta = time_reached(now, deadline) ? 0 : deadline - now;
        if (!have_sip_timeout || delta < next_ms) {
          next_ms = delta;
          have_sip_timeout = true;
        }
      };
      {
        LockGuard lock(this->dialog_mutex_);
        auto include_txn = [&include_at](const UdpTransaction &txn) {
          if (txn.empty()) return;
          include_at(txn.udp && !txn.completed ? txn.next_ms
                                               : txn.deadline_ms);
        };
        if (this->outgoing_invite_pending_.load(std::memory_order_acquire)) {
          include_txn(this->pending_invite_);
        }
        include_txn(this->pending_cancel_);
        include_txn(this->pending_bye_);
        include_txn(this->pending_update_);
        if (this->refer_dialog_) {
          include_txn(this->refer_dialog_->pending_invite_);
          include_txn(this->refer_dialog_->pending_cancel_);
          include_txn(this->refer_dialog_->pending_bye_);
          include_txn(this->refer_dialog_->pending_notify_);
          include_txn(this->refer_dialog_->pending_notify_final_);
          include_at(this->refer_expires_);
        }
        for (const auto &completed : this->completed_refers_) {
          if (completed) include_at(completed->expires_ms);
        }
        if (!this->pending_provisional_response_.empty()) {
          include_at(this->pending_provisional_response_.next_ms);
        }
        if (this->completed_invite_.awaiting_ack) {
          const bool retransmits =
              this->completed_invite_.udp ||
              this->completed_invite_.status < 300;
          include_at(retransmits
                         ? this->completed_invite_.next_retransmit_ms
                         : this->completed_invite_.deadline_ms);
        }
        auto include_completed_expiry = [&include_at](const auto &transaction) {
          if (!transaction.empty()) {
            include_at(transaction.completed_ms +
                       SIP_TRANSACTION_TIMEOUT_MS);
          }
        };
        include_completed_expiry(this->completed_invite_);
        include_completed_expiry(this->completed_control_);
        include_completed_expiry(this->completed_invite_client_);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
        include_completed_expiry(this->completed_video_direction_invite_);
#endif
      }
      if (have_sip_timeout && (!have_timeout || next_ms < timeout_ms)) {
        timeout_ms = next_ms;
        have_timeout = true;
        timeout_ptr = &timeout;
      }
    }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
    {
      const uint32_t now = millis();
      LockGuard lock(this->dialog_mutex_);
      const auto &pending = this->pending_video_direction_invite_;
      if (pending.pending()) {
        const uint32_t deadline =
            pending.waiting_retry
                ? pending.retry_at_ms
                : (pending.transaction.udp &&
                           !pending.transaction.completed
                       ? pending.transaction.next_ms
                       : pending.response_deadline_ms);
        const uint32_t delta =
            time_reached(now, deadline) ? 0 : deadline - now;
        if (!have_timeout || delta < timeout_ms) {
          timeout_ms = delta;
          have_timeout = true;
          timeout_ptr = &timeout;
        }
      }
    }
#endif
#ifdef USE_ESPHOME_VOIP_REGISTRATION
    {
      LockGuard lock(this->dialog_mutex_); uint32_t registration_timeout = 0;
      if (this->registration_.next_deadline(millis(), &registration_timeout) &&
          (!have_timeout || registration_timeout < timeout_ms)) {
        timeout_ms = registration_timeout; have_timeout = true; timeout_ptr = &timeout;
      }
#if defined(USE_ESPHOME_VOIP_MWI) || defined(USE_ESPHOME_VOIP_BLF)
      if (this->mwi_.next_deadline(millis(), &registration_timeout) &&
          (!have_timeout || registration_timeout < timeout_ms)) {
        timeout_ms = registration_timeout; have_timeout = true; timeout_ptr = &timeout;
      }
#endif
    }
#endif
#ifdef USE_ESPHOME_VOIP_REGISTRATION
    for (auto *flow : {&this->tcp_flow_, &this->inbound_tcp_flow_}) {
      const uint32_t deadline = flow->ingress_deadline.load(std::memory_order_acquire);
      if (flow->socket.load(std::memory_order_acquire) < 0 || deadline == 0) continue;
      const uint32_t now = millis();
      const uint32_t remaining = time_reached(now, deadline) ? 0 : deadline - now;
      if (!have_timeout || remaining < timeout_ms) {
        timeout_ms = remaining; have_timeout = true; timeout_ptr = &timeout;
      }
    }
#endif
    if (timeout_ptr != nullptr) {
      timeout.tv_sec = timeout_ms / 1000;
      timeout.tv_usec = (timeout_ms % 1000) * 1000;
    }
    const int ready = ::select(max_fd + 1, &readfds, &writefds, nullptr, timeout_ptr);
#ifdef USE_ESPHOME_VOIP_REGISTRATION
    for (auto *flow : {&this->tcp_flow_, &this->inbound_tcp_flow_}) {
      const uint32_t deadline = flow->ingress_deadline.load(std::memory_order_acquire);
      if (flow->socket.load(std::memory_order_acquire) >= 0 && deadline != 0 &&
          time_reached(millis(), deadline)) this->handle_tcp_peer_loss_(flow, deadline);
    }
#endif
    bool connect_expired = false;
        for (auto *flow :
#ifdef USE_ESPHOME_VOIP_REGISTRATION
         this->signaling_flows_()
#else
         std::array<SipTcpFlow *, 2>{&this->tcp_flow_, this->refer_dialog_ ? &this->refer_dialog_->tcp_flow_ : nullptr}
#endif
         ) {
      if (flow != nullptr && flow->connecting_socket >= 0 && time_reached(millis(), flow->connect_deadline)) {
        this->fail_tcp_flow_(*flow, ETIMEDOUT);
        connect_expired = true;
      }
    }
    if (connect_expired) continue;
    if (ready <= 0) {
      continue;
    }

    if (this->sip_wake_socket_ >= 0 &&
        FD_ISSET(this->sip_wake_socket_, &readfds)) {
      uint8_t wake_bytes[16];
      while (recv(this->sip_wake_socket_, wake_bytes, sizeof(wake_bytes), 0) >
             0) {
      }
      if (!this->running_.load(std::memory_order_acquire)) continue;
    }

        for (auto *flow :
#ifdef USE_ESPHOME_VOIP_REGISTRATION
         this->signaling_flows_()
#else
         std::array<SipTcpFlow *, 2>{&this->tcp_flow_, this->refer_dialog_ ? &this->refer_dialog_->tcp_flow_ : nullptr}
#endif
         ) {
      if (flow == nullptr || flow->connecting_socket < 0 || !FD_ISSET(flow->connecting_socket, &writefds)) continue;
      int error = 0;
      socklen_t len = sizeof(error);
      if (getsockopt(flow->connecting_socket, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0)
        this->promote_tcp_flow_(*flow);
      else
        this->fail_tcp_flow_(*flow, error == 0 ? errno : error);
    }

    if (this->sip_tcp_listener_socket_ >= 0 && FD_ISSET(this->sip_tcp_listener_socket_, &readfds)) {
      struct sockaddr_in src{};
      socklen_t slen = sizeof(src);
      int client = accept(this->sip_tcp_listener_socket_, reinterpret_cast<struct sockaddr *>(&src), &slen);
      if (client >= 0) {
#ifdef USE_ESPHOME_VOIP_REGISTRATION
        this->accept_tcp_client_(client, src);
#else
        int opt = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        int flags = fcntl(client, F_GETFL, 0);
        fcntl(client, F_SETFL, flags | O_NONBLOCK);
        const uint32_t accepted_ip_v4 = ntohl(src.sin_addr.s_addr);
        const int active_client = this->tcp_flow_.socket.load(std::memory_order_acquire);
        const uint32_t active_ip_v4 = this->tcp_flow_.peer_ip.load(std::memory_order_acquire);
        bool signaling_owned = false;
        bool signaling_uses_tcp = false;
        {
          LockGuard lock(this->dialog_mutex_);
          signaling_owned =
              !this->call_id_.empty() ||
              this->outgoing_invite_pending_.load(std::memory_order_acquire) ||
              this->media_active_.load(std::memory_order_acquire) ||
              this->terminal_transaction_pending_locked_();
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
          signaling_owned =
              signaling_owned ||
              this->pending_video_direction_invite_.pending();
#endif
          signaling_uses_tcp =
              this->remote_sip_tcp_.load(std::memory_order_acquire);
        }
        const bool replaces_owned_transport =
            signaling_owned &&
            (!signaling_uses_tcp || active_client >= 0 ||
             this->tcp_flow_.connecting_socket >= 0 ||
             this->tcp_flow_.connect_requested.load(std::memory_order_acquire));
        if (replaces_owned_transport ||
            (this->dialog_active_() && active_client >= 0 &&
             active_ip_v4 != 0 && active_ip_v4 != accepted_ip_v4)) {
          char ip[16];
          inet_ntoa_r(src.sin_addr, ip, sizeof(ip));
          ESP_LOGW(TAG,
                   "SIP TCP accept rejected: signaling transport owned by %s",
                   ip);
          close(client);
          continue;
        }
        if (!this->should_accept_session_() && active_client < 0) {
          close(client);
          continue;
        }
        this->close_tcp_client_from_sip_task_();
        this->tcp_flow_.socket.store(client, std::memory_order_release);
        this->tcp_flow_.peer_ip.store(accepted_ip_v4, std::memory_order_release);
        this->tcp_flow_.close_requested.store(false, std::memory_order_release);
        this->tcp_flow_.rx_buffer.clear();
        this->remote_sip_tcp_.store(true, std::memory_order_release);
        char ip[16];
        inet_ntoa_r(src.sin_addr, ip, sizeof(ip));
        ESP_LOGI(TAG, "SIP TCP accepted from %s:%u", ip, (unsigned) ntohs(src.sin_port));
#endif
      }
    }

    if (this->sip_socket_ >= 0 && FD_ISSET(this->sip_socket_, &readfds)) {
      struct sockaddr_in src{};
      const auto received = receive_sip_udp(this->sip_socket_, buf, this->sip_rx_scratch_.size(), &src);
      if (received.truncated) {
        ESP_LOGW(TAG, "SIP UDP datagram exceeds receive capacity; discarded");
      } else if (received.bytes > 0) {
        this->handle_sip_udp_datagram_(buf, received.bytes, src);
      }
    }

    // A commit can exchange the two owners while dispatching a record. Resolve
    // each saved descriptor again instead of retaining a pointer across dispatch.
#ifdef USE_ESPHOME_VOIP_REGISTRATION
    std::array<int, 3 + SipRegistration::CAPACITY> sockets{};
    auto flows = this->signaling_flows_();
    for (size_t i = 0; i < sockets.size(); ++i) sockets[i] = flows[i] == nullptr ? -1 : flows[i]->socket.load(std::memory_order_acquire);
#else
    const int sockets[] = {this->tcp_flow_.socket.load(std::memory_order_acquire),
        this->refer_dialog_ ? this->refer_dialog_->tcp_flow_.socket.load(std::memory_order_acquire) : -1};
#endif
    for (int socket : sockets) {
      if (socket < 0 || !FD_ISSET(socket, &readfds) || this->tcp_flow_for_socket_(socket) == nullptr) continue;
      struct sockaddr_in src{};
      socklen_t length = sizeof(src);
      if (getpeername(socket, reinterpret_cast<struct sockaddr *>(&src), &length) == 0)
        this->handle_sip_stream_(socket, src);
    }
  }
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  this->close_tcp_flow_(this->inbound_tcp_flow_);
  for (auto &flow : this->registration_flows_) this->close_tcp_flow_(flow);
#endif
  this->close_tcp_client_from_sip_task_();
  if (this->refer_dialog_) this->close_tcp_flow_(this->refer_dialog_->tcp_flow_);
  if (this->sip_task_done_ != nullptr) {
    xSemaphoreGive(this->sip_task_done_);
  }
  vTaskDelete(nullptr);
}

void SipTransport::rtp_task_() {
  uint8_t buf[1600];
  // The wire payload is capped at 1488 bytes. L24-in-S32 expands by 4/3
  // while decoding, so 2 KiB covers every format accepted by the schema.
  uint8_t pcm[2048];
#ifdef USE_ESPHOME_VOIP_DTMF
  DtmfSender dtmf_sender;
  uint32_t dtmf_session_epoch = 0, dtmf_ssrc = 0, dtmf_clock_ms = 0, dtmf_clock_ts = 0;
  uint16_t dtmf_sequence = 0;
  bool dtmf_separate = false;
#ifdef USE_ESPHOME_VOIP_DTMF_EVENTS
  DtmfReceiver dtmf_receiver;
#endif
#endif
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    VOIP_STARTUP_EVENT(WAKE);
    while (!this->rtp_task_terminate_.load(std::memory_order_acquire) &&
           this->rtp_running_.load(std::memory_order_acquire)) {
      const int socket = this->rtp_socket_;
      if (socket < 0) {
        break;
      }
      fd_set readfds;
      FD_ZERO(&readfds);
      FD_SET(socket, &readfds);
      timeval timeout{};
      timeval *timeout_ptr = nullptr;
#ifdef USE_ESPHOME_VOIP_DTMF
      const uint32_t dtmf_now = millis();
      DtmfFormat dtmf_tx; uint32_t dtmf_epoch = 0;
      this->get_dtmf_config_(&dtmf_tx, nullptr, &dtmf_epoch);
      if (dtmf_epoch != dtmf_session_epoch && !dtmf_sender.active()) {
        dtmf_session_epoch = dtmf_epoch;
        dtmf_ssrc = esp_random(); if (dtmf_ssrc == this->rtp_ssrc_) dtmf_ssrc ^= 0x80000000U;
        dtmf_sequence = static_cast<uint16_t>(esp_random());
        dtmf_clock_ms = dtmf_now; dtmf_clock_ts = esp_random();
      }
      const bool stable_proposal = !(this->media_proposal_epoch_.load(std::memory_order_acquire) & 1U);
      if (!this->media_active_.load(std::memory_order_acquire)) {
        dtmf_sender.cancel(); if (this->dtmf_queue_ != nullptr) xQueueReset(this->dtmf_queue_);
      } else if (!dtmf_tx.valid()) {
        if (this->dtmf_queue_ != nullptr) xQueueReset(this->dtmf_queue_);
      } else if (stable_proposal && dtmf_sender.ready(dtmf_now) && this->dtmf_queue_ != nullptr) {
        LockGuard socket_lock(this->rtp_socket_mutex_);
        DtmfRequest request{};
        while (xQueueReceive(this->dtmf_queue_, &request, 0) == pdTRUE) {
          if (request.epoch != dtmf_epoch) continue;
          AudioFormat audio_tx; this->get_media_config_(&audio_tx, nullptr, nullptr, nullptr);
          dtmf_separate = audio_format_rtp_clock_rate(audio_tx) != dtmf_tx.clock;
          const uint32_t timestamp = dtmf_separate
              ? dtmf_clock_ts + uint64_t(dtmf_now - dtmf_clock_ms) * dtmf_tx.clock / 1000
              : this->rtp_tx_clock_timestamp_ + uint64_t(dtmf_now - this->rtp_tx_clock_ms_) * dtmf_tx.clock / 1000;
          dtmf_sender.start(request.event, request.duration_ms, dtmf_tx, timestamp, dtmf_epoch, dtmf_now);
          break;
        }
      }
      if (dtmf_sender.active() || (this->dtmf_queue_ != nullptr && uxQueueMessagesWaiting(this->dtmf_queue_) != 0)) {
        const int32_t remaining = static_cast<int32_t>(dtmf_sender.next() - dtmf_now);
        timeout.tv_usec = !stable_proposal ? 20000 : remaining > 0 ? remaining * 1000 : 0;
        timeout_ptr = &timeout;
      }
#endif
      const int ready = ::select(socket + 1, &readfds, nullptr, nullptr, timeout_ptr);
      if (ready < 0) {
        if (errno != EINTR) {
          const int err = errno;
          ESP_LOGW(TAG, "RTP select failed: %s (%d: %s)",
                   socket_errno_name(err), err, socket_errno_text(err));
          break;
        }
        continue;
      }
#ifdef USE_ESPHOME_VOIP_DTMF
      DtmfSender::Report report;
      if (!(this->media_proposal_epoch_.load(std::memory_order_acquire) & 1U) && dtmf_sender.poll(millis(), &report))
        // The active digit keeps its format, clock, SSRC and timestamp through
        // a re-offer. Queue admission still rejects retired configurations.
        this->send_dtmf_packet_(report, dtmf_sender.format, this->dtmf_generation_.load(std::memory_order_acquire),
                                dtmf_ssrc, dtmf_separate ? &dtmf_sequence : nullptr);
#endif
      if (ready == 0 || !FD_ISSET(socket, &readfds)) {
        continue;
      }
      struct sockaddr_in src{};
      socklen_t slen = sizeof(src);
      int n = recvfrom(socket, buf, sizeof(buf), 0,
                       reinterpret_cast<struct sockaddr *>(&src), &slen);
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
      const uint8_t startup_index = n > 0 ? this->record_startup_packet_(buf, static_cast<size_t>(n)) : 255;
#endif
      // Count socket deliveries before RTP admission, including invalid input.
      if (n > 0) this->rtp_rx_datagrams_.fetch_add(1, std::memory_order_relaxed);
      if (n <= 12 || (buf[0] & 0xC0) != 0x80) {
        continue;
      }
      if (!this->media_active_.load(std::memory_order_acquire) ||
          !this->audio_receive_enabled_.load(std::memory_order_acquire)) {
        continue;
      }
      const uint32_t proposal_epoch =
          this->media_proposal_epoch_.load(std::memory_order_acquire);
      if ((proposal_epoch & 1U) != 0) continue;
      const uint32_t src_ip = ntohl(src.sin_addr.s_addr);
      const uint16_t src_port = ntohs(src.sin_port);
      const uint32_t expected_ip = this->remote_rtp_ip_v4_.load(std::memory_order_acquire);
      if (expected_ip != 0 && src_ip != expected_ip) {
        continue;
      }
      const uint8_t csrc_count = buf[0] & 0x0F;
      size_t header = 12u + static_cast<size_t>(csrc_count) * 4u;
      if (static_cast<size_t>(n) <= header) {
        continue;
      }
      if ((buf[0] & 0x10) != 0) {
        if (static_cast<size_t>(n) < header + 4) continue;
        const uint16_t ext_len = static_cast<uint16_t>((buf[header + 2] << 8) | buf[header + 3]);
        header += 4u + static_cast<size_t>(ext_len) * 4u;
        if (static_cast<size_t>(n) <= header) continue;
      }
      size_t payload_len = static_cast<size_t>(n) - header;
      if ((buf[0] & 0x20) != 0 && payload_len > 0) {
        const uint8_t pad = buf[n - 1];
        if (pad == 0 || pad > payload_len) continue;
        payload_len -= pad;
      }
      const uint16_t sequence = static_cast<uint16_t>((buf[2] << 8) | buf[3]);
      const uint32_t timestamp = (static_cast<uint32_t>(buf[4]) << 24) |
                                 (static_cast<uint32_t>(buf[5]) << 16) |
                                 (static_cast<uint32_t>(buf[6]) << 8) |
                                 static_cast<uint32_t>(buf[7]);
      const uint8_t *payload = buf + header;
      const uint32_t ssrc = (static_cast<uint32_t>(buf[8]) << 24) |
                            (static_cast<uint32_t>(buf[9]) << 16) |
                            (static_cast<uint32_t>(buf[10]) << 8) |
                            static_cast<uint32_t>(buf[11]);
#ifdef USE_ESPHOME_VOIP_DTMF
      if (this->handle_dtmf_packet_(payload, payload_len, buf[1] & 0x7F, ssrc, timestamp,
              src_port, proposal_epoch,
#ifdef USE_ESPHOME_VOIP_DTMF_EVENTS
              &dtmf_receiver
#else
              nullptr
#endif
              )) continue;
#endif
      AudioFormat rx_format;
      if (!this->received_audio_format_(buf[1] & 0x7F, &rx_format, payload, payload_len)) continue;
      const uint8_t *audio_data = payload;
      size_t audio_bytes = payload_len;
      if (rx_format.codec == AudioCodec::PCM) {
#ifndef USE_ESPHOME_VOIP_STACK_PCM
        continue;
#else
        audio_bytes = rtp_payload_to_pcm(payload, payload_len, rx_format, pcm,
                                         sizeof(pcm));
        audio_data = pcm;
        if (audio_bytes == 0 ||
            audio_bytes != rx_format.nominal_frame_bytes()) continue;
#endif
      }
      if (this->media_proposal_epoch_.load(std::memory_order_acquire) !=
          proposal_epoch) {
        continue;
      }
      const bool ssrc_latched =
          this->rtp_ssrc_latched_.load(std::memory_order_acquire);
      bool source_changed = false;
      if (ssrc_latched &&
          this->latched_rtp_ip_v4_.load(std::memory_order_acquire) != src_ip) {
        continue;
      }
      if (ssrc_latched &&
          this->latched_rtp_ssrc_.load(std::memory_order_acquire) != ssrc) {
        // RFC 3550 permits a restarted endpoint to choose a new SSRC. The
        // already-latched IP:port normally remains the media identity. After
        // an actual RX resume, permit one validated source to also change its
        // port (RFC 3550 section 8.2), then restore the ordinary source guard.
        if (this->latched_rtp_port_.load(std::memory_order_acquire) !=
                src_port &&
            !this->rtp_source_reacquire_pending_.load(std::memory_order_acquire)) {
          continue;
        }
        const uint32_t previous_ssrc =
            this->latched_rtp_ssrc_.load(std::memory_order_acquire);
        this->latched_rtp_ssrc_.store(ssrc, std::memory_order_release);
        this->latched_rtp_port_.store(src_port, std::memory_order_release);
        this->remote_rtp_port_.store(src_port, std::memory_order_release);
        source_changed = true;
        ESP_LOGI(TAG, "RTP source changed SSRC %08" PRIx32 " -> %08" PRIx32,
                 previous_ssrc, ssrc);
      } else if (!ssrc_latched) {
        this->latched_rtp_ip_v4_.store(src_ip, std::memory_order_release);
        this->latched_rtp_port_.store(src_port, std::memory_order_release);
        this->latched_rtp_ssrc_.store(ssrc, std::memory_order_release);
        this->remote_rtp_port_.store(src_port, std::memory_order_release);
        this->rtp_ssrc_latched_.store(true, std::memory_order_release);
      } else if (this->latched_rtp_port_.load(std::memory_order_acquire) != src_port) {
        // Keep the SSRC identity but follow a legitimate NAT port rebind.
        this->latched_rtp_port_.store(src_port, std::memory_order_release);
        this->remote_rtp_port_.store(src_port, std::memory_order_release);
      }
      this->rtp_rx_packets_.fetch_add(1, std::memory_order_acq_rel);
      this->rtp_rx_bytes_.fetch_add(static_cast<uint32_t>(n), std::memory_order_acq_rel);
      this->rtp_source_reacquire_pending_.store(false, std::memory_order_release);
      source_changed |= this->audio_receive_reset_pending_.exchange(false, std::memory_order_acq_rel);
      this->emit_audio_frame_(audio_data, audio_bytes, sequence, timestamp,
                              source_changed, pack_audio_format(rx_format));
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
      if (startup_index < 64 && this->startup_trace_) {
        auto &packet = this->startup_trace_->packets[startup_index];
        packet.done_us = static_cast<uint32_t>(esp_timer_get_time());
        packet.phase |= 128U;
      }
#endif
    }

#ifdef USE_ESPHOME_VOIP_DTMF
    dtmf_sender.cancel();
    if (this->dtmf_queue_ != nullptr) xQueueReset(this->dtmf_queue_);
#endif
    // A socket/select failure must not leave the transport claiming an active
    // media worker that has actually parked. Gate the path and clean every
    // subordinate video/socket resource through the same event-driven exit.
    const bool worker_failed =
        this->rtp_running_.exchange(false, std::memory_order_acq_rel);
    if (worker_failed) {
      ESP_LOGE(TAG, "RTP worker exited unexpectedly; quiescing media");
      this->close_media_session_();
      {
        LockGuard media_lock(this->media_lifecycle_mutex_);
        if (this->media_lifecycle_phase_.load(std::memory_order_acquire) ==
            MediaLifecyclePhase::ACTIVE) {
          this->media_lifecycle_phase_.store(
              MediaLifecyclePhase::CLEANING, std::memory_order_release);
        }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
        if (this->video_session_ != nullptr)
          this->video_session_->request_stop();
#endif
      }
    }

    bool cleanup_requested = false;
    {
      LockGuard media_lock(this->media_lifecycle_mutex_);
      cleanup_requested =
          this->media_lifecycle_phase_.load(std::memory_order_acquire) ==
          MediaLifecyclePhase::CLEANING;
    }
    if (cleanup_requested) {
      this->finish_audio_path_stop_();
    }

    {
      LockGuard media_lock(this->media_lifecycle_mutex_);
      if (this->media_lifecycle_phase_.load(std::memory_order_acquire) ==
          MediaLifecyclePhase::CLEANING) {
        this->media_lifecycle_phase_.store(MediaLifecyclePhase::IDLE,
                                            std::memory_order_release);
      }
      this->rtp_task_quiesced_.store(true, std::memory_order_release);
      if (this->rtp_cleanup_done_ != nullptr)
        xSemaphoreGive(this->rtp_cleanup_done_);
      this->emit_media_quiesced_();
    }

    if (worker_failed) {
      SipSignal signal;
      signal.type = SipSignalType::PROTOCOL_ERROR;
      signal.status_code = 500;
      signal.reason = "rtp_worker_failed";
      {
        LockGuard dialog_lock(this->dialog_mutex_);
        signal.call_id = this->call_id_;
        if (!signal.call_id.empty()) {
          signal.terminal_transaction_pending =
              this->send_bye_unlocked_(signal.call_id);
          if (!signal.terminal_transaction_pending)
            this->reset_dialog_();
        }
      }
      if (!signal.call_id.empty()) this->emit_sip_signal_(std::move(signal));
    }

    if (this->rtp_task_terminate_.load(std::memory_order_acquire)) break;
  }
  if (this->rtp_task_done_ != nullptr) {
    xSemaphoreGive(this->rtp_task_done_);
  }
  vTaskDelete(nullptr);
}

}  // namespace voip_stack
}  // namespace esphome

#endif
