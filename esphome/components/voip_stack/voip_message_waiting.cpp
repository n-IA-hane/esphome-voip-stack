#include "voip_stack.h"
#include <cctype>

#if defined(USE_ESP32) && defined(USE_ESPHOME_VOIP_MWI)
#ifdef USE_ESPHOME_VOIP_MWI
#include "sip_transport.h"
#include "sip_message.h"
#endif

namespace esphome::voip_stack {

MessageWaitingState VoipStack::get_message_waiting(const std::string &id) {
  MessageWaitingState state;
  state.account_id = id;

#ifdef USE_ESPHOME_VOIP_MWI
  if (this->transport_ != nullptr)
    static_cast<SipTransport *>(this->transport_.get())->get_message_waiting(id, &state, this->default_line_);
#endif
  return state;
}

bool VoipStack::has_waiting_messages(const std::string &id) {

#ifdef USE_ESPHOME_VOIP_MWI
  return this->transport_ != nullptr && static_cast<SipTransport *>(this->transport_.get())->has_waiting_messages(id);
#else
  return false;
#endif
}

void VoipStack::message_waiting_callback_(void *ctx, SipTransport *owner, size_t index) {
  auto *self = static_cast<VoipStack *>(ctx);
  if (index >= SipRegistration::CAPACITY) return;
  self->message_waiting_owner_.store(owner, std::memory_order_release);
  self->message_waiting_pending_.fetch_or(1U << index, std::memory_order_release);
  self->enable_loop_soon_any_context();
}
void VoipStack::pump_message_waiting_() {
  const uint32_t pending = this->message_waiting_pending_.exchange(0, std::memory_order_acq_rel);
  auto *owner = this->message_waiting_owner_.load(std::memory_order_acquire);
  if (owner == nullptr || this->transport_.get() != owner) return;
  for (size_t i = 0; i < SipRegistration::CAPACITY; ++i) if (pending & (1U << i)) {
    MessageWaitingState state;
    if (owner->copy_message_waiting_slot(i, &state)) this->publish_message_waiting_(state);
  }
}
void VoipStack::publish_message_waiting_(const MessageWaitingState &state) {
  this->message_waiting_trigger_.trigger(state.account_id, state.available, state.waiting,
      state.new_messages, state.old_messages, state.urgent_new, state.urgent_old);
  this->message_waiting_callbacks_.call();
}

bool VoipStack::mailbox_target_(const std::string &account_id, const std::string &override_target,
                               std::string *target, std::string *line) {
  const auto state = this->get_message_waiting(account_id);
  *line = account_id.empty() ? state.account_id : account_id;
  if (line->empty() || !this->is_registered(*line)) return false;
  if (override_target.empty() && !state.available) return false;
  const std::string destination = override_target.empty() ? static_cast<std::string>(state.message_account) : override_target;
  // Account-URI can use other schemes; this endpoint places SIP calls only.
  const auto colon = destination.find(':');
  if (colon != std::string::npos &&
      (colon != 3 || std::tolower(static_cast<unsigned char>(destination[0])) != 's' ||
       std::tolower(static_cast<unsigned char>(destination[1])) != 'i' ||
       std::tolower(static_cast<unsigned char>(destination[2])) != 'p')) return false;
  *target = sip_normalize_dial_target(destination, Phonebook::MAX_ROUTE_BYTES);
  return !target->empty();
}

bool VoipStack::can_call_mailbox(const std::string &account_id, const std::string &target) {
  std::string resolved, line;
  return this->can_start_call_() && this->mailbox_target_(account_id, target, &resolved, &line);
}

bool VoipStack::call_mailbox(const std::string &account_id, const std::string &target) {
  std::string resolved, line;
  if (!this->can_start_call_() || !this->mailbox_target_(account_id, target, &resolved, &line)) return false;
  this->start_call_on_line(resolved, line);
  return true;
}



}  // namespace esphome::voip_stack
#endif
