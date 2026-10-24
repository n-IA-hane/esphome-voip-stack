#include "sip_transport.h"
#include "sip_message.h"

#if defined(USE_ESP32) && defined(USE_ESPHOME_VOIP_SIP_TRANSPORT)
#include <algorithm>
#include <cctype>
#include <utility>
#include "esphome/core/hal.h"

namespace esphome {
namespace voip_stack {

SipTransport::ReferScope::ReferScope(SipTransport &transport) : transport_(transport) {
  transport_.media_proposal_epoch_.fetch_add(1);
  transport_.refer_context_active_ = true;
  transport_.swap_dialog_(*transport_.refer_dialog_);
  transport_.swap_negotiation_(*transport_.refer_dialog_);
}

SipTransport::ReferScope::~ReferScope() {
  if (!this->committed_) {
    transport_.swap_negotiation_(*transport_.refer_dialog_);
    transport_.swap_dialog_(*transport_.refer_dialog_);
  }
  transport_.refer_context_active_ = false;
  transport_.media_proposal_epoch_.fetch_add(1);
}

void SipTransport::emit_sip_signal_(SipSignal signal) {
  if (this->refer_context_active_) {
    this->refer_signal_type_ = signal.type;
    this->refer_status_ = signal.status_code;
    this->refer_signal_pending_ = true;
    return;
  }
  if (!this->logical_call_id_.empty() && signal.call_id == this->logical_wire_id_)
    signal.call_id = this->logical_call_id_;
  SipPhoneTransport::emit_sip_signal_(std::move(signal));
}

void SipTransport::notify_refer_(uint32_t refer_cseq, uint16_t status, const char *reason) {
  if (!this->refer_dialog_ || !this->refer_dialog_->reporter_available) return;
  SipRequestOptions options;
  options.content_type = "message/sipfrag;version=2.0";
  options.extra_headers = "Event: refer;id=" + std::to_string(refer_cseq) + "\r\nSubscription-State: " +
      (status < 200 ? "active;expires=32\r\n" : "terminated;reason=noresource\r\n");
  this->send_request_("NOTIFY", "SIP/2.0 " + std::to_string(status) + " " + reason + "\r\n", options);
}

bool SipTransport::end_refer_invite_usage_(bool reporter_available) {
  if (this->refer_context_active_ || !this->refer_dialog_ || this->refer_retiring_ ||
      this->call_id_ != this->refer_dialog_->owner_call_id) return false;
  // REFER's subscription and action outlive the INVITE usage (RFC 3515/5057).
  this->refer_dialog_->owner_invite_ended = true;
  this->refer_dialog_->reporter_available &= reporter_available;
  this->refer_media_waiting_.store(true, std::memory_order_release);
  this->pending_update_.clear();
  this->close_media_session_();
  return true;
}

void SipTransport::cancel_refer_() {
  if (this->refer_context_active_ || !this->refer_dialog_ || this->refer_retiring_) return;
  this->notify_refer_(this->refer_cseq_, 487, "Request Terminated");
  this->refer_retiring_ = true;
  ReferScope scope(*this);
  if (this->outgoing_invite_pending_.load()) this->send_cancel_unlocked_(this->call_id_);
  else if (!this->call_id_.empty()) this->send_bye_unlocked_(this->call_id_);
}

void SipTransport::finish_failed_refer_(uint16_t status, const char *reason) {
  this->notify_refer_(this->refer_cseq_, status, reason);
  this->refer_retiring_ = true;
  if (this->refer_dialog_->owner_invite_ended && this->call_id_ == this->refer_dialog_->owner_call_id) {
    SipSignal signal;
    signal.type = SipSignalType::BYE;
    signal.call_id = this->call_id_;
    signal.reason = "transfer_failed";
    this->emit_sip_signal_(std::move(signal));
    this->reset_dialog_();
  }
}

bool SipTransport::handle_refer_(const std::string &message, const sockaddr_in &src) {
  if (this->reject_if_stale_dialog_(message, src, "REFER")) return false;
  if (!this->media_active_.load() || this->refer_context_active_ || this->refer_dialog_ || this->completed_invite_.awaiting_ack) {
    this->send_stateless_response_(message, src, 491, "Request Pending");
    return false;
  }
  const std::string raw = header_values(message, "Refer-To");
  const std::string compact = header_values(message, "r");
  if ((!raw.empty() && !compact.empty()) || raw.find('\n') != std::string::npos || compact.find('\n') != std::string::npos) {
    this->send_stateless_response_(message, src, 400, "Bad Request");
    return false;
  }
  const std::string target = name_addr_uri(raw.empty() ? compact : raw);
  uint32_t ip = 0;
  uint16_t port = 0;
  // Unsupported URI schemes and extensions must not turn into a different call.
  if (target.empty()) {
    this->send_stateless_response_(message, src, 400, "Bad Request");
    return false;
  }
  std::string normalized = target;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  bool target_tcp = false;
  bool supported = normalized.rfind("sip:", 0) == 0 && target.find('?') == std::string::npos;
  size_t parameter = normalized.find(';', normalized.rfind('@') == std::string::npos ? 4 : normalized.rfind('@'));
  while (parameter != std::string::npos) {
    const size_t end = normalized.find(';', parameter + 1);
    const std::string value = normalized.substr(parameter + 1, end - parameter - 1);
    if (value.rfind("transport=", 0) == 0) {
      supported &= value == "transport=udp" || value == "transport=tcp";
      target_tcp = value == "transport=tcp";
    } else if (value.rfind("method=", 0) == 0) {
      supported &= value == "method=invite";
    } else if (value.rfind("maddr=", 0) == 0 || value.rfind("ttl=", 0) == 0) {
      supported = false;
    }
    parameter = end;
  }
  if (!supported || !sip_uri_ipv4_target(target, &ip, &port)) {
    this->send_stateless_response_(message, src, 416, "Unsupported URI Scheme");
    return false;
  }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  if (this->video_negotiated_) {
    this->send_stateless_response_(message, src, 488, "Not Acceptable Here");
    return false;
  }
#endif
  const std::string user = sip_user_from_header(target);
  if (user.empty()) {
    this->send_stateless_response_(message, src, 400, "Bad Request");
    return false;
  }
  auto *memory = RAMAllocator<ReferDialog>().allocate(1);
  if (memory == nullptr) {
    this->send_stateless_response_(message, src, 503, "Service Unavailable");
    return false;
  }
  std::unique_ptr<ReferDialog, ReferDeleter> candidate(::new (memory) ReferDialog());
  candidate->owner_call_id = this->call_id_;
  candidate->target_uri = target;
  candidate->remote_sip_tcp_ = target_tcp;
  candidate->remote_ip_v4_ = ip;
  candidate->remote_sip_port_ = port;
  this->refer_cseq_ = cseq_number(header_value(message, "CSeq"));
  if (!this->send_stateless_response_(message, src, 200, "OK", "", true)) return false;
  candidate->accepted_refer = std::move(this->completed_control_);
  this->remote_dialog_cseq_ = this->refer_cseq_;
  this->refer_wire_id_ = make_token("refer-");
  this->refer_expires_ = millis() + 2 * SIP_TRANSACTION_TIMEOUT_MS;
  this->refer_retiring_ = false;
  this->refer_signal_pending_ = false;
  this->refer_dialog_ = std::move(candidate);
  this->notify_refer_(this->refer_cseq_, 100, "Trying");
  const std::string local_user = sip_user_from_header(this->local_uri_);
  const std::string local_name = sip_display_name_from_header(this->local_uri_, VOIP_STACK_MAX_NAME_LEN);
  bool sent;
  {
    ReferScope scope(*this);
    if (target_tcp) {
      this->tcp_flow_.target_ip.store(ip, std::memory_order_release);
      this->tcp_flow_.target_port.store(port, std::memory_order_release);
      this->tcp_flow_.connect_requested.store(true, std::memory_order_release);
    }
    sent = this->send_invite_unlocked_(this->refer_wire_id_, local_user, local_name, user, user);
  }
  if (!sent) {
    this->notify_refer_(this->refer_cseq_, 503, "Service Unavailable");
    this->refer_retiring_ = true;
  }
  this->wake_sip_task_();
  return true;
}

void SipTransport::swap_dialog_(ReferDialog &other) {
  using std::swap;
#define SWAP_FIELD(field) swap(this->field, other.field)
  SWAP_FIELD(call_id_); SWAP_FIELD(local_tag_); SWAP_FIELD(remote_tag_); SWAP_FIELD(branch_);
  SWAP_FIELD(local_uri_); SWAP_FIELD(local_contact_uri_); SWAP_FIELD(remote_uri_); SWAP_FIELD(remote_target_uri_);
  SWAP_FIELD(last_invite_via_); SWAP_FIELD(last_invite_from_); SWAP_FIELD(last_invite_to_);
  SWAP_FIELD(last_invite_cseq_); SWAP_FIELD(last_invite_response_); SWAP_FIELD(last_invite_cseq_number_);
  SWAP_FIELD(remote_dialog_cseq_); SWAP_FIELD(last_invite_peer_ip_v4_); SWAP_FIELD(last_invite_peer_port_);
  SWAP_FIELD(caller_); SWAP_FIELD(destination_); SWAP_FIELD(dialog_originated_);
  SWAP_FIELD(peer_supports_from_change_); SWAP_FIELD(connected_identity_sent_);
  SWAP_FIELD(cseq_); SWAP_FIELD(invite_cseq_); SWAP_FIELD(sdp_session_id_); SWAP_FIELD(sdp_session_version_);
  SWAP_FIELD(completed_invite_); SWAP_FIELD(completed_control_); SWAP_FIELD(completed_invite_client_);
  SWAP_FIELD(pending_invite_); SWAP_FIELD(pending_cancel_); SWAP_FIELD(pending_bye_); SWAP_FIELD(pending_update_);
  SWAP_FIELD(delayed_offer_pending_); SWAP_FIELD(terminate_after_invite_ack_); SWAP_FIELD(peer_directional_audio_v1_);
#undef SWAP_FIELD
  other.remote_sip_tcp_ = this->remote_sip_tcp_.exchange(other.remote_sip_tcp_);
  {
    LockGuard send_lock(this->tcp_send_mutex_);
    LockGuard pending_lock(this->tcp_tx_pending_mutex_);
    this->tcp_flow_.swap(other.tcp_flow_);
  }
  other.remote_ip_v4_ = this->remote_ip_v4_.exchange(other.remote_ip_v4_);
  other.remote_sip_port_ = this->remote_sip_port_.exchange(other.remote_sip_port_);
  other.outgoing_invite_pending_ = this->outgoing_invite_pending_.exchange(other.outgoing_invite_pending_);
  other.cancel_requested_ = this->cancel_requested_.exchange(other.cancel_requested_);
}

void SipTransport::swap_negotiation_(ReferDialog &other) {
  AudioFormat tx, rx;
  uint8_t tx_pt, rx_pt;
  RtpAudioPayloads payloads;
  this->get_media_config_(&tx, &rx, &tx_pt, &rx_pt, &payloads);
  this->set_media_config_(other.selected_tx_format_, other.selected_rx_format_,
      other.rtp_tx_payload_type_, other.rtp_rx_payload_type_, &other.negotiated_rx_payloads_);
  other.selected_tx_format_ = tx;
  other.selected_rx_format_ = rx;
  other.rtp_tx_payload_type_ = tx_pt;
  other.rtp_rx_payload_type_ = rx_pt;
  other.negotiated_rx_payloads_ = payloads;
  other.remote_rtp_ip_v4_ = this->remote_rtp_ip_v4_.exchange(other.remote_rtp_ip_v4_);
  other.remote_rtp_port_ = this->remote_rtp_port_.exchange(other.remote_rtp_port_);
  using std::swap;
  swap(this->remote_media_shape_, other.remote_media_shape_);
  swap(this->local_offered_rx_payloads_, other.local_offered_rx_payloads_);
  swap(this->local_offered_tx_formats_, other.local_offered_tx_formats_);
  swap(this->remote_directional_audio_v1_, other.remote_directional_audio_v1_);
  swap(this->local_offered_directional_audio_v1_, other.local_offered_directional_audio_v1_);
}

bool SipTransport::prepare_refer_media_(ReferDialog &other) {
  AudioFormat tx, rx;
  this->get_media_config_(&tx, &rx, nullptr, nullptr);
  if (!(tx == other.selected_tx_format_)) return false;
#ifdef USE_ESPHOME_VOIP_STACK_OPUS
  // Retain the existing REFER codec boundary. Per-packet fallback within one
  // negotiated call does not qualify changing a retained dialog's codec.
  if (rx.codec == AudioCodec::OPUS || other.selected_rx_format_.codec == AudioCodec::OPUS)
    return rx == other.selected_rx_format_;
#endif
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
  if (!rx.is_valid()) return !other.selected_rx_format_.is_valid();
  if (!this->pcm_receive_converter_.prepare_additional(other.negotiated_rx_payloads_.pcm_only(),
                                                       decoded_pcm_format(rx))) return false;
  other.selected_rx_format_ = rx;
  return true;
#else
  return rx == other.selected_rx_format_;
#endif
}

void SipTransport::pump_refer_() {
  if (this->refer_context_active_) return;
  for (auto &completed : this->completed_refers_) {
    if (completed && time_reached(millis(), completed->expires_ms)) completed.reset();
  }
  if (!this->refer_dialog_) return;
  {
    ReferScope scope(*this);
    this->pump_udp_retransmits_unlocked_();
  }
  if (!this->refer_retiring_ && this->call_id_ != this->refer_dialog_->owner_call_id) {
    ReferScope scope(*this);
    if (this->outgoing_invite_pending_.load()) this->send_cancel_unlocked_(this->call_id_);
    else if (!this->call_id_.empty()) this->send_bye_unlocked_(this->call_id_);
    this->refer_retiring_ = true;
  }
  if (this->refer_signal_pending_ && !this->refer_retiring_) {
    this->refer_signal_pending_ = false;
    const auto type = this->refer_signal_type_;
    if (type == SipSignalType::STATUS_200_OK && !this->call_id_.empty() &&
        (this->media_active_.load() || this->refer_dialog_->owner_invite_ended)) {
      if (!this->prepare_refer_media_(*this->refer_dialog_)) {
        { ReferScope scope(*this); this->send_bye_unlocked_(this->call_id_); }
        this->finish_failed_refer_(488, "Not Acceptable Here");
        return;
      }
      const std::string original = this->call_id_;
      const bool original_ended = this->refer_dialog_->owner_invite_ended;
      this->notify_refer_(this->refer_cseq_, 200, "OK");
      {
        ReferScope scope(*this);
        scope.commit();
      }
      this->open_media_session_();
      this->reset_rtp_latch_();
      this->refer_media_waiting_.store(false, std::memory_order_release);
      if (this->logical_call_id_.empty()) this->logical_call_id_ = original;
      this->logical_wire_id_ = this->call_id_;
      this->refer_wire_id_ = original;
      this->refer_retiring_ = true;
      {
        ReferScope scope(*this);
        if (original_ended) this->reset_dialog_();
        else this->send_bye_unlocked_(this->call_id_);
      }
      SipSignal signal;
      signal.type = SipSignalType::CONNECTED_IDENTITY;
      signal.media_source_changed = true;
      signal.call_id = this->call_id_;
      signal.connected_route = this->destination_.route;
      signal.connected_name = this->destination_.name;
      this->emit_sip_signal_(std::move(signal));
    } else if (type == SipSignalType::FINAL_RESPONSE || type == SipSignalType::PROTOCOL_ERROR ||
               type == SipSignalType::MEDIA_INCOMPATIBLE || type == SipSignalType::AUTH_REQUIRED ||
               type == SipSignalType::PROXY_AUTH_REQUIRED) {
      this->finish_failed_refer_(this->refer_status_ >= 300 ? this->refer_status_ : 500,
                                 "Transfer Failed");
    }
  }
  const auto &dialog = *this->refer_dialog_;
  if (this->refer_retiring_ && dialog.call_id_.empty() && dialog.pending_invite_.empty() &&
      dialog.pending_cancel_.empty() && dialog.pending_bye_.empty() &&
      dialog.pending_notify_.empty() && dialog.pending_notify_final_.empty()) {
    // RFC 3261 Timer J is zero on a reliable transport. Once both usages
    // finish, retain only transactions which can still be retransmitted on UDP.
    const bool udp_replay = (!dialog.accepted_refer.empty() && dialog.accepted_refer.udp) ||
        (!dialog.completed_control_.empty() && dialog.completed_control_.udp) ||
        (!dialog.completed_invite_client_.empty() && dialog.completed_invite_client_.udp);
    if (!udp_replay) {
      this->release_refer_dialog_();
      return;
    }
    for (auto &slot : this->completed_refers_) {
      if (slot) continue;
      auto *memory = RAMAllocator<CompletedRefer>().allocate(1);
      if (memory == nullptr) break;
      slot.reset(::new (memory) CompletedRefer());
      slot->accepted = std::move(this->refer_dialog_->accepted_refer);
      slot->control = std::move(this->refer_dialog_->completed_control_);
      slot->invite = std::move(this->refer_dialog_->completed_invite_client_);
      slot->expires_ms = millis() + SIP_TRANSACTION_TIMEOUT_MS;
      this->release_refer_dialog_();
      break;
    }
  }
  if (this->refer_dialog_ && time_reached(millis(), this->refer_expires_)) this->release_refer_dialog_();
}

}  // namespace voip_stack
}  // namespace esphome
#endif
