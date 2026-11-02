#include "directory_contact.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace esphome::ldap_directory {
namespace {
bool clean(const std::string &value, size_t max) {
  return !value.empty() && value.size() <= max && std::none_of(value.begin(), value.end(), [](unsigned char c) {
    return c < 0x20 || c == 0x7f || c == '|' || c == ',' || c == ';';
  });
}
bool port_number(const std::string &value, uint16_t *port) {
  unsigned parsed = 0;
  if (value.empty() || value.size() > 5) return false;
  for (const unsigned char c : value) { if (!std::isdigit(c)) return false; parsed = parsed * 10 + c - '0'; }
  if (parsed == 0 || parsed > 65535) return false;
  *port = static_cast<uint16_t>(parsed); return true;
}
bool unescape(const std::string &in, std::string *out) {
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
  };
  out->clear();
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] != '%') { out->push_back(in[i]); continue; }
    if (in.size() - i < 3) return false;
    const int a = hex(in[i + 1]), b = hex(in[i + 2]);
    if (a < 0 || b < 0) return false;
    out->push_back(static_cast<char>((a << 4) | b)); i += 2;
  }
  return true;
}
}

bool map_contact(const Entry &entry, const std::string &default_host, uint16_t default_port,
                 bool default_tcp, DirectoryContact *contact) {
  if (contact == nullptr) return false;
  DirectoryContact result;
  result.id = entry.first("uid");
  result.name = entry.first("displayName");
  if (result.name.empty()) result.name = entry.first("cn");
  if (result.id.empty()) result.id = entry.first("entryUUID");
  if (result.id.empty()) {
    // An opaque stable key for ordinary LDAP contacts without uid. It is not
    // an authentication identifier; the core detects duplicate-key conflicts.
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char c : entry.dn) { hash ^= c; hash *= UINT64_C(1099511628211); }
    char key[24]; snprintf(key, sizeof(key), "ldap:%016llx", static_cast<unsigned long long>(hash));
    result.id = key;
  }
  if (result.name.empty()) result.name = result.id;
  result.number = entry.first("telephoneNumber");
  // Optional display metadata must not invalidate an otherwise valid SIP URI.
  if (!result.number.empty() && !clean(result.number, 96)) result.number.clear();
  result.kind = entry.first("ou");
  std::transform(result.kind.begin(), result.kind.end(), result.kind.begin(), [](unsigned char c) { return std::tolower(c); });
  result.host = default_host; result.port = default_port; result.tcp = default_tcp;
  std::string uri;
  for (const auto &attribute : entry.attributes) {
    std::string key = attribute.name;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
    if (key != "labeleduri") continue;
    for (const auto &candidate : attribute.values) {
      std::string scheme = candidate.substr(0, 5);
      std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return std::tolower(c); });
      if (scheme.rfind("sip:", 0) == 0 || scheme == "sips:") { uri = candidate; break; }
    }
    if (!uri.empty()) break;
  }
  // RFC 2079 separates an optional human label with a space. A URI itself
  // percent-encodes spaces; do not turn its label into part of a dial target.
  const size_t label = uri.find(' ');
  if (label != std::string::npos) uri.resize(label);
  if (!uri.empty()) {
    std::string scheme = uri.substr(0, 4);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return std::tolower(c); });
    if (scheme != "sip:" || uri.find('?') != std::string::npos) return false;
    uri.replace(0, 4, "sip:");
    if (uri.size() > 96 || uri.find_first_of("\r\n\0|,", 0, 5) != std::string::npos) return false;
    for (size_t at = 0; (at = uri.find('%', at)) != std::string::npos; at += 3) {
      std::string decoded;
      if (uri.size() - at < 3 || !unescape(uri.substr(at, 3), &decoded)) return false;
    }
    result.uri = uri;
    uri.erase(0, 4);
    const size_t at = uri.find('@');
    if (at == std::string::npos || uri.find('@', at + 1) != std::string::npos || uri.substr(0, at).find(':') != std::string::npos ||
        !unescape(uri.substr(0, at), &result.user)) return false;
    std::string address = uri.substr(at + 1);
    const size_t semicolon = address.find(';');
    if (semicolon != std::string::npos) {
      const std::string parameters = address.substr(semicolon + 1); address.resize(semicolon);
      size_t start = 0; bool transport_seen = false;
      while (start < parameters.size()) {
        const size_t next = parameters.find(';', start);
        std::string value = parameters.substr(start, next == std::string::npos ? next : next - start);
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
        if (value.rfind("transport=", 0) == 0) {
          if (transport_seen) return false;
          transport_seen = true;
          if (value == "transport=tcp") result.tcp = true;
          else if (value == "transport=udp") result.tcp = false;
          else return false;
        }
        if (next == std::string::npos) break;
        start = next + 1;
      }
    }
    // The current SIP phonebook uses IPv4/DNS destinations. Do not silently
    // reinterpret an unsupported IPv6 literal or SIP password as a hostname.
    if (address.empty() || address[0] == '[') return false;
    const size_t colon = address.find(':');
    if (colon != std::string::npos) {
      if (!port_number(address.substr(colon + 1), &result.port)) return false;
      address.resize(colon);
    } else result.port = 5060;
    result.host = address;
  } else result.user = entry.first("telephoneNumber");
  if (result.name.size() > 64) {
    size_t end = 64;
    while (end != 0 && (static_cast<unsigned char>(result.name[end]) & 0xc0) == 0x80) --end;
    result.name.resize(end);
  }
  // An escaped SIP user may contain %20. Keep the original URI for
  // routing; a bare telephone number must still contain no spaces.
  if (!clean(result.id, 96) || !clean(result.name, 64) || !clean(result.user, 96) ||
      !clean(result.host, 64) || result.port == 0 || result.user.find_first_of("@<>/\\\"") != std::string::npos ||
      (result.uri.empty() && result.user.find(' ') != std::string::npos) ||
      result.host.find_first_of("@<>/\\\" :") != std::string::npos) return false;
  *contact = std::move(result);
  return true;
}

}  // namespace esphome::ldap_directory
