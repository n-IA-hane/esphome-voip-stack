#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace esphome::voip_stack {

inline constexpr size_t SIP_NAME_BYTES = 64;
inline constexpr size_t CONTACT_ID_BYTES = 96;
inline constexpr size_t SIP_HOST_BYTES = 253;
inline constexpr size_t SIP_URI_BYTES = 512;

inline bool sip_valid_text(std::string_view name, size_t max_bytes) {
  if (name.size() > max_bytes) return false;
  // RFC 3629 permits only canonical sequences and Unicode scalar values.
  for (size_t i = 0; i < name.size();) {
    const auto lead = static_cast<unsigned char>(name[i++]);
    if (lead < 0x20 || lead == 0x7f) return false;
    if (lead < 0x80) continue;
    if (lead < 0xC2 || lead > 0xF4) return false;
    const size_t following = lead < 0xE0 ? 1 : (lead < 0xF0 ? 2 : 3);
    if (following > name.size() - i) return false;
    const auto next = static_cast<unsigned char>(name[i]);
    if ((lead == 0xE0 && next < 0xA0) || (lead == 0xED && next >= 0xA0) ||
        (lead == 0xF0 && next < 0x90) || (lead == 0xF4 && next >= 0x90)) return false;
    for (size_t end = i + following; i < end; ++i)
      if ((static_cast<unsigned char>(name[i]) & 0xC0) != 0x80) return false;
  }
  return true;
}

inline uint8_t sip_hex_value(char ch) {
  if (ch >= '0' && ch <= '9') return static_cast<uint8_t>(ch - '0');
  if (ch >= 'A' && ch <= 'F') return static_cast<uint8_t>(ch - 'A' + 10);
  if (ch >= 'a' && ch <= 'f') return static_cast<uint8_t>(ch - 'a' + 10);
  return 0xFF;
}

inline std::string sip_decode_user(std::string_view raw) {
  std::string out;
  for (size_t i = 0; i < raw.size(); ++i) {
    auto ch = static_cast<unsigned char>(raw[i]);
    if (ch == '%') {
      if (i + 2 >= raw.size()) return "";
      const auto hi = sip_hex_value(raw[i + 1]);
      const auto lo = sip_hex_value(raw[i + 2]);
      if (hi == 0xFF || lo == 0xFF) return "";
      ch = static_cast<unsigned char>((hi << 4) | lo);
      i += 2;
    }
    if (ch < 32 || ch == 127) return "";
    out.push_back(static_cast<char>(ch));
  }
  return out;
}

inline bool sip_valid_host(std::string_view host) {
  if (host.empty() || host.size() > SIP_HOST_BYTES) return false;
  size_t label_size = 0;
  for (size_t i = 0; i < host.size(); ++i) {
    if (host[i] == '.') {
      if (label_size == 0 || host[i - 1] == '-') return false;
      label_size = 0;
    } else {
      if ((label_size == 0 && host[i] == '-') || ++label_size > 63) return false;
    }
  }
  if (host.back() == '-') return false;
  for (unsigned char ch : host)
    if (!std::isalnum(ch) && ch != '-' && ch != '.') return false;
  return true;
}

struct SipTargetParts {
  std::string user;
  std::string_view host;
  uint16_t port{5060};
  std::string_view authority_and_parameters;
  bool has_transport{false};
  bool tcp{false};
};

// The user is decoded after locating the literal @. The authority and URI
// parameters remain wire text and can be appended without another decoding pass.
inline bool sip_parse_target_uri(std::string_view value, SipTargetParts &parts) {
  if (value.size() < 5 || value.size() > SIP_URI_BYTES ||
      (value[0] != 's' && value[0] != 'S') || (value[1] != 'i' && value[1] != 'I') ||
      (value[2] != 'p' && value[2] != 'P') || value[3] != ':') return false;
  for (unsigned char c : value)
    if (c <= 32 || c == 127 || c == '<' || c == '>' || c == '"' || c == '|') return false;
  if (value.find('?') != value.npos) return false;
  const auto at = value.find('@', 4);
  if (at == std::string::npos || at == 4 || at + 1 == value.size()) return false;
  const auto user = value.substr(4, at - 4);
  if (user.find(':') != std::string::npos) return false;
  parts.user = sip_decode_user(user);
  if (parts.user.empty()) return false;
  if (value.find('@', at + 1) != value.npos) return false;
  const auto authority_end = value.find_first_of(";?", at + 1);
  const auto authority = value.substr(at + 1, authority_end - at - 1);
  if (authority.empty() || authority.find('@') != std::string::npos) return false;
  const auto colon = authority.find(':');
  const auto host = authority.substr(0, colon);
  if (!sip_valid_host(host)) return false;
  parts.host = host;
  parts.authority_and_parameters = value.substr(at);
  parts.port = 5060;
  parts.has_transport = false;
  parts.tcp = false;
  if (colon != std::string::npos) {
    uint32_t port = 0;
    const auto text = authority.substr(colon + 1);
    if (text.empty()) return false;
    for (unsigned char ch : text) {
      if (ch < '0' || ch > '9') return false;
      port = port * 10U + ch - '0';
      if (port > 65535) return false;
    }
    if (port == 0) return false;
    parts.port = static_cast<uint16_t>(port);
  }
  for (size_t position = authority_end; position != std::string::npos && position < value.size();) {
    const auto end = value.find(';', position + 1);
    const auto parameter = value.substr(position + 1, end - position - 1);
    if (parameter.empty()) return false;
    for (size_t i = 0; i < parameter.size(); ++i) {
      if (parameter[i] != '%') continue;
      if (i + 2 >= parameter.size()) return false;
      const auto hi = sip_hex_value(parameter[i + 1]), lo = sip_hex_value(parameter[i + 2]);
      if (hi == 0xff || lo == 0xff || ((hi << 4) | lo) < 32 || ((hi << 4) | lo) == 127) return false;
      i += 2;
    }
    const auto equals = parameter.find('=');
    std::string key(parameter.substr(0, equals));
    if (key.empty()) return false;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) { return std::tolower(ch); });
    if (key == "transport") {
      if (parts.has_transport || equals == std::string::npos) return false;
      std::string transport(parameter.substr(equals + 1));
      std::transform(transport.begin(), transport.end(), transport.begin(), [](unsigned char ch) { return std::tolower(ch); });
      if (transport != "udp" && transport != "tcp") return false;
      parts.tcp = transport == "tcp";
      parts.has_transport = true;
    }
    position = end;
  }
  return true;
}

inline bool sip_matches_aor(const SipTargetParts &own, std::string_view route) {
  if (own.user.empty()) return false;
  SipTargetParts candidate;
  return sip_parse_target_uri(route, candidate) && own.user == candidate.user && own.port == candidate.port &&
         own.host.size() == candidate.host.size() &&
         std::equal(own.host.begin(), own.host.end(), candidate.host.begin(), [](unsigned char a, unsigned char b) {
           return std::tolower(a) == std::tolower(b);
         });
}

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
