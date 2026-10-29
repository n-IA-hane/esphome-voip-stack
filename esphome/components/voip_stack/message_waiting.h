#pragma once

#include <cstdint>
#include <string>

namespace esphome::voip_stack {

struct MessageWaitingState {
  std::string account_id;
  bool available{false}, waiting{false};
  uint32_t new_messages{0}, old_messages{0}, urgent_new{0}, urgent_old{0};
};

}  // namespace esphome::voip_stack
