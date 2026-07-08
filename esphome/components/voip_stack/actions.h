#pragma once

// Action / Condition templates for voip_stack YAML automation.
// voip_stack.h re-includes this at the bottom; pragma once handles the loop.

#include "voip_stack.h"

#ifdef USE_ESP32

namespace esphome {
namespace voip_stack {

template<typename... Ts>
class NextContactAction : public Action<Ts...>, public Parented<VoipStack> {
 public:
  void play(const Ts &...x) override { this->parent_->next_contact(); }
};

template<typename... Ts>
class PrevContactAction : public Action<Ts...>, public Parented<VoipStack> {
 public:
  void play(const Ts &...x) override { this->parent_->prev_contact(); }
};

template<typename... Ts>
class StartAction : public Action<Ts...>, public Parented<VoipStack> {
 public:
  void play(const Ts &...x) override { this->parent_->start(); }
};
template<typename... Ts>
class StopAction : public Action<Ts...>, public Parented<VoipStack> {
 public:
  void play(const Ts &...x) override { this->parent_->stop(); }
};

template<typename... Ts>
class AnswerCallAction : public Action<Ts...>, public Parented<VoipStack> {
 public:
  void play(const Ts &...x) override { this->parent_->answer_call(); }
};

template<typename... Ts>
class DeclineCallAction : public Action<Ts...>, public Parented<VoipStack> {
 public:
  TEMPLATABLE_VALUE(std::string, reason)
  void play(const Ts &...x) override {
    this->parent_->decline_call(this->reason_.optional_value(x...).value_or(""));
  }
};

template<typename... Ts>
class SetVolumeAction : public Action<Ts...>, public Parented<VoipStack> {
 public:
  TEMPLATABLE_VALUE(float, volume)
  void play(const Ts &...x) override {
    this->parent_->set_volume(this->volume_.value(x...));
  }
};

template<typename... Ts>
class SetMicGainDbAction : public Action<Ts...>, public Parented<VoipStack> {
 public:
  TEMPLATABLE_VALUE(float, gain_db)
  void play(const Ts &...x) override {
    this->parent_->set_mic_gain_db(this->gain_db_.value(x...));
  }
};

template<typename... Ts>
class SetContactsAction : public Action<Ts...>, public Parented<VoipStack> {
 public:
  TEMPLATABLE_VALUE(std::string, contacts_csv)
  void play(const Ts &...x) override {
    this->parent_->set_contacts(this->contacts_csv_.value(x...));
  }
};

template<typename... Ts>
class SetContactAction : public Action<Ts...>, public Parented<VoipStack> {
 public:
  TEMPLATABLE_VALUE(std::string, contact)
  void play(const Ts &...x) override {
    this->parent_->set_contact(this->contact_.value(x...));
  }
};

template<typename... Ts>
class CallAction : public Action<Ts...>, public Parented<VoipStack> {
 public:
  TEMPLATABLE_VALUE(std::string, target)
  void play(const Ts &...x) override {
    this->parent_->call(this->target_.value(x...));
  }
};

template<typename... Ts>
class CallToggleAction : public Action<Ts...>, public Parented<VoipStack> {
 public:
  void play(const Ts &...x) override { this->parent_->call_toggle(); }
};

template<typename... Ts>
class FlushContactsAction : public Action<Ts...>, public Parented<VoipStack> {
 public:
  void play(const Ts &...x) override { this->parent_->flush_contacts(); }
};

template<typename... Ts>
class UpdateContactsAction : public Action<Ts...>, public Parented<VoipStack> {
 public:
  void play(const Ts &...x) override { this->parent_->update_contacts(); }
};

template<typename... Ts>
class AddContactAction : public Action<Ts...>, public Parented<VoipStack> {
 public:
  TEMPLATABLE_VALUE(std::string, entry)
  TEMPLATABLE_VALUE(std::string, name)
  TEMPLATABLE_VALUE(std::string, ip)
  TEMPLATABLE_VALUE(std::string, sip_transport)
  TEMPLATABLE_VALUE(uint16_t, port)
  TEMPLATABLE_VALUE(uint16_t, rtp_port)
  void play(const Ts &...x) override {
    const auto raw = this->entry_.optional_value(x...);
    if (raw.has_value() && !raw.value().empty()) {
      this->parent_->add_contact(raw.value());
      return;
    }

    const std::string name = this->name_.value(x...);
    const auto ip = this->ip_.optional_value(x...);
    if (!ip.has_value() || ip.value().empty()) {
      this->parent_->add_contact(name);
      return;
    }

    const auto explicit_transport = this->sip_transport_.optional_value(x...);
    const std::string transport =
        explicit_transport.has_value() && !explicit_transport.value().empty()
            ? explicit_transport.value()
            : this->parent_->configured_sip_transport_name();
    const std::string sip_transport = transport == "tcp" ? "sip_tcp" : "sip_udp";
    const uint16_t port = this->port_.optional_value(x...).value_or(5060);
    const uint16_t rtp_port = this->rtp_port_.optional_value(x...).value_or(40000);
    this->parent_->add_contact(name + "|" + ip.value() + "|" +
                               std::to_string(port) + "|" +
                               std::to_string(rtp_port) + "|" + sip_transport);
  }
};

template<typename... Ts>
class RemoveContactAction : public Action<Ts...>, public Parented<VoipStack> {
 public:
  TEMPLATABLE_VALUE(std::string, entry)
  void play(const Ts &...x) override {
    this->parent_->remove_contact(this->entry_.value(x...));
  }
};

template<typename... Ts>
class SetHaPeerNameAction : public Action<Ts...>, public Parented<VoipStack> {
 public:
  TEMPLATABLE_VALUE(std::string, name)
  void play(const Ts &...x) override {
    this->parent_->set_ha_peer_name(this->name_.value(x...));
  }
};

template<typename... Ts>
class SetRemoteEndpointAction : public Action<Ts...>, public Parented<VoipStack> {
 public:
  TEMPLATABLE_VALUE(std::string, ip)
  TEMPLATABLE_VALUE(uint16_t, port)
  TEMPLATABLE_VALUE(uint16_t, rtp_port)
  void play(const Ts &...x) override {
    this->parent_->set_remote_endpoint(
        this->ip_.value(x...), this->port_.value(x...),
        this->rtp_port_.value(x...));
  }
};

template<typename... Ts>
class PublishEntityStatesAction : public Action<Ts...>, public Parented<VoipStack> {
 public:
  void play(const Ts &...x) override { this->parent_->publish_entity_states(); }
};

template<typename... Ts>
class VoipIsIdleCondition : public Condition<Ts...>, public Parented<VoipStack> {
 public:
  bool check(const Ts &...x) override { return this->parent_->is_idle(); }
};

template<typename... Ts>
class VoipIsRingingCondition : public Condition<Ts...>, public Parented<VoipStack> {
 public:
  bool check(const Ts &...x) override { return this->parent_->is_ringing(); }
};

template<typename... Ts>
class VoipIsInCallCondition : public Condition<Ts...>, public Parented<VoipStack> {
 public:
  bool check(const Ts &...x) override { return this->parent_->is_in_call(); }
};

template<typename... Ts>
class VoipIsCallingCondition : public Condition<Ts...>, public Parented<VoipStack> {
 public:
  bool check(const Ts &...x) override { return this->parent_->get_call_state() == CallState::CALLING; }
};

template<typename... Ts>
class VoipIsIncomingCondition : public Condition<Ts...>, public Parented<VoipStack> {
 public:
  bool check(const Ts &...x) override {
    return this->parent_->get_call_state() == CallState::RINGING;
  }
};

template<typename... Ts>
class VoipDestinationIsCondition : public Condition<Ts...>, public Parented<VoipStack> {
 public:
  TEMPLATABLE_VALUE(std::string, destination)
  bool check(const Ts &...x) override {
    return this->parent_->get_current_destination() == this->destination_.value(x...);
  }
};

template<typename... Ts>
class VoipIsHaDestinationCondition : public Condition<Ts...>, public Parented<VoipStack> {
 public:
  bool check(const Ts &...x) override { return this->parent_->is_ha_destination(); }
};

}  // namespace voip_stack
}  // namespace esphome

#endif  // USE_ESP32
