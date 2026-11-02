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

void VoipStack::initialize_blf_() {
  auto *transport = static_cast<SipTransport *>(this->transport_.get());
  this->blf_ready_ = transport->configure_blf(this->blf_capacity_, blf_callback_);
  if (!this->blf_ready_) ESP_LOGE("voip_stack.blf", "Could not allocate bounded subscription pool");
  this->sync_blf_contacts_();
}
void VoipStack::blf_callback_(void *context, SipTransport *owner, size_t slot) {
  auto *self = static_cast<VoipStack *>(context);
  if (!self->blf_notifications_requested_.load(std::memory_order_acquire) || slot >= self->blf_capacity_) return;
  self->blf_event_owner_.store(owner, std::memory_order_release);
  self->blf_pending_[slot / 32].fetch_or(1U << (slot % 32), std::memory_order_release);
  self->enable_loop_soon_any_context();
}
void VoipStack::pump_blf_events_() {
  std::array<uint32_t, 2> pending{{this->blf_pending_[0].exchange(0, std::memory_order_acq_rel),
      this->blf_pending_[1].exchange(0, std::memory_order_acq_rel)}};
  auto *owner = this->blf_event_owner_.load(std::memory_order_acquire);
  if (owner == nullptr || this->transport_.get() != owner) return;
  uint8_t budget = 8;
  for (size_t slot = 0; slot < this->blf_capacity_ && budget != 0; ++slot) {
    const uint32_t mask = 1U << (slot % 32);
    if (!(pending[slot / 32] & mask)) continue;
    pending[slot / 32] &= ~mask; --budget;
    BlfStateInfo state;
    if (owner->copy_blf_slot(slot, &state)) {
      this->blf_callbacks_.call(state);
      this->blf_state_trigger_.trigger(state.contact_id, state.account_id, blf_state_name(state.state), state.available);
    }
  }
  for (size_t word = 0; word < pending.size(); ++word) if (pending[word] != 0) {
    this->blf_pending_[word].fetch_or(pending[word], std::memory_order_release); this->enable_loop_soon_any_context();
  }
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
    if (entry == nullptr || !contact_can_auto_blf(*entry)) continue;
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
  if (!this->is_registered(state.account_id)) return false;
  this->start_call_on_line(target, state.account_id);
  return true;
}

}  // namespace esphome::voip_stack
#endif
