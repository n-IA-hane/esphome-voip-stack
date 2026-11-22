#include "sip_transport_internal.h"

#if defined(USE_ESP32) && defined(USE_ESPHOME_VOIP_SIP_TRANSPORT)

namespace esphome {
namespace voip_stack {

bool SipTransport::send_invite(const std::string &call_id,
                               const std::string &caller_route,
                               const std::string &caller_name,
                               const std::string &dest_route,
                               const std::string &dest_name) {
  LockGuard lock(this->dialog_mutex_);
  return this->send_invite_unlocked_(call_id, caller_route, caller_name, dest_route, dest_name);
}

bool SipTransport::send_invite_unlocked_(const std::string &call_id,
    const std::string &caller_route, const std::string &caller_name,
    const std::string &dest_route, const std::string &dest_name) {
  if (!this->running_.load(std::memory_order_acquire) ||
      this->transport_stopping_.load(std::memory_order_acquire)) {
    return false;
  }
  if (this->terminal_transaction_pending_locked_()) {
    ESP_LOGW(TAG,
             "SIP INVITE deferred: prior terminal transaction is pending");
    return false;
  }
  if (!this->refer_context_active_) {
    LockGuard media_lock(this->media_lifecycle_mutex_);
    if (this->media_lifecycle_phase_.load(std::memory_order_acquire) !=
        MediaLifecyclePhase::IDLE || this->rtp_socket_ >= 0) {
      ESP_LOGW(TAG, "SIP INVITE deferred: prior media lifecycle not idle");
      return false;
    }
  }
  if (!this->refer_context_active_) {
    this->logical_call_id_.clear();
    this->logical_wire_id_.clear();
  }
  // A fresh INVITE must never inherit tags or transaction state from a
  // cancelled dialog whose final response was lost.
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  const int prepared_account = this->refer_context_active_ ? this->active_account_ : this->next_account_;
#endif
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  const CallVideoIntent prepared_video = this->refer_context_active_ ? this->video_call_intent_
                                                                   : this->next_video_call_intent_;
#endif
  this->reset_dialog_();
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  this->video_call_intent_ = prepared_video;
#endif
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  this->active_account_ = prepared_account;
  if (!this->refer_context_active_) this->next_account_ = -1;
#endif
  this->call_id_ = call_id;
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
  if (!this->refer_context_active_) this->reset_startup_trace_();
#endif
  VOIP_STARTUP_EVENT(INVITE);
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  if (this->active_account_ < 0) this->tcp_flow_.ingress_deadline = 0;
#endif
  this->sdp_session_id_ = esp_random();
  if (this->sdp_session_id_ == 0) this->sdp_session_id_ = 1;
  this->sdp_session_version_ = 0;
  this->dialog_originated_ = true;
  this->caller_ = {caller_route, caller_name};
  this->destination_ = {dest_route, dest_name};
  this->last_sip_status_code_.store(0, std::memory_order_release);
  const uint32_t ip = this->remote_ip_v4_.load(std::memory_order_acquire);
  if (ip == 0) {
    this->reset_dialog_();
    return false;
  }
  if (this->local_tag_.empty()) this->local_tag_ = make_token("tag");
  this->branch_ = "z9hG4bK" + make_token("");
  this->invite_cseq_ = this->cseq_;
  std::string local_ip = "0.0.0.0";
  this->local_ip_for_peer_(ip, &local_ip);
  struct in_addr a{};
  a.s_addr = htonl(ip);
  char ip_text[16];
  inet_ntoa_r(a, ip_text, sizeof(ip_text));
  const char *uri_transport = this->remote_sip_tcp_.load(std::memory_order_acquire) ? "tcp" : "udp";
  const std::string local_identity_uri =
      "sip:" + sip_uri_user_encode(this->caller_.route) + "@" + local_ip +
      ":" + std::to_string(this->sip_port_) + ";transport=" + uri_transport;
  const std::string peer_authority = std::string(ip_text) + ":" +
      std::to_string(this->remote_sip_port_.load(std::memory_order_acquire)) + ";transport=" + uri_transport;
  const std::string remote_identity_uri = this->refer_context_active_ && this->refer_dialog_
      ? this->refer_dialog_->target_uri : sip_target_uri(dest_route, peer_authority);
  if (remote_identity_uri.empty()) {
    this->reset_dialog_();
    return false;
  }
  this->local_uri_ = sip_name_addr(local_identity_uri, this->caller_.name);
  this->local_contact_uri_ = sip_name_addr(local_identity_uri);
  this->remote_uri_ = sip_name_addr(remote_identity_uri, this->destination_.name);
  this->remote_target_uri_ = strip_angle_uri(this->remote_uri_);
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  if (this->active_account_ >= 0) {
    const auto &account = this->registration_.account(this->active_account_);
    this->local_uri_ = sip_name_addr(account.config.address_of_record(), this->caller_.name);
    this->local_contact_uri_ = sip_name_addr(account.contact);
    const std::string account_target = this->refer_context_active_ && this->refer_dialog_
        ? this->refer_dialog_->target_uri
        : sip_target_uri(dest_route, account.config.domain);
    if (account_target.empty()) {
      this->reset_dialog_();
      return false;
    }
    this->remote_uri_ = sip_name_addr(account_target, dest_name);
    this->remote_target_uri_ = strip_angle_uri(this->remote_uri_);
  }
#endif
  ESP_LOGI(TAG, "SIP INVITE call_id=%s from=%s to=%s", this->call_id_.c_str(),
           this->caller_.name.c_str(), this->destination_.name.c_str());
  std::string sdp;
  {
    LockGuard media_lock(this->media_lifecycle_mutex_);
    sdp = this->build_sdp_offer_();
    // Receive on the advertised port before publishing the offer. The final
    // format preparation adopts this same socket without draining it.
    if (!sdp.empty() && !this->refer_context_active_ && this->rtp_socket_ < 0 &&
        !this->bind_udp_(&this->rtp_socket_, this->rtp_port_, "RTP")) sdp.clear();
  }
  if (sdp.empty()) {
    this->reset_dialog_();
    return false;
  }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  if (!this->refer_context_active_) this->confirmed_local_sdp_ = sdp;
#endif
  SipRequestOptions options;
  options.cseq_number = this->invite_cseq_;
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  if (this->active_account_ >= 0) this->authenticated_invite_body_ = sdp;
#endif
  const bool sent = this->send_request_("INVITE", sdp, options);
  if (sent) {
    this->outgoing_invite_pending_.store(true, std::memory_order_release);
    if (this->cseq_ <= this->invite_cseq_) this->cseq_ = this->invite_cseq_ + 1;
  } else {
    this->reset_dialog_();
  }
  return sent;
}

bool SipTransport::send_ringing(const std::string &call_id) {
  LockGuard lock(this->dialog_mutex_);
  if (!call_id.empty() && call_id != this->logical_call_id_) this->call_id_ = call_id;
  return this->send_response_(180, "Ringing");
}

void SipTransport::set_connected_identity(const std::string &route,
                                          const std::string &name) {
  LockGuard lock(this->dialog_mutex_);
  if (this->dialog_originated_ || this->call_id_.empty()) return;
  std::string identity_uri = strip_angle_uri(this->local_uri_);
  if (identity_uri.empty()) return;
  const std::string clean_route =
      sip_route_id(route, VOIP_STACK_MAX_ROUTE_ID_LEN);
  const size_t at = identity_uri.find('@', 4);
  if (!clean_route.empty() && identity_uri.rfind("sip:", 0) == 0 &&
      at != std::string::npos) {
    identity_uri.replace(4, at - 4, sip_uri_user_encode(clean_route));
  }
  const std::string updated = sip_name_addr(identity_uri, name);
  if (!updated.empty()) this->local_uri_ = updated;
}

bool SipTransport::send_answer(const std::string &call_id,
                               const AudioFormat &caller_to_dest_format,
                               const AudioFormat &dest_to_caller_format) {
  LockGuard lock(this->dialog_mutex_);
  if (!this->running_.load(std::memory_order_acquire) ||
      this->transport_stopping_.load(std::memory_order_acquire)) {
    return false;
  }
  if (!call_id.empty() && call_id != this->logical_call_id_) this->call_id_ = call_id;
  uint8_t tx_payload_type = 96;
  uint8_t rx_payload_type = 96;
  RtpAudioPayloads rx_payloads;
  this->get_media_config_(nullptr, nullptr, &tx_payload_type, &rx_payload_type, &rx_payloads);
  this->set_media_config_(dest_to_caller_format, caller_to_dest_format,
                          tx_payload_type, rx_payload_type, &rx_payloads);
  this->outgoing_invite_pending_.store(false, std::memory_order_release);
  std::string answer;
  {
    LockGuard media_lock(this->media_lifecycle_mutex_);
    const MediaLifecyclePhase phase =
        this->media_lifecycle_phase_.load(std::memory_order_acquire);
    if (phase == MediaLifecyclePhase::CLEANING ||
        phase == MediaLifecyclePhase::SHUTTING_DOWN) {
      return false;
    }
    answer = this->delayed_offer_pending_ ? this->build_sdp_offer_()
                                          : this->build_sdp_answer_();
  }
  if (answer.empty()) {
    const bool sent = this->send_response_(488, "Not Acceptable Here", "", "media_incompatible");
    this->reset_dialog_();
    return sent;
  }
  const bool sent = this->send_response_(200, "OK", answer);
  if (sent) {
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
    this->confirmed_local_sdp_ = answer;
#endif
    if (!this->delayed_offer_pending_) this->open_media_session_();
  }
  return sent;
}

bool SipTransport::send_cancel(const std::string &call_id) {
  LockGuard lock(this->dialog_mutex_);
  return this->send_cancel_unlocked_(call_id);
}

bool SipTransport::send_cancel_unlocked_(const std::string &call_id) {
  if (!call_id.empty() && call_id != this->logical_call_id_) this->call_id_ = call_id;
  if (!this->outgoing_invite_pending_.load(std::memory_order_acquire)) {
    return this->send_bye_unlocked_(call_id);
  }
  if (this->remote_sip_tcp_.load(std::memory_order_acquire)) {
    bool cancelled_before_flush = false;
    {
      // This lock is shared with promote_tcp_flow_(). If the INVITE still
      // occupies the one pre-connect slot, it has not reached the wire and
      // must be retracted rather than followed by a meaningless CANCEL. If
      // promotion already owns/sent it, release the lock and send a real
      // CANCEL on the established stream below.
      LockGuard send_lock(this->tcp_send_mutex_);
      LockGuard pending_lock(this->tcp_tx_pending_mutex_);
      if (this->tcp_flow_.pending.rfind("INVITE ", 0) == 0 &&
          header_value(this->tcp_flow_.pending, "Call-ID") == this->call_id_) {
        reset_string(this->tcp_flow_.pending);
        this->tcp_flow_.connect_requested.store(false, std::memory_order_release);
        this->tcp_flow_.close_requested.store(
            true, std::memory_order_release);
        cancelled_before_flush = true;
      }
    }
    if (cancelled_before_flush) {
      ESP_LOGI(TAG, "SIP TCP INVITE cancelled before first byte was sent");
      this->reset_dialog_();
      this->wake_sip_task_();
      return true;
    }
  }
  this->cancel_requested_.store(true, std::memory_order_release);
  const bool sent = this->send_invite_transaction_request_("CANCEL", "", this->pending_invite_, false, nullptr);
  if (sent) {
    // Stop retransmission but retain the original wire request until its final
    // response: the non-2xx ACK must reuse that exact transaction, even after180.
    this->pending_invite_.completed = true;
    this->pending_invite_.next_ms = this->pending_invite_.deadline_ms;
  } else {
    this->cancel_requested_.store(false, std::memory_order_release);
  }
  return sent;
}

bool SipTransport::send_bye(const std::string &call_id) {
  LockGuard lock(this->dialog_mutex_);
  return this->send_bye_unlocked_(call_id);
}

bool SipTransport::send_bye_unlocked_(const std::string &call_id) {
  // A local stop can trail the deferred remote-BYE notification. It must not
  // recreate a retired dialog or retarget a newer one from its application ID.
  if (this->call_id_.empty() ||
      (!call_id.empty() && call_id != this->call_id_ && call_id != this->logical_call_id_)) return false;
  const bool ended_invite = !this->refer_context_active_ && this->refer_dialog_ &&
      this->refer_dialog_->owner_invite_ended && this->call_id_ == this->refer_dialog_->owner_call_id;
  this->cancel_refer_();
  if (ended_invite) {
    const std::string terminated = this->call_id_;
    this->reset_dialog_();
    SipSignal signal;
    signal.type = SipSignalType::TERMINAL_COMPLETE;
    signal.call_id = terminated;
    this->emit_sip_signal_(std::move(signal));
    return true;
  }
  // A UAS dialog is not confirmed until the ACK for its 2xx arrives. A local
  // hangup can race the initial final-response send, especially when lwIP is
  // briefly short of pbuf memory. Retain the terminal intent under the INVITE
  // transaction instead of putting an invalid pre-ACK BYE on the wire.
  if (this->defer_bye_until_uas_ack_locked_()) {
    if (!this->refer_context_active_) this->stop_audio_path();
    return true;
  }
  // Connected-identity is observational and must not outlive termination.
  // Dropping its pending transaction also prevents a lower-CSeq UPDATE from
  // being retransmitted after the dialog's BYE.
  this->pending_update_.clear();
  // BYE is the latency-critical dialog action. Video/audio workers can take
  // time to leave codec and socket calls, so put the SIP request on the wire
  // before performing subordinate media teardown.
  const bool sent = this->send_request_("BYE");
  if (!this->refer_context_active_) this->stop_audio_path();
  return sent;
}

bool SipTransport::defer_bye_until_uas_ack_locked_() {
  const bool accepted_uas_dialog =
      this->completed_invite_.awaiting_ack &&
      this->completed_invite_.status >= 200 &&
      this->completed_invite_.status < 300 &&
      !this->call_id_.empty() &&
      this->completed_invite_.call_id == this->call_id_ &&
      this->last_invite_cseq_number_ == this->completed_invite_.cseq;
  if (!accepted_uas_dialog) return false;
  this->terminate_after_invite_ack_ = true;
  this->wake_sip_task_();
  return true;
}

bool SipTransport::send_final_response(const std::string &call_id,
                                       uint16_t status,
                                       const std::string &reason) {
  LockGuard lock(this->dialog_mutex_);
  if (!call_id.empty() && call_id != this->logical_call_id_) this->call_id_ = call_id;
  if (this->outgoing_invite_pending_.load(std::memory_order_acquire)) {
    return this->send_cancel_unlocked_(call_id);
  }
  const char *phrase = "Busy Here";
  if (status == 603) phrase = "Decline";
  else if (status == 488) phrase = "Not Acceptable Here";
  else if (status == 487) phrase = "Request Terminated";
  else if (status == 500) phrase = "Server Internal Error";
  const bool sent = this->send_response_(status, phrase, "", reason);
  this->reset_dialog_();
  return sent;
}

bool SipTransport::replay_completed_response_(const std::string &request, const sockaddr_in &src,
                                              const std::string &method, bool incoming_udp) {
  size_t body_length = 0;
  const size_t separator = request.find("\r\n\r\n");
  const bool has_length = !header_value(request, "Content-Length").empty() ||
                          !header_value(request, "l").empty();
  if (separator == std::string::npos || !sip_content_length(request, &body_length) ||
      (has_length && request.size() - separator - 4 != body_length)) return false;
  CompletedServerTransaction *completed = method == "REFER" && this->refer_dialog_
      ? &this->refer_dialog_->accepted_refer
      : method == "INVITE" ? &this->completed_invite_ : &this->completed_control_;
  for (auto &entry : this->completed_refers_) {
    if (!entry || time_reached(millis(), entry->expires_ms)) continue;
    auto &candidate = method == "REFER" ? entry->accepted : entry->control;
    if (candidate.method == method && candidate.call_id == header_value(request, "Call-ID") &&
        candidate.cseq == cseq_number(header_value(request, "CSeq"))) {
      completed = &candidate;
      break;
    }
  }
  if (completed->empty()) return false;
  if (millis() - completed->completed_ms > SIP_TRANSACTION_TIMEOUT_MS) {
    completed->clear();
    return false;
  }
  const std::string call_id = header_value(request, "Call-ID");
  const std::string cseq = header_value(request, "CSeq");
  const std::string branch = via_branch(header_value(request, "Via"));
  const uint32_t peer_ip = ntohl(src.sin_addr.s_addr);
  const bool transport_matches =
      completed->udp == incoming_udp;
  if (completed->method != method || completed->call_id != call_id ||
      completed->cseq != cseq_number(cseq) || cseq_method(cseq) != method ||
      completed->peer_ip_v4 != peer_ip || !transport_matches ||
      (!completed->branch.empty() && completed->branch != branch)) {
    return false;
  }
  ESP_LOGI(TAG, "SIP %s retransmission replaying cached response for call_id=%s",
           method.c_str(), call_id.c_str());
  if (incoming_udp) {
    this->send_sip_udp_bytes_(completed->response.data(), completed->response.size(),
                              peer_ip, ntohs(src.sin_port));
  } else {
    this->send_sip_(completed->response, peer_ip, ntohs(src.sin_port));
  }
  return true;
}

void SipTransport::remember_completed_response_(const std::string &request, uint32_t peer_ip_v4,
                                                uint16_t peer_port,
                                                const std::string &method,
                                                const std::string &response, bool header_block) {
  if (response.empty() || peer_ip_v4 == 0 || peer_port == 0 ||
      (method != "INVITE" && method != "CANCEL" && method != "BYE" &&
       method != "UPDATE" && method != "REFER") ||
      response.rfind("SIP/2.0 ", 0) != 0 || response.size() < 12) {
    return;
  }
  uint32_t parsed_status = 0;
  if (!parse_decimal_u32(response.substr(8, 3), 699, &parsed_status) || parsed_status < 200) return;
  CompletedServerTransaction *completed = method == "INVITE" ? &this->completed_invite_
                                                               : &this->completed_control_;
  const auto request_header = [&](const char *name) {
    return sip_record_unfold(header_block ? sip_record_first_header(request, name)
                                          : sip_record_first(request, name));
  };
  const std::string call_id = request_header("Call-ID");
  const uint32_t request_cseq = cseq_number(request_header("CSeq"));
  const std::string request_branch = via_branch(request_header("Via"));
  if (call_id.empty() || request_cseq == 0) return;
  if (method == "INVITE" && completed->awaiting_ack &&
      completed->call_id == this->call_id_ && this->last_invite_cseq_number_ != 0 &&
      (completed->call_id != call_id || completed->cseq != request_cseq ||
       completed->branch != request_branch)) {
    // Do not let a colliding/re-INVITE stateless error evict the active UAS
    // final response before its ACK arrives.
    return;
  }
  const uint32_t now = millis();
  completed->method = method;
  completed->call_id = call_id;
  completed->cseq = request_cseq;
  completed->branch = request_branch;
  completed->from_tag = tag_from_header(header_value(response, "From"));
  completed->to_tag = tag_from_header(header_value(response, "To"));
  completed->response = response;
  completed->peer_ip_v4 = peer_ip_v4;
  completed->peer_port = peer_port;
  completed->status = static_cast<uint16_t>(parsed_status);
  completed->completed_ms = now;
  completed->next_retransmit_ms = now + SIP_T1_MS;
  completed->deadline_ms = now + SIP_TRANSACTION_TIMEOUT_MS;
  completed->retransmit_interval_ms = SIP_T1_MS;
  completed->retransmits = 0;
  completed->udp = this->dispatch_is_udp_();
  // Every final INVITE response is completed by ACK. UDP retransmits every
  // final response; 2xx is retransmitted by the UAS core even on TCP because
  // its ACK is a separate transaction.
  completed->awaiting_ack = method == "INVITE";
  if (completed->awaiting_ack) this->wake_sip_task_();
}

uint16_t SipTransport::acknowledge_completed_invite_(
    const std::string &request, const sockaddr_in &src,
    bool *terminate_after_ack) {
  if (terminate_after_ack != nullptr) *terminate_after_ack = false;
  if (this->completed_invite_.empty()) return 0;
  const uint32_t now = millis();
  if (time_reached(now, this->completed_invite_.completed_ms + SIP_TRANSACTION_TIMEOUT_MS)) {
    this->completed_invite_.clear();
    return 0;
  }
  const std::string cseq = header_value(request, "CSeq");
  const uint32_t peer_ip = ntohl(src.sin_addr.s_addr);
  const std::string branch = via_branch(header_value(request, "Via"));
  const bool transport_matches =
      this->completed_invite_.status < 300 || this->completed_invite_.udp == this->dispatch_is_udp_();
  const bool transaction_matches =
      cseq_method(cseq) == "ACK" && cseq_number(cseq) == this->completed_invite_.cseq &&
      header_value(request, "Call-ID") == this->completed_invite_.call_id &&
      peer_ip == this->completed_invite_.peer_ip_v4 && transport_matches &&
      tag_from_header(header_value(request, "From")) == this->completed_invite_.from_tag &&
      tag_from_header(header_value(request, "To")) == this->completed_invite_.to_tag &&
      (this->completed_invite_.status < 300 ||
       this->completed_invite_.branch.empty() || branch == this->completed_invite_.branch);
  if (!transaction_matches) return 0;
  this->completed_invite_.awaiting_ack = false;
  if (this->completed_invite_.status < 300 &&
      this->terminate_after_invite_ack_) {
    this->terminate_after_invite_ack_ = false;
    if (terminate_after_ack != nullptr) *terminate_after_ack = true;
  }
  ESP_LOGI(TAG, "SIP ACK completed cached INVITE server transaction call_id=%s",
           this->completed_invite_.call_id.c_str());
  return this->completed_invite_.status;
}

bool SipTransport::replay_completed_invite_ack_(const std::string &response,
                                                const sockaddr_in &src, bool incoming_udp) {
  size_t body_length = 0;
  const size_t separator = response.find("\r\n\r\n");
  const bool has_length = !header_value(response, "Content-Length").empty() ||
                          !header_value(response, "l").empty();
  if (separator == std::string::npos || !sip_content_length(response, &body_length) ||
      (has_length && response.size() - separator - 4 != body_length)) return false;
  CompletedInviteClientTransaction *completed = &this->completed_invite_client_;
  for (auto &entry : this->completed_refers_) {
    if (entry && !time_reached(millis(), entry->expires_ms) &&
        entry->invite.call_id == header_value(response, "Call-ID")) {
      completed = &entry->invite;
      break;
    }
  }

  if (completed->empty()) return false;
  if (millis() - completed->completed_ms > SIP_TRANSACTION_TIMEOUT_MS) {
    completed->clear();
    return false;
  }
  if (response.rfind("SIP/2.0 ", 0) != 0 || response.size() < 12) return false;
  const int status = std::atoi(response.substr(8, 3).c_str());
  const std::string cseq = header_value(response, "CSeq");
  const uint32_t peer_ip = ntohl(src.sin_addr.s_addr);
  const bool transport_matches =
      completed->udp == incoming_udp;
  if (status < 200 || cseq_method(cseq) != "INVITE" ||
      header_value(response, "Call-ID") != completed->call_id ||
      cseq_number(cseq) != completed->cseq ||
      peer_ip != completed->response_ip_v4 ||
      !transport_matches ||
      (!completed->branch.empty() &&
       via_branch(header_value(response, "Via")) != completed->branch)) {
    return false;
  }
  ESP_LOGI(TAG, "SIP INVITE final retransmission replaying ACK for call_id=%s",
           completed->call_id.c_str());
  if (incoming_udp) {
    this->send_sip_udp_bytes_(completed->ack.data(), completed->ack.size(),
                              completed->ack_ip_v4, completed->ack_port);
  } else {
    this->send_sip_(completed->ack, completed->ack_ip_v4, completed->ack_port);
  }
  return true;
}

void SipTransport::remember_completed_invite_ack_(const std::string &request,
                                                  uint32_t target_ip_v4,
                                                  uint16_t target_port) {
  if (request.empty() || target_ip_v4 == 0 || target_port == 0) return;
  this->completed_invite_client_.call_id = this->call_id_;
  this->completed_invite_client_.cseq = this->invite_cseq_;
  this->completed_invite_client_.branch = this->branch_;
  this->completed_invite_client_.ack = request;
  this->completed_invite_client_.response_ip_v4 =
      this->remote_ip_v4_.load(std::memory_order_acquire);
  this->completed_invite_client_.ack_ip_v4 = target_ip_v4;
  this->completed_invite_client_.ack_port = target_port;
  this->completed_invite_client_.completed_ms = millis();
  this->completed_invite_client_.udp =
      !this->remote_sip_tcp_.load(std::memory_order_acquire);
}

}  // namespace voip_stack
}  // namespace esphome

#endif
