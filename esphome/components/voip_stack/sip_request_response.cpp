#include "sip_transport.h"
#include "sip_message.h"

#if defined(USE_ESP32) && defined(USE_ESPHOME_VOIP_SIP_TRANSPORT)

#include "esphome/core/log.h"

namespace esphome {
namespace voip_stack {

static const char *const TAG = "voip_stack.sip";

bool SipTransport::send_request_(const std::string &method, const std::string &body) {
  SipRequestOptions options;
  return this->send_request_(method, body, options);
}

bool SipTransport::send_request_(const std::string &method, const std::string &body,
                                 const SipRequestOptions &options) {
  uint32_t ip = this->remote_ip_v4_.load(std::memory_order_acquire);
  uint16_t port =
      this->remote_sip_port_.load(std::memory_order_acquire);
  if (ip == 0 || port == 0 || this->call_id_.empty()) return false;
  std::string branch;
  if (!options.branch_override.empty()) {
    branch = options.branch_override;
  } else if (method == "INVITE") {
    if (this->branch_.empty())
      this->branch_ = "z9hG4bK" + make_token("");
    branch = this->branch_;
  } else if (method == "CANCEL") {
    if (this->branch_.empty())
      this->branch_ = "z9hG4bK" + make_token("");
    branch = this->branch_;
  } else {
    branch = "z9hG4bK" + make_token("");
  }
  std::string local_ip = "0.0.0.0";
  this->local_ip_for_peer_(ip, &local_ip);
  std::string request_uri = this->remote_target_uri_;
  if (request_uri.empty())
    request_uri = strip_angle_uri(this->remote_uri_);
  if (request_uri.empty()) {
    struct in_addr remote_address {};
    remote_address.s_addr = htonl(ip);
    char remote_ip[16];
    inet_ntoa_r(remote_address, remote_ip, sizeof(remote_ip));
    request_uri = "sip:voip@" + std::string(remote_ip);
  }
  // A confirmed dialog is retargeted by Contact. For UDP the Request-URI and
  // actual next hop must agree; retaining the original port here breaks peers
  // whose Contact listens on a different socket. TCP keeps using its existing
  // connection, so the parsed destination is intentionally UDP-only.
  if (!this->remote_sip_tcp_.load(std::memory_order_acquire)) {
    uint32_t target_ip = 0;
    uint16_t target_port = 0;
    if (sip_uri_ipv4_target(request_uri, &target_ip, &target_port)) {
      ip = target_ip;
      port = target_port;
    }
  }
  const char *transport =
      this->remote_sip_tcp_.load(std::memory_order_acquire) ? "TCP"
                                                            : "UDP";
  std::string msg = method + " " + request_uri + " SIP/2.0\r\n";
  msg += "Via: SIP/2.0/" + std::string(transport) + " " + local_ip +
         ":" + std::to_string(this->sip_port_) + ";branch=" + branch +
         ";rport\r\n";
  msg += "Max-Forwards: 70\r\n";
  msg += "From: " + this->local_uri_ + ";tag=" + this->local_tag_ +
         "\r\n";
  msg += "To: " + this->remote_uri_;
  if (!this->remote_tag_.empty())
    msg += ";tag=" + this->remote_tag_;
  msg += "\r\n";
  msg += "Call-ID: " + this->call_id_ + "\r\n";
  const uint32_t request_cseq =
      options.cseq_number == 0 ? this->cseq_++ : options.cseq_number;
  const std::string cseq_method =
      options.cseq_method.empty() ? method : options.cseq_method;
  msg += "CSeq: " + std::to_string(request_cseq) + " " + cseq_method +
         "\r\n";
  msg += "Contact: " +
         (this->local_contact_uri_.empty() ? this->local_uri_
                                           : this->local_contact_uri_) +
         "\r\n";
  msg += "User-Agent: ESPHome-VoIP-Stack-SIP\r\n";
  msg += "Allow: ACK, BYE, CANCEL, INVITE, OPTIONS, UPDATE\r\n";
  msg += "Supported: from-change\r\n";
  if (method == "INVITE") {
    msg += "X-Voip-Stack-Caller-Route: " +
           sip_route_id(this->caller_.route, VOIP_STACK_MAX_ROUTE_ID_LEN) +
           "\r\n";
    msg += "X-Voip-Stack-Caller-Name: " +
           sip_header_token(this->caller_.name, VOIP_STACK_MAX_NAME_LEN) +
           "\r\n";
    msg += "X-Voip-Stack-Dest-Route: " +
           sip_route_id(this->destination_.route, VOIP_STACK_MAX_ROUTE_ID_LEN) +
           "\r\n";
    msg += "X-Voip-Stack-Dest-Name: " +
           sip_header_token(this->destination_.name, VOIP_STACK_MAX_NAME_LEN) +
           "\r\n";
  }
  if (!body.empty())
    msg += "Content-Type: application/sdp\r\n";
  msg += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
  msg += body;
  if (options.formatted_request != nullptr)
    *options.formatted_request = msg;
  if (method == "ACK" && options.remember_invite_ack) {
    this->remember_completed_invite_ack_(msg, ip, port);
  }
  const bool transaction_request =
      options.remember_transaction &&
      (method == "INVITE" || method == "CANCEL" || method == "BYE" ||
       method == "UPDATE");
  const bool udp_transaction =
      transaction_request &&
      !this->remote_sip_tcp_.load(std::memory_order_acquire);
  const bool sent = this->send_sip_(msg, ip, port);
  if (sent || udp_transaction) {
    const SipEvent event = sip_event_from_method_(method);
    if (event != SipEvent::NONE)
      this->mark_sip_event_(event);
    if (transaction_request) {
      this->remember_udp_transaction_(method, msg, ip, port);
    }
  }
  if (!sent && udp_transaction) {
    ESP_LOGW(TAG,
             "SIP UDP %s initial send deferred to transaction retry",
             method.c_str());
  }
  return sent || udp_transaction;
}

bool SipTransport::send_invite_error_ack_() {
  if (this->branch_.empty()) return false;
  SipRequestOptions options;
  options.cseq_number = this->invite_cseq_;
  options.cseq_method = "ACK";
  // A non-2xx INVITE ACK reuses the INVITE client transaction branch per RFC
  // 3261 section 17.1.1.3.
  options.branch_override = this->branch_;
  return this->send_request_("ACK", "", options);
}

bool SipTransport::send_connected_identity_update_() {
  if (this->dialog_originated_ || this->connected_identity_sent_ ||
      !this->peer_supports_from_change_ ||
      !this->media_active_.load(std::memory_order_acquire)) {
    return false;
  }
  const bool sent = this->send_request_("UPDATE", "");
  if (sent) {
    this->connected_identity_sent_ = true;
  }
  return sent;
}

std::string SipTransport::format_response_(
    uint16_t status, const char *reason, const std::string &via,
    const std::string &from, const std::string &to,
    const std::string &call_id, const std::string &cseq,
    const std::string &app_reason, const std::string &body,
    bool add_contact_ua, bool add_to_tag, bool stateless,
    int retry_after_seconds) {
  std::string msg =
      "SIP/2.0 " + std::to_string(status) + " " + reason + "\r\n";
  msg += "Via: " + via + "\r\n";
  msg += "From: " + from + "\r\n";
  msg += "To: " + to;
  if (add_to_tag && status != 100 && tag_from_header(to).empty()) {
    if (this->local_tag_.empty())
      this->local_tag_ = make_token("tag");
    msg += ";tag=" + this->local_tag_;
  }
  msg += "\r\n";
  msg += "Call-ID: " + call_id + "\r\n";
  msg += "CSeq: " + cseq + "\r\n";
  if (add_contact_ua) {
    msg += "Contact: " +
           (this->local_contact_uri_.empty() ? this->local_uri_
                                             : this->local_contact_uri_) +
           "\r\n";
    msg += "User-Agent: ESPHome-VoIP-Stack-SIP\r\n";
  }
  if (cseq_method(cseq) == "INVITE" && status > 100 && status < 300) {
    msg += "Supported: from-change\r\n";
  }
  const std::string clean_reason = sip_header_token(app_reason);
  if (!clean_reason.empty() && (!stateless || status >= 300)) {
    msg += "Reason: X-Voip-Stack;cause=" + std::to_string(status) +
           ";text=" + sip_quoted(clean_reason) + "\r\n";
    msg += "X-Voip-Stack-Decline-Reason: " + clean_reason + "\r\n";
  }
  if (status == 405) {
    msg += "Allow: ACK, BYE, CANCEL, INVITE, OPTIONS, UPDATE\r\n";
  }
  if (retry_after_seconds >= 0) {
    msg += "Retry-After: " + std::to_string(retry_after_seconds) +
           "\r\n";
  }
  if (!body.empty())
    msg += "Content-Type: application/sdp\r\n";
  msg += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
  msg += body;
  return msg;
}

bool SipTransport::send_response_(uint16_t status, const char *reason,
                                  const std::string &body,
                                  const std::string &app_reason) {
  const uint32_t ip = this->last_invite_peer_ip_v4_;
  const uint16_t port = this->last_invite_peer_port_;
  if (ip == 0 || port == 0 || this->last_invite_via_.empty())
    return false;
  const std::string msg = this->format_response_(
      status, reason,
      response_via_with_rport(this->last_invite_via_, ip, port),
      this->last_invite_from_, this->last_invite_to_, this->call_id_,
      this->last_invite_cseq_, app_reason, body, true, true, false, -1);
  bool response_owned = false;
  if (status >= 200) {
    this->pending_provisional_response_.clear();
    this->last_invite_response_ = msg;
    this->remember_completed_response_(
        "Via: " + this->last_invite_via_ + "\r\nCall-ID: " +
            this->call_id_ + "\r\nCSeq: " + this->last_invite_cseq_ +
            "\r\n",
        ip, port, "INVITE", msg);
    response_owned = this->completed_invite_.awaiting_ack &&
                     this->completed_invite_.response == msg;
  } else {
    // Keep the latest provisional response before touching the socket. If the
    // local WiFi/lwIP TX pool is temporarily exhausted, the SIP task owns a
    // bounded retry and an INVITE retransmission can replay the same response.
    this->last_invite_response_ = msg;
    this->pending_provisional_response_.clear();
  }
  const bool sent = this->send_sip_(msg, ip, port);
  if (!sent && status < 200 &&
      !this->remote_sip_tcp_.load(std::memory_order_acquire)) {
    this->defer_udp_provisional_response_(msg, ip, port);
    response_owned = true;
  }
  if (!sent && !response_owned) return false;
  this->mark_sip_event_(SipEvent::RESPONSE, status);
  return true;
}

bool SipTransport::send_stateless_response_(
    const std::string &request, const sockaddr_in &src, uint16_t status,
    const char *reason, const std::string &app_reason,
    bool cache_transaction, int retry_after_seconds) {
  const uint32_t ip = ntohl(src.sin_addr.s_addr);
  const uint16_t port = ntohs(src.sin_port);
  const std::string via = header_values(request, "Via");
  const std::string from = header_value(request, "From");
  const std::string to = header_value(request, "To");
  const std::string call_id = header_value(request, "Call-ID");
  const std::string cseq = header_value(request, "CSeq");
  if (via.empty() || from.empty() || to.empty() || call_id.empty() ||
      cseq.empty())
    return false;
  std::string response_to = to;
  const std::string method = cseq_method(cseq);
  if (cache_transaction && tag_from_header(response_to).empty()) {
    const std::string tag =
        (method == "INVITE" || this->local_tag_.empty())
            ? make_token("tag")
            : this->local_tag_;
    response_to += ";tag=" + tag;
  }
  const bool add_contact = method == "UPDATE" &&
                           status >= 200 && status < 300;
  const std::string msg = this->format_response_(
      status, reason, response_via_with_rport(via, ip, port), from,
      response_to, call_id, cseq, app_reason, "", add_contact, false, true,
      retry_after_seconds);

  bool response_owned = false;
  if (cache_transaction && status >= 200) {
    this->remember_completed_response_(request, ip, port, method, msg);
    const CompletedServerTransaction &completed =
        method == "INVITE" ? this->completed_invite_
                           : this->completed_control_;
    response_owned = completed.udp && completed.response == msg;
  }
  const bool sent = this->send_sip_(msg, ip, port);
  if (!sent && !response_owned) return false;
  this->mark_sip_event_(SipEvent::RESPONSE, status);
  return true;
}

}  // namespace voip_stack
}  // namespace esphome

#endif
