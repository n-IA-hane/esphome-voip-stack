#pragma once

#include <cctype>
#include <string_view>

namespace esphome::voip_stack {

// RFC 3261 7.3.3: compact and long header names have identical semantics.
// Keep name recognition shared without changing each reader's framing rules.
inline bool sip_header_name_equal(std::string_view left, std::string_view right) {
  const auto expand = [](std::string_view name) -> std::string_view {
    if (name.size() != 1) return name;
    switch (std::tolower(static_cast<unsigned char>(name[0]))) {
      case 'v': return "Via";
      case 'f': return "From";
      case 't': return "To";
      case 'i': return "Call-ID";
      case 'm': return "Contact";
      case 'c': return "Content-Type";
      case 'l': return "Content-Length";
      case 'o': return "Event";
      default: return name;
    }
  };
  left = expand(left);
  right = expand(right);
  if (left.size() != right.size()) return false;
  for (size_t i = 0; i < left.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(left[i])) !=
        std::tolower(static_cast<unsigned char>(right[i]))) return false;
  return true;
}

}  // namespace esphome::voip_stack
