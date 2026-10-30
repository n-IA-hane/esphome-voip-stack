#include "voip_stack.h"
#if defined(USE_ESP32) && defined(USE_ESPHOME_VOIP_BLF)
#include "sip_transport.h"
#include "sip_message.h"

namespace esphome::voip_stack {
namespace {
const char *blf_state_name(BlfState state) {
  switch (state) {
    case BlfState::IDLE: return "idle";
    case BlfState::RINGING: return "ringing";
    case BlfState::ACTIVE: return "active";
    case BlfState::HELD: return "held";
    case BlfState::CALLING: return "calling";
    default: return "unknown";
  }
}
}  // namespace

void VoipStack::blf_callback_(void *context, const BlfStateInfo &state) {
  auto *self = static_cast<VoipStack *>(context);
  if (!self->blf_notifications_requested_.load(std::memory_order_acquire)) return;
  self->defer([self, id = state.contact_id]() {
    const auto current = self->get_contact_blf_info_(id);
    self->blf_callbacks_.call(current);
    self->blf_state_trigger_.trigger(current.contact_id, current.account_id, blf_state_name(current.state), current.available);
  });
}

BlfStatus VoipStack::get_contact_blf(std::string_view id) {
  BlfStatus status;
  if (this->transport_ != nullptr && this->blf_ready_)
    static_cast<SipTransport *>(this->transport_.get())->get_blf_state(id, &status);
  return status;
}

BlfStateInfo VoipStack::get_contact_blf_info_(std::string_view id) {
  BlfStateInfo state;
  state.contact_id.assign(id.data(), id.size());
  if (this->transport_ != nullptr && this->blf_ready_)
    static_cast<SipTransport *>(this->transport_.get())->get_blf_info(id, &state);
  return state;
}

bool VoipStack::set_blf_watches(const std::vector<BlfWatchConfig> &watches) {
  return this->transport_ != nullptr && this->blf_ready_ &&
      static_cast<SipTransport *>(this->transport_.get())->set_blf_watches(watches);
}

void VoipStack::sync_blf_contacts_() {
  if (!this->blf_ready_ || !this->blf_watch_contacts_) return;
  std::vector<BlfWatchConfig> watches;
  watches.reserve(std::min<size_t>(this->phonebook_.size(), this->blf_capacity_));
  for (size_t i = 0; i < this->phonebook_.size() && watches.size() < this->blf_capacity_; ++i) {
    const auto *entry = this->phonebook_.at(i);
    if (entry == nullptr || !entry->auto_blf || entry->kind == ContactKind::AUTOMATION) continue;
    std::string account = this->get_contact_account(entry->stable_id());
    if (account == "direct") account = this->blf_account_id_;
    watches.push_back({std::string(entry->stable_id()), std::move(account), static_cast<std::string>(entry->route)});
  }
  this->set_blf_watches(watches);
}

bool VoipStack::pickup_contact(const std::string &id) {
  if (!this->can_start_call_() || this->blf_pickup_prefix_.empty()) return false;
  const auto *entry = this->phonebook_.find_id(id);
  if (entry == nullptr) return false;
  const auto state = this->get_contact_blf_info_(id);
  if (!state.available || state.ringing_dialogs == 0) return false;
  const std::string target = sip_prefix_target_user(entry->route, this->blf_pickup_prefix_);
  if (target.empty() || target.size() > Phonebook::MAX_ROUTE_BYTES) return false;
  this->start_call_on_line(target, state.account_id);
  return true;
}

}  // namespace esphome::voip_stack
#endif
