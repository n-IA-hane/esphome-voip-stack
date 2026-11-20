#pragma once

#include "sip_header_name.h"

#include <string>
#include <string_view>

namespace esphome::voip_stack {

inline std::string_view sip_record_trim(std::string_view value) {
  const auto first = value.find_first_not_of(" \t");
  return first == std::string_view::npos
             ? std::string_view{}
             : value.substr(first, value.find_last_not_of(" \t") - first + 1);
}

template<class F> void sip_record_header_fields(std::string_view message, std::string_view wanted, F consume) {
  size_t start = 0;
  while (start < message.size()) {
    const size_t end = message.find("\r\n", start);
    if (end == std::string_view::npos || end == start) break;
    size_t last = end, next = message.find("\r\n", end + 2);
    while (end + 2 < message.size() && last + 2 < message.size() &&
           (message[last + 2] == ' ' || message[last + 2] == '\t') && next != std::string_view::npos) {
      last = next;
      next = message.find("\r\n", last + 2);
    }
    const auto colon = message.find(':', start);
    if (colon < end && sip_header_name_equal(sip_record_trim(message.substr(start, colon - start)), wanted)) {
      if (!consume(sip_record_trim(message.substr(colon + 1, last - colon - 1)))) return;
    }
    start = last + 2;
  }
}

template<class F> void sip_record_fields(std::string_view message, std::string_view wanted, F consume) {
  const size_t first_line = message.find("\r\n");
  if (first_line != std::string_view::npos)
    sip_record_header_fields(message.substr(first_line + 2), wanted, consume);
}

inline std::string_view sip_record_first_header(std::string_view block, std::string_view name) {
  std::string_view value;
  sip_record_header_fields(block, name, [&](std::string_view field) {
    value = field;
    return false;
  });
  return value;
}

// Find one name-addr without mistaking quoted display text for URI delimiters.
inline bool sip_name_addr_bounds(std::string_view field, size_t &open, size_t &close) {
  open = close = field.npos;
  bool quoted = false;
  for (size_t i = 0; i < field.size(); ++i) {
    const char ch = field[i];
    if (quoted && ch == '\\') {
      if (++i == field.size()) return false;
    } else if (ch == '"') {
      quoted = !quoted;
    } else if (!quoted && ch == '<') {
      if (open != field.npos) return false;
      open = i;
    } else if (!quoted && ch == '>') {
      if (open == field.npos || close != field.npos) return false;
      close = i;
    }
  }
  return !quoted && ((open == field.npos) == (close == field.npos));
}

// A Via chain may use repeated fields or a comma list. Quoted parameters and
// comments can themselves contain commas; folded LWS is not a new field.
inline std::string_view sip_record_first_list_value(std::string_view field) {
  bool quoted = false, escaped = false;
  unsigned comments = 0;
  for (size_t i = 0; i < field.size(); ++i) {
    const char ch = field[i];
    if (escaped) { escaped = false; continue; }
    if ((quoted || comments) && ch == '\\') { escaped = true; continue; }
    if (!comments && ch == '"') { quoted = !quoted; continue; }
    if (!quoted && ch == '(') { ++comments; continue; }
    if (!quoted && comments && ch == ')') { --comments; continue; }
    if (quoted || comments) continue;
    if (ch == ',' || (ch == '\r' && i + 1 < field.size() && field[i + 1] == '\n' &&
                      (i + 2 >= field.size() || (field[i + 2] != ' ' && field[i + 2] != '\t'))))
      return field.substr(0, i);
  }
  return field;
}

inline std::string_view sip_record_first(std::string_view message, std::string_view name) {
  std::string_view value;
  sip_record_fields(message, name, [&](std::string_view field) {
    value = field;
    return false;
  });
  return value;
}

inline std::string sip_record_unfold(std::string_view field) {
  std::string result;
  result.reserve(field.size());
  for (size_t start = 0; start < field.size();) {
    const auto end = field.find("\r\n", start);
    const auto part = sip_record_trim(field.substr(start, end - start));
    if (!result.empty()) result += ' ';
    result.append(part.data(), part.size());
    if (end == std::string_view::npos) break;
    start = end + 2;
  }
  return result;
}

struct SipContentLength {
  size_t bytes{0};
  bool present{false}, valid{true};

  bool matches_body(std::string_view message) const {
    if (!valid) return false;
    if (!present) return true;
    const auto separator = message.find("\r\n\r\n");
    return separator != message.npos && bytes == message.size() - separator - 4;
  }
};

inline SipContentLength sip_record_content_length(std::string_view message, size_t limit) {
  SipContentLength result;
  sip_record_fields(message, "Content-Length", [&](std::string_view field) {
    if (result.present) {
      result.valid = false;
      return false;
    }
    result.present = true;
    const auto value = sip_record_unfold(field);
    if (value.empty()) {
      result.valid = false;
      return false;
    }
    size_t length = 0;
    for (const char ch : value) {
      if (ch < '0' || ch > '9') {
        result.valid = false;
        return false;
      }
      const size_t digit = static_cast<size_t>(ch - '0');
      if (digit > limit || length > (limit - digit) / 10) {
        result.valid = false;
        return false;
      }
      length = length * 10 + digit;
    }
    result.bytes = length;
    return true;
  });
  return result;
}

}  // namespace esphome::voip_stack
