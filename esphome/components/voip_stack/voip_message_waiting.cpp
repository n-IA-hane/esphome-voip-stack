#include "voip_stack.h"

#if defined(USE_ESP32) && defined(USE_ESPHOME_VOIP_MWI)
#ifdef USE_ESPHOME_VOIP_MWI
#include "sip_transport.h"
#endif

namespace esphome::voip_stack {

MessageWaitingState VoipStack::get_message_waiting(const std::string &id) {
  MessageWaitingState state;
  state.account_id = id;

#ifdef USE_ESPHOME_VOIP_MWI
  if (this->transport_ != nullptr)
    static_cast<SipTransport *>(this->transport_.get())->get_message_waiting(id, &state);
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

void VoipStack::publish_message_waiting_(const MessageWaitingState &state) {
  this->defer([this, state]() {
    this->message_waiting_trigger_.trigger(state.account_id, state.available, state.waiting,
        state.new_messages, state.old_messages, state.urgent_new, state.urgent_old);
    this->message_waiting_callbacks_.call();
  });
}



}  // namespace esphome::voip_stack
#endif
