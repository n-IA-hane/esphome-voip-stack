#pragma once

#include "esphome/components/phone_client/worker.h"
#include "esphome/components/voip_stack/voip_stack.h"
#include "directory_contact.h"
#include "esphome/core/automation.h"
#ifdef USE_LDAP_DIRECTORY_API
#include "esphome/components/api/custom_api_device.h"
#endif

namespace esphome::ldap_directory {

class LdapDirectory : public phone_client::WorkerComponent
#ifdef USE_LDAP_DIRECTORY_API
    , public api::CustomAPIDevice
#endif
{
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }
  void set_parent(voip_stack::VoipStack *parent) { this->parent_ = parent; }
  void set_account_id(const std::string &value) { this->account_id_ = value; }
  void set_server(const std::string &host, uint16_t port, bool tls);
  void set_directory(const std::string &base, const std::string &bind, const std::string &filter);
  void set_credentials(const std::string &username, const std::string &password);
  void set_ca_certificate(const std::string &value) { this->config_.ca_pem = value; }
  void set_timeout(uint32_t value) { this->timeout_ms_ = value; }
  void set_refresh_interval(uint32_t value) { this->refresh_ms_ = value; }
  void set_max_entries(uint8_t value) { this->max_entries_ = value; }
  void set_receive_buffer_size(size_t value) { this->receive_size_ = value; }
  void refresh();
  void refresh_if_stale();
  void configure(std::string host, int32_t port, std::string base_dn, std::string bind_dn,
                 std::string filter, bool tls);
  void configure_credentials(std::string username, std::string password);
  void configure_directory(std::string server, int32_t port, std::string base_dn, std::string bind_dn,
                           std::string account_id, bool tls, bool enabled);
  void set_managed_by_ha(bool value) { this->managed_by_ha_ = value; this->enabled_ = !value; }
  const char *status() const { return this->status_; }
  size_t contact_count() const { return this->contact_count_; }
  Trigger<uint32_t> *get_update_trigger() { return &this->updated_; }
  Trigger<uint32_t> *get_error_trigger() { return &this->error_; }
 protected:
  struct Configuration {
    std::string host, base_dn, bind_dn, filter{"(|(telephoneNumber=*)(labeledURI=*))"}, username, password, ca_pem;
    std::string sip_host;
    uint16_t port{389}, sip_port{5060};
    bool tls{false}, sip_tcp{false}, credentials_ready{false};
  } config_;
  void worker_run_() override;
  void account_changed_();
  void reset_stage_();
  voip_stack::VoipStack *parent_{nullptr};
  std::string account_id_;
  uint32_t account_revision_{0}, timeout_ms_{5000}, refresh_ms_{300000}, last_success_{0};
  uint8_t max_entries_{64};
  char *receive_buffer_{nullptr};
  size_t receive_size_{8192};
  bool ready_{false}, active_{false}, pending_result_{false}, account_loaded_{false};
  bool managed_by_ha_{false}, enabled_{true};
  uint32_t result_generation_{0}, result_code_{0};
  voip_stack::ContactEntry *stage_{nullptr};
  size_t stage_count_{0}, stage_capacity_{0};
  size_t contact_count_{0};
  const char *status_{"unconfigured"};
  Trigger<uint32_t> updated_, error_;
};

template<typename... Ts> class RefreshAction : public Action<Ts...>, public Parented<LdapDirectory> {
 public:
  void play(const Ts &...x) override { this->parent_->refresh(); }
};
}  // namespace esphome::ldap_directory
