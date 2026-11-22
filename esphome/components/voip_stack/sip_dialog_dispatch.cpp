#include "sip_transport_internal.h"

#if defined(USE_ESP32) && defined(USE_ESPHOME_VOIP_SIP_TRANSPORT)

namespace esphome {
namespace voip_stack {

#ifdef USE_ESPHOME_VOIP_PAGING
void SipTransport::set_paging_auto_answer_peers(const std::vector<std::string> &peers) {
  LockGuard lock(this->dialog_mutex_);
  this->paging_auto_answer_peers_.clear();
  for (const auto &peer : peers) {
    in_addr address{};
    if (this->paging_auto_answer_peers_.size() >= 16) break;
    if (inet_pton(AF_INET, peer.c_str(), &address) == 1) {
      const uint32_t value = ntohl(address.s_addr);
      if (value != 0 && std::find(this->paging_auto_answer_peers_.begin(), this->paging_auto_answer_peers_.end(), value) ==
                            this->paging_auto_answer_peers_.end())
        this->paging_auto_answer_peers_.push_back(value);
    }
  }
}
#endif

bool SipTransport::handle_invite_(const std::string &message, const sockaddr_in &src) {
  const std::string body = message_body(message);
  const uint32_t src_ip = ntohl(src.sin_addr.s_addr);
  const std::string incoming_call_id = header_value(message, "Call-ID");
  const std::string incoming_via = header_values(message, "Via");
  const std::string incoming_from = header_value(message, "From");
  const std::string incoming_to = header_value(message, "To");
  const std::string incoming_cseq = header_value(message, "CSeq");
  const uint32_t incoming_cseq_number = cseq_number(incoming_cseq);
  if (incoming_call_id.empty() || incoming_via.empty() || incoming_from.empty() || incoming_to.empty() ||
      incoming_cseq_number == 0 || cseq_method(incoming_cseq) != "INVITE" ||
      tag_from_header(incoming_from).empty()) {
    ESP_LOGW(TAG, "SIP INVITE rejected: missing or invalid transaction headers");
    return this->send_stateless_response_(message, src, 400, "Bad Request");
  }
    if (this->replay_completed_response_(message, src, "INVITE",
#ifdef USE_ESPHOME_VOIP_REGISTRATION
                                        this->dispatch_is_udp_()
#else
                                        !this->remote_sip_tcp_.load(std::memory_order_acquire)
#endif
                                        )) {
    return true;
  }
  const std::string incoming_from_tag = tag_from_header(incoming_from);
  const std::string incoming_to_tag = tag_from_header(incoming_to);
  const bool matching_dialog = incoming_call_id == this->call_id_ &&
      !this->local_tag_.empty() && !this->remote_tag_.empty() &&
      incoming_to_tag == this->local_tag_ && incoming_from_tag == this->remote_tag_;
  // RFC 3261 sections 8.2.2 and 12.2.2: a To-tag addresses a dialog,
  // never a new initial INVITE. Validate before changing any owned metadata.
  if (!incoming_to_tag.empty() && !matching_dialog) {
    return this->send_stateless_response_(message, src, 481,
                                          "Call/Transaction Does Not Exist");
  }
  if (this->terminal_transaction_pending_locked_()) {
    const bool same_dialog = incoming_call_id == this->call_id_;
    ESP_LOGW(TAG,
             "SIP INVITE deferred while prior dialog transaction completes");
    return this->send_stateless_response_(
        message, src, same_dialog ? 500 : 503,
        same_dialog ? "Server Internal Error" : "Service Unavailable",
        "transaction_pending", false, 1);
  }
  std::string incoming_caller_name =
      sip_display_name_from_header(incoming_from, VOIP_STACK_MAX_NAME_LEN);
  std::string incoming_dest_name =
      sip_display_name_from_header(incoming_to, VOIP_STACK_MAX_NAME_LEN);
  if (incoming_caller_name.empty()) {
    incoming_caller_name = sip_route_id(
        header_value(message, "X-Voip-Stack-Caller-Name"),
        VOIP_STACK_MAX_NAME_LEN);
  }
  if (incoming_dest_name.empty()) {
    incoming_dest_name = sip_route_id(
        header_value(message, "X-Voip-Stack-Dest-Name"),
        VOIP_STACK_MAX_NAME_LEN);
  }
  if (incoming_caller_name.empty()) {
    incoming_caller_name = sip_route_id(sip_user_from_header(incoming_from), VOIP_STACK_MAX_NAME_LEN);
  }
  if (incoming_dest_name.empty()) {
    incoming_dest_name = sip_route_id(sip_user_from_header(incoming_to), VOIP_STACK_MAX_NAME_LEN);
  }
  if (!incoming_call_id.empty() && !this->call_id_.empty() && incoming_call_id != this->call_id_) {
    const uint32_t active_peer_ip = this->remote_ip_v4_.load(std::memory_order_acquire);
    const bool glare = this->outgoing_invite_pending_.load(std::memory_order_acquire) &&
                       active_peer_ip == src_ip && !this->caller_.name.empty() &&
                       incoming_caller_name == this->destination_.name;
    if (!glare) {
      ESP_LOGW(TAG, "SIP INVITE rejected busy: active_call_id=%s incoming_call_id=%s",
               this->call_id_.c_str(), incoming_call_id.c_str());
      return this->send_stateless_response_(message, src, 486, "Busy Here", "busy", true);
    }
    // One transport owns one dialog. Do not destroy the active client INVITE
    // (and its CANCEL/ACK lifetime) to accept a crossed initial INVITE in the
    // same storage. Ask the peer to retry after this transaction completes.
    ESP_LOGW(TAG, "SIP glare with %s: retaining local INVITE",
             incoming_caller_name.c_str());
    return this->send_stateless_response_(
        message, src, 491, "Request Pending", "glare", true,
        static_cast<int>(esp_random() % 3U));
  }
  const uint32_t active_peer_ip = this->remote_ip_v4_.load(std::memory_order_acquire);
  if (!this->call_id_.empty() && incoming_call_id == this->call_id_ && active_peer_ip != 0 &&
      src_ip != active_peer_ip) {
    ESP_LOGW(TAG, "SIP INVITE rejected for active call_id=%s from unexpected peer",
             incoming_call_id.c_str());
    return this->send_stateless_response_(message, src, 481,
                                          "Call/Transaction Does Not Exist");
  }
  int incoming_account = -1;
  if (!matching_dialog && this->call_id_.empty()) {
#ifdef USE_ESPHOME_VOIP_REGISTRATION
    incoming_account = this->registration_.match_incoming(sip_user_from_header(sip_request_uri(message)), src_ip,
        this->dispatch_socket_ >= 0, this->registration_socket_index_(this->dispatch_socket_));
#endif
    if (!this->accept_direct_calls_ && incoming_account < 0)
      return this->send_stateless_response_(message, src, 403, "Forbidden");
  }
  const bool in_dialog_invite = matching_dialog;
  if (in_dialog_invite &&
      incoming_cseq_number == this->last_invite_cseq_number_ &&
      via_branch(incoming_via) != via_branch(this->last_invite_via_)) {
    ESP_LOGW(TAG, "SIP merged in-dialog INVITE rejected for call_id=%s",
             incoming_call_id.c_str());
    return this->send_stateless_response_(message, src, 482,
                                          "Loop Detected");
  }
  if (in_dialog_invite) {
    return this->handle_reinvite_(message, src);
  }
  if (!incoming_call_id.empty() && incoming_call_id == this->call_id_ &&
      this->last_invite_cseq_number_ == incoming_cseq_number) {
    const std::string incoming_branch = via_branch(incoming_via);
    const std::string active_branch = via_branch(this->last_invite_via_);
    if (!active_branch.empty() && incoming_branch != active_branch) {
      ESP_LOGW(TAG, "SIP merged INVITE rejected for call_id=%s", incoming_call_id.c_str());
      return this->send_stateless_response_(message, src, 482, "Loop Detected");
    }
    if (!this->last_invite_response_.empty()) {
      ESP_LOGD(TAG, "SIP INVITE retransmission replaying latest provisional response");
      this->send_sip_(this->last_invite_response_, src_ip, ntohs(src.sin_port));
      return true;
    }
  }
  if (!this->call_id_.empty()) {
    return this->send_stateless_response_(message, src, 486, "Busy Here", "busy");
  }
  // The application can own an outgoing attempt before its INVITE reaches
  // the wire. Authorize/replay first, then apply the same gate to UDP and TCP.
  if (!this->should_accept_session_())
    return this->send_stateless_response_(message, src, 486, "Busy Here", "busy");
#ifdef USE_ESPHOME_VOIP_PAGING
  const auto answer_mode = sip_requested_answer_mode(message);
  if (!answer_mode.valid)
    return this->send_stateless_response_(message, src, 400, "Bad Request");
  bool account_matched = false, account_allowed = false;
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  account_matched = incoming_account >= 0;
  if (account_matched)
    account_allowed = this->registration_.account(incoming_account).config.allow_paging_auto_answer;
#endif
  const bool direct_peer_allowed = std::find(this->paging_auto_answer_peers_.begin(), this->paging_auto_answer_peers_.end(), src_ip) !=
                                   this->paging_auto_answer_peers_.end();
  const bool paging_auto_answer = sip_paging_auto_answer_authorized(
      answer_mode.automatic, account_matched, account_allowed, direct_peer_allowed);
  if (answer_mode.automatic && answer_mode.required && !paging_auto_answer)
    return this->send_stateless_response_(message, src, 403, "Forbidden", "automatic_answer_forbidden");
#endif
  {
    LockGuard media_lock(this->media_lifecycle_mutex_);
    const MediaLifecyclePhase phase =
        this->media_lifecycle_phase_.load(std::memory_order_acquire);
    if (phase == MediaLifecyclePhase::CLEANING ||
        phase == MediaLifecyclePhase::SHUTTING_DOWN) {
      // A prior call still owns worker/socket teardown. Reject the new
      // transaction before mutating dialog metadata so the peer can retry
      // cleanly instead of receiving a late local transport failure.
      return this->send_stateless_response_(
          message, src, 503, "Service Unavailable", "media_cleanup", true, 1);
    }
  }
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  if (this->dispatch_socket_ >= 0 && !this->claim_inbound_tcp_(this->dispatch_socket_))
    return this->send_stateless_response_(message, src, 503, "Service Unavailable", "retry_new_connection", false, 1);
  if (this->dispatch_socket_ == -1) this->remote_sip_tcp_.store(false, std::memory_order_release);
#endif
  this->remote_ip_v4_.store(src_ip, std::memory_order_release);
  this->remote_sip_port_.store(ntohs(src.sin_port), std::memory_order_release);
  this->logical_call_id_.clear();
  this->logical_wire_id_.clear();
  this->call_id_ = incoming_call_id;
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
  this->reset_startup_trace_();
#endif
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  this->active_account_ = incoming_account;
  this->dialog_account_socket_ = incoming_account >= 0 ? this->dispatch_socket_ : -1;
  if (incoming_account >= 0) this->remember_registration_routes_(message, false);
#endif
  this->sdp_session_id_ = esp_random();
  if (this->sdp_session_id_ == 0) this->sdp_session_id_ = 1;
  this->sdp_session_version_ = 0;
  this->dialog_originated_ = false;
  this->last_invite_via_ = incoming_via;
  this->last_invite_from_ = incoming_from;
  this->last_invite_to_ = incoming_to;
  this->last_invite_cseq_ = incoming_cseq;
  this->last_invite_cseq_number_ = incoming_cseq_number;
  this->remote_dialog_cseq_ = incoming_cseq_number;
  this->last_invite_peer_ip_v4_ = src_ip;
  this->last_invite_peer_port_ = ntohs(src.sin_port);
  this->remote_tag_ = tag_from_header(this->last_invite_from_);
  this->peer_supports_from_change_ =
      sip_option_supported(message, "from-change");
  if (this->local_tag_.empty()) this->local_tag_ = make_token("tag");
  const std::string remote_identity_uri = strip_angle_uri(this->last_invite_from_);
  this->remote_uri_ = remote_identity_uri.empty()
                          ? ""
                          : sip_name_addr(remote_identity_uri,
                                          incoming_caller_name);
  this->remote_target_uri_ = strip_angle_uri(header_value(message, "Contact"));
  if (this->remote_target_uri_.empty()) {
    this->remote_target_uri_ = strip_angle_uri(this->last_invite_from_);
  }
  const std::string local_identity_uri =
      strip_angle_uri(this->last_invite_to_);
  this->local_uri_ = sip_name_addr(
      local_identity_uri,
      incoming_dest_name);
  std::string local_contact_ip = "0.0.0.0";
  this->local_ip_for_peer_(src_ip, &local_contact_ip);
  const char *contact_transport = this->remote_sip_tcp_.load(std::memory_order_acquire) ? "tcp" : "udp";
  const std::string local_contact_user = sip_uri_user_encode(sip_user_from_header(this->last_invite_to_));
  this->local_contact_uri_ =
      "<sip:" + local_contact_user + "@" + local_contact_ip + ":" +
      std::to_string(this->sip_port_) + ";transport=" + contact_transport + ">";
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  if (this->active_account_ >= 0)
    this->local_contact_uri_ = sip_name_addr(this->registration_.account(this->active_account_).contact);
#endif
  bool media_compatible = false;
  {
    LockGuard media_lock(this->media_lifecycle_mutex_);
    if (body.empty()) {
      const auto &tx = this->offer_tx_formats_;
      const auto &rx = this->offer_rx_formats_;
      const auto &preferred = tx.count == 0 ? rx : tx;
      // Seed the pending offer with configured RTP-compatible directions.
      // Their packet durations remain independent until the ACK answers it.
      for (uint8_t i = 0; i < preferred.count; ++i) {
        const auto &format = preferred.formats[i];
        AudioFormat receive;
        if (!format.is_valid() || audio_format_rtp_encoding(format, this->udp_max_payload_) == nullptr ||
            !audio_format_list_match_rtp_mapping(rx.count == 0 ? tx : rx, format, 0, &receive,
                                                this->udp_max_payload_)) continue;
        this->set_media_config_(tx.count == 0 ? disabled_audio_format() : format,
                                rx.count == 0 ? disabled_audio_format() : receive, 96, 96);
        this->delayed_offer_pending_ = true;
        media_compatible = true;
        break;
      }
    } else {
      this->delayed_offer_pending_ = false;
      media_compatible = this->learn_remote_rtp_from_sdp_(body, src_ip);
    }
  }
  if (!media_compatible) {
    const bool sent = this->send_response_(488, "Not Acceptable Here");
    this->reset_dialog_();
    return sent;
  }
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  if (auto *flow = this->tcp_flow_for_socket_(this->dispatch_socket_); flow != nullptr)
    flow->ingress_deadline = 0;
#endif
  this->send_response_(100, "Trying");

  std::string from_user = incoming_caller_name;
  std::string to_user = incoming_dest_name;
  if (from_user.empty() || to_user.empty()) {
    const bool sent = this->send_response_(400, "Bad Request");
    this->reset_dialog_();
    return sent;
  }
  this->caller_.name = from_user;
  this->destination_.name = to_user;
  this->caller_.route = sip_route_id(
      sip_user_from_header(incoming_from), VOIP_STACK_MAX_ROUTE_ID_LEN);
  this->destination_.route = sip_route_id(
      sip_user_from_header(sip_request_uri(message)),
      VOIP_STACK_MAX_ROUTE_ID_LEN);
  if (this->caller_.route.empty()) {
    this->caller_.route = sip_route_id(
        header_value(message, "X-Voip-Stack-Caller-Route"),
        VOIP_STACK_MAX_ROUTE_ID_LEN);
  }
  if (this->destination_.route.empty()) {
    this->destination_.route = sip_route_id(
        header_value(message, "X-Voip-Stack-Dest-Route"),
        VOIP_STACK_MAX_ROUTE_ID_LEN);
  }
  if (this->caller_.route.empty()) this->caller_.route = this->caller_.name;
  if (this->destination_.route.empty())
    this->destination_.route = this->destination_.name;

  ESP_LOGI(TAG, "SIP INVITE accepted into FSM call_id=%s", this->call_id_.c_str());
  AudioFormat selected_tx;
  AudioFormat selected_rx;
  this->get_media_config_(&selected_tx, &selected_rx, nullptr, nullptr);
  SipSignal signal;
  signal.type = SipSignalType::INVITE;
  signal.call_id = this->call_id_;
  signal.caller_route = this->caller_.route;
  signal.caller_name = this->caller_.name;
  signal.dest_route = this->destination_.route;
  signal.dest_name = this->destination_.name;
  signal.caller_tx_formats.formats[0] = selected_rx;
  signal.caller_tx_formats.count = selected_rx.is_valid() ? 1 : 0;
  signal.caller_rx_formats.formats[0] = selected_tx;
  signal.caller_rx_formats.count = selected_tx.is_valid() ? 1 : 0;
  signal.selected_rx_format = selected_rx;
  signal.selected_tx_format = selected_tx;
  signal.media_answer_pending = this->delayed_offer_pending_;
  signal.from_registered_account = incoming_account >= 0;
#ifdef USE_ESPHOME_VOIP_PAGING
  signal.paging_auto_answer = paging_auto_answer;
  signal.paging_manual_answer = answer_mode.manual;
#endif
  this->emit_sip_signal_(std::move(signal));
  return true;
}

bool SipTransport::handle_reinvite_(const std::string &message,
                                    const sockaddr_in &src) {
  if (this->refer_dialog_ && this->refer_dialog_->owner_invite_ended && !this->refer_retiring_)
    return this->send_stateless_response_(message, src, 481, "Call/Transaction Does Not Exist");
  if (this->refer_dialog_ && !this->refer_retiring_)
    return this->send_stateless_response_(message, src, 491, "Request Pending");
  const uint32_t src_ip = ntohl(src.sin_addr.s_addr);
  const uint32_t incoming_cseq =
      cseq_number(header_value(message, "CSeq"));
  const std::string incoming_from = header_value(message, "From");
  const std::string incoming_to = header_value(message, "To");
  const std::string content_type =
      trim_copy(header_value(message, "Content-Type"));
  const size_t content_type_end = content_type.find(';');
  std::string media_type = content_type.substr(0, content_type_end);
  for (char &ch : media_type) {
    ch = static_cast<char>(
        std::tolower(static_cast<unsigned char>(ch)));
  }

  // RFC 3261 section 14: a re-INVITE belongs to the existing dialog and must
  // not create a second call lifecycle. Reject stale/cross-dialog requests
  // without disturbing the established media session.
  if (!this->media_active_.load(std::memory_order_acquire) ||
      src_ip != this->remote_ip_v4_.load(std::memory_order_acquire) ||
      tag_from_header(incoming_from) != this->remote_tag_ ||
      tag_from_header(incoming_to) != this->local_tag_) {
    return this->send_stateless_response_(
        message, src, 481, "Call/Transaction Does Not Exist", "", true);
  }
  if (this->completed_invite_.awaiting_ack) {
    // RFC 3261 section 14.2: a UAS that receives another INVITE before the
    // prior INVITE transaction is ACKed answers 500 with randomized
    // Retry-After. The occupied transaction cache must not be replaced.
    return this->send_stateless_response_(
        message, src, 500, "Server Internal Error", "request_pending",
        false, static_cast<int>(esp_random() % 11U));
  }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  if (this->pending_video_direction_invite_.pending()) {
    // 491 is reserved for glare with our own outstanding in-dialog INVITE.
    return this->send_stateless_response_(
        message, src, 491, "Request Pending");
  }
#endif
  if (incoming_cseq <= this->remote_dialog_cseq_) {
    return this->send_stateless_response_(
        message, src, 500, "Server Internal Error", "stale_cseq", true,
        static_cast<int>(esp_random() % 11U));
  }
  if (media_type != "application/sdp" || message_body(message).empty()) {
    return this->send_stateless_response_(
        message, src, 415, "Unsupported Media Type", "", true);
  }

  // handle_sip_datagram_ already owns dialog_mutex_. Serialize the complete
  // media proposal/admission/commit below against deferred main-loop starts
  // and concurrent teardown. No code under this lock reacquires dialog_mutex_.
  LockGuard media_lock(this->media_lifecycle_mutex_);
  const MediaLifecyclePhase phase =
      this->media_lifecycle_phase_.load(std::memory_order_acquire);
  if (phase == MediaLifecyclePhase::CLEANING ||
      phase == MediaLifecyclePhase::SHUTTING_DOWN ||
      !this->media_active_.load(std::memory_order_acquire)) {
    return this->send_stateless_response_(
        message, src, 491, "Request Pending");
  }

  const auto old_media = this->negotiated_media_();
  const auto &old_tx = old_media.tx, &old_rx = old_media.rx;
  const auto old_tx_pt = old_media.tx_pt, old_rx_pt = old_media.rx_pt;
  const auto &old_rx_payloads = old_media.rx_payloads;
  const auto old_audio_ip = old_media.audio_ip;
  const auto old_audio_port = old_media.audio_port;
  const auto old_audio_receive = old_media.receive;
  const std::string old_last_invite_via = this->last_invite_via_;
  const std::string old_last_invite_from = this->last_invite_from_;
  const std::string old_last_invite_to = this->last_invite_to_;
  const std::string old_last_invite_cseq = this->last_invite_cseq_;
  const std::string old_last_invite_response =
      this->last_invite_response_;
  const uint32_t old_last_invite_cseq_number =
      this->last_invite_cseq_number_;
  const uint32_t old_remote_dialog_cseq = this->remote_dialog_cseq_;
  const uint32_t old_last_invite_peer_ip =
      this->last_invite_peer_ip_v4_;
  const uint16_t old_last_invite_peer_port =
      this->last_invite_peer_port_;
  const std::string old_remote_target_uri = this->remote_target_uri_;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  const auto old_video_negotiated = old_media.video_negotiated;
  const auto old_video_send = old_media.video_send, old_video_receive = old_media.video_receive;
  const auto old_video_ip = old_media.video_ip, old_video_rtcp_ip = old_media.video_rtcp_ip;
  const auto old_video_port = old_media.video_port, old_video_rtcp_port = old_media.video_rtcp_port;
  const auto &old_video_capability = old_media.video;
#endif

  const auto restore_old_dialog_metadata = [&]() {
    this->last_invite_via_ = old_last_invite_via;
    this->last_invite_from_ = old_last_invite_from;
    this->last_invite_to_ = old_last_invite_to;
    this->last_invite_cseq_ = old_last_invite_cseq;
    this->last_invite_response_ = old_last_invite_response;
    this->last_invite_cseq_number_ = old_last_invite_cseq_number;
    this->remote_dialog_cseq_ = old_remote_dialog_cseq;
    this->last_invite_peer_ip_v4_ = old_last_invite_peer_ip;
    this->last_invite_peer_port_ = old_last_invite_peer_port;
    this->remote_target_uri_ = old_remote_target_uri;
  };

  ScopedMediaProposal proposal_scope(this->media_proposal_epoch_);
  NegotiatedMedia proposed;
  if (!this->parse_remote_media_(message_body(message), src_ip, false, old_media, &proposed, true)) {
    return this->send_stateless_response_(
        message, src, 488, "Not Acceptable Here", "media_incompatible", true);
  }
  const uint32_t proposed_sdp_version = this->sdp_session_version_ + 1;
  const std::string answer = this->build_negotiated_sdp_answer_(proposed, proposed_sdp_version);
  if (answer.empty()) {
    return this->send_stateless_response_(
        message, src, 488, "Not Acceptable Here", "media_incompatible", true);
  }
  const auto &new_tx = proposed.tx, &new_rx = proposed.rx;
  const auto new_tx_pt = proposed.tx_pt, new_rx_pt = proposed.rx_pt;
  const auto &new_rx_payloads = proposed.rx_payloads;
  const auto new_audio_ip = proposed.audio_ip;
  const auto new_audio_port = proposed.audio_port;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  const auto new_video_negotiated = proposed.video_negotiated;
  const auto new_video_send = proposed.video_send, new_video_receive = proposed.video_receive;
  const auto new_video_ip = proposed.video_ip, new_video_rtcp_ip = proposed.video_rtcp_ip;
  const auto new_video_port = proposed.video_port, new_video_rtcp_port = proposed.video_rtcp_port;
  const auto &new_video_capability = proposed.video;
#endif

  const bool same_audio =
      new_tx == old_tx && new_rx == old_rx &&
      new_tx_pt == old_tx_pt && new_rx_pt == old_rx_pt &&
      new_rx_payloads == old_rx_payloads &&
      new_audio_ip == old_audio_ip && new_audio_port == old_audio_port;
  if (!same_audio) {
    return this->send_stateless_response_(
        message, src, 488, "Not Acceptable Here",
        "audio_renegotiation_unsupported", true);
  }

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  // Keep one authoritative media child. A video attachment can be admitted
  // before the final response when this re-INVITE adds it to an audio-only
  // dialog. Codec/PT changes and active endpoint retargeting remain unsupported.
  // RFC3264 permits inactive endpoint changes: admit them only while both
  // directions are inactive and commit through the existing prepared child.
  const bool same_video_format =
      old_video_capability.payload_type == new_video_capability.payload_type &&
      old_video_capability.receive_payload_type() == new_video_capability.receive_payload_type() &&
      old_video_capability.clock_rate == new_video_capability.clock_rate &&
      old_video_capability.max_fps == new_video_capability.max_fps &&
      old_video_capability.packetization_mode ==
          new_video_capability.packetization_mode &&
      old_video_capability.level_asymmetry_allowed ==
          new_video_capability.level_asymmetry_allowed &&
      old_video_capability.encoding == new_video_capability.encoding &&
      old_video_capability.profile_level_id ==
          new_video_capability.profile_level_id &&
      old_video_capability.max_bitrate_bps ==
          new_video_capability.max_bitrate_bps &&
      old_video_capability.rtcp_feedback_pli ==
          new_video_capability.rtcp_feedback_pli &&
      old_video_capability.rtcp_feedback_fir ==
          new_video_capability.rtcp_feedback_fir;
  const bool same_video_media = same_video_format &&
      old_video_ip == new_video_ip &&
      old_video_port == new_video_port &&
      old_video_rtcp_ip == new_video_rtcp_ip &&
      old_video_rtcp_port == new_video_rtcp_port;
  const bool same_video_direction =
      old_video_send == new_video_send &&
      old_video_receive == new_video_receive;
  const bool inactive_video_retarget =
      old_video_negotiated && new_video_negotiated && same_video_format &&
      !old_video_send && !old_video_receive && !new_video_send && !new_video_receive &&
      !same_video_media && this->video_session_ != nullptr &&
      this->video_session_->can_retarget_inactive_remote();
  if (old_video_negotiated && new_video_negotiated &&
      !same_video_media && !inactive_video_retarget) {
    return this->send_stateless_response_(
        message, src, 488, "Not Acceptable Here",
        "video_renegotiation_unsupported", true);
  }

  bool new_video_prepared = false;
  bool video_direction_requested = false;
  bool video_remove_requested = false;
  const bool replace_video_direction =
      old_video_negotiated && new_video_negotiated &&
      !same_video_direction;
  if (!old_video_negotiated && new_video_negotiated) {
    if (this->video_session_ == nullptr) {
      return this->send_stateless_response_(
          message, src, 488, "Not Acceptable Here",
          "video_resources_unavailable", true);
    }
    if (this->video_session_->is_running()) {
      if (!this->video_session_->negotiation_matches(
              new_video_capability,
              this->local_video_direction_capability_(
                  new_video_capability, true),
              this->local_video_direction_capability_(
                  new_video_capability, false),
              new_video_ip, new_video_port,
              new_video_rtcp_ip, new_video_rtcp_port) ||
          !this->video_session_->can_request_media_direction(
              new_video_send, new_video_receive)) {
        return this->send_stateless_response_(
            message, src, 488, "Not Acceptable Here",
            "video_renegotiation_unsupported", true);
      }
      video_direction_requested = true;
    } else if (!this->video_session_->set_negotiated(
                   new_video_capability,
                   this->local_video_direction_capability_(
                       new_video_capability, true),
                   this->local_video_direction_capability_(
                       new_video_capability, false),
                   new_video_ip, new_video_port,
                   new_video_rtcp_ip, new_video_rtcp_port, new_video_send,
                   new_video_receive) ||
               !this->video_session_->start(false)) {
      if (this->video_session_ != nullptr)
        this->video_session_->request_stop();
      return this->send_stateless_response_(
          message, src, 488, "Not Acceptable Here",
          "video_resources_unavailable", true);
    } else {
      new_video_prepared = true;
    }
    video_direction_requested = true;
  } else if ((replace_video_direction ||
              (old_video_negotiated && !new_video_negotiated)) &&
             (this->video_session_ == nullptr ||
              !this->video_session_->can_request_media_direction(
                  new_video_negotiated && new_video_send,
                  new_video_negotiated && new_video_receive))) {
    return this->send_stateless_response_(
        message, src, 488, "Not Acceptable Here",
        "video_resources_unavailable", true);
  } else if (replace_video_direction ||
             (old_video_negotiated && !new_video_negotiated)) {
    video_remove_requested =
        old_video_negotiated && !new_video_negotiated;
    video_direction_requested = !video_remove_requested;
  }
#endif

  this->last_invite_via_ = header_values(message, "Via");
  this->last_invite_from_ = incoming_from;
  this->last_invite_to_ = incoming_to;
  this->last_invite_cseq_ = header_value(message, "CSeq");
  this->last_invite_cseq_number_ = incoming_cseq;
  this->remote_dialog_cseq_ = incoming_cseq;
  this->last_invite_peer_ip_v4_ = src_ip;
  this->last_invite_peer_port_ = ntohs(src.sin_port);
  const std::string refreshed_target =
      strip_angle_uri(header_value(message, "Contact"));
  if (!refreshed_target.empty())
    this->remote_target_uri_ = refreshed_target;

  // Resource admission above has completed, so the final response never
  // promises a newly-added video stream that failed to allocate. Existing
  // media is left untouched until this offer/answer commits.
  if (!this->send_response_(200, "OK", answer)) {
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
    if (new_video_prepared && this->video_session_ != nullptr)
      this->video_session_->request_stop();
#endif
    restore_old_dialog_metadata();
    return false;
  }

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  // The final response commits offer/answer. Only now may the prepared camera
  // producer or receive presentation become active.
  if ((old_video_negotiated || new_video_negotiated) && this->video_session_ != nullptr) {
    // RFC 3264 keeps RTCP for ordinary inactive media, but forbids sending
    // either protocol to an effective legacy zero connection address.
    this->video_session_->set_rtcp_send_enabled(!proposed.video_zero_connection);
  }
  if (video_remove_requested) {
    // Publish the inactive direction before terminating the RTP child so the
    // camera and renderer leave video mode immediately on m=video 0.
    this->video_session_->request_media_direction(false, false);
    this->video_session_->request_stop();
  } else if ((inactive_video_retarget &&
              !this->video_session_->retarget_inactive_remote(
                  new_video_ip, new_video_port, new_video_rtcp_ip, new_video_rtcp_port)) ||
             (video_direction_requested &&
              !this->video_session_->request_media_direction(
                  new_video_negotiated && new_video_send,
                  new_video_negotiated && new_video_receive))) {
    ESP_LOGE(TAG,
             "Prepared video direction failed after 200; terminating dialog");
    // A UAS must not originate BYE before the ACK for its 2xx response. Gate
    // media now and let the ACK path (or its timeout) terminate the dialog.
    // media_lifecycle_mutex_ is already held, so use the lock-aware primitive.
    this->request_audio_path_stop_locked_();
    this->terminate_after_invite_ack_ = true;
    SipSignal signal;
    signal.type = SipSignalType::MEDIA_INCOMPATIBLE;
    signal.status_code = 500;
    signal.call_id = this->call_id_;
    signal.reason = "video_activation_failed";
    signal.terminal_transaction_pending = true;
    this->emit_sip_signal_(std::move(signal));
    return true;
  }
#endif

  this->commit_negotiated_media_(proposed);
  // Only an actual RX resume creates notifications. These are consumed by
  // the worker and never copied into negotiated signaling values.
  if (proposed.receive && !old_audio_receive) {
    this->audio_receive_reset_pending_.store(true, std::memory_order_release);
    this->rtp_source_reacquire_pending_.store(true, std::memory_order_release);
  }
  this->sdp_session_version_ = proposed_sdp_version;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  this->confirmed_local_sdp_ = answer;
  this->emit_video_active_state_(
      this->video_negotiated_ && this->video_session_ != nullptr &&
      this->video_session_->is_running() &&
      (this->video_send_enabled_ || this->video_receive_enabled_));
  this->emit_video_send_state_(
      this->video_negotiated_ && this->video_send_enabled_, false);
#endif
  ESP_LOGI(TAG, "SIP re-INVITE accepted in existing dialog call_id=%s cseq=%u",
           this->call_id_.c_str(), (unsigned) incoming_cseq);
  return true;
}

bool SipTransport::handle_update_(const std::string &message,
                                  const sockaddr_in &src) {
  if (this->replay_completed_response_(message, src, "UPDATE", !this->remote_sip_tcp_.load(std::memory_order_acquire))) {
    return true;
  }
  const std::string call_id = header_value(message, "Call-ID");
  const std::string from = header_value(message, "From");
  const std::string to = header_value(message, "To");
  const std::string cseq = header_value(message, "CSeq");
  const uint32_t sequence = cseq_number(cseq);
  if (!this->media_active_.load(std::memory_order_acquire) ||
      call_id.empty() || call_id != this->call_id_ ||
      cseq_method(cseq) != "UPDATE" || sequence == 0 ||
      tag_from_header(from) != this->remote_tag_ ||
      tag_from_header(to) != this->local_tag_) {
    return this->send_stateless_response_(
        message, src, 481, "Call/Transaction Does Not Exist", "", true);
  }
  if (sequence <= this->remote_dialog_cseq_) {
    return this->send_stateless_response_(
        message, src, 500, "Server Internal Error", "stale_cseq", true,
        static_cast<int>(esp_random() % 11U));
  }
  if (!message_body(message).empty()) {
    return this->send_stateless_response_(
        message, src, 488, "Not Acceptable Here",
        "media_update_unsupported", true);
  }
  const std::string identity_uri = strip_angle_uri(from);
  const std::string identity_route = sip_route_id(
      sip_user_from_header(from), VOIP_STACK_MAX_ROUTE_ID_LEN);
  if (identity_uri.empty() || identity_route.empty()) {
    return this->send_stateless_response_(
        message, src, 400, "Bad Request", "", true);
  }
  const std::string contact = header_value(message, "Contact");
  const std::string refreshed_target = strip_angle_uri(contact);
  if (!contact.empty() && refreshed_target.empty()) {
    return this->send_stateless_response_(
        message, src, 400, "Bad Request", "", true);
  }
  if (!this->send_stateless_response_(message, src, 200, "OK", "", true)) {
    return false;
  }

  std::string identity_name = sip_display_name_from_header(
      from, VOIP_STACK_MAX_NAME_LEN);
  if (identity_name.empty()) identity_name = identity_route;
  this->remote_uri_ = sip_name_addr(identity_uri, identity_name);
  if (!refreshed_target.empty()) this->remote_target_uri_ = refreshed_target;
  this->remote_dialog_cseq_ = sequence;
  if (this->dialog_originated_) {
    this->destination_ = {identity_route, identity_name};
  } else {
    this->caller_ = {identity_route, identity_name};
  }
  SipSignal signal;
  signal.type = SipSignalType::CONNECTED_IDENTITY;
  signal.call_id = this->call_id_;
  signal.connected_route = identity_route;
  signal.connected_name = identity_name;
  this->emit_sip_signal_(std::move(signal));
  return true;
}

void SipTransport::handle_sip_datagram_(const char *data, size_t len, const sockaddr_in &src) {
  LockGuard lock(this->dialog_mutex_);
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  this->dispatch_socket_ = -1;
#endif
  this->handle_sip_datagram_unlocked_(data, len, src);
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  this->dispatch_socket_ = -2;
#endif
}

void SipTransport::handle_sip_datagram_unlocked_(const char *data, size_t len, const sockaddr_in &src) {
  const std::string msg(data, len);
  const SipStartLine start(msg);
  if (!sip_record_content_length(msg, MAX_SIP_TCP_RX_BUFFER).matches_body(msg)) {
    ESP_LOGW(TAG, "SIP message rejected: invalid Content-Length framing");
    if (!start.response) {
      this->send_stateless_response_(msg, src, 400, "Bad Request");
    }
    return;
  }
  this->dispatch_dialog_message_(msg, start, src);
}

void SipTransport::handle_options_(const std::string &message, const sockaddr_in &src) {
  const std::string sequence = header_value(message, "CSeq");
  if (cseq_method(sequence) != "OPTIONS" || cseq_number(sequence) == 0 ||
      header_value(message, "Call-ID").empty()) {
    this->send_stateless_response_(message, src, 400, "Bad Request");
    return;
  }
  const std::string to_tag = tag_from_header(header_value(message, "To"));
  const bool in_dialog = !this->call_id_.empty() && header_value(message, "Call-ID") == this->call_id_ &&
      !this->local_tag_.empty() && to_tag == this->local_tag_ &&
      tag_from_header(header_value(message, "From")) == this->remote_tag_ &&
      ntohl(src.sin_addr.s_addr) == this->remote_ip_v4_.load(std::memory_order_acquire);
  if (!to_tag.empty() && !in_dialog) {
    this->send_stateless_response_(message, src, 481, "Call/Transaction Does Not Exist");
    return;
  }
  if (!in_dialog) {
    if (!this->running_.load(std::memory_order_acquire) || this->transport_stopping_.load(std::memory_order_acquire) ||
        this->terminal_transaction_pending_locked_()) {
      this->send_stateless_response_(message, src, 503, "Service Unavailable", "", false, 1);
      return;
    }
    if (!this->call_id_.empty() || this->media_active_.load(std::memory_order_acquire) ||
        this->outgoing_invite_pending_.load(std::memory_order_acquire) || !this->should_accept_session_()) {
      this->send_stateless_response_(message, src, 486, "Busy Here");
      return;
    }
    {
      LockGuard media_lock(this->media_lifecycle_mutex_);
      const auto phase = this->media_lifecycle_phase_.load(std::memory_order_acquire);
      if (phase == MediaLifecyclePhase::CLEANING || phase == MediaLifecyclePhase::SHUTTING_DOWN) {
        this->send_stateless_response_(message, src, 503, "Service Unavailable", "", false, 1);
        return;
      }
    }
    bool admitted = this->accept_direct_calls_;
#ifdef USE_ESPHOME_VOIP_REGISTRATION
    admitted = admitted || this->registration_.match_incoming(
        sip_user_from_header(sip_request_uri(message)), ntohl(src.sin_addr.s_addr),
        this->dispatch_socket_ >= 0, this->registration_socket_index_(this->dispatch_socket_)) >= 0;
#endif
    if (!admitted) {
      this->send_stateless_response_(message, src, 403, "Forbidden");
      return;
    }
  }
  // A capability description is not an offer and acquires no dialog or media.
  this->send_stateless_response_(message, src, 200, "OK", "", false, -1,
      sip_accepts_sdp(message) ? this->build_sdp_capabilities_(ntohl(src.sin_addr.s_addr)) : std::string());
}

bool SipTransport::reject_if_stale_dialog_(const std::string &request, const sockaddr_in &src,
                                           const char *method_name) {
  const std::string request_call_id = header_value(request, "Call-ID");
  const bool call_id_matches =
      !request_call_id.empty() && !this->call_id_.empty() && request_call_id == this->call_id_;
  const uint32_t expected_ip = this->remote_ip_v4_.load(std::memory_order_acquire);
  const bool host_matches = expected_ip == 0 || ntohl(src.sin_addr.s_addr) == expected_ip;
  bool dialog_tags_match = true;
  if (std::strcmp(method_name, "BYE") == 0 || std::strcmp(method_name, "REFER") == 0) {
    const std::string from_tag = tag_from_header(header_value(request, "From"));
    const std::string to_tag = tag_from_header(header_value(request, "To"));
    dialog_tags_match = !this->remote_tag_.empty() && !this->local_tag_.empty() &&
                        from_tag == this->remote_tag_ && to_tag == this->local_tag_ &&
                        cseq_method(header_value(request, "CSeq")) == method_name &&
                        cseq_number(header_value(request, "CSeq")) > this->remote_dialog_cseq_;
  }
  if (call_id_matches && host_matches && dialog_tags_match) return false;
  ESP_LOGW(TAG, "SIP %s ignored for stale call_id=%s current=%s",
           method_name, request_call_id.c_str(), this->call_id_.c_str());
  this->send_stateless_response_(request, src, 481, "Call/Transaction Does Not Exist");
  return true;
}

}  // namespace voip_stack
}  // namespace esphome

#endif
