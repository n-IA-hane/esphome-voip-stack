#include "sip_transport_internal.h"

#if defined(USE_ESP32) && defined(USE_ESPHOME_VOIP_SIP_TRANSPORT)

namespace esphome {
namespace voip_stack {

void SipTransport::clear_invite_transaction_() {
  this->pending_invite_.clear();
}

void SipTransport::clear_bye_transaction_() {
  this->pending_bye_.clear();
}

void SipTransport::clear_udp_transactions_() {
  this->pending_invite_.clear();
  this->pending_cancel_.clear();
  this->pending_bye_.clear();
  this->pending_update_.clear();
}

void SipTransport::reset_rtp_latch_() {
  this->audio_receive_reset_pending_.store(false, std::memory_order_release);
  this->rtp_source_reacquire_pending_.store(false, std::memory_order_release);
  this->latched_rtp_ip_v4_.store(0, std::memory_order_release);
  this->latched_rtp_port_.store(0, std::memory_order_release);
  this->latched_rtp_ssrc_.store(0, std::memory_order_release);
  this->rtp_ssrc_latched_.store(false, std::memory_order_release);
}

void SipTransport::open_media_session_() {
  if (this->refer_context_active_) return;
  this->media_active_.store(true, std::memory_order_release);
}

void SipTransport::close_media_session_() {
  if (this->refer_context_active_) return;
  this->media_active_.store(false, std::memory_order_release);
#ifdef USE_ESPHOME_VOIP_DTMF
  this->dtmf_generation_.fetch_add(1, std::memory_order_release);
#endif
}

void SipTransport::reset_dialog_() {
  // Callers already own dialog_mutex_. Holding the media lock until every
  // negotiated field is cleared prevents a deferred main-loop start from
  // observing the old video configuration after teardown has completed.
  LockGuard media_lock(this->media_lifecycle_mutex_);
  if (!this->refer_context_active_)
    this->refer_media_waiting_.store(false, std::memory_order_release);
  this->reset_dialog_media_locked_();
}

void SipTransport::reset_dialog_media_locked_() {
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  this->video_imageattr_state_ = 0;
#endif
  if (!this->refer_context_active_) {
    this->request_audio_path_stop_locked_();
#ifdef USE_ESPHOME_VOIP_STACK_OPUS
    // This dialog has relinquished its media; retained REFER media keeps its constraints.
    this->set_opus_tx_constraints_({});
#endif
  }
  bool abort_queued_tcp_record = false;
  {
    // Serialize with promote_tcp_flow_(): once that path swaps the queued
    // record it owns transmission; before the swap a reset can still retract
    // the record without leaving an orphan request at the peer.
    LockGuard send_lock(this->tcp_send_mutex_);
    LockGuard pending_lock(this->tcp_tx_pending_mutex_);
    abort_queued_tcp_record = !this->tcp_flow_.pending.empty();
    reset_string(this->tcp_flow_.pending);
    if (abort_queued_tcp_record) {
      this->tcp_flow_.connect_requested.store(false, std::memory_order_release);
      this->tcp_flow_.close_requested.store(true,
                                                   std::memory_order_release);
    }
  }
  if (abort_queued_tcp_record) this->wake_sip_task_();
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  if (this->tcp_flow_.socket.load(std::memory_order_acquire) >= 0 && this->tcp_flow_.ingress_deadline == 0)
    this->tcp_flow_.ingress_deadline = millis() + SIP_TRANSACTION_TIMEOUT_MS;
  {
    this->active_account_ = -1; this->dialog_digest_ = {}; this->dialog_proxy_digest_ = {};
    this->dialog_auth_attempts_ = 0; this->dialog_proxy_auth_attempts_ = 0;
    this->authenticated_invite_body_.clear(); this->invite_authorization_.clear();
    this->dialog_account_socket_ = -1; this->dialog_route_count_ = 0;
  }
#endif
  reset_string(this->call_id_);
  reset_string(this->local_tag_);
  reset_string(this->remote_tag_);
  reset_string(this->branch_);
  reset_string(this->local_uri_);
  reset_string(this->local_contact_uri_);
  reset_string(this->remote_uri_);
  reset_string(this->remote_target_uri_);
  reset_string(this->last_invite_via_);
  reset_string(this->last_invite_from_);
  reset_string(this->last_invite_to_);
  reset_string(this->last_invite_cseq_);
  reset_string(this->last_invite_response_);
  this->last_invite_cseq_number_ = 0;
  this->remote_dialog_cseq_ = 0;
  this->last_invite_peer_ip_v4_ = 0;
  this->last_invite_peer_port_ = 0;
  this->caller_.clear();
  this->destination_.clear();
  this->peer_supports_from_change_ = false;
  this->connected_identity_sent_ = false;
  this->dialog_originated_ = false;
  this->delayed_offer_pending_ = false;
  this->delayed_offer_reinvite_ = false;
  this->remote_media_shape_.clear();
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  this->video_call_intent_ = {};
  this->next_video_call_intent_ = {};
  this->video_payloads_ = {};
#endif
  this->audio_zero_connection_ = false;
  this->close_media_session_();
  this->outgoing_invite_pending_.store(false, std::memory_order_release);
  this->cancel_requested_.store(false, std::memory_order_release);
  this->terminate_after_invite_ack_ = false;
  this->clear_udp_transactions_();
  this->pending_provisional_response_.clear();
  if (!this->refer_context_active_) this->reset_rtp_latch_();
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  if (!this->refer_context_active_) {
  this->pending_video_direction_invite_.clear();
  reset_string(this->confirmed_local_sdp_);
  this->reset_video_negotiation_();
  this->emit_video_active_state_(false);
  this->emit_video_send_state_(false, false);
  }
#endif
  this->sdp_session_id_ = 0;
  this->sdp_session_version_ = 0;
}

bool SipTransport::terminal_transaction_pending_locked_() const {
  return !this->pending_cancel_.empty() || !this->pending_bye_.empty() ||
         this->completed_invite_.awaiting_ack ||
         this->terminate_after_invite_ack_;
}

void SipTransport::remember_udp_transaction_(const std::string &method, const std::string &message,
                                             uint32_t ip_v4, uint16_t port) {
  if (message.empty() || ip_v4 == 0 || port == 0) {
    return;
  }
  UdpTransaction *txn = nullptr;
  if (method == "NOTIFY") {
    if (!this->refer_dialog_) return;
    txn = this->refer_dialog_->pending_notify_.empty()
        ? &this->refer_dialog_->pending_notify_ : &this->refer_dialog_->pending_notify_final_;
  } else if (method == "INVITE") {
    txn = &this->pending_invite_;
  } else if (method == "CANCEL") {
    txn = &this->pending_cancel_;
  } else if (method == "BYE") {
    txn = &this->pending_bye_;
  } else if (method == "UPDATE") {
    txn = &this->pending_update_;
  }
  if (txn == nullptr) return;
  const uint32_t now = millis();
  txn->request = message;
  txn->ip_v4 = ip_v4;
  txn->port = port;
  txn->interval_ms = SIP_T1_MS;
  txn->next_ms = now + txn->interval_ms;
  txn->deadline_ms = now + SIP_TRANSACTION_TIMEOUT_MS;
  txn->retries = 0;
  txn->udp = !this->remote_sip_tcp_.load(std::memory_order_acquire);
  txn->completed = false;
  this->wake_sip_task_();
}

void SipTransport::defer_udp_provisional_response_(
    const std::string &response, uint32_t ip_v4, uint16_t port) {
  if (response.empty() || ip_v4 == 0 || port == 0) return;
  const uint32_t now = millis();
  this->pending_provisional_response_.response = response;
  this->pending_provisional_response_.ip_v4 = ip_v4;
  this->pending_provisional_response_.port = port;
  this->pending_provisional_response_.interval_ms = 20;
  this->pending_provisional_response_.next_ms = now + 20;
  this->pending_provisional_response_.deadline_ms = now + SIP_T2_MS;
  this->pending_provisional_response_.retries = 0;
  this->wake_sip_task_();
  ESP_LOGW(TAG, "SIP UDP provisional response deferred to event-driven retry");
}

void SipTransport::pump_udp_retransmits_() {
  LockGuard lock(this->dialog_mutex_);
  this->pump_udp_retransmits_unlocked_();
}

void SipTransport::pump_udp_retransmits_unlocked_() {
  const uint32_t now = millis();
  bool reset_terminal_dialog = false;
  bool invite_timed_out = false;
  bool ack_timed_out = false;
  std::string timed_out_call_id;
  auto pump = [this, now, &reset_terminal_dialog, &invite_timed_out,
               &timed_out_call_id](UdpTransaction &txn, const char *method, bool owns_dialog = true, int tcp_socket = -1) {
    if (txn.empty() || txn.ip_v4 == 0 || txn.port == 0) return;
    if (time_reached(now, txn.deadline_ms)) {
      ESP_LOGW(TAG, "SIP %s %s transaction timed out after %u ms",
               txn.udp ? "UDP" : "TCP", method,
               (unsigned) SIP_TRANSACTION_TIMEOUT_MS);
      txn.clear();
      if (!owns_dialog) return;
      if (std::strcmp(method, "INVITE") == 0) {
        invite_timed_out = true;
        timed_out_call_id = this->call_id_;
        this->outgoing_invite_pending_.store(false, std::memory_order_release);
      } else if (std::strcmp(method, "UPDATE") != 0 && std::strcmp(method, "NOTIFY") != 0) {
        reset_terminal_dialog = true;
        timed_out_call_id = this->call_id_;
      }
      return;
    }
    if (!time_reached(now, txn.next_ms)) return;
    if (txn.completed) {
      txn.next_ms = txn.deadline_ms;
      return;
    }
    if (!txn.udp) {
      txn.next_ms = txn.deadline_ms;
      return;
    }
    bool sent;
    if (owns_dialog) sent = this->send_sip_(txn.request, txn.ip_v4, txn.port);
    else if (txn.udp) sent = this->send_sip_udp_bytes_(txn.request.data(), txn.request.size(), txn.ip_v4, txn.port);
    else {
      LockGuard send_lock(this->tcp_send_mutex_);
      sent = this->send_sip_tcp_record_(txn.request.data(), txn.request.size(), tcp_socket);
    }
    txn.retries++;
    if (sent) {
      ESP_LOGD(TAG, "SIP UDP %s retransmit #%u", method, (unsigned) txn.retries);
    } else {
      ESP_LOGW(TAG, "SIP UDP %s retransmit #%u failed", method, (unsigned) txn.retries);
    }
    txn.interval_ms = std::min<uint16_t>(static_cast<uint16_t>(txn.interval_ms * 2), SIP_T2_MS);
    txn.next_ms = now + txn.interval_ms;
    if (time_reached(txn.next_ms, txn.deadline_ms)) txn.next_ms = txn.deadline_ms;
  };

  if (this->outgoing_invite_pending_.load(std::memory_order_acquire)) {
    pump(this->pending_invite_, "INVITE");
  }
  pump(this->pending_cancel_, "CANCEL");
  pump(this->pending_bye_, "BYE");
  pump(this->pending_update_, "UPDATE");
  auto pump_forks = [&](CompletedInviteClientTransaction &completed) {
    for (auto &fork : completed.rejected) pump(fork.bye, "BYE", false, fork.tcp_socket);
    completed.rejected.erase(std::remove_if(completed.rejected.begin(), completed.rejected.end(),
        [now](const auto &fork) { return time_reached(now, fork.expires_ms); }), completed.rejected.end());
  };
  pump_forks(this->completed_invite_client_);
  for (auto &entry : this->completed_refers_) if (entry) pump_forks(entry->invite);
  if (!this->refer_context_active_ && this->refer_dialog_) {
    pump(this->refer_dialog_->pending_notify_, "NOTIFY");
    pump(this->refer_dialog_->pending_notify_final_, "NOTIFY");
  }
  this->pump_refer_();
  auto &provisional = this->pending_provisional_response_;
  if (!provisional.empty() && time_reached(now, provisional.next_ms)) {
    if (time_reached(now, provisional.deadline_ms)) {
      ESP_LOGW(TAG, "SIP UDP provisional response retry expired");
      provisional.clear();
    } else {
      const bool sent = this->send_sip_(provisional.response,
                                        provisional.ip_v4,
                                        provisional.port);
      provisional.retries++;
      if (sent) {
        ESP_LOGI(TAG, "SIP UDP provisional response sent after %u retries",
                 static_cast<unsigned>(provisional.retries));
        provisional.clear();
      } else {
        provisional.interval_ms = std::min<uint16_t>(
            static_cast<uint16_t>(provisional.interval_ms * 2), SIP_T1_MS);
        provisional.next_ms = now + provisional.interval_ms;
        if (time_reached(provisional.next_ms, provisional.deadline_ms)) {
          provisional.next_ms = provisional.deadline_ms;
        }
      }
    }
  }
  if (this->completed_invite_.awaiting_ack) {
    if (time_reached(now, this->completed_invite_.deadline_ms)) {
      ESP_LOGW(TAG, "SIP %s INVITE final response timed out waiting for ACK after %u ms",
               this->completed_invite_.udp ? "UDP" : "TCP",
               (unsigned) SIP_TRANSACTION_TIMEOUT_MS);
      this->completed_invite_.awaiting_ack = false;
      const bool active_2xx_dialog = this->completed_invite_.status < 300 &&
                                     !this->call_id_.empty() &&
                                     this->call_id_ == this->completed_invite_.call_id &&
                                     this->last_invite_cseq_number_ == this->completed_invite_.cseq;
      if (active_2xx_dialog) {
        this->terminate_after_invite_ack_ = false;
        ack_timed_out = true;
        timed_out_call_id = this->call_id_;
      } else if (this->completed_invite_.status >= 300) {
        // Timer H retires this server transaction even when its dialog and
        // media already ended. Wake the application cleanup barrier once.
        this->emit_dialog_status_(SipSignalType::TERMINAL_COMPLETE, 408,
                                  this->completed_invite_.call_id, "ack_timeout",
                                  this->terminal_transaction_pending_locked_());
      }
    } else if ((this->completed_invite_.udp ||
                this->completed_invite_.status < 300) &&
               time_reached(now, this->completed_invite_.next_retransmit_ms)) {
      const bool sent = this->send_sip_(this->completed_invite_.response,
                                        this->completed_invite_.peer_ip_v4,
                                        this->completed_invite_.peer_port);
      this->completed_invite_.retransmits++;
      if (sent) {
        ESP_LOGD(TAG, "SIP UDP INVITE final response retransmit #%u",
                 (unsigned) this->completed_invite_.retransmits);
      } else {
        ESP_LOGW(TAG, "SIP UDP INVITE final response retransmit #%u failed",
                 (unsigned) this->completed_invite_.retransmits);
      }
      this->completed_invite_.retransmit_interval_ms =
          std::min<uint16_t>(static_cast<uint16_t>(this->completed_invite_.retransmit_interval_ms * 2),
                             SIP_T2_MS);
      this->completed_invite_.next_retransmit_ms =
          now + this->completed_invite_.retransmit_interval_ms;
      if (time_reached(this->completed_invite_.next_retransmit_ms,
                       this->completed_invite_.deadline_ms)) {
        this->completed_invite_.next_retransmit_ms = this->completed_invite_.deadline_ms;
      }
    }
  }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  // This one in-dialog client transaction is also pumped for TCP: TCP does
  // not retransmit, but its final-response timeout and 491 retry deadline must
  // still wake the same event-driven SIP select loop.
  this->pump_video_direction_transaction_();
#endif
  // Completed transaction records are kept for 64*T1 so delayed UDP
  // retransmissions can be replayed. Expire them on the SIP task deadline,
  // even when no further packet arrives. Otherwise their std::string storage
  // survives every completed call and gradually consumes the small internal
  // heap needed by lwIP for the next socket send.
  auto expire_completed = [now](auto &transaction) {
    if (!transaction.empty() &&
        time_reached(now, transaction.completed_ms +
                              SIP_TRANSACTION_TIMEOUT_MS)) {
      transaction.clear();
    }
  };
  expire_completed(this->completed_invite_);
  expire_completed(this->completed_control_);
  expire_completed(this->completed_invite_client_);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  expire_completed(this->completed_video_direction_invite_);
#endif
  if (reset_terminal_dialog) {
    this->reset_dialog_();
    SipSignal signal;
    signal.type = SipSignalType::TERMINAL_COMPLETE;
    signal.status_code = 408;
    signal.call_id = timed_out_call_id;
    signal.reason = "terminal_timeout";
    this->emit_sip_signal_(std::move(signal));
  }
  if (invite_timed_out) {
    SipSignal signal;
    signal.type = SipSignalType::FINAL_RESPONSE;
    signal.status_code = 408;
    signal.call_id = timed_out_call_id;
    signal.reason = "timeout";
    this->reset_dialog_();
    this->emit_sip_signal_(std::move(signal));
  }
  if (ack_timed_out) {
    SipSignal signal;
    signal.type = SipSignalType::PROTOCOL_ERROR;
    signal.status_code = 408;
    signal.call_id = timed_out_call_id;
    signal.reason = "ack_timeout";
    const bool bye_pending = this->send_bye_unlocked_(timed_out_call_id);
    signal.terminal_transaction_pending = bye_pending;
    if (!bye_pending) this->reset_dialog_();
    this->emit_sip_signal_(std::move(signal));
  }
}

}  // namespace voip_stack
}  // namespace esphome

#endif
