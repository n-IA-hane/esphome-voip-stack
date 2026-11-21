#include "roster_contact.h"
#include "sip_uri_user.h"

namespace esphome::voip_stack {
namespace {
std::string_view trim_route(std::string_view value) {
  const auto start = value.find_first_not_of(" \t\r\n");
  if (start == value.npos) return {};
  return value.substr(start, value.find_last_not_of(" \t\r\n") - start + 1);
}

bool public_route(std::string_view value) {
  if (value.empty()) return false;
  if (value.rfind("sip:", 0) == 0) {
    SipTargetParts target;
    return sip_parse_target_uri(value, target);
  }
  // Resource identifiers such as queue:<uuid> and paging:<uuid> are metadata.
  return value.find(':') == value.npos && sip_valid_text(value, CONTACT_ID_BYTES);
}
}  // namespace

bool resolve_roster_contact(const RosterContactFields &fields, RosterContactIdentity *out) {
  if (out == nullptr) return false;
  RosterContactIdentity result;
  const auto id = trim_route(fields.id);
  bool explicit_route = false;
  for (const auto candidate : {fields.extension, fields.number, fields.sip_uri, id}) {
    const auto route = trim_route(candidate);
    if (route.empty()) continue;
    if (!public_route(route)) return false;
    result.route.assign(route);
    explicit_route = !trim_route(fields.extension).empty() || !trim_route(fields.number).empty() ||
                     !trim_route(fields.sip_uri).empty();
    break;
  }
  if (result.route.empty()) return false;
  const auto stable = fields.endpoint_id.empty() ? id : fields.endpoint_id;
  if (!sip_valid_text(stable, CONTACT_ID_BYTES)) return false;
  result.stable_id.assign(stable.empty() ? std::string_view(result.route) : stable);
  const auto number = fields.extension.empty() ? trim_route(fields.number) : trim_route(fields.extension);
  if (!number.empty() && sip_valid_text(number, CONTACT_ID_BYTES)) result.number.assign(number);
  // Validate the complete UTF-8 text before truncation, including its suffix.
  // A public destination is a readable fallback; an opaque resource key is not.
  if (fields.name.empty() && !explicit_route && result.route.rfind("sip:", 0) != 0 &&
      result.route.find_first_not_of("+0123456789*#") != std::string::npos) return false;
  const auto name = fields.name.empty() ? std::string_view(result.route) : fields.name;
  if (!sip_valid_text(name, name.size())) return false;
  size_t end = std::min(name.size(), SIP_NAME_BYTES);
  while (end < name.size() && end != 0 && (static_cast<unsigned char>(name[end]) & 0xc0) == 0x80) --end;
  result.name.assign(name.substr(0, end));
  if (result.name.empty()) return false;
  *out = std::move(result);
  return true;
}

}  // namespace esphome::voip_stack
