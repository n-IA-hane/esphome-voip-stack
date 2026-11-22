#include "directory_contact.h"
#include "../voip_stack/sip_uri_user.h"

#include <algorithm>
#include <cctype>

namespace esphome::ldap_directory {
bool map_contact(const Entry &entry, const std::string &default_host, uint16_t default_port,
                 bool default_tcp, DirectoryContact *contact, bool managed_by_ha) {
  if (contact == nullptr) return false;
  DirectoryContact result;
  result.id = entry.first("uid");
  result.name = entry.first("displayName");
  if (result.name.empty()) result.name = entry.first("cn");
  result.kind = entry.first("ou");
  std::transform(result.kind.begin(), result.kind.end(), result.kind.begin(), [](unsigned char c) { return std::tolower(c); });
  const bool phone = result.kind == "phone" || result.kind == "sip_account" ||
                     result.kind == "esphome" || result.kind == "browser";
  result.number = entry.first("telephoneNumber");
  // Optional display metadata must not invalidate an otherwise valid SIP URI.
  if (!result.number.empty() && !voip_stack::sip_valid_text(result.number, voip_stack::CONTACT_ID_BYTES)) result.number.clear();
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
  } else {
    result.user = entry.first(phone ? "uid" : "telephoneNumber");
    if (result.user.empty()) result.user = entry.first(phone ? "telephoneNumber" : "uid");
  }
  if (phone) {
    const std::string username = entry.first("uid");
    if (!voip_stack::sip_valid_username(result.user) ||
        (managed_by_ha && (!voip_stack::sip_valid_username(username) || username != result.user))) return false;
    if (result.uri.empty()) {
      if (result.user.empty() || !voip_stack::sip_valid_username(result.user) ||
          !voip_stack::sip_valid_host(result.host) || result.port == 0) return false;
      result.uri = "sip:" + voip_stack::sip_uri_user_encode(result.user) + "@" + result.host;
      if (result.port != 5060) result.uri += ":" + std::to_string(result.port);
      if (result.tcp) result.uri += ";transport=tcp";
    }
    if (result.uri.size() > voip_stack::SIP_URI_BYTES) return false;
    result.id = result.user;
    result.kind = "sip_account";
  }
  if (result.id.empty()) result.id = result.user;
  if (result.name.empty()) result.name = result.user;
  if (!voip_stack::sip_valid_text(result.name, result.name.size())) return false;
  if (result.name.size() > voip_stack::SIP_NAME_BYTES) {
    size_t end = voip_stack::SIP_NAME_BYTES;
    while (end != 0 && (static_cast<unsigned char>(result.name[end]) & 0xc0) == 0x80) --end;
    result.name.resize(end);
  }
  // An escaped SIP user may contain %20. Keep the original URI for
  // routing; a bare telephone number must still contain no spaces.
  if (result.id.empty() || !voip_stack::sip_valid_contact_identity(result.id) ||
      result.name.empty() || !voip_stack::sip_valid_text(result.name, voip_stack::SIP_NAME_BYTES) ||
      result.user.empty() || !voip_stack::sip_valid_host(result.host) || result.port == 0) return false;
  if (result.uri.empty() && (!voip_stack::sip_valid_text(result.user, voip_stack::CONTACT_ID_BYTES) ||
                             result.user.find_first_of(" @<>/\\\"|,;") != std::string::npos)) return false;
  *contact = std::move(result);
  return true;
}

}  // namespace esphome::ldap_directory
