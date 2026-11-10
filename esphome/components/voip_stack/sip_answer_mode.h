#pragma once

#include "sip_record.h"

namespace esphome::voip_stack {

// RFC 5373: a request, never authorization and never a persistent setting.
struct SipAnswerMode {
  bool automatic{false};
  bool manual{false};
  bool required{false};
  bool valid{true};
};

inline bool sip_answer_mode_token(std::string_view value) {
  if (value.empty()) return false;
  for (const unsigned char ch : value) {
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || std::string_view("-.!%*_+`'~").find(ch) != std::string_view::npos))
      return false;
  }
  return true;
}

inline SipAnswerMode sip_requested_answer_mode(std::string_view message) {
  SipAnswerMode result;
  unsigned count = 0;
  sip_record_fields(message, "Answer-Mode", [&](std::string_view field) {
    if (++count != 1 || field.size() > 128) {
      result.valid = false;
      return false;
    }
    const std::string unfolded = sip_record_unfold(field);
    std::string_view value = sip_record_trim(unfolded);
    const auto separator = value.find(';');
    const auto mode = sip_record_trim(value.substr(0, separator));
    result.automatic = sip_header_name_equal(mode, "Auto");
    result.manual = sip_header_name_equal(mode, "Manual");
    // RFC 5373 section 2 requires unknown token values and parameters to be ignored.
    result.valid = sip_answer_mode_token(mode);
    size_t at = separator;
    while (result.valid && at != std::string_view::npos) {
      const size_t start = at + 1;
      size_t end = start;
      bool quoted = false, escaped = false;
      for (; end < value.size(); ++end) {
        const char ch = value[end];
        if (escaped) { escaped = false; continue; }
        if (quoted && ch == '\\') { escaped = true; continue; }
        if (ch == '"') quoted = !quoted;
        if (!quoted && ch == ';') break;
      }
      const auto parameter = sip_record_trim(value.substr(start, end - start));
      const auto equal = parameter.find('=');
      const auto name = sip_record_trim(parameter.substr(0, equal));
      result.valid = !quoted && !escaped && sip_answer_mode_token(name) &&
                     (equal == std::string_view::npos || !sip_record_trim(parameter.substr(equal + 1)).empty());
      if (equal == std::string_view::npos && sip_header_name_equal(name, "require")) {
        if (result.required) result.valid = false;
        result.required = true;
      }
      at = end < value.size() ? end : std::string_view::npos;
    }
    return true;
  });
  return result;
}

inline bool sip_paging_auto_answer_authorized(bool requested, bool account_matched,
                                             bool account_allowed, bool direct_peer_allowed) {
  return requested && (account_matched ? account_allowed : direct_peer_allowed);
}

}  // namespace esphome::voip_stack
