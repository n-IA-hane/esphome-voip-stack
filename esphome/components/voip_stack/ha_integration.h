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
    this->register_service(&VoipHAIntegration::start_call_via_ha, "start_call_via_ha", {"dest", "call_id"});
    this->register_service(&VoipHAIntegration::hangup_call_if_id, "hangup_call_if_id", {"call_id"});
    this->register_service(&VoipHAIntegration::answer_call, "answer_call");
    this->register_service(&VoipHAIntegration::decline_call, "decline_call", {"reason"});
    this->register_service(&VoipHAIntegration::hangup_call, "hangup_call");
#ifdef USE_ESPHOME_VOIP_DTMF
    this->register_service(&VoipHAIntegration::send_dtmf, "send_dtmf", {"digit"});
#endif
    this->register_service(&VoipHAIntegration::set_media_route, "set_media_route", {"call_id", "route"});
    this->register_service(&VoipHAIntegration::set_ha_peer_name, "set_ha_peer_name", {"name"});
#ifdef USE_VOIP_HA_LEGACY_ROSTER
    this->register_service(&VoipHAIntegration::set_contacts, "set_roster_json", {"roster_json"});
#endif
    this->register_service(&VoipHAIntegration::set_contacts, "set_contacts", {"contacts_csv"});
    this->register_service(&VoipHAIntegration::add_contact, "add_contact", {"entry"});
    this->register_service(&VoipHAIntegration::remove_contact, "remove_contact", {"entry"});
    this->register_service(&VoipHAIntegration::flush_contacts, "flush_contacts");
    this->register_service(&VoipHAIntegration::update_contacts, "update_contacts");
#ifdef USE_VOIP_HA_AUTO_REGISTER
    this->register_service(&VoipHAIntegration::set_ha_display_name, "set_ha_display_name", {"name"});
#endif
#ifdef USE_ESPHOME_VOIP_REGISTRATION
    this->register_service(&VoipHAIntegration::set_account, "set_account",
        {"account_id", "registrar", "port", "domain", "username", "password", "auth_username", "proxy", "proxy_port", "transport", "expires", "persist"});
    this->register_service(&VoipHAIntegration::remove_account, "remove_account", {"account_id", "persist"});
    this->register_service(&VoipHAIntegration::reset_account, "reset_account", {"account_id"});
    this->register_service(&VoipHAIntegration::register_account, "register_account", {"account_id"});
    this->register_service(&VoipHAIntegration::unregister_account, "unregister_account", {"account_id"});
    this->register_service(&VoipHAIntegration::select_line, "select_line", {"line"});
    this->register_service(&VoipHAIntegration::start_call_on_line, "start_call_on_line", {"dest", "line"});
#endif
    // ESPHome sends retained entity states to each new API subscriber.
    this->defer([this]() { this->parent_->publish_entity_states(); });
  }
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }
#ifdef USE_VOIP_HA_AUTO_REGISTER
  void set_ha_display_name(std::string name) { this->parent_->set_ha_display_name(name); }
#endif
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  void set_account(std::string id, std::string registrar, int32_t port, std::string domain, std::string username,
      std::string password, std::string auth_username, std::string proxy, int32_t proxy_port,
      std::string transport, int32_t expires, bool persist) {
    bool tcp = transport == "tcp";
#ifdef USE_VOIP_HA_AUTO_REGISTER
    if (transport == "auto" && id == "ha") {
      tcp = this->parent_->get_sip_transport_tcp();
      transport = tcp ? "tcp" : "udp";
    }
#endif
    if (port < 1 || port > 65535 || proxy_port < 1 || proxy_port > 65535 || expires < 30 || expires > 86400 ||
        (transport != "udp" && transport != "tcp")) return;
    SipAccountConfig c;
    c.id = id; c.registrar = registrar; c.port = port; c.domain = domain.empty() ? registrar : domain;
    c.username = username; c.password = password; c.auth_username = auth_username; c.proxy = proxy;
    c.proxy_port = proxy_port; c.tcp = tcp; c.expires = expires;
    this->parent_->set_account(c, persist);
  }
  void remove_account(std::string id, bool persist) { this->parent_->remove_account(id, persist); }
  void reset_account(std::string id) { this->parent_->reset_account(id); }
  void register_account(std::string id) { this->parent_->register_account(id); }
  void unregister_account(std::string id) { this->parent_->unregister_account(id); }
  void select_line(std::string id) { this->parent_->select_line(id); }
  void start_call_on_line(std::string dest, std::string line) { this->parent_->start_call_on_line(dest, line); }
#endif
  void start_call(std::string dest) { this->parent_->call(dest); }
  void start_call_via_ha(std::string dest, std::string call_id) { this->parent_->call_via_ha(dest, call_id); }
  void hangup_call_if_id(std::string call_id) { this->parent_->hangup_call_if_id(call_id); }
  void answer_call() { this->parent_->answer_call(); }
  void decline_call(std::string reason) { this->parent_->decline_call(reason); }
  void hangup_call() { this->parent_->stop(); }
#ifdef USE_ESPHOME_VOIP_DTMF
  void send_dtmf(std::string digit) {
    if (digit.size() == 1) this->parent_->send_dtmf(digit[0]);
  }
#endif
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
