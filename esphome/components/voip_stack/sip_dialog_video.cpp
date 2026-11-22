#include "sip_transport_internal.h"

#if defined(USE_ESP32) && defined(USE_ESPHOME_VOIP_SIP_TRANSPORT)

namespace esphome {
namespace voip_stack {

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
std::string SipTransport::build_video_direction_offer_(
    bool enabled, uint32_t session_version) const {
  if (this->confirmed_local_sdp_.empty()) return "";
  std::string offer = this->confirmed_local_sdp_;

  const size_t origin = offer.find("o=- ");
  const size_t origin_end =
      origin == std::string::npos ? std::string::npos
                                  : offer.find("\r\n", origin);
  if (origin == std::string::npos || origin_end == std::string::npos)
    return "";
  const size_t network = offer.find(" IN IP4 ", origin);
  if (network == std::string::npos || network >= origin_end) return "";
  offer.replace(origin, origin_end - origin,
                "o=- " + std::to_string(this->sdp_session_id_) + " " +
                    std::to_string(session_version) +
                    offer.substr(network, origin_end - network));

  const size_t video = offer.find("m=video ");
  if (!this->video_negotiated_) {
    if (!enabled || !this->local_video_send_capability_().valid()) return "";
    std::string local_ip;
    if (!this->local_ip_for_peer_(
            this->remote_ip_v4_.load(std::memory_order_acquire), &local_ip)) return "";
    const auto section = this->append_video_sdp_("", local_ip, false);
    if (section.empty()) return "";
    // RFC 3264 section 8.1: reuse a rejected video slot in its original
    // position. Append a new section only when video has never been offered.
    if (video == std::string::npos) {
      offer.append(section);
    } else {
      const auto end = offer.find("\r\nm=", video);
      offer.replace(video, end == std::string::npos ? offer.size() - video
                                                   : end + 2 - video, section);
    }
    return offer;
  }
  if (video == std::string::npos) return "";
  const size_t video_line_end = offer.find("\r\n", video);
  if (video_line_end == std::string::npos ||
      offer.compare(video, sizeof("m=video 0") - 1, "m=video 0") == 0) {
    return "";
  }
  size_t video_end = offer.find("\r\nm=", video_line_end);
  if (video_end == std::string::npos) video_end = offer.size();
  const bool receive = this->video_receive_enabled_;
  const char *direction =
      enabled && receive ? "a=sendrecv"
      : enabled          ? "a=sendonly"
      : receive          ? "a=recvonly"
                         : "a=inactive";
  const char *directions[] = {
      "a=sendrecv", "a=sendonly", "a=recvonly", "a=inactive"};
  size_t direction_pos = std::string::npos;
  size_t direction_len = 0;
  for (const char *candidate : directions) {
    const size_t found = offer.find(candidate, video_line_end);
    if (found != std::string::npos && found < video_end) {
      direction_pos = found;
      direction_len = std::strlen(candidate);
      break;
    }
  }
  if (direction_pos != std::string::npos) {
    offer.replace(direction_pos, direction_len, direction);
  } else if (video_end == offer.size() && offer.size() >= 2 &&
             offer.compare(offer.size() - 2, 2, "\r\n") == 0) {
    offer.append(direction).append("\r\n");
  } else {
    offer.insert(video_end, std::string("\r\n") + direction);
  }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  if (this->video_imageattr_state_ & IMAGEATTR_DISABLED) {
    size_t attr = offer.find("\r\na=imageattr:", video_line_end);
    while (attr != std::string::npos) {
      const size_t next_media = offer.find("\r\nm=", video_line_end);
      if (next_media != std::string::npos && attr >= next_media) break;
      const size_t start = attr + 2;
      const size_t end = offer.find("\r\n", start);
      offer.erase(start, (end == std::string::npos ? offer.size() : end + 2) - start);
      attr = offer.find("\r\na=imageattr:", video_line_end);
    }
  }
#endif
  return offer;
}

bool SipTransport::send_video_direction_reinvite_unlocked_(bool enabled,
                                                           bool retry, bool imageattr_fallback) {
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  const bool attribute_update = retry ? (this->video_imageattr_state_ & IMAGEATTR_ACTIVE) != 0 : imageattr_fallback;
#else
  const bool attribute_update = false;
  (void) imageattr_fallback;
#endif
  if (!this->media_active_.load(std::memory_order_acquire) ||
      (this->video_source_ == nullptr && !attribute_update) ||
      this->remote_tag_.empty()) {
    return false;
  }
  const bool previous_send =
      retry ? this->pending_video_direction_invite_.previous_send
            : this->video_send_enabled_;
  const uint8_t retry_count =
      retry ? this->pending_video_direction_invite_.retry_count : 0;
  const uint32_t session_version =
      retry ? this->pending_video_direction_invite_.session_version
            : this->sdp_session_version_ + 1;
  this->video_send_requested_.store(enabled, std::memory_order_release);
  const std::string offer =
      retry ? this->pending_video_direction_invite_.offered_sdp
            : this->build_video_direction_offer_(enabled, session_version);
  if (offer.empty()) {
    this->video_send_requested_.store(previous_send, std::memory_order_release);
    return false;
  }

  // The first locally originated offer may be a video update to an incoming
  // dialog. Preserve its existing audio contract for answer validation too.
  AudioFormat offered_tx;
  this->get_media_config_(&offered_tx, nullptr, nullptr, nullptr,
                          &this->local_offered_rx_payloads_);
  this->local_offered_tx_formats_.count = 0;
  this->local_offered_tx_formats_.formats[0] = offered_tx;
  this->local_offered_tx_formats_.count = 1;
  this->local_offered_directional_audio_v1_ =
      offer.find("a=x-voip-stack-flow:") != std::string::npos;

  const uint32_t cseq = this->cseq_++;
  const std::string branch = "z9hG4bK" + make_token("");
  std::string request;
  uint32_t target_ip = 0;
  uint16_t target_port = 0;
  SipRequestOptions options;
  options.cseq_number = cseq;
  options.branch_override = branch;
  options.remember_transaction = false;
  options.remember_invite_ack = false;
  options.formatted_request = &request;
  options.destination_ip = &target_ip;
  options.destination_port = &target_port;
  if (!this->send_request_("INVITE", offer, options)) {
    this->video_send_requested_.store(previous_send,
                                      std::memory_order_release);
    this->emit_video_send_state_(previous_send, false);
    return false;
  }

  const uint32_t now = millis();
  auto &pending = this->pending_video_direction_invite_;
  pending.clear();
  pending.branch = branch;
  pending.offered_sdp = offer;
  pending.cseq = cseq;
  pending.session_version = session_version;
  pending.response_deadline_ms = now + SIP_TRANSACTION_TIMEOUT_MS;
  pending.target_send = enabled;
  pending.previous_send = previous_send;
  pending.retry_count = retry_count;
  pending.transaction.request = request;
  pending.transaction.ip_v4 = target_ip;
  pending.transaction.port = target_port;
  pending.transaction.interval_ms = SIP_T1_MS;
  pending.transaction.next_ms = now + SIP_T1_MS;
  pending.transaction.deadline_ms = pending.response_deadline_ms;
  pending.transaction.udp =
      !this->remote_sip_tcp_.load(std::memory_order_acquire);
  pending.transaction.completed = false;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  if (offer.find("\r\na=imageattr:26 ") != std::string::npos)
    this->video_imageattr_state_ |= IMAGEATTR_OFFERED;
  else
    this->video_imageattr_state_ &= ~IMAGEATTR_OFFERED;
  if (attribute_update) this->video_imageattr_state_ |= IMAGEATTR_ACTIVE;
#endif
  ESP_LOGI(TAG,
           "SIP video direction re-INVITE sent cseq=%u target=%s retry=%u",
           (unsigned) cseq, enabled ? "send" : "receive-only",
           (unsigned) retry_count);
  this->emit_video_send_state_(previous_send, true);
  this->wake_sip_task_();
  return true;
}

bool SipTransport::request_video_send(bool enabled) {
  LockGuard lock(this->dialog_mutex_);
  // Before a dialog exists this is the preference used by the initial SDP.
  // Do not turn an in-progress INVITE or teardown into an idle settings write.
  if (this->call_id_.empty() && this->video_source_ != nullptr &&
      !this->media_active_.load(std::memory_order_acquire) &&
      !this->outgoing_invite_pending_.load(std::memory_order_acquire) &&
      !this->transport_stopping_.load(std::memory_order_acquire) &&
      !this->terminal_transaction_pending_locked_()) {
    this->video_send_requested_.store(enabled, std::memory_order_release);
    this->emit_video_send_state_(enabled, false);
    return true;
  }
  if (!this->running_.load(std::memory_order_acquire) ||
      this->transport_stopping_.load(std::memory_order_acquire) ||
      !this->media_active_.load(std::memory_order_acquire) ||
      this->video_source_ == nullptr) {
    ESP_LOGW(TAG,
             "Video send direction rejected: no active dialog or video source");
    return false;
  }
  if (this->pending_video_direction_invite_.pending() ||
      this->outgoing_invite_pending_.load(std::memory_order_acquire) ||
      this->completed_invite_.awaiting_ack) {
    ESP_LOGW(TAG, "Video send direction rejected: INVITE transaction pending");
    return false;
  }
  if (enabled == this->video_send_enabled_) {
    this->video_send_requested_.store(enabled, std::memory_order_release);
    this->emit_video_send_state_(enabled, false);
    return true;
  }
  return this->send_video_direction_reinvite_unlocked_(enabled);
}

bool SipTransport::apply_video_direction_answer_(const std::string &sdp,
                                                 uint32_t default_ip,
                                                 bool *accepted_send) {
  if (accepted_send == nullptr || sdp.empty()) return false;
  const auto retained = this->negotiated_media_();
  ScopedMediaProposal proposal_scope(this->media_proposal_epoch_);
  NegotiatedMedia proposed;
  if (!this->parse_remote_media_(sdp, default_ip, true, retained, &proposed, true)) return false;
  const bool same_audio =
      proposed.tx == retained.tx && proposed.rx == retained.rx &&
      proposed.tx_pt == retained.tx_pt && proposed.rx_pt == retained.rx_pt &&
      proposed.rx_payloads == retained.rx_payloads &&
      proposed.send == retained.send && proposed.receive == retained.receive &&
      proposed.audio_ip == retained.audio_ip && proposed.audio_port == retained.audio_port;
  const auto &before = retained.video, &after = proposed.video;
  const bool same_video =
      proposed.video_negotiated &&
      after.payload_type == before.payload_type && after.clock_rate == before.clock_rate &&
      after.packetization_mode == before.packetization_mode && after.encoding == before.encoding &&
      after.profile_level_id == before.profile_level_id && after.max_fps == before.max_fps &&
      after.level_asymmetry_allowed == before.level_asymmetry_allowed &&
      after.max_bitrate_bps == before.max_bitrate_bps &&
      after.rtcp_feedback_pli == before.rtcp_feedback_pli && after.rtcp_feedback_fir == before.rtcp_feedback_fir &&
      proposed.video_ip == retained.video_ip && proposed.video_port == retained.video_port &&
      proposed.video_rtcp_ip == retained.video_rtcp_ip && proposed.video_rtcp_port == retained.video_rtcp_port &&
      proposed.video_receive == retained.video_receive;
  if (!same_audio || (retained.video_negotiated && !same_video)) return false;
#ifdef USE_ESPHOME_VOIP_DTMF
  if (!(retained.dtmf == proposed.dtmf)) return false;
#endif
  if (!retained.video_negotiated && proposed.video_negotiated) {
    // Admit the same media child used by initial and remote offers. Audio
    // and its RTP worker retain their committed contract throughout.
    if (!this->prepare_video_session_locked_(proposed) ||
        !this->video_session_->request_media_direction(
            proposed.video_send, proposed.video_receive)) {
      if (this->video_session_ != nullptr) this->video_session_->request_stop();
      return false;
    }
  } else if (proposed.video_send != retained.video_send &&
      (this->video_session_ == nullptr ||
       !this->video_session_->request_send_direction(proposed.video_send))) return false;
  if (this->video_session_ != nullptr)
    this->video_session_->set_rtcp_send_enabled(!proposed.video_zero_connection);
  this->commit_negotiated_media_(proposed);
  if (!retained.video_negotiated && proposed.video_negotiated)
    this->emit_video_active_state_(proposed.video_send || proposed.video_receive);
  this->video_send_requested_.store(proposed.video_send, std::memory_order_release);
  *accepted_send = proposed.video_send;
  return true;
}

bool SipTransport::replay_completed_video_direction_ack_(
    const std::string &response, const sockaddr_in &src) {
  auto &completed = this->completed_video_direction_invite_;
  if (completed.empty()) return false;
  if (millis() - completed.completed_ms > SIP_TRANSACTION_TIMEOUT_MS) {
    completed.clear();
    return false;
  }
  const std::string cseq = header_value(response, "CSeq");
  const bool transport_matches =
      completed.udp ==
      !this->remote_sip_tcp_.load(std::memory_order_acquire);
  if (response.rfind("SIP/2.0 ", 0) != 0 ||
      cseq_method(cseq) != "INVITE" ||
      header_value(response, "Call-ID") != completed.call_id ||
      cseq_number(cseq) != completed.cseq ||
      ntohl(src.sin_addr.s_addr) != completed.response_ip_v4 ||
      !transport_matches ||
      via_branch(header_value(response, "Via")) != completed.branch) {
    return false;
  }
  ESP_LOGI(TAG,
           "SIP video re-INVITE final retransmission replaying ACK cseq=%u",
           (unsigned) completed.cseq);
  this->send_sip_(completed.ack, completed.ack_ip_v4,
                  completed.ack_port);
  return true;
}
#endif

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
bool SipTransport::handle_video_direction_response_(
    const std::string &message, const sockaddr_in &src, uint16_t status,
    uint32_t response_cseq) {
  auto &pending = this->pending_video_direction_invite_;
  if (!pending.pending() || pending.waiting_retry ||
      response_cseq != pending.cseq) {
    return false;
  }
  const uint32_t src_ip = ntohl(src.sin_addr.s_addr);
  const bool tcp = !pending.transaction.udp;
  const uint32_t expected_response_ip =
      tcp ? this->remote_ip_v4_.load(std::memory_order_acquire)
          : pending.transaction.ip_v4;
  if (header_value(message, "Call-ID") != this->call_id_ ||
      src_ip != expected_response_ip ||
      via_branch(header_value(message, "Via")) != pending.branch ||
      tag_from_header(header_value(message, "From")) != this->local_tag_ ||
      tag_from_header(header_value(message, "To")) != this->remote_tag_) {
    ESP_LOGD(TAG, "SIP video re-INVITE response ignored: transaction mismatch");
    return true;
  }

  this->mark_sip_event_(SipEvent::RESPONSE, status);
  if (status < 200) {
    pending.transaction.completed = true;
    pending.transaction.next_ms = pending.response_deadline_ms;
    return true;
  }

  if (status < 300) {
    const std::string contact =
        strip_angle_uri(header_value(message, "Contact"));
    if (!contact.empty()) this->remote_target_uri_ = contact;
  }
  std::string ack;
  SipRequestOptions ack_options;
  ack_options.cseq_number = pending.cseq;
  ack_options.cseq_method = "ACK";
  ack_options.branch_override =
      status < 300 ? "z9hG4bK" + make_token("") : pending.branch;
  ack_options.remember_transaction = false;
  ack_options.remember_invite_ack = false;
  ack_options.formatted_request = &ack;
  uint32_t ack_ip = pending.transaction.ip_v4;
  uint16_t ack_port = pending.transaction.port;
  ack_options.destination_ip = &ack_ip;
  ack_options.destination_port = &ack_port;
  const bool ack_sent = status < 300
      ? this->send_request_("ACK", "", ack_options)
      : this->send_invite_error_ack_(message, pending.transaction, false, &ack);
  if (!ack_sent) {
    ESP_LOGW(TAG,
             "SIP video re-INVITE ACK initial send failed; retaining it for "
             "a retransmitted final response");
  }
  if (!ack.empty()) {
    auto &completed = this->completed_video_direction_invite_;
    completed.clear();
    completed.call_id = this->call_id_;
    completed.cseq = pending.cseq;
    completed.branch = pending.branch;
    completed.ack = ack;
    completed.response_ip_v4 = src_ip;
    completed.ack_ip_v4 = ack_ip;
    completed.ack_port = ack_port;
    completed.completed_ms = millis();
    completed.udp = status >= 300 ? pending.transaction.udp
        : !this->remote_sip_tcp_.load(std::memory_order_acquire);
  }

  const bool previous_send = pending.previous_send;
  const bool target_send = pending.target_send;
  const uint32_t accepted_version = pending.session_version;
  const std::string accepted_offer = pending.offered_sdp;
  const uint8_t retry_count = pending.retry_count;
  if (status >= 200 && status < 300) {
    bool accepted_send = previous_send;
    bool media_ok = false;
    {
      LockGuard media_lock(this->media_lifecycle_mutex_);
      media_ok = this->apply_video_direction_answer_(
          message_body(message), src_ip, &accepted_send);
    }
    if (!media_ok) {
      ESP_LOGE(TAG,
               "SIP video re-INVITE 2xx changed established media; "
               "terminating the invalid dialog");
      pending.clear();
      this->video_send_requested_.store(previous_send,
                                        std::memory_order_release);
      this->emit_video_send_state_(previous_send, false);
      const bool bye_sent = this->send_bye_unlocked_(this->call_id_);
      SipSignal signal;
      signal.type = SipSignalType::MEDIA_INCOMPATIBLE;
      signal.status_code = 488;
      signal.call_id = this->call_id_;
      signal.reason = "video_reinvite_answer_incompatible";
      signal.terminal_transaction_pending = bye_sent;
      if (!bye_sent) this->reset_dialog_();
      this->emit_sip_signal_(std::move(signal));
      return true;
    }
    this->sdp_session_version_ = accepted_version;
    this->confirmed_local_sdp_ = accepted_offer;
    pending.clear();
    this->emit_video_send_state_(accepted_send, false);
    ESP_LOGI(TAG,
             "SIP video direction committed cseq=%u requested=%s accepted=%s",
             (unsigned) response_cseq, target_send ? "send" : "receive-only",
             accepted_send ? "send" : "receive-only");
    return true;
  }

  if (status == 491 && retry_count == 0) {
    const uint32_t retry_delay =
        this->dialog_originated_ ? 2100U + (esp_random() % 1901U)
                                 : esp_random() % 2001U;
    pending.transaction.clear();
    pending.cseq = 0;
    pending.branch.clear();
    pending.response_deadline_ms = 0;
    pending.waiting_retry = true;
    pending.retry_count = 1;
    pending.retry_at_ms = millis() + retry_delay;
    ESP_LOGI(TAG,
             "SIP video direction glare; one retry scheduled in %u ms",
             (unsigned) retry_delay);
    this->wake_sip_task_();
    return true;
  }

  pending.clear();
  this->video_send_requested_.store(previous_send,
                                    std::memory_order_release);
  this->emit_video_send_state_(previous_send, false);
  ESP_LOGW(TAG,
           "SIP video direction rejected status=%u; established media retained",
           (unsigned) status);
  if (status == 408 || status == 481) {
    const std::string call_id = this->call_id_;
    SipSignal signal;
    signal.type = SipSignalType::PROTOCOL_ERROR;
    signal.status_code = status;
    signal.call_id = call_id;
    signal.reason =
        status == 481 ? "dialog_not_found" : "video_reinvite_timeout";
    // RFC 3261 section 12.2.1.2: a 408/481 to an in-dialog request means the
    // dialog is no longer usable. Attempt a standards-based BYE first and
    // fall back to local cleanup only if it cannot be put on the wire.
    const bool bye_pending = this->send_bye_unlocked_(call_id);
    signal.terminal_transaction_pending = bye_pending;
    if (!bye_pending) this->reset_dialog_();
    this->emit_sip_signal_(std::move(signal));
  }
  return true;
}

void SipTransport::pump_video_direction_transaction_() {
  auto &pending = this->pending_video_direction_invite_;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  if ((this->video_imageattr_state_ & IMAGEATTR_FALLBACK) && !pending.pending() &&
      this->media_active_.load(std::memory_order_acquire) && this->video_negotiated_ &&
      !this->outgoing_invite_pending_.load(std::memory_order_acquire) &&
      !this->completed_invite_.awaiting_ack && this->pending_bye_.empty() &&
      !this->transport_stopping_.load(std::memory_order_acquire)) {
    // One attempt per dialog, serialised by the same owner as camera changes.
    this->video_imageattr_state_ &= ~IMAGEATTR_FALLBACK;
    if (!this->send_video_direction_reinvite_unlocked_(this->video_send_enabled_, false, true))
      ESP_LOGW(TAG, "Image attribute fallback offer could not be sent; media retained");
  }
#endif
  if (!pending.pending()) {
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
    this->video_imageattr_state_ &= ~IMAGEATTR_ACTIVE;
#endif
    return;
  }
  const uint32_t now = millis();
  if (pending.waiting_retry) {
    if (!time_reached(now, pending.retry_at_ms)) return;
    const bool target = pending.target_send;
    if (!this->send_video_direction_reinvite_unlocked_(target, true)) {
      const bool previous = pending.previous_send;
      pending.clear();
      this->video_send_requested_.store(previous,
                                        std::memory_order_release);
      this->emit_video_send_state_(previous, false);
      ESP_LOGW(TAG, "SIP video direction retry could not be sent");
    }
    return;
  }
  if (time_reached(now, pending.response_deadline_ms)) {
    const bool previous = pending.previous_send;
    const std::string call_id = this->call_id_;
    pending.clear();
    this->video_send_requested_.store(previous,
                                      std::memory_order_release);
    this->emit_video_send_state_(previous, false);
    ESP_LOGE(TAG, "SIP video direction re-INVITE timed out");
    const bool bye_sent = this->send_bye_unlocked_(call_id);
    SipSignal signal;
    signal.type = SipSignalType::PROTOCOL_ERROR;
    signal.status_code = 408;
    signal.call_id = call_id;
    signal.reason = "video_reinvite_timeout";
    signal.terminal_transaction_pending = bye_sent;
    if (!bye_sent) this->reset_dialog_();
    this->emit_sip_signal_(std::move(signal));
    return;
  }
  if (!pending.transaction.udp ||
      pending.transaction.completed ||
      !time_reached(now, pending.transaction.next_ms)) {
    return;
  }
  const bool sent =
      this->send_sip_(pending.transaction.request,
                      pending.transaction.ip_v4,
                      pending.transaction.port);
  pending.transaction.retries++;
  ESP_LOGD(TAG, "SIP UDP video re-INVITE retransmit #%u%s",
           (unsigned) pending.transaction.retries,
           sent ? "" : " failed");
  pending.transaction.interval_ms =
      std::min<uint16_t>(
          static_cast<uint16_t>(pending.transaction.interval_ms * 2),
          SIP_T2_MS);
  pending.transaction.next_ms =
      now + pending.transaction.interval_ms;
  if (time_reached(pending.transaction.next_ms,
                   pending.response_deadline_ms)) {
    pending.transaction.next_ms = pending.response_deadline_ms;
  }
}
#endif

}  // namespace voip_stack
}  // namespace esphome

#endif
