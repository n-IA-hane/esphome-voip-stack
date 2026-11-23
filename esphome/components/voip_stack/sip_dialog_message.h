#pragma once

#include <cstdint>
#include <string_view>

namespace esphome::voip_stack {

// A datagram or a framed TCP record has the same SIP start line. Keep the
// method as a view into that record until the dispatcher hands it to an owner.
struct SipStartLine {
  bool response;
  std::string_view method;

  explicit SipStartLine(std::string_view message)
      : response(message.substr(0, 8) == "SIP/2.0 ") {
    if (!response) {
      const auto end = message.find(' ');
      if (end != message.npos) method = message.substr(0, end);
    }
  }

  static uint16_t response_status(std::string_view message) {
    if (message.size() < 12 || message.substr(0, 8) != "SIP/2.0 ") return 0;
    const auto code = message.substr(8, 3);
    if (code[0] < '1' || code[0] > '6' || code[1] < '0' || code[1] > '9' || code[2] < '0' || code[2] > '9')
      return 0;
    return (code[0] - '0') * 100 + (code[1] - '0') * 10 + code[2] - '0';
  }
};

enum class SipClientMethod { INVITE, CANCEL, BYE, UPDATE, OTHER };

inline SipClientMethod sip_client_method(std::string_view method) {
  if (method == "INVITE") return SipClientMethod::INVITE;
  if (method == "CANCEL") return SipClientMethod::CANCEL;
  if (method == "BYE") return SipClientMethod::BYE;
  if (method == "UPDATE") return SipClientMethod::UPDATE;
  return SipClientMethod::OTHER;
}

// Both sets of strings borrow the active dialog or fields already parsed from
// this response. Matching cannot retarget the dialog or publish a SIP event.
struct SipResponseKey {
  SipClientMethod method;
  uint16_t status;
  uint32_t sequence, source_ip;
  std::string_view from_tag, to_tag, branch;
};

struct SipExpectedResponse {
  uint32_t sequence, peer_ip;
  std::string_view local_tag, remote_tag, branch;
  bool has_transaction;
};

enum class SipResponseMismatch { NONE, CSEQ, METHOD, FROM_TAG, TRANSACTION, BRANCH, PEER, TO_TAG, MISSING_TO_TAG };

inline const char *sip_response_mismatch_name(SipResponseMismatch reason) {
  switch (reason) {
    case SipResponseMismatch::CSEQ: return "stale INVITE CSeq";
    case SipResponseMismatch::METHOD: return "unsupported transaction method";
    case SipResponseMismatch::FROM_TAG: return "mismatched local From tag";
    case SipResponseMismatch::TRANSACTION: return "mismatched pending transaction";
    case SipResponseMismatch::BRANCH: return "mismatched transaction branch";
    case SipResponseMismatch::PEER: return "unexpected UDP peer";
    case SipResponseMismatch::TO_TAG: return "mismatched remote To tag";
    case SipResponseMismatch::MISSING_TO_TAG: return "missing remote To tag";
    default: return "";
  }
}

inline SipResponseMismatch match_sip_response(const SipResponseKey &key, const SipExpectedResponse &expected) {
  const bool invite_sequence = key.method == SipClientMethod::INVITE || key.method == SipClientMethod::CANCEL;
  if (invite_sequence && key.sequence != expected.sequence) return SipResponseMismatch::CSEQ;
  if (key.method == SipClientMethod::OTHER) return SipResponseMismatch::METHOD;
  if (expected.local_tag.empty() || key.from_tag != expected.local_tag) return SipResponseMismatch::FROM_TAG;
  if (invite_sequence) {
    if (expected.branch.empty() || key.branch.empty() || key.branch != expected.branch)
      return SipResponseMismatch::BRANCH;
  } else if (!expected.has_transaction || key.sequence != expected.sequence || key.branch != expected.branch) {
    return SipResponseMismatch::TRANSACTION;
  }
  if (expected.peer_ip != 0 && key.source_ip != expected.peer_ip) return SipResponseMismatch::PEER;
  if (key.status > 100) {
    if (!invite_sequence && !key.to_tag.empty() && !expected.remote_tag.empty() && key.to_tag != expected.remote_tag)
      return SipResponseMismatch::TO_TAG;
    if (key.to_tag.empty() && key.method != SipClientMethod::INVITE) return SipResponseMismatch::MISSING_TO_TAG;
  }
  return SipResponseMismatch::NONE;
}

}  // namespace esphome::voip_stack
