#pragma once

#include "esphome/core/defines.h"
#ifdef USE_ESPHOME_VOIP_REGISTRATION
#include "sip_registration.h"

namespace esphome::voip_stack {

// Cold-path copy only. Consumers own these strings and must not retain a
// pointer into the SIP account pool, which can be replaced or removed.
struct SipAccountSnapshot {
  SipAccountConfig config;
  uint32_t revision{0};
  bool registered{false};
};

}  // namespace esphome::voip_stack
#endif
