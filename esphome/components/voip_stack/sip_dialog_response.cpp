#include "sip_transport.h"

#if defined(USE_ESP32) && defined(USE_ESPHOME_VOIP_SIP_TRANSPORT)

#include "sip_dialog_message.h"
#include "sip_message.h"
#include "sip_record.h"
#include "esphome/core/log.h"

namespace esphome::voip_stack {

static const char *const TAG = "voip_stack.sip";

bool SipTransport::handle_response_(const std::string &message, const sockaddr_in &src) {
  const uint32_t src_ip = ntohl(src.sin_addr.s_addr);
  const uint16_t status = SipStartLine::response_status(message);
  if (status == 0) return false;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  if (this->replay_completed_video_direction_ack_(message, src)) return true;
#endif
  if (this->replay_completed_invite_ack_(message, src, this->dispatch_is_udp_())) return true;

  const std::string response_call_id = header_value(message, "Call-ID");
  const std::string response_cseq = header_value(message, "CSeq");
  const std::string method = cseq_method(response_cseq);
  if (method == "NOTIFY" && this->refer_dialog_) {
    const auto branch = via_branch(header_value(message, "Via"));
    for (auto *transaction : {&this->refer_dialog_->pending_notify_, &this->refer_dialog_->pending_notify_final_}) {
      if (transaction->empty() || src_ip != transaction->ip_v4 || ntohs(src.sin_port) != transaction->port) continue;
      const std::string_view request = transaction->request;
      if (response_call_id == sip_record_unfold(sip_record_first(request, "Call-ID")) &&
          response_cseq == sip_record_unfold(sip_record_first(request, "CSeq")) &&
          branch == via_branch(sip_record_unfold(sip_record_first(request, "Via")))) {
        if (status >= 200) transaction->clear();
        return true;
      }
    }
    return false;
  }
  if (response_call_id.empty() || this->call_id_.empty() || response_call_id != this->call_id_) {
    ESP_LOGD(TAG, "SIP response ignored for stale/unknown call_id=%s current=%s",
             response_call_id.empty() ? "(empty)" : response_call_id.c_str(),
             this->call_id_.empty() ? "(none)" : this->call_id_.c_str());
    return true;
  }
  const uint32_t response_cseq_number = cseq_number(response_cseq);
  if (method.empty() || response_cseq_number == 0) {
    ESP_LOGD(TAG, "SIP response ignored without a valid CSeq");
    return true;
  }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  if (method == "INVITE" &&
      this->handle_video_direction_response_(
          message, src, static_cast<uint16_t>(status),
          response_cseq_number)) {
    return true;
  }
#endif
  const auto kind = sip_client_method(method);
  {
    const UdpTransaction *transaction;
    switch (kind) {
      case SipClientMethod::INVITE: transaction = &this->pending_invite_; break;
      case SipClientMethod::CANCEL: transaction = &this->pending_cancel_; break;
      case SipClientMethod::BYE: transaction = &this->pending_bye_; break;
      case SipClientMethod::UPDATE: transaction = &this->pending_update_; break;
      default:
        ESP_LOGD(TAG, "SIP response ignored for unsupported transaction method %s", method.c_str());
        return true;
    }

    const bool pending = !transaction->empty();
    uint32_t expected_sequence = this->invite_cseq_;
    std::string control_branch;
    std::string_view expected_branch = this->branch_;
    if (pending && (kind == SipClientMethod::BYE || kind == SipClientMethod::UPDATE)) {
      const std::string_view request = transaction->request;
      expected_sequence = cseq_number(sip_record_unfold(sip_record_first(request, "CSeq")));
      control_branch = via_branch(sip_record_unfold(sip_record_first(request, "Via")));
      expected_branch = control_branch;
    }
    uint32_t expected_ip = 0;
    if (!this->remote_sip_tcp_.load(std::memory_order_acquire)) {
      expected_ip = pending && transaction->ip_v4 != 0 ? transaction->ip_v4 :
          this->remote_ip_v4_.load(std::memory_order_acquire);
    }
    const auto from = tag_from_header(header_value(message, "From"));
    auto to = tag_from_header(header_value(message, "To"));
    const auto branch = via_branch(header_value(message, "Via"));
    const auto mismatch = match_sip_response({kind, status, response_cseq_number, src_ip, from, to, branch},
        {expected_sequence, expected_ip, this->local_tag_, this->remote_tag_, expected_branch, pending});
    if (mismatch != SipResponseMismatch::NONE) {
      ESP_LOGD(TAG, "SIP %s response ignored: %s (CSeq %u, expected %u)", method.c_str(),
               sip_response_mismatch_name(mismatch), (unsigned) response_cseq_number, (unsigned) expected_sequence);
      return true;
    }

    if (status > 100 && this->remote_tag_.empty() && !to.empty()) this->remote_tag_ = std::move(to);
  }
  if (method == "INVITE" && status > 100 &&
      sip_option_supported(message, "from-change")) {
    this->peer_supports_from_change_ = true;
  }
  // Only a response belonging to the active dialog may retarget subsequent
  // ACK/BYE traffic. Stale or spoofed responses must be side-effect free.
  this->remote_ip_v4_.store(src_ip, std::memory_order_release);
  this->remote_sip_port_.store(ntohs(src.sin_port), std::memory_order_release);
  this->mark_sip_event_(SipEvent::RESPONSE, static_cast<uint16_t>(status));
  if (method == "INVITE" && status >= 100) {
    if (status < 200 && !this->pending_invite_.empty()) {
      // A provisional response stops INVITE retransmission but not Timer B.
      // Keep the bounded transaction until its original 64*T1 deadline so a
      // peer that sends 100/180 and then disappears cannot strand the call.
      this->pending_invite_.completed = true;
      this->pending_invite_.next_ms = this->pending_invite_.deadline_ms;
    } else {
      if (status >= 300)
        this->send_invite_error_ack_(message, this->pending_invite_, true, nullptr);
      this->clear_invite_transaction_();
    }
  }
  if (status > 100 && status < 200 && method == "INVITE") {
    this->emit_dialog_status_(SipSignalType::STATUS_180_RINGING, status, this->call_id_);
    return true;
  }
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  if ((method == "INVITE" || method == "BYE" || method == "UPDATE") && (status == 401 || status == 407) &&
      this->active_account_ >= 0 && this->retry_authenticated_request_(message, method, status)) return true;
#endif
  if (status >= 200 && status < 300) {
#ifdef USE_ESPHOME_VOIP_REGISTRATION
    this->dialog_auth_attempts_ = 0;
    this->dialog_proxy_auth_attempts_ = 0;
    if (kind == SipClientMethod::INVITE && this->active_account_ >= 0) this->remember_registration_routes_(message, true);
#endif
    if (kind == SipClientMethod::INVITE) return this->accept_invite_response_(message, src_ip, status);
  }
  if (status >= 200 && kind != SipClientMethod::INVITE) {
    if (status >= 300) {
      ESP_LOGW(TAG, "SIP %u response for %s", (unsigned) status, method.c_str());
    }
    this->finish_control_response_(kind, status);
    return true;
  }
  if (status >= 300) {
    this->outgoing_invite_pending_.store(false, std::memory_order_release);
    std::string reason = sip_header_token(header_value(message, "X-Voip-Stack-Decline-Reason"));
    if (reason.empty()) reason = reason_text_from_header(header_value(message, "Reason"));
    if (reason.empty()) reason = sip_failure_reason_(status);
    const auto type = status == 401 ? SipSignalType::AUTH_REQUIRED
                : status == 407 ? SipSignalType::PROXY_AUTH_REQUIRED
                : status == 488 ? SipSignalType::MEDIA_INCOMPATIBLE
                                : SipSignalType::FINAL_RESPONSE;
    this->emit_dialog_status_(type, status, this->call_id_, std::move(reason));
    this->reset_dialog_();
    return true;
  }
  return true;
}

// The dialog lock is held by the receive path. The 2xx must be acknowledged
// before publishing media readiness or ending an unusable confirmed dialog.
bool SipTransport::accept_invite_response_(const std::string &message, uint32_t src_ip, uint16_t status) {
  this->outgoing_invite_pending_.store(false, std::memory_order_release);
  const std::string contact_target = strip_angle_uri(header_value(message, "Contact"));
  if (!contact_target.empty()) this->remote_target_uri_ = contact_target;
  SipRequestOptions options;
  options.cseq_number = this->invite_cseq_;
  this->send_request_("ACK", "", options);
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
  this->record_startup_(StartupEvent::ACK);
#endif
  if (this->remote_tag_.empty()) {
    std::string failed_call_id = this->call_id_;
    this->reset_dialog_();
    this->emit_dialog_status_(SipSignalType::PROTOCOL_ERROR, 500, std::move(failed_call_id), "malformed_2xx");
    return true;
  }
  bool media_ok = false;
  bool video_prepared = true;
  {
    LockGuard media_lock(this->media_lifecycle_mutex_);
    media_ok = this->learn_remote_rtp_from_sdp_(
        message_body(message), src_ip, true);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
    if (media_ok && !this->refer_context_active_)
      video_prepared = this->prepare_video_session_locked_();
#endif
  }
  if (this->cancel_requested_.load(std::memory_order_acquire)) {
    // CANCEL crossed the final 2xx. The dialog is confirmed despite the
    // cancellation, so ACK it and immediately terminate it with BYE.
    if (!this->send_bye_unlocked_(this->call_id_)) {
      this->reset_dialog_();
    }
    return true;
  }
  if (!media_ok || !video_prepared) {
    // A 2xx INVITE response still requires ACK. Terminate the confirmed SIP
    // dialog instead of promising media whose negotiated resources could
    // not be admitted locally.
    const bool bye_pending = this->send_bye_unlocked_(this->call_id_);
    std::string failed_call_id = this->call_id_;
    if (!bye_pending) this->reset_dialog_();
    this->emit_dialog_status_(SipSignalType::MEDIA_INCOMPATIBLE, 488, std::move(failed_call_id),
                              "media_incompatible", bye_pending);
    return true;
  }
  this->open_media_session_();
  this->emit_negotiated_media_(SipSignalType::STATUS_200_OK, status);
  return true;
}

void SipTransport::emit_dialog_status_(SipSignalType type, uint16_t status, std::string call_id,
                                       std::string reason, bool terminal_pending) {
  SipSignal signal;
  signal.type = type;
  signal.status_code = status;
  signal.call_id = std::move(call_id);
  signal.reason = std::move(reason);
  signal.terminal_transaction_pending = terminal_pending;
  this->emit_sip_signal_(std::move(signal));
}

void SipTransport::emit_negotiated_media_(SipSignalType type, uint16_t status) {
  SipSignal signal;
  signal.type = type;
  signal.status_code = status;
  signal.call_id = this->call_id_;
  this->get_media_config_(&signal.selected_tx_format, &signal.selected_rx_format, nullptr, nullptr);
  this->emit_sip_signal_(std::move(signal));
}

void SipTransport::finish_control_response_(SipClientMethod method, uint16_t status) {
  switch (method) {
    case SipClientMethod::BYE: {
      std::string completed_call_id = this->call_id_;
      if (status < 300) {
        ESP_LOGI(TAG, "SIP BYE completed call_id=%s", completed_call_id.c_str());
      }
      this->clear_bye_transaction_();
      this->reset_dialog_();
      this->emit_dialog_status_(SipSignalType::TERMINAL_COMPLETE, status, std::move(completed_call_id));
      break;
    }
    case SipClientMethod::CANCEL:
      if (status < 300) {
        ESP_LOGI(TAG, "SIP CANCEL completed call_id=%s", this->call_id_.c_str());
      }
      if (!this->pending_cancel_.empty()) {
        // Timer F still bounds the wait for the INVITE's final response.
        this->pending_cancel_.completed = true;
        this->pending_cancel_.next_ms = this->pending_cancel_.deadline_ms;
      }
      break;
    case SipClientMethod::UPDATE:
      if (status < 300) {
        ESP_LOGI(TAG, "SIP connected identity UPDATE completed call_id=%s", this->call_id_.c_str());
      }
      this->pending_update_.clear();
      break;
    default:
      break;
  }
}

}  // namespace esphome::voip_stack
#endif
