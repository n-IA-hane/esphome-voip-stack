#include "sip_transport_internal.h"

#if defined(USE_ESP32) && defined(USE_ESPHOME_VOIP_SIP_TRANSPORT)

namespace esphome {
namespace voip_stack {

bool SipTransport::bind_udp_(int *fd, uint16_t port, const char *label) {
  if (label != nullptr && strcmp(label, "RTP") == 0) VOIP_STARTUP_EVENT(BIND);
  *fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (*fd < 0) {
    const int err = errno;
    ESP_LOGE(TAG, "Failed to create %s socket: %s (%d: %s)",
             label, socket_errno_name(err), err, socket_errno_text(err));
    return false;
  }
  int flags = fcntl(*fd, F_GETFL, 0);
  fcntl(*fd, F_SETFL, flags | O_NONBLOCK);
  if (strcmp(label, "RTP") == 0) {
    const int rx_buffer = kRtpSocketRxBufferBytes;
    setsockopt(*fd, SOL_SOCKET, SO_RCVBUF, &rx_buffer, sizeof(rx_buffer));
    const int tos = 0xB8;
    setsockopt(*fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
  }
  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);
  if (bind(*fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
    const int err = errno;
    ESP_LOGE(TAG, "%s bind on UDP/%u failed: %s (%d: %s)",
             label, (unsigned) port, socket_errno_name(err), err, socket_errno_text(err));
    close(*fd);
    *fd = -1;
    return false;
  }
  return true;
}

bool SipTransport::bind_tcp_(int *fd, uint16_t port, const char *label) {
  *fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (*fd < 0) {
    const int err = errno;
    ESP_LOGE(TAG, "Failed to create %s TCP socket: %s (%d: %s)",
             label, socket_errno_name(err), err, socket_errno_text(err));
    return false;
  }
  int opt = 1;
  setsockopt(*fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  setsockopt(*fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
  int flags = fcntl(*fd, F_GETFL, 0);
  fcntl(*fd, F_SETFL, flags | O_NONBLOCK);
  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);
  if (bind(*fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
    const int err = errno;
    ESP_LOGE(TAG, "%s bind on TCP/%u failed: %s (%d: %s)",
             label, (unsigned) port, socket_errno_name(err), err, socket_errno_text(err));
    close(*fd);
    *fd = -1;
    return false;
  }
  if (listen(*fd, 2) < 0) {
    const int err = errno;
    ESP_LOGE(TAG, "%s listen on TCP/%u failed: %s (%d: %s)",
             label, (unsigned) port, socket_errno_name(err), err, socket_errno_text(err));
    close(*fd);
    *fd = -1;
    return false;
  }
  return true;
}

void SipTransport::request_tcp_client_close_() {
  LockGuard send_lock(this->tcp_send_mutex_);
  const int socket = this->tcp_flow_.socket.load(std::memory_order_acquire);
  this->tcp_flow_.connect_requested.store(false, std::memory_order_release);
  {
    LockGuard lock(this->tcp_tx_pending_mutex_);
    reset_string(this->tcp_flow_.pending);
  }
  this->tcp_flow_.close_requested.store(true, std::memory_order_release);
  if (socket >= 0) shutdown(socket, SHUT_RDWR);
  this->wake_sip_task_();
}

void SipTransport::close_tcp_flow_(SipTcpFlow &flow) {
  LockGuard send_lock(this->tcp_send_mutex_);
  flow.clear_tx();
  const int socket = flow.socket.exchange(-1, std::memory_order_acq_rel);
  flow.close_requested.store(false, std::memory_order_release);
  flow.peer_ip.store(0, std::memory_order_release);
  if (socket >= 0) close(socket);
  if (flow.connecting_socket >= 0) close(flow.connecting_socket);
  flow.connecting_socket = -1;
  flow.connect_deadline = 0;
  flow.connecting_ip = 0;
  flow.connecting_port = 0;
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  flow.ingress_deadline = 0;
  flow.received_record = false;
#endif
  reset_string(flow.rx_buffer);
}

void SipTransport::close_tcp_client_from_sip_task_() {
  this->close_tcp_flow_(this->tcp_flow_);
}

bool SipTransport::close_tcp_dialog_() {
  const bool dialog_lost = this->remote_sip_tcp_.load(std::memory_order_acquire) &&
      (!this->call_id_.empty() || this->outgoing_invite_pending_.load(std::memory_order_acquire) ||
       this->media_active_.load(std::memory_order_acquire) || this->terminal_transaction_pending_locked_());
  this->close_tcp_client_from_sip_task_();
  this->tcp_flow_.connect_requested.store(false, std::memory_order_release);
  { LockGuard pending_lock(this->tcp_tx_pending_mutex_); reset_string(this->tcp_flow_.pending); }
    // Completed transactions intentionally survive an ordinary dialog reset
    // so UDP retransmissions can still be answered. A dead TCP connection is
    // different: records tied to that byte stream can neither be completed nor
    // replayed, and retaining awaiting_ack would freeze the next call for 64*T1.
    if (!this->completed_invite_.empty() &&
        !this->completed_invite_.udp) {
      this->completed_invite_.clear();
      this->terminate_after_invite_ack_ = false;
    }
    if (!this->completed_control_.empty() &&
        !this->completed_control_.udp) {
      this->completed_control_.clear();
    }
    if (!this->completed_invite_client_.empty() &&
        !this->completed_invite_client_.udp) {
      this->completed_invite_client_.clear();
    }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
    if (!this->completed_video_direction_invite_.empty() &&
        !this->completed_video_direction_invite_.udp) {
      this->completed_video_direction_invite_.clear();
    }
#endif
  if (dialog_lost && this->end_refer_invite_usage_(false)) return false;
  this->remote_sip_tcp_.store(false, std::memory_order_release);
  if (dialog_lost) this->reset_dialog_();
  return dialog_lost;
}

void SipTransport::handle_tcp_peer_loss_(SipTcpFlow *flow, uint32_t expired_deadline) {
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  if (flow == &this->inbound_tcp_flow_) {
    LockGuard lock(this->dialog_mutex_);
    if (expired_deadline != 0 && flow->ingress_deadline.load(std::memory_order_acquire) != expired_deadline) return;
    this->close_tcp_flow_(*flow);
    return;
  }
#endif
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  const int account = this->registration_flow_index_(flow);
  if (account >= 0) {
    bool dialog_lost = false;
    {
      LockGuard lock(this->dialog_mutex_);
      // A main-loop account replacement has already retired this old flow.
      // Its delayed EOF must neither cancel a newly queued record nor apply
      // failure/backoff to the replacement account now occupying the slot.
      if (flow->close_requested.load(std::memory_order_acquire)) {
        this->close_tcp_flow_(*flow);
        return;
      }
      const int lost_socket = flow->socket.load(std::memory_order_acquire);
      this->retire_registration_flow_(account);
      this->close_tcp_flow_(*flow);
      this->registration_.connection_lost(account, millis());
#if defined(USE_ESPHOME_VOIP_MWI) || defined(USE_ESPHOME_VOIP_BLF)
      this->mwi_.connection_lost(account, millis());
#endif

      if (this->refer_dialog_ && this->refer_dialog_->active_account_ == account &&
          !this->refer_dialog_->call_id_.empty() &&
          (this->refer_dialog_->dialog_account_socket_ < 0 ||
           this->refer_dialog_->dialog_account_socket_ == lost_socket)) {
        ReferScope scope(*this);
        if (this->close_tcp_dialog_()) {
          SipSignal signal;
          signal.type = SipSignalType::PROTOCOL_ERROR;
          signal.status_code = 503;
          this->emit_sip_signal_(std::move(signal));
        }
      }
      this->pump_refer_();
      if (this->active_account_ == account && !this->call_id_.empty() &&
          (this->dialog_account_socket_ < 0 || this->dialog_account_socket_ == lost_socket))
        dialog_lost = this->close_tcp_dialog_();
    }
    if (dialog_lost) this->emit_connection_change_(false);
    return;
  }
#endif
  bool dialog_lost = false;
  {
    LockGuard lock(this->dialog_mutex_);
#ifdef USE_ESPHOME_VOIP_REGISTRATION
    if (expired_deadline != 0 && flow->ingress_deadline.load(std::memory_order_acquire) != expired_deadline) return;
    if (flow == &this->tcp_flow_ && this->active_account_ >= 0 &&
        this->dialog_account_socket_ != flow->socket.load(std::memory_order_acquire)) {
      // An unrelated inbound TCP peer must never end an account-owned dialog.
      this->close_tcp_flow_(*flow);
      return;
    }
#endif
    if (this->refer_dialog_ && flow == &this->refer_dialog_->tcp_flow_) {
      {
        ReferScope scope(*this);
        if (this->close_tcp_dialog_()) {
          SipSignal signal;
          signal.type = SipSignalType::PROTOCOL_ERROR;
          signal.status_code = 503;
          this->emit_sip_signal_(std::move(signal));
        }
      }
      this->pump_refer_();
    } else {
      dialog_lost = this->close_tcp_dialog_();
    }
  }
  if (dialog_lost) this->emit_connection_change_(false);
}

SipTransport::SipTcpFlow *SipTransport::tcp_flow_for_socket_(int socket) {
  if (socket < 0) return nullptr;
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  if (this->inbound_tcp_flow_.socket.load(std::memory_order_acquire) == socket) return &this->inbound_tcp_flow_;
  for (auto &flow : this->registration_flows_)
    if (flow.socket.load(std::memory_order_acquire) == socket) return &flow;
#endif
  if (this->tcp_flow_.socket.load(std::memory_order_acquire) == socket) return &this->tcp_flow_;
  if (this->refer_dialog_ && this->refer_dialog_->tcp_flow_.socket.load(std::memory_order_acquire) == socket)
    return &this->refer_dialog_->tcp_flow_;
  return nullptr;
}

void SipTransport::release_refer_dialog_() {
  if (!this->refer_dialog_) return;
  this->close_tcp_flow_(this->refer_dialog_->tcp_flow_);
  this->refer_dialog_.reset();
  this->refer_media_waiting_.store(false, std::memory_order_release);
}

void SipTransport::fail_tcp_flow_(SipTcpFlow &flow, int error) {
  char ip[16];
  struct in_addr address{};
  address.s_addr = htonl(flow.connecting_ip);
  inet_ntoa_r(address, ip, sizeof(ip));
  ESP_LOGW(TAG, "SIP TCP connect to %s:%u failed: %s (%d: %s)", ip,
           (unsigned) flow.connecting_port, socket_errno_name(error), error, socket_errno_text(error));
  this->handle_tcp_peer_loss_(&flow);
}

void SipTransport::promote_tcp_flow_(SipTcpFlow &flow) {
  bool sent = true;
  {
    LockGuard send_lock(this->tcp_send_mutex_);
    flow.socket.store(flow.connecting_socket, std::memory_order_release);
    flow.peer_ip.store(flow.connecting_ip, std::memory_order_release);
    flow.close_requested.store(false, std::memory_order_release);
    flow.connect_requested.store(false, std::memory_order_release);
    reset_string(flow.rx_buffer);
    std::string pending;
    { LockGuard pending_lock(this->tcp_tx_pending_mutex_); pending = flow.pending.str(); flow.pending.clear(); }
    if (!pending.empty()) sent = this->send_sip_tcp_record_(pending, flow.connecting_socket);
    flow.connecting_socket = -1;
    flow.connect_deadline = 0;
    flow.connecting_ip = 0;
    flow.connecting_port = 0;
  }
  if (!sent) this->handle_tcp_peer_loss_(&flow);
}

void SipTransport::prepare_tcp_flow_(SipTcpFlow &flow) {
  if (flow.close_requested.load(std::memory_order_acquire)) this->close_tcp_flow_(flow);
  if (!flow.connect_requested.load(std::memory_order_acquire)) return;
  this->close_tcp_flow_(flow);
  flow.connecting_ip = flow.target_ip.load(std::memory_order_acquire);
  flow.connecting_port = flow.target_port.load(std::memory_order_acquire);
  if (flow.connecting_ip == 0 || flow.connecting_port == 0) {
    this->fail_tcp_flow_(flow, EINVAL);
    return;
  }
  flow.connecting_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (flow.connecting_socket < 0) {
    this->fail_tcp_flow_(flow, errno);
    return;
  }
  int opt = 1;
  setsockopt(flow.connecting_socket, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
  const int flags = fcntl(flow.connecting_socket, F_GETFL, 0);
  fcntl(flow.connecting_socket, F_SETFL, flags | O_NONBLOCK);
  struct sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_addr.s_addr = htonl(flow.connecting_ip);
  target.sin_port = htons(flow.connecting_port);
  const int result = connect(flow.connecting_socket, reinterpret_cast<struct sockaddr *>(&target), sizeof(target));
  if (result == 0) {
    this->promote_tcp_flow_(flow);
  } else if (errno == EINPROGRESS) {
    flow.connect_deadline = millis() + 2000;
    flow.connect_requested.store(false, std::memory_order_release);
  } else {
    this->fail_tcp_flow_(flow, errno);
  }
}

void SipTransport::wake_sip_task_() {
  if (this->sip_task_handle_ != nullptr) {
    xTaskNotifyGive(this->sip_task_handle_);
  }
  const int socket = this->sip_wake_socket_;
  const uint16_t port = this->sip_wake_port_;
  if (socket < 0 || port == 0) return;
  struct sockaddr_in self{};
  self.sin_family = AF_INET;
  self.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  self.sin_port = htons(port);
  constexpr uint8_t WAKE_BYTE = 1;
  if (sendto(socket, &WAKE_BYTE, sizeof(WAKE_BYTE), 0,
             reinterpret_cast<struct sockaddr *>(&self), sizeof(self)) < 0 &&
      !this->running_.load(std::memory_order_acquire)) {
    // During teardown a failed loopback send must not strand select(). The
    // private wake socket is recreated on the next transport start.
    shutdown(socket, SHUT_RDWR);
  }
}

void SipTransport::wake_rtp_task_() {
  const int socket = this->rtp_socket_;
  if (socket >= 0) {
    // The active task is blocked in select(), so wake it through the socket.
    // Also giving a task notification here would survive the inner media loop
    // and produce a second, false quiesced completion before the task parks.
    struct sockaddr_in self{};
    self.sin_family = AF_INET;
    self.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    self.sin_port = htons(this->rtp_port_);
    constexpr uint8_t WAKE_BYTE = 1;
    if (sendto(socket, &WAKE_BYTE, sizeof(WAKE_BYTE), 0,
               reinterpret_cast<struct sockaddr *>(&self), sizeof(self)) < 0) {
      // No periodic timeout is needed: force select() awake and let the RTP
      // worker close this per-call socket after it has left recv.
      shutdown(socket, SHUT_RDWR);
    }
  } else if (this->rtp_task_handle_ != nullptr) {
    // No socket means the task is parked in ulTaskNotifyTake().
    xTaskNotifyGive(this->rtp_task_handle_);
  }
}

bool SipTransport::local_ip_for_peer_(uint32_t peer_ip_v4, std::string *out) const {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  bool ok = false;
  if (fd >= 0) {
    struct sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(9);
    peer.sin_addr.s_addr = htonl(peer_ip_v4);
    if (connect(fd, reinterpret_cast<struct sockaddr *>(&peer), sizeof(peer)) == 0) {
      struct sockaddr_in local{};
      socklen_t len = sizeof(local);
      if (getsockname(fd, reinterpret_cast<struct sockaddr *>(&local), &len) == 0 &&
          local.sin_addr.s_addr != 0) {
        char ip[16];
        inet_ntoa_r(local.sin_addr, ip, sizeof(ip));
        *out = ip;
        ok = true;
      }
    }
    close(fd);
  }
  if (ok) return true;
  char ip[network::IP_ADDRESS_BUFFER_SIZE];
  for (auto &address : network::get_ip_addresses()) {
    if (!address.is_ip4()) continue;
    address.str_to(ip);
    if (std::strcmp(ip, "0.0.0.0") != 0) {
      *out = ip;
      ESP_LOGD(TAG, "SIP local IP selected %s for peer %08x", ip, (unsigned) peer_ip_v4);
      return true;
    }
  }
  return false;
}

bool SipTransport::send_sip_(const std::string &message, uint32_t ip_v4, uint16_t port) {
  return this->send_sip_bytes_(message.data(), message.size(), ip_v4, port);
}

bool SipTransport::send_sip_(const SipSignalingString &message, uint32_t ip_v4,
                             uint16_t port) {
  return this->send_sip_bytes_(message.data(), message.size(), ip_v4, port);
}

bool SipTransport::send_sip_bytes_(const char *data, size_t size,
                                   uint32_t ip_v4, uint16_t port) {
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  if (this->dispatch_socket_ != -2 &&
      ((size >= 8 && std::memcmp(data, "SIP/2.0 ", 8) == 0) ||
       (size >= 4 && std::memcmp(data, "ACK ", 4) == 0))) {
    return this->dispatch_is_udp_() ? this->send_sip_udp_bytes_(data, size, ip_v4, port)
                                    : this->send_sip_tcp_bytes_(data, size);
  }
#endif
  if (this->remote_sip_tcp_.load(std::memory_order_acquire)) {
    return this->send_sip_tcp_bytes_(data, size);
  }
  return this->send_sip_udp_bytes_(data, size, ip_v4, port);
}

bool SipTransport::send_sip_udp_bytes_(const char *data, size_t size,
                                      uint32_t ip_v4, uint16_t port) {
  if (this->sip_socket_ < 0 || ip_v4 == 0 || port == 0) return false;
  struct sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_addr.s_addr = htonl(ip_v4);
  dest.sin_port = htons(port);
  constexpr uint8_t MAX_ATTEMPTS = 3;
  int sent = -1;
  int err = 0;
  for (uint8_t attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
    sent = sendto(this->sip_socket_, data, size, 0,
                  reinterpret_cast<struct sockaddr *>(&dest), sizeof(dest));
    if (sent == static_cast<int>(size)) break;
    err = errno;
    if (!socket_errno_is_transient_tx_pressure(err) || attempt + 1 == MAX_ATTEMPTS) break;
    ESP_LOGW(TAG, "SIP TX deferred by %s; retrying (%u/%u)", socket_errno_name(err),
             static_cast<unsigned>(attempt + 1), static_cast<unsigned>(MAX_ATTEMPTS));
    delay(2);
  }
  if (sent != static_cast<int>(size)) {
    ESP_LOGW(TAG, "SIP TX failed: %s (%d: %s)", socket_errno_name(err), err,
             socket_errno_text(err));
    return false;
  }
#ifdef ESPHOME_LOG_HAS_DEBUG
  char ip[16];
  struct in_addr a{};
  a.s_addr = htonl(ip_v4);
  inet_ntoa_r(a, ip, sizeof(ip));
  ESP_LOGD(TAG, "SIP TX %u bytes to %s:%u", (unsigned) size, ip, (unsigned) port);
#endif
  return true;
}

bool SipTransport::send_sip_tcp_bytes_(const char *data, size_t size) {
  // SIP commands can originate from the component/main task while the SIP
  // task sends responses. Serialize the complete record so partial TCP writes
  // from two callers can never interleave into an invalid SIP message.
  LockGuard send_lock(this->tcp_send_mutex_);
  if (size == 0) return false;
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  if (this->dispatch_socket_ >= 0 &&
      ((size >= 8 && std::memcmp(data, "SIP/2.0 ", 8) == 0) ||
       (size >= 4 && std::memcmp(data, "ACK ", 4) == 0)))
    return this->send_sip_tcp_record_(data, size, this->dispatch_socket_);
  if (this->active_account_ >= 0) {
    const int socket = this->dialog_account_socket_ >= 0 ? this->dialog_account_socket_ :
        this->registration_flows_[this->active_account_].socket.load(std::memory_order_acquire);
    return this->send_sip_tcp_record_(data, size, socket);
  }
#endif
  const bool replacing_session =
      this->tcp_flow_.close_requested.load(std::memory_order_acquire) ||
      this->tcp_flow_.connect_requested.load(std::memory_order_acquire);
  const int socket = this->tcp_flow_.socket.load(std::memory_order_acquire);
  if (socket < 0 || replacing_session) {
    if (this->remote_sip_tcp_.load(std::memory_order_acquire)) {
      {
        LockGuard lock(this->tcp_tx_pending_mutex_);
        if (!this->tcp_flow_.pending.empty()) {
          ESP_LOGW(TAG, "SIP TCP connect still pending; refusing to replace queued %s with a newer request",
                   this->tcp_flow_.pending.rfind("INVITE ", 0) == 0 ? "INVITE" : "message");
          return false;
        }
        this->tcp_flow_.pending.assign(data, size);
      }
      this->wake_sip_task_();
      return true;
    }
    return false;
  }
  return this->send_sip_tcp_record_(data, size, socket);
}

bool SipTransport::send_sip_tcp_record_(const std::string &message, int socket) {
  return this->send_sip_tcp_record_(message.data(), message.size(), socket);
}

bool SipTransport::send_sip_tcp_record_(const char *data, size_t size,
                                        int socket) {
  // Caller owns tcp_send_mutex_. The fast path remains allocation-free; only
  // backpressure retains bytes in the existing flow owner. Append whole later
  // records after the unfinished record, never interleave their bytes.
  if (size == 0 || size > SipTcpFlow::TX_PENDING_LIMIT || socket < 0) return false;
  auto *flow = this->tcp_flow_for_socket_(socket);
  if (flow != nullptr && !flow->tx_pending.empty()) {
    if (!this->flush_tcp_tx_locked_(*flow)) return false;
    if (!flow->tx_pending.empty()) {
      const size_t remaining = flow->tx_pending.size() - flow->tx_offset;
      if (size > SipTcpFlow::TX_PENDING_LIMIT - remaining) {
        ESP_LOGW(TAG, "SIP TCP pending write budget exhausted");
        return false;
      }
      if (flow->tx_offset != 0) {
        flow->tx_pending.erase(0, flow->tx_offset);
        flow->tx_offset = 0;
      }
      flow->tx_pending.append(data, size);
      this->wake_sip_task_();
      return true;
    }
  }
  size_t offset = 0;
  if (!this->write_sip_tcp_record_(data, size, &offset, socket)) return false;
  if (offset == size) return true;
  if (flow == nullptr) {
    // Every production signaling socket has a flow owner. An unowned stream
    // cannot retain a partial record safely.
    if (offset != 0) shutdown(socket, SHUT_RDWR);
    return false;
  }
  flow->tx_pending.reserve(SipTcpFlow::TX_PENDING_LIMIT);
  flow->tx_pending.assign(data + offset, size - offset);
  flow->tx_offset = 0;
  uint32_t deadline = millis() + SIP_TRANSACTION_TIMEOUT_MS;
  flow->tx_deadline.store(deadline != 0 ? deadline : 1, std::memory_order_release);
  this->wake_sip_task_();
  return true;
}

bool SipTransport::write_sip_tcp_record_(const char *data, size_t size,
                                         size_t *offset, int socket) {
  while (*offset < size) {
    const int sent = send(socket, data + *offset, size - *offset, 0);
    if (sent <= 0) {
      const int err = errno;
      if (sent < 0 && err == EINTR) continue;
      if (sent < 0 && (err == EAGAIN || err == EWOULDBLOCK)) return true;
      ESP_LOGW(TAG, "SIP TCP TX failed: %s (%d: %s)", socket_errno_name(err), err, socket_errno_text(err));
      shutdown(socket, SHUT_RDWR);
      return false;
    }
    *offset += static_cast<size_t>(sent);
  }
  return true;
}

bool SipTransport::flush_tcp_tx_locked_(SipTcpFlow &flow) {
  if (flow.tx_pending.empty()) return true;
  const int socket = flow.socket.load(std::memory_order_acquire);
  const uint32_t deadline = flow.tx_deadline.load(std::memory_order_acquire);
  if (socket < 0 || (deadline != 0 && time_reached(millis(), deadline))) {
    ESP_LOGW(TAG, "SIP TCP pending write expired");
    if (socket >= 0) shutdown(socket, SHUT_RDWR);
    flow.clear_tx();
    return false;
  }
  if (!this->write_sip_tcp_record_(flow.tx_pending.data(), flow.tx_pending.size(),
                                  &flow.tx_offset, socket)) {
    flow.clear_tx();
    return false;
  }
  if (flow.tx_offset == flow.tx_pending.size()) flow.clear_tx();
  return true;
}

bool SipTransport::pump_tcp_tx_(SipTcpFlow &flow, bool writable) {
  {
    LockGuard send_lock(this->tcp_send_mutex_);
    const uint32_t deadline = flow.tx_deadline.load(std::memory_order_acquire);
    if (deadline == 0 || (!writable && !time_reached(millis(), deadline))) return true;
    if (!this->flush_tcp_tx_locked_(flow)) return false;
  }
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  // A stateless response can outlive its last read under backpressure. Return
  // the bounded spare only after its bytes were sent, using normal idle checks.
  // Acquire dialog ownership after releasing the send lock, in the usual order.
  if (&flow == &this->inbound_tcp_flow_ && flow.tx_deadline.load(std::memory_order_acquire) == 0) {
    LockGuard lock(this->dialog_mutex_);
    this->reclaim_idle_inbound_tcp_(flow);
  }
#endif
  return true;
}

void SipTransport::handle_sip_stream_(int socket, const sockaddr_in &src) {
#ifdef USE_ESPHOME_VOIP_REGISTRATION

  char *const buf = this->sip_rx_scratch_.data();
  auto *flow = this->tcp_flow_for_socket_(socket);
  if (flow == nullptr) return;
  auto drop_tcp_stream = [&](const char *reason) {
    char ip[16];
    inet_ntoa_r(src.sin_addr, ip, sizeof(ip));
    ESP_LOGW(TAG, "%s from %s, dropping connection", reason, ip);
    this->handle_tcp_peer_loss_(flow);
  };
  size_t bytes_read = 0;
  while (true) {
    flow = this->tcp_flow_for_socket_(socket);
    if (flow == nullptr) return;
    // Ignore CRLF before a stream start-line (RFC 3261 section 7.5), including
    // across reads. Consume only that prefix, never bytes inside a SIP body.
    size_t prefix = 0;
    while (prefix + 1 < flow->rx_buffer.size() && flow->rx_buffer.data()[prefix] == '\r' &&
           flow->rx_buffer.data()[prefix + 1] == '\n')
      prefix += 2;
    if (prefix != 0) flow->rx_buffer.erase(0, prefix);
    const size_t sep = flow->rx_buffer.find("\r\n\r\n");
    size_t total = 0;
    if (sep != std::string::npos) {
      size_t body_len = 0;
      if (!sip_content_length(flow->rx_buffer, &body_len)) {
        drop_tcp_stream("SIP TCP invalid or ambiguous Content-Length");
        return;
      }
      if (body_len > MAX_SIP_BODY_BYTES) {
        drop_tcp_stream("SIP TCP Content-Length exceeds limit");
        return;
      }
      total = sep + 4 + body_len;
      if (total > MAX_SIP_TCP_RX_BUFFER) {
        drop_tcp_stream("SIP TCP framed message exceeds RX buffer");
        return;
      }
    }
    if (total == 0 || flow->rx_buffer.size() < total) {
      if (flow->rx_buffer.size() >= MAX_SIP_TCP_RX_BUFFER) {
        drop_tcp_stream("SIP TCP RX buffer overflow");
        return;
      }
      // Parse each chunk before reading again. A coalesced burst must not
      // count against the per-record limit or monopolize the SIP worker.
      if (bytes_read >= MAX_SIP_TCP_RX_BUFFER) return;
      const size_t available = MAX_SIP_TCP_RX_BUFFER - flow->rx_buffer.size();
      const size_t count = std::min<size_t>(1024, std::min(available, MAX_SIP_TCP_RX_BUFFER - bytes_read));
      const int n = recv(socket, buf, count, 0);
      if (n > 0) {
        flow->rx_buffer.append(buf, static_cast<size_t>(n));
        bytes_read += static_cast<size_t>(n);
        continue;
      }
      if (n == 0) {
        ESP_LOGI(TAG, "SIP TCP peer closed");
        this->handle_tcp_peer_loss_(flow);
        return;
      }
      const int err = errno;
      if (err == EINTR) continue;
      if (err == EWOULDBLOCK || err == EAGAIN || err == ENOTCONN || err == EINPROGRESS || err == EALREADY) {
        // Both unread requests and retained response bytes still own the spare.
        if (flow == &this->inbound_tcp_flow_ && flow->rx_buffer.empty() &&
            flow->tx_deadline.load(std::memory_order_acquire) == 0) this->close_tcp_flow_(*flow);
        return;
      }
      ESP_LOGW(TAG, "SIP TCP RX failed: %s (%d: %s)", socket_errno_name(err), err, socket_errno_text(err));
      this->handle_tcp_peer_loss_(flow);
      return;
    }
    const std::string msg = flow->rx_buffer.substr(0, total);
    flow->rx_buffer.erase(0, total);
    flow->received_record = true;
    {
      LockGuard lock(this->dialog_mutex_);
      this->dispatch_socket_ = socket;
      if (flow == &this->inbound_tcp_flow_ && msg.rfind("INVITE ", 0) == 0) {
        bool owned = this->media_active_.load(std::memory_order_acquire) ||
                     this->outgoing_invite_pending_.load(std::memory_order_acquire) ||
                     this->terminal_transaction_pending_locked_();
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
        owned = owned || this->pending_video_direction_invite_.pending();
#endif
        if (this->call_id_.empty() && (owned || !this->should_accept_session_())) {
          this->send_stateless_response_(msg, src, 503, "Service Unavailable", "retry_new_connection", false, 1);
          this->dispatch_socket_ = -2;
          // Drain already framed control records through the same path. The
          // spare is released at read exhaustion or after a retained TX flush.
          continue;
        }
      }
      this->handle_sip_datagram_unlocked_(msg.data(), msg.size(), src);
      this->dispatch_socket_ = -2;
    }
    // The authorized INVITE may have exchanged the two storage slots.
    // Reacquire by socket before the next record or read.
  }
#else

  char *const buf = this->sip_rx_scratch_.data();
  auto *flow = this->tcp_flow_for_socket_(socket);
  if (flow == nullptr) return;
  auto drop_tcp_stream = [&](const char *reason) {
    char ip[16];
    inet_ntoa_r(src.sin_addr, ip, sizeof(ip));
    ESP_LOGW(TAG, "%s from %s, dropping connection", reason, ip);
    this->handle_tcp_peer_loss_(flow);
  };
  bool peer_closed = false;
  while (true) {
    const int n = recv(socket, buf, 1024, 0);
    if (n > 0) {
      flow->rx_buffer.append(buf, static_cast<size_t>(n));
      if (flow->rx_buffer.size() > MAX_SIP_TCP_RX_BUFFER) {
        drop_tcp_stream("SIP TCP RX buffer overflow");
        return;
      }
      continue;
    }
    if (n == 0) {
      ESP_LOGI(TAG, "SIP TCP peer closed");
      peer_closed = true;
      break;
    }
    const int err = errno;
    if (err == EINTR) continue;
    if (err == EWOULDBLOCK || err == EAGAIN || err == ENOTCONN || err == EINPROGRESS || err == EALREADY) break;
    ESP_LOGW(TAG, "SIP TCP RX failed: %s (%d: %s)", socket_errno_name(err), err, socket_errno_text(err));
    peer_closed = true;
    break;
  }

  while (true) {
    flow = this->tcp_flow_for_socket_(socket);
    if (flow == nullptr) return;
    const size_t sep = flow->rx_buffer.find("\r\n\r\n");
    if (sep == std::string::npos) {
      if (peer_closed) this->handle_tcp_peer_loss_(flow);
      return;
    }
    size_t body_len = 0;
    if (!sip_content_length(flow->rx_buffer, &body_len)) {
      drop_tcp_stream("SIP TCP invalid or ambiguous Content-Length");
      return;
    }
    if (body_len > MAX_SIP_BODY_BYTES) {
      drop_tcp_stream("SIP TCP Content-Length exceeds limit");
      return;
    }
    const size_t total = sep + 4 + body_len;
    if (total > MAX_SIP_TCP_RX_BUFFER) {
      drop_tcp_stream("SIP TCP framed message exceeds RX buffer");
      return;
    }
    if (flow->rx_buffer.size() < total) {
      if (peer_closed) this->handle_tcp_peer_loss_(flow);
      return;
    }
    const std::string msg = flow->rx_buffer.substr(0, total);
    flow->rx_buffer.erase(0, total);
#ifdef USE_ESPHOME_VOIP_REGISTRATION
    this->dispatch_socket_ = socket;
#endif
    this->handle_sip_datagram_(msg.data(), msg.size(), src);
#ifdef USE_ESPHOME_VOIP_REGISTRATION
    this->dispatch_socket_ = -1;
#endif
  }
#endif
}

void SipTransport::handle_sip_udp_datagram_(char *buf, int n, const sockaddr_in &src) {
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  this->handle_sip_datagram_(buf, static_cast<size_t>(n), src);
#else

#ifdef USE_ESPHOME_VOIP_REGISTRATION
  {
    LockGuard lock(this->dialog_mutex_);
    if (this->registration_ingress_(std::string(buf, n), src, -1)) return;
  }
#endif
  // Retired transactions retain their own transport after a mixed handoff.
  // Replay before the active-dialog gate, without retargeting that dialog.
  {
    LockGuard lock(this->dialog_mutex_);
    const std::string message(buf, static_cast<size_t>(n));
    const std::string method = message.substr(0, message.find(' '));
    if (this->replay_completed_response_(message, src, method, true) ||
        this->replay_completed_invite_ack_(message, src, true)) return;
  }
  bool tcp_call_active = false;
  {
    LockGuard lock(this->dialog_mutex_);
    tcp_call_active =
        !this->call_id_.empty() ||
        this->outgoing_invite_pending_.load(std::memory_order_acquire) ||
        this->media_active_.load(std::memory_order_acquire) ||
        this->terminal_transaction_pending_locked_();
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
    tcp_call_active =
        tcp_call_active ||
        this->pending_video_direction_invite_.pending();
#endif
  }
  bool tcp_session_active =
      tcp_call_active && this->remote_sip_tcp_.load(std::memory_order_acquire) &&
      (this->tcp_flow_.socket.load(std::memory_order_acquire) >= 0 || this->tcp_flow_.connecting_socket >= 0 ||
       this->tcp_flow_.connect_requested.load(std::memory_order_acquire));
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  tcp_session_active = tcp_session_active ||
      (tcp_call_active && this->remote_sip_tcp_.load(std::memory_order_acquire) && this->active_account_ >= 0 &&
       (this->dialog_account_socket_ >= 0 ||
        this->registration_flows_[this->active_account_].socket.load(std::memory_order_acquire) >= 0));
#endif
  bool refer_udp = false;
  {
    LockGuard lock(this->dialog_mutex_);
    refer_udp = this->refer_dialog_ && !this->refer_dialog_->remote_sip_tcp_ &&
        header_value(std::string(buf, static_cast<size_t>(n)), "Call-ID") == this->refer_wire_id_;
  }
  if (tcp_session_active && !refer_udp) {
#ifdef USE_ESPHOME_VOIP_REGISTRATION
    LockGuard lock(this->dialog_mutex_);
    const std::string message(buf, n);
    if (message.rfind("INVITE ", 0) == 0) this->send_stateless_response_(message, src, 486, "Busy Here");
#endif
    ESP_LOGD(TAG, "SIP UDP datagram ignored while TCP signaling is active");
    return;
  }
  if (!refer_udp && this->tcp_flow_.socket.load(std::memory_order_acquire) >= 0) {
    // An idle keep-alive connection must not coexist with a new UDP
    // dialog, otherwise later TCP traffic could retarget the call.
    this->close_tcp_client_from_sip_task_();
  }
  if (!refer_udp) this->remote_sip_tcp_.store(false, std::memory_order_release);
  buf[n] = 0;
  char ip[16];
  inet_ntoa_r(src.sin_addr, ip, sizeof(ip));
  ESP_LOGI(TAG, "SIP UDP RX %d bytes from %s:%u", n, ip, (unsigned) ntohs(src.sin_port));
  this->handle_sip_datagram_(reinterpret_cast<const char *>(buf), static_cast<size_t>(n), src);
#endif
}

#ifdef USE_ESPHOME_VOIP_REGISTRATION
bool SipTransport::park_inbound_tcp_(int socket, const sockaddr_in &source) {
  if (this->inbound_tcp_flow_.socket.load(std::memory_order_acquire) >= 0) return false;
  // Admission reserves storage only. The normal SIP handler authorizes the
  // complete request before it can claim the single dialog owner.
  this->inbound_tcp_flow_.socket.store(socket, std::memory_order_release);
  this->inbound_tcp_flow_.peer_ip.store(ntohl(source.sin_addr.s_addr), std::memory_order_release);
  this->inbound_tcp_flow_.rx_buffer.clear();
  this->inbound_tcp_flow_.received_record = false;
  this->inbound_tcp_flow_.ingress_deadline = millis() + SIP_TRANSACTION_TIMEOUT_MS;
  return true;
}

bool SipTransport::claim_inbound_tcp_(int socket) {
  auto *flow = this->tcp_flow_for_socket_(socket);
  if (flow == nullptr || !this->call_id_.empty() ||
      this->media_active_.load(std::memory_order_acquire) ||
      this->outgoing_invite_pending_.load(std::memory_order_acquire) ||
      this->terminal_transaction_pending_locked_() || !this->should_accept_session_()) return false;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  if (this->pending_video_direction_invite_.pending()) return false;
#endif
  if (flow == &this->inbound_tcp_flow_) {
    // Called with the dialog mutex held after INVITE validation and account
    // authorization. Preserve the prior stream, including unread records.
    this->tcp_flow_.swap(this->inbound_tcp_flow_);
  }
  this->remote_sip_tcp_.store(true, std::memory_order_release);
  return true;
}

bool SipTransport::reclaim_idle_inbound_tcp_(SipTcpFlow &flow) {
  // The SIP worker holds dialog_mutex_. A new/unread or partial stream is
  // never evicted merely because another connection arrived.
  const int socket = flow.socket.load(std::memory_order_acquire);
  if (socket < 0 || !flow.received_record || !flow.rx_buffer.empty() ||
      flow.connecting_socket >= 0 || flow.connect_requested.load(std::memory_order_acquire) ||
      flow.close_requested.load(std::memory_order_acquire) ||
      flow.tx_deadline.load(std::memory_order_acquire) != 0 || this->terminal_transaction_pending_locked_())
    return false;
  {
    LockGuard pending_lock(this->tcp_tx_pending_mutex_);
    if (!flow.pending.empty()) return false;
  }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  if (this->pending_video_direction_invite_.pending()) return false;
#endif
  if (&flow == &this->tcp_flow_ && this->remote_sip_tcp_.load(std::memory_order_acquire) &&
      (!this->call_id_.empty() || this->outgoing_invite_pending_.load(std::memory_order_acquire) ||
       this->media_active_.load(std::memory_order_acquire))) {
#ifdef USE_ESPHOME_VOIP_REGISTRATION
    if (this->active_account_ < 0 || this->dialog_account_socket_ == socket)
#endif
      return false;
  }
  char unread;
  const int count = recv(socket, &unread, 1, MSG_PEEK | MSG_DONTWAIT);
  if (count > 0 || (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) return false;
  this->close_tcp_flow_(flow);
  return true;
}

void SipTransport::accept_tcp_client_(int client, const sockaddr_in &src) {
  int opt = 1;
  setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
  int flags = fcntl(client, F_GETFL, 0);
  fcntl(client, F_SETFL, flags | O_NONBLOCK);
  LockGuard dialog_lock(this->dialog_mutex_);
  const uint32_t accepted_ip_v4 = ntohl(src.sin_addr.s_addr);
  const int active_client = this->tcp_flow_.socket.load(std::memory_order_acquire);
  bool signaling_owned = false;
  {
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
  }
  // A registered dialog can own its account flow while the primary slot is
  // empty. Admit control traffic to the same bounded spare in either case.
  if (active_client >= 0 || signaling_owned) {
    if (this->park_inbound_tcp_(client, src)) return;
    if (this->reclaim_idle_inbound_tcp_(this->inbound_tcp_flow_) && this->park_inbound_tcp_(client, src)) return;
    if (this->reclaim_idle_inbound_tcp_(this->tcp_flow_)) {
      this->tcp_flow_.swap(this->inbound_tcp_flow_);
      if (this->park_inbound_tcp_(client, src)) return;
    }
    char ip[16];
    inet_ntoa_r(src.sin_addr, ip, sizeof(ip));
    ESP_LOGW(TAG,
             "SIP TCP accept rejected: signaling transport owned by %s",
             ip);
    close(client);
    return;
  }
  // A connection is not a new dialog. The framed INVITE handler applies the
  // application admission gate; unrelated control traffic keeps its owner.
  this->close_tcp_client_from_sip_task_();
  this->tcp_flow_.socket.store(client, std::memory_order_release);
  this->tcp_flow_.peer_ip.store(accepted_ip_v4, std::memory_order_release);
  this->tcp_flow_.close_requested.store(false, std::memory_order_release);
  this->tcp_flow_.rx_buffer.clear();
  this->tcp_flow_.received_record = false;
  this->tcp_flow_.ingress_deadline = millis() + SIP_TRANSACTION_TIMEOUT_MS;
  char ip[16];
  inet_ntoa_r(src.sin_addr, ip, sizeof(ip));
  ESP_LOGI(TAG, "SIP TCP accepted from %s:%u", ip, (unsigned) ntohs(src.sin_port));

}
#endif

}  // namespace voip_stack
}  // namespace esphome

#endif
