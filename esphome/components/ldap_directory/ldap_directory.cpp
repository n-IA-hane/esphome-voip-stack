#include "ldap_directory.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include <esp_heap_caps.h>
#include <new>

namespace esphome::ldap_directory {
static const char *const TAG = "ldap_directory";

void LdapDirectory::set_server(const std::string &host, uint16_t port, bool tls) {
  this->lock_(); this->config_.host = host; this->config_.port = port; this->config_.tls = tls; this->unlock_();
}
void LdapDirectory::set_directory(const std::string &base, const std::string &bind, const std::string &filter) {
  this->lock_(); this->config_.base_dn = base; this->config_.bind_dn = bind; this->config_.filter = filter; this->unlock_();
}
void LdapDirectory::set_credentials(const std::string &username, const std::string &password) {
  this->lock_(); this->config_.username = username; this->config_.password = password;
  this->config_.credentials_ready = !username.empty() && !password.empty(); this->unlock_();
}

void LdapDirectory::setup() {
  if (this->parent_ == nullptr) { this->mark_failed(); return; }
  if (this->managed_by_ha_) this->parent_->set_ha_roster_enabled(false);
  this->receive_buffer_ = static_cast<char *>(allocate_(this->receive_size_, this->buffers_psram_));
  if (this->receive_buffer_ == nullptr) { this->mark_failed(); return; }
  this->stage_capacity_ = this->max_entries_;
  this->stage_ = static_cast<voip_stack::ContactEntry *>(allocate_(
      this->stage_capacity_ * sizeof(voip_stack::ContactEntry), this->buffers_psram_));
  if (this->stage_ == nullptr) {
    heap_caps_free(this->receive_buffer_); this->receive_buffer_ = nullptr;
    this->mark_failed(); return;
  }
  for (size_t i = 0; i < this->stage_capacity_; ++i) new (&this->stage_[i]) voip_stack::ContactEntry();
  if (!this->setup_worker_("ldap_directory")) {
    for (size_t i = 0; i < this->stage_capacity_; ++i) this->stage_[i].~ContactEntry();
    heap_caps_free(this->stage_); this->stage_ = nullptr; this->stage_capacity_ = 0;
    heap_caps_free(this->receive_buffer_); this->receive_buffer_ = nullptr;
    return;
  }
#ifdef USE_LDAP_DIRECTORY_API
  if (this->managed_by_ha_)
    this->register_service(&LdapDirectory::configure_directory, "configure_directory",
        {"server", "port", "base_dn", "bind_dn", "account_id", "tls", "enabled"});
  else
    this->register_service(&LdapDirectory::configure, "configure_ldap_directory",
        {"host", "port", "base_dn", "bind_dn", "filter", "tls"});
  this->register_service(&LdapDirectory::refresh, "refresh_ldap_directory");
  if (this->account_id_.empty())
    this->register_service(&LdapDirectory::configure_credentials, "configure_ldap_credentials", {"username", "password"});
#endif
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  if (!this->account_id_.empty()) {
    this->parent_->add_on_account_changed_callback([this](const std::string &id) {
      if (id == this->account_id_) this->account_changed_();
    });
    this->account_changed_();
  }
#endif
  this->ready_ = true;
  this->refresh();
}

void LdapDirectory::configure_credentials(std::string username, std::string password) {
  if (!this->account_id_.empty() || username.empty() || username.size() > 1024 || password.empty() || password.size() > 256)
    return;
  this->set_credentials(username, password);
  this->interrupt_worker_();
  this->refresh();
}

void LdapDirectory::account_changed_() {
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  voip_stack::SipAccountSnapshot snapshot;
  if (!this->parent_->get_account_snapshot(this->account_id_, &snapshot)) {
    this->lock_(); this->config_.credentials_ready = false; this->active_ = false; this->unlock_();
    this->interrupt_worker_();
    this->parent_->commit_contact_directory(this, {}, this->account_id_);
    this->account_loaded_ = false;
    this->contact_count_ = 0;
    this->last_success_ = 0;
    this->status_ = "unconfigured";
    return;
  }
  if (this->account_loaded_ && snapshot.revision == this->account_revision_) {
    if (snapshot.registered && this->status_ == std::string("error")) this->refresh_if_stale();
    return;
  }
  this->account_revision_ = snapshot.revision;
  this->account_loaded_ = true;
  this->lock_();
  this->config_.username = snapshot.config.username;
  this->config_.password = snapshot.config.password;
  this->config_.sip_host = snapshot.config.registrar;
  this->config_.sip_port = snapshot.config.port;
  this->config_.sip_tcp = snapshot.config.tcp;
  this->config_.credentials_ready = true;
  this->unlock_();
  this->interrupt_worker_();
  if (this->ready_) this->refresh();
#endif
}

void LdapDirectory::configure(std::string host, int32_t port, std::string base, std::string bind,
                              std::string filter, bool tls) {
#ifndef USE_PHONE_CLIENT_TLS
  if (tls) { ESP_LOGW(TAG, "TLS support is not included in this firmware"); return; }
#endif
  if (host.size() > 253 || base.size() > 1024 || bind.size() > 1024 || port < 1 || port > 65535 ||
      host.find_first_of("\r\n\0", 0, 3) != std::string::npos || !valid_filter(filter)) {
    ESP_LOGW(TAG, "Rejected invalid LDAP configuration"); return;
  }
  this->set_server(host, static_cast<uint16_t>(port), tls);
  this->set_directory(base, bind, filter);
  this->interrupt_worker_();
  this->refresh();
}

void LdapDirectory::configure_directory(std::string server, int32_t port, std::string base, std::string bind,
                                       std::string account, bool tls, bool enabled) {
  if (!this->managed_by_ha_ || account != "ha" || account != this->account_id_) return;
  if (!enabled) {
    this->lock_(); this->enabled_ = false; this->active_ = false; this->unlock_();
    this->cancel_timeout("refresh");
    this->interrupt_worker_();
    this->parent_->commit_contact_directory(this, {}, this->account_id_);
    this->contact_count_ = 0; this->status_ = "disabled";
    return;
  }
  if (server.empty() || server.size() > 253 || base.empty() || base.size() > 1024 || bind.size() > 1024 ||
      port < 1 || port > 65535 || server.find_first_of("\r\n\0", 0, 3) != std::string::npos) return;
#ifndef USE_PHONE_CLIENT_TLS
  if (tls) { ESP_LOGW(TAG, "TLS support is not included in this firmware"); return; }
#endif
  this->lock_();
  const bool same = this->enabled_ && this->config_.host == server && this->config_.port == port &&
      this->config_.base_dn == base && this->config_.bind_dn == bind && this->config_.tls == tls;
  this->unlock_();
  if (same) return;
  this->set_server(server, static_cast<uint16_t>(port), tls);
  this->set_directory(base, bind, "(|(telephoneNumber=*)(labeledURI=*))");
  this->lock_(); this->enabled_ = true; this->unlock_();
  this->interrupt_worker_();
  this->refresh();
}

void LdapDirectory::refresh() {
  if (!this->ready_ || this->is_failed() || !this->enabled_) return;
  this->cancel_timeout("refresh");
  this->lock_();
  const bool configured = this->config_.credentials_ready && !this->config_.base_dn.empty() &&
      (!this->config_.host.empty() || !this->config_.sip_host.empty());
  if (configured) this->active_ = true;
  this->unlock_();
  if (!configured) { this->status_ = "unconfigured"; return; }
  this->status_ = "updating";
  this->wake_worker_();
}

void LdapDirectory::refresh_if_stale() {
  if (this->status_ != std::string("updating") &&
      (this->last_success_ == 0 || millis() - this->last_success_ >= this->refresh_ms_)) this->refresh();
}

void LdapDirectory::worker_run_() {
  this->lock_();
  // The main loop owns a completed stage until its atomic commit finishes.
  // Keep any later refresh intent active; the consumer wakes us afterwards.
  if (this->pending_result_) { this->unlock_(); return; }
  if (!this->active_ || !this->enabled_ || !this->config_.credentials_ready) { this->unlock_(); return; }
  this->active_ = false;
  this->reset_stage_();
  Configuration cfg = this->config_;
  const uint32_t generation = this->generation_.load();
  this->unlock_();
  const std::string host = cfg.host.empty() ? cfg.sip_host : cfg.host;
  bool ok = this->stream_.connect(host, cfg.port, this->timeout_ms_, cfg.tls, cfg.ca_pem) &&
      generation == this->generation_.load();
  Client client(this->stream_);
  client.set_receive_buffer(this->receive_buffer_, this->receive_size_);
  if (ok) {
    std::string dn = cfg.bind_dn.empty() ? cfg.username : cfg.bind_dn;
    const std::string token = "{username}";
    const size_t position = dn.find(token);
    if (position != std::string::npos) dn.replace(position, token.size(), escape_dn_value(cfg.username));
    ok = client.bind(dn, cfg.password, this->timeout_ms_) && generation == this->generation_.load();
    Search search;
    search.base_dn = cfg.base_dn; search.filter = cfg.filter; search.size_limit = this->max_entries_;
    ok = ok && client.search_each(search, [this, &cfg, generation](Entry &&entry) {
      if (generation != this->generation_.load() || this->stage_count_ == this->stage_capacity_) return false;
      // Subtree searches may include the organizational-unit container.
      if (entry.first("labeledURI").empty() && entry.first("telephoneNumber").empty()) return true;
      DirectoryContact mapped;
      if (!map_contact(entry, cfg.sip_host, cfg.sip_port, cfg.sip_tcp, &mapped)) return false;
      auto &contact = this->stage_[this->stage_count_];
      contact.id = std::move(mapped.id); contact.name = std::move(mapped.name);
      contact.route = mapped.uri.empty() ? std::move(mapped.user) : std::move(mapped.uri);
      contact.ip = std::move(mapped.host); contact.port = mapped.port; contact.sip_transport_tcp = mapped.tcp;
      contact.endpoint_type = voip_stack::ContactEndpointType::SIP;
      const std::string &kind = mapped.kind;
      contact.kind = kind == "esphome" ? voip_stack::ContactKind::ESP : kind == "sip_account" ? voip_stack::ContactKind::SIP_ACCOUNT :
          kind == "browser" ? voip_stack::ContactKind::BROWSER : kind == "automation" ? voip_stack::ContactKind::AUTOMATION :
          kind == "group" ? voip_stack::ContactKind::GROUP : voip_stack::ContactKind::CONTACT;
      ++this->stage_count_;
      return true;
    }, this->timeout_ms_) && generation == this->generation_.load();
  }
  if (ok) client.unbind(500);
  this->stream_.close();
  this->lock_();
  if (generation == this->generation_.load()) {
    this->result_generation_ = generation;
    this->result_code_ = ok ? 0 : (client.result_code() == 0 ? 2 : client.result_code());
    this->pending_result_ = true;
  } else this->reset_stage_();
  this->unlock_();
  this->notify_main_();
}

void LdapDirectory::reset_stage_() {
  for (size_t i = 0; i < this->stage_count_; ++i) {
    this->stage_[i].~ContactEntry();
    new (&this->stage_[i]) voip_stack::ContactEntry();
  }
  this->stage_count_ = 0;
}

void LdapDirectory::loop() {
  this->disable_loop();
  this->lock_();
  if (!this->pending_result_) { this->unlock_(); return; }
  const auto generation = this->result_generation_, code = this->result_code_;
  const size_t count = this->stage_count_;
  this->unlock_();
  uint32_t error = code;
  if (generation == this->generation_.load() && error == 0) {
    // No LDAP lock across the core callback: consumers may reconfigure or
    // request another refresh. The pending flag keeps the worker off stage.
    const auto committed = this->parent_->commit_contact_directory(this, this->stage_, count, this->account_id_);
    if (committed != voip_stack::DirectoryCommitResult::APPLIED && committed != voip_stack::DirectoryCommitResult::UNCHANGED) error = 80;
  }
  this->lock_();
  this->reset_stage_();
  this->pending_result_ = false;
  const bool again = this->active_;
  this->unlock_();
  if (again) this->wake_worker_();
  if (generation != this->generation_.load()) return;
  // Install the expiry before user triggers so a reentrant reconfiguration
  // can cancel it rather than inheriting the previous generation's timer.
  this->Component::set_timeout("refresh", error == 0 ? this->refresh_ms_ : std::min<uint32_t>(this->refresh_ms_, 30000),
                               [this]() { this->refresh(); });
  if (error == 0) {
    this->status_ = "ready"; this->last_success_ = millis(); this->contact_count_ = count;
    this->status_clear_warning(); this->updated_.trigger(static_cast<uint32_t>(count));
  } else {
    this->status_ = "error"; this->status_set_warning(); this->error_.trigger(error);
    ESP_LOGW(TAG, "Directory update failed (code=%u); previous directory retained", static_cast<unsigned>(error));
  }
}

void LdapDirectory::dump_config() {
  ESP_LOGCONFIG(TAG, "LDAP directory: %s, port %u, TLS %s, capacity %u", this->status_, this->config_.port,
                this->config_.tls ? "on" : "off", this->max_entries_);
}
}  // namespace esphome::ldap_directory
