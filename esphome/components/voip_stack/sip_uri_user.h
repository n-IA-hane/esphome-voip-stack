#pragma once

#include <string>
#include <string_view>

namespace esphome::voip_stack {

// Append directly to an existing SIP transaction buffer, including PSRAM storage.
template<typename Buffer> void sip_append_uri_user(Buffer &out, std::string_view raw) {
  static constexpr char HEX[] = "0123456789ABCDEF";
  const auto start = out.size();
  for (const char ch : raw) {
    if (ch == '\r' || ch == '\n') continue;
    const auto byte = static_cast<unsigned char>(ch);
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
        ch == '-' || ch == '.' || ch == '_' || ch == '~') {
      out.append(&ch, 1);
    } else {
      const char escaped[] = {'%', HEX[byte >> 4], HEX[byte & 15]};
      out.append(escaped, sizeof(escaped));
    }
  }
  if (out.size() == start) out.append("voip", 4);
}

inline std::string sip_uri_user_encode(const std::string &raw) {
  std::string out;
  sip_append_uri_user(out, raw);
  return out;
}

}  // namespace esphome::voip_stack
