#pragma once

#include "esphome/core/defines.h"
#include <cstdint>
#include <string>
#ifdef USE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM
#include "sip_signaling_string.h"
#endif

namespace esphome::voip_stack {

struct MessageWaitingState {
  std::string account_id;
  bool available{false}, waiting{false};
  uint32_t new_messages{0}, old_messages{0}, urgent_new{0}, urgent_old{0};
#ifdef USE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM
  SipSignalingString message_account{};
#else
  std::string message_account{};
#endif
};

}  // namespace esphome::voip_stack
