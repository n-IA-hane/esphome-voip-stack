#include "voip_stack.h"

#if defined(USE_ESP32) && (defined(USE_ESPHOME_VOIP_MWI) || defined(USE_ESPHOME_VOIP_HA_MWI))
#ifdef USE_ESPHOME_VOIP_MWI
#include "sip_transport.h"
#endif

namespace esphome::voip_stack {

MessageWaitingState VoipStack::get_message_waiting(const std::string &id) {
  MessageWaitingState state;
  state.account_id = id;
#ifdef USE_ESPHOME_VOIP_HA_MWI
  if (id == "direct") {
    state.available = this->ha_message_waiting_available_;
    state.waiting = this->ha_message_waiting_;
    state.new_messages = this->ha_new_messages_;
    state.old_messages = this->ha_old_messages_;
    return state;
  }
#endif
#ifdef USE_ESPHOME_VOIP_MWI
  if (this->transport_ != nullptr)
    static_cast<SipTransport *>(this->transport_.get())->get_message_waiting(id, &state);
#endif
  return state;
}

bool VoipStack::has_waiting_messages(const std::string &id) {
#ifdef USE_ESPHOME_VOIP_HA_MWI
  if ((id.empty() || id == "direct") && this->ha_message_waiting_available_ && this->ha_message_waiting_)
    return true;
#endif
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

#ifdef USE_ESPHOME_VOIP_HA_MWI
void VoipStack::set_ha_message_waiting(bool available, bool waiting, uint32_t new_messages, uint32_t old_messages) {
  waiting = available && waiting;
  if (!available) new_messages = old_messages = 0;
  if (this->ha_message_waiting_available_ == available && this->ha_message_waiting_ == waiting &&
      this->ha_new_messages_ == new_messages && this->ha_old_messages_ == old_messages)
    return;
  this->ha_message_waiting_available_ = available;
  this->ha_message_waiting_ = waiting;
  this->ha_new_messages_ = new_messages;
  this->ha_old_messages_ = old_messages;
  this->publish_message_waiting_(this->get_message_waiting("direct"));
}
#endif

}  // namespace esphome::voip_stack
#endif
