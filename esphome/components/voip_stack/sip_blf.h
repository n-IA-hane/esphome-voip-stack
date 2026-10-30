#pragma once

#include "esphome/core/defines.h"
#ifdef USE_ESPHOME_VOIP_BLF
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace esphome::voip_stack {

enum class BlfState : uint8_t { UNKNOWN, IDLE, RINGING, ACTIVE, HELD, CALLING };
enum class PresenceState : uint8_t { UNKNOWN, OPEN, CLOSED };
enum class BlfDocumentResult : uint8_t { APPLIED, IGNORED, NEED_FULL, INVALID, LIMIT };
enum class BlfDialogDirection : uint8_t { NONE, UNKNOWN, INITIATOR, RECIPIENT };

struct BlfStatus {
  bool available{false};
  BlfState state{BlfState::UNKNOWN};
  PresenceState presence{PresenceState::UNKNOWN};
  uint32_t version{0};
  uint8_t ringing_dialogs{0}, active_dialogs{0}, held_dialogs{0};
};
struct BlfStateInfo : BlfStatus {
  std::string account_id, contact_id;
};
struct BlfWatchConfig {
  std::string contact_id, account_id, target;
};
#ifdef USE_ESPHOME_VOIP_PRESENCE
// RFC 3863 basic availability is independent of RFC 4235 call activity.
// The result is committed only after a complete bounded document validates.
bool parse_presence(std::string_view xml, PresenceState *result);
#endif

// Fixed memory per watched resource. A missing partial update must never turn
// an active phone into a false idle state. Unsupported volume requests a full
// resynchronization and keeps the public state unavailable.
class BlfDialogCache {
 public:
  static constexpr size_t MAX_DIALOGS = 4;
  static constexpr size_t MAX_ID_BYTES = 128;
  BlfDocumentResult apply(std::string_view xml);
  void invalidate() { this->valid_ = false; }
  void snapshot(BlfStatus *out) const;

 protected:
  struct Dialog {
    std::array<char, MAX_ID_BYTES + 1> id{};
    BlfState state{BlfState::UNKNOWN};
    BlfDialogDirection direction{BlfDialogDirection::NONE};
  };
  std::array<Dialog, MAX_DIALOGS> dialogs_{};
  uint32_t version_{0};
  bool valid_{false};
};

}  // namespace esphome::voip_stack
#endif
