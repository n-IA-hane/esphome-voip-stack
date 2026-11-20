#include "sip_transport.h"

#if defined(USE_ESP32) && defined(USE_ESPHOME_VOIP_SIP_TRANSPORT)

#include "sip_dialog_message.h"
#include "sip_message.h"
#include "esphome/core/log.h"

namespace esphome::voip_stack {

static const char *const TAG = "voip_stack.sip";

void SipTransport::dispatch_dialog_message_(const std::string &msg, const SipStartLine &start,
                                            const sockaddr_in &src) {
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  if (this->registration_ingress_(msg, src, this->dispatch_socket_)) return;
#endif
  if (!this->refer_context_active_ && this->refer_dialog_ && header_value(msg, "Call-ID") == this->refer_wire_id_) {
    {
      ReferScope scope(*this);
      this->dispatch_dialog_message_(msg, start, src);
    }
    this->pump_refer_();
    return;
  }
  if (start.response) {
    this->handle_response_(msg, src);
    this->pump_refer_();
    return;
  }
  const std::string method(start.method);
  ESP_LOGD(TAG, "SIP RX method=%s len=%u", method.c_str(), (unsigned) msg.size());
  if ((method == "CANCEL" || method == "BYE" || method == "REFER") &&
      this->replay_completed_response_(msg, src, method,
#ifdef USE_ESPHOME_VOIP_REGISTRATION
                                         this->dispatch_is_udp_()
#else
                                         !this->remote_sip_tcp_.load(std::memory_order_acquire)
#endif
                                         )) {
    return;
  }
  const SipEvent event = sip_event_from_method_(method);
  if (event != SipEvent::NONE) this->mark_sip_event_(event);
  if (this->refer_context_active_ && (method == "INVITE" || method == "UPDATE")) {
    this->send_stateless_response_(msg, src, 491, "Request Pending");
  } else if (method == "REFER") {
    this->handle_refer_(msg, src);
  } else if (method == "INVITE") {
    this->handle_invite_(msg, src);
  } else if (method == "ACK") {
    this->handle_dialog_ack_(msg, src);
  } else if (method == "BYE") {
    this->handle_dialog_bye_(msg, src);
  } else if (method == "CANCEL") {
    this->handle_dialog_cancel_(msg, src);
  } else if (method == "UPDATE") {
    this->handle_update_(msg, src);
  } else if (method == "OPTIONS") {
    this->handle_options_(msg, src);
  } else if (sip_method_known_(method)) {
    this->send_stateless_response_(msg, src, 405, "Method Not Allowed");
  } else {
    this->send_stateless_response_(msg, src, 501, "Not Implemented");
  }
}

void SipTransport::handle_dialog_ack_(const std::string &msg, const sockaddr_in &src) {
  std::string request_call_id = header_value(msg, "Call-ID");
  bool terminate_after_ack = false;
  const uint16_t completed_status =
      this->acknowledge_completed_invite_(msg, src, &terminate_after_ack);
  if (terminate_after_ack) {
    const std::string call_id = this->call_id_;
    if (!this->send_bye_unlocked_(call_id)) this->reset_dialog_();
    return;
  }
  if (completed_status >= 300) {
    // ACK is the terminal event for a non-2xx INVITE server transaction.
    // The dialog may already have been detached after CANCEL, but the call
    // lifecycle must still cross its cleanup barrier and become reusable.
    this->emit_dialog_status_(SipSignalType::TERMINAL_COMPLETE, completed_status, std::move(request_call_id));
    return;
  }
  const uint32_t expected_ip = this->remote_ip_v4_.load(std::memory_order_acquire);
  const uint32_t request_cseq = cseq_number(header_value(msg, "CSeq"));
  const bool valid_ack = completed_status >= 200 && completed_status < 300 &&
                         !request_call_id.empty() && !this->call_id_.empty() &&
                         request_call_id == this->call_id_ &&
                         (expected_ip == 0 || ntohl(src.sin_addr.s_addr) == expected_ip) &&
                         this->last_invite_cseq_number_ != 0 &&
                         request_cseq == this->last_invite_cseq_number_ &&
                         cseq_method(header_value(msg, "CSeq")) == "ACK" &&
                         tag_from_header(header_value(msg, "From")) == this->remote_tag_ &&
                         tag_from_header(header_value(msg, "To")) == this->local_tag_;
  if (!valid_ack) {
    ESP_LOGD(TAG, "SIP ACK ignored for stale/invalid call_id=%s current=%s",
             request_call_id.empty() ? "(empty)" : request_call_id.c_str(),
             this->call_id_.empty() ? "(none)" : this->call_id_.c_str());
    return;
  }
  // A failed answer or local stop already owns the BYE. Replayed ACKs must
  // not reopen media or publish another completion during that transaction.
  if (!this->pending_bye_.empty()) return;
  bool media_answer_completed = false;
  if (this->delayed_offer_pending_) {
    const std::string answer = message_body(msg);
    bool accepted = false;
    if (!answer.empty()) {
      LockGuard media_lock(this->media_lifecycle_mutex_);
      accepted = this->learn_remote_rtp_from_sdp_(
          answer, ntohl(src.sin_addr.s_addr), true);
    }
    this->delayed_offer_pending_ = false;
    if (!accepted) {
      ESP_LOGW(TAG, "SIP delayed offer ACK missing a compatible SDP answer");
      std::string failed_call_id = this->call_id_;
      if (!this->send_bye_unlocked_(this->call_id_)) this->reset_dialog_();
      this->emit_dialog_status_(SipSignalType::MEDIA_INCOMPATIBLE, 488, std::move(failed_call_id), "media_incompatible",
                               this->terminal_transaction_pending_locked_());
      return;
    }
    media_answer_completed = true;
  }
  this->outgoing_invite_pending_.store(false, std::memory_order_release);
  this->open_media_session_();
  this->send_connected_identity_update_();
  if (media_answer_completed) {
    this->emit_negotiated_media_(SipSignalType::MEDIA_READY, 0);
  }
}

void SipTransport::handle_dialog_bye_(const std::string &msg, const sockaddr_in &src) {
  if (this->reject_if_stale_dialog_(msg, src, "BYE")) return;
  const bool local_bye_pending = !this->pending_bye_.empty();
  if (!this->media_active_.load(std::memory_order_acquire) &&
      !local_bye_pending) {
    this->send_stateless_response_(msg, src, 481, "Call/Transaction Does Not Exist");
    return;
  }
  this->send_stateless_response_(msg, src, 200, "OK", "", true);
  if (!local_bye_pending && this->end_refer_invite_usage_(true)) {
    this->remote_dialog_cseq_ = cseq_number(header_value(msg, "CSeq"));
    return;
  }
  this->emit_dialog_status_(SipSignalType::BYE, 0, this->call_id_, {}, local_bye_pending);
  if (!local_bye_pending) this->reset_dialog_();
}

void SipTransport::handle_dialog_cancel_(const std::string &msg, const sockaddr_in &src) {
  if (this->reject_if_stale_dialog_(msg, src, "CANCEL")) return;
  const uint32_t incoming_cseq_number = cseq_number(header_value(msg, "CSeq"));
  const std::string incoming_via = header_value(msg, "Via");
  const std::string incoming_branch = via_branch(incoming_via);
  const std::string invite_branch = via_branch(this->last_invite_via_);
  const std::string incoming_from_tag = tag_from_header(header_value(msg, "From"));
  const bool same_transaction_via = !incoming_branch.empty() && !invite_branch.empty()
                                        ? incoming_branch == invite_branch
                                        : incoming_via == this->last_invite_via_;
  if (cseq_method(header_value(msg, "CSeq")) != "CANCEL" ||
      this->media_active_.load(std::memory_order_acquire) || this->last_invite_cseq_number_ == 0 ||
      incoming_cseq_number != this->last_invite_cseq_number_ || !same_transaction_via) {
    this->send_stateless_response_(msg, src, 481, "Call/Transaction Does Not Exist");
    return;
  }
  if (incoming_from_tag.empty() || incoming_from_tag != this->remote_tag_) {
    this->send_stateless_response_(msg, src, 481, "Call/Transaction Does Not Exist");
    return;
  }
  this->send_stateless_response_(msg, src, 200, "OK", "", true);
  this->send_response_(487, "Request Terminated");
  this->emit_dialog_status_(SipSignalType::CANCEL, 487, this->call_id_, "cancelled");
  this->reset_dialog_();
}

}  // namespace esphome::voip_stack
#endif
