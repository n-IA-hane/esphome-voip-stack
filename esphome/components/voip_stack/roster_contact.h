#pragma once

#include <string>
#include <string_view>

namespace esphome::voip_stack {

struct RosterContactFields {
  std::string_view id, endpoint_id, name, extension, number, sip_uri;
};

struct RosterContactIdentity {
  std::string stable_id, name, route, number;
};

// Public dialing fields are independent of the provider's stable resource key.
bool resolve_roster_contact(const RosterContactFields &fields, RosterContactIdentity *out);

}  // namespace esphome::voip_stack
