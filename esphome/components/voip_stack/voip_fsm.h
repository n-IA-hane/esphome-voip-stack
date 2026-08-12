#pragma once

#include <cstdint>

namespace esphome {
namespace voip_stack {

// Public SIP phone state for display / triggers.
//   IDLE -> CALLING -> IN_CALL (outbound INVITE path)
//   IDLE -> RINGING -> IN_CALL (inbound INVITE path)
enum class CallState : uint8_t {
  IDLE,
  CALLING,
  REMOTE_RINGING,
  RINGING,
  CONNECTING,
  IN_CALL,
  TERMINATING,
  BUSY,
  DECLINED,
  CANCELLED,
  MEDIA_INCOMPATIBLE,
  TRANSPORT_UNREACHABLE,
  AUTH_REQUIRED_UNSUPPORTED,
};

enum class CallEndReason : uint8_t {
  LOCAL_HANGUP,
  REMOTE_HANGUP,
  DECLINED,
  TIMEOUT,
  BUSY,
  CANCELLED,
  TRANSPORT_UNREACHABLE,
  MEDIA_INCOMPATIBLE,
  MEDIA_TIMEOUT,
  AUTH_REQUIRED_UNSUPPORTED,
  PROXY_AUTH_REQUIRED_UNSUPPORTED,
  PROTOCOL_ERROR,
};

static constexpr const char *kReasonLocalHangup = "local_hangup";
static constexpr const char *kReasonRemoteHangup = "remote_hangup";
static constexpr const char *kReasonDeclined = "declined";
static constexpr const char *kReasonTimeout = "timeout";
static constexpr const char *kReasonBusy = "busy";
static constexpr const char *kReasonCancelled = "cancelled";
static constexpr const char *kReasonTransportUnreachable = "transport_unreachable";
static constexpr const char *kReasonMediaIncompatible = "media_incompatible";
static constexpr const char *kReasonMediaTimeout = "media_timeout";
static constexpr const char *kReasonAuthRequiredUnsupported = "auth_required_unsupported";
static constexpr const char *kReasonProxyAuthRequiredUnsupported = "proxy_auth_required_unsupported";
static constexpr const char *kReasonProtocolError = "protocol_error";

inline const char *call_state_to_str(CallState state) {
  switch (state) {
    case CallState::IDLE: return "idle";
    case CallState::CALLING: return "calling";
    case CallState::REMOTE_RINGING: return "remote_ringing";
    case CallState::RINGING: return "ringing";
    case CallState::CONNECTING: return "connecting";
    case CallState::IN_CALL: return "in_call";
    case CallState::TERMINATING: return "terminating";
    case CallState::BUSY: return "busy";
    case CallState::DECLINED: return "declined";
    case CallState::CANCELLED: return "cancelled";
    case CallState::MEDIA_INCOMPATIBLE: return "media_incompatible";
    case CallState::TRANSPORT_UNREACHABLE: return "transport_unreachable";
    case CallState::AUTH_REQUIRED_UNSUPPORTED: return "auth_required_unsupported";
    default: return "unknown";
  }
}

inline const char *call_end_reason_to_str(CallEndReason reason) {
  switch (reason) {
    case CallEndReason::LOCAL_HANGUP: return kReasonLocalHangup;
    case CallEndReason::REMOTE_HANGUP: return kReasonRemoteHangup;
    case CallEndReason::DECLINED: return kReasonDeclined;
    case CallEndReason::TIMEOUT: return kReasonTimeout;
    case CallEndReason::BUSY: return kReasonBusy;
    case CallEndReason::CANCELLED: return kReasonCancelled;
    case CallEndReason::TRANSPORT_UNREACHABLE: return kReasonTransportUnreachable;
    case CallEndReason::MEDIA_INCOMPATIBLE: return kReasonMediaIncompatible;
    case CallEndReason::MEDIA_TIMEOUT: return kReasonMediaTimeout;
    case CallEndReason::AUTH_REQUIRED_UNSUPPORTED: return kReasonAuthRequiredUnsupported;
    case CallEndReason::PROXY_AUTH_REQUIRED_UNSUPPORTED: return kReasonProxyAuthRequiredUnsupported;
    case CallEndReason::PROTOCOL_ERROR: return kReasonProtocolError;
    default: return "unknown";
  }
}

}  // namespace voip_stack
}  // namespace esphome
