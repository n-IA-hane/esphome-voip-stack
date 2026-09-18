#pragma once
#ifdef USE_VOIP_HA_INTEGRATION
#include "esphome/components/api/custom_api_device.h"
#include "esphome/core/component.h"
#include "voip_stack.h"

namespace esphome::voip_stack {
// Native API adapter only. The parent retains all call and phonebook ownership.
class VoipHAIntegration : public Component, public api::CustomAPIDevice, public Parented<VoipStack> {
 public:
  void setup() override {
    this->register_service(&VoipHAIntegration::start_call, "start_call", {"dest"});
    this->register_service(&VoipHAIntegration::answer_call, "answer_call");
    this->register_service(&VoipHAIntegration::decline_call, "decline_call", {"reason"});
    this->register_service(&VoipHAIntegration::hangup_call, "hangup_call");
    this->register_service(&VoipHAIntegration::set_media_route, "set_media_route", {"call_id", "route"});
    this->register_service(&VoipHAIntegration::set_ha_peer_name, "set_ha_peer_name", {"name"});
    this->register_service(&VoipHAIntegration::set_contacts, "set_roster_json", {"roster_json"});
    this->register_service(&VoipHAIntegration::set_contacts, "set_contacts", {"contacts_csv"});
    this->register_service(&VoipHAIntegration::add_contact, "add_contact", {"entry"});
    this->register_service(&VoipHAIntegration::remove_contact, "remove_contact", {"entry"});
    this->register_service(&VoipHAIntegration::flush_contacts, "flush_contacts");
    this->register_service(&VoipHAIntegration::update_contacts, "update_contacts");
    // ESPHome sends retained entity states to each new API subscriber.
    this->defer([this]() { this->parent_->publish_entity_states(); });
  }
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }
  void start_call(std::string dest) { this->parent_->call(dest); }
  void answer_call() { this->parent_->answer_call(); }
  void decline_call(std::string reason) { this->parent_->decline_call(reason); }
  void hangup_call() { this->parent_->stop(); }
  void set_media_route(std::string call_id, std::string route) { this->parent_->set_media_route(call_id, route); }
  void set_ha_peer_name(std::string name) { this->parent_->set_ha_peer_name(name); }
  void set_contacts(std::string contacts) { this->parent_->set_contacts(contacts); }
  void add_contact(std::string entry) { this->parent_->add_contact(entry); }
  void remove_contact(std::string entry) { this->parent_->remove_contact(entry); }
  void flush_contacts() { this->parent_->flush_contacts(); }
  void update_contacts() { this->parent_->update_contacts(); }
};
}  // namespace esphome::voip_stack
#endif
