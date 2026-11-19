#include "directory_contact.h"
#include "../voip_stack/sip_uri_user.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace esphome::ldap_directory {
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
  if (!result.number.empty() && !voip_stack::sip_valid_text(result.number, voip_stack::CONTACT_ID_BYTES)) result.number.clear();
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
    voip_stack::SipTargetParts parsed;
    if (!voip_stack::sip_parse_target_uri(uri, parsed)) return false;
    result.uri = uri;
    result.uri.replace(0, 4, "sip:");
    result.user = std::move(parsed.user);
    result.host.assign(parsed.host);
    result.port = parsed.port;
    if (parsed.has_transport) result.tcp = parsed.tcp;
  } else result.user = entry.first("telephoneNumber");
  if (result.name.size() > voip_stack::SIP_NAME_BYTES) {
    size_t end = voip_stack::SIP_NAME_BYTES;
    while (end != 0 && (static_cast<unsigned char>(result.name[end]) & 0xc0) == 0x80) --end;
    result.name.resize(end);
  }
  // An escaped SIP user may contain %20. Keep the original URI for
  // routing; a bare telephone number must still contain no spaces.
  if (result.id.empty() || !voip_stack::sip_valid_text(result.id, voip_stack::CONTACT_ID_BYTES) ||
      result.name.empty() || !voip_stack::sip_valid_text(result.name, voip_stack::SIP_NAME_BYTES) ||
      result.user.empty() || !voip_stack::sip_valid_host(result.host) || result.port == 0) return false;
  if (result.uri.empty() && (!voip_stack::sip_valid_text(result.user, voip_stack::CONTACT_ID_BYTES) ||
                             result.user.find_first_of(" @<>/\\\"|,;") != std::string::npos)) return false;
  *contact = std::move(result);
  return true;
}

}  // namespace esphome::ldap_directory
