#include "voip_stack.h"
#if defined(USE_ESP32) && defined(USE_ESPHOME_VOIP_REGISTRATION)
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <new>
#include <esp_memory_utils.h>
#include <cstring>

#include "sip_transport.h"
#include "sip_message.h"

namespace esphome::voip_stack {
void VoipStack::dump_registration_memory_() {
  ESP_LOGI("voip_stack.registration", "capacity=%u saved_bytes=%u saved_allocated=%s saved_psram=%s",
      static_cast<unsigned>(SipRegistration::CAPACITY), static_cast<unsigned>(sizeof(SavedRegistration)),
      YESNO(this->registration_saved_ != nullptr),
      YESNO(this->registration_saved_ && esp_ptr_external_ram(this->registration_saved_.get())));
#ifdef USE_VOIP_HA_AUTO_REGISTER
  if (this->transport_ != nullptr)
    ESP_LOGI("voip_stack.registration", "managed_retirement_status=%u",
        static_cast<unsigned>(static_cast<SipTransport *>(this->transport_.get())->get_managed_retirement_status()));
#endif
}
namespace {
template <size_t N>
void copy_setting(char (&target)[N], const std::string& value) {
  std::memset(target, 0, N);
  std::memcpy(target, value.data(), std::min(value.size(), N - 1));
}
}  // namespace

void VoipStack::add_registration_account(const std::string& id, const std::string& registrar, uint16_t port,
                                         const std::string& domain, const std::string& username,
                                         const std::string& password, const std::string& auth_username,
                                         const std::string& proxy, uint16_t proxy_port, bool tcp,
                                         uint32_t expires, bool enabled) {
  if (this->registration_baseline_count_ >= SipRegistration::CAPACITY) return;
  if (!this->registration_cpp_baseline_) {
    auto owned = std::make_unique<std::vector<SipAccountConfig>>();
    owned->reserve(SipRegistration::CAPACITY);
    for (size_t i = 0; i < this->registration_baseline_count_; ++i)
      owned->push_back(this->registration_baseline_[i].materialize());
    this->registration_cpp_baseline_ = std::move(owned);
    this->registration_baseline_ = nullptr;
  }
  this->registration_cpp_baseline_->emplace_back();
  ++this->registration_baseline_count_;
  auto& c = this->registration_cpp_baseline_->back();
  c.id = id;
  c.registrar = registrar;
  c.port = port;
  c.domain = domain;
  c.username = username;
  c.password = password;
  c.auth_username = auth_username;
  c.proxy = proxy;
  c.proxy_port = proxy_port;
  c.tcp = tcp;
  c.expires = expires;
  c.enabled = enabled;
}

void VoipStack::initialize_registration_() {
  auto* transport = static_cast<SipTransport*>(this->transport_.get());
  transport->initialize_registration();
  transport->set_accept_direct_calls(this->registration_accept_direct_);
  transport->set_registration_callback(registration_state_callback_, this, registration_main_ready_callback_);
#ifdef USE_ESPHOME_VOIP_BLF
  this->blf_ready_ = transport->configure_blf(this->blf_capacity_, blf_callback_);
  if (!this->blf_ready_) ESP_LOGE("voip_stack.blf", "Could not preallocate BLF subscription pool");
#endif
#ifdef USE_ESPHOME_VOIP_MWI
  transport->set_message_waiting_callback(message_waiting_callback_);
#endif
  if (!this->registration_saved_) {
#ifdef USE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM
    RAMAllocator<SavedRegistration> allocator(RAMAllocator<SavedRegistration>::ALLOC_EXTERNAL);
#else
    RAMAllocator<SavedRegistration> allocator(RAMAllocator<SavedRegistration>::ALLOC_INTERNAL);
#endif
    if (auto *memory = allocator.allocate(1)) this->registration_saved_.reset(new (memory) SavedRegistration());
  }
  if (!this->registration_saved_) {
    // A persistence allocation failure must not disable ordinary volatile SIP.
    for (size_t i = 0; i < this->registration_baseline_count_; ++i) transport->set_account(this->registration_baseline_at_(i));
    return;
  }
  if (this->registration_nvs_handle_ != 0) nvs_close(this->registration_nvs_handle_);
  if (nvs_open("voip_reg", NVS_READWRITE, &this->registration_nvs_handle_) != ESP_OK)
    this->registration_nvs_handle_ = 0;
  this->load_registration_saved_();
  for (size_t i = 0; i < this->registration_baseline_count_; ++i) {
    const auto base = this->registration_baseline_at_(i);
    bool overridden = false;
    for (const auto& item : this->registration_saved_->accounts)
      if (item.used && item.id[sizeof(item.id) - 1] == 0 && base.id == item.id) overridden = true;
    for (const auto& removed : this->registration_saved_->removed)
      if (removed.back() == 0 && base.id == removed.data()) overridden = true;
    if (!overridden) transport->set_account(base);
  }
  for (const auto& s : this->registration_saved_->accounts) {
    if (!s.used || s.removed || s.id[sizeof(s.id) - 1] || s.registrar[sizeof(s.registrar) - 1] ||
        s.domain[sizeof(s.domain) - 1] || s.username[sizeof(s.username) - 1] ||
        s.password[sizeof(s.password) - 1] || s.auth_username[sizeof(s.auth_username) - 1] ||
        s.proxy[sizeof(s.proxy) - 1])
      continue;
    SipAccountConfig c;
    c.id = s.id;
    c.registrar = s.registrar;
    c.domain = s.domain;
    c.username = s.username;
    c.password = s.password;
    c.auth_username = s.auth_username;
    c.proxy = s.proxy;
    c.port = s.port;
    c.proxy_port = s.proxy_port;
    c.tcp = s.tcp;
    c.expires = s.expires;
    c.enabled = s.enabled;
    if (c.valid()) transport->set_account(c);
  }
}

bool VoipStack::load_registration_saved_() {
  if (this->registration_nvs_handle_ == 0 || !this->registration_saved_) return false;
  size_t size = 0;
  if (nvs_get_blob(this->registration_nvs_handle_, "accounts", nullptr, &size) != ESP_OK) return false;
  const size_t header_size = offsetof(SavedRegistration, accounts);
  const size_t maximum = header_size + 4 * sizeof(SavedRegistrationAccount) + 4 * 33 + 3;
  if (size < header_size || size > maximum) return false;
#ifdef USE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM
  RAMAllocator<uint8_t> allocator(RAMAllocator<uint8_t>::ALLOC_EXTERNAL);
#else
  RAMAllocator<uint8_t> allocator(RAMAllocator<uint8_t>::ALLOC_INTERNAL);
#endif
  uint8_t* blob = allocator.allocate(size);
  if (blob == nullptr) return false;
  bool valid = nvs_get_blob(this->registration_nvs_handle_, "accounts", blob, &size) == ESP_OK;
  uint32_t version = 0;
  uint16_t count = 0, removed = 0;
  if (valid) {
    std::memcpy(&version, blob, sizeof(version));
    std::memcpy(&count, blob + sizeof(version), sizeof(count));
    std::memcpy(&removed, blob + sizeof(version) + sizeof(count), sizeof(removed));
    const size_t expected = header_size + count * sizeof(SavedRegistrationAccount) + removed * 33;
    valid = version == 2 && count <= 4 && removed <= 4 && size >= expected && size - expected <= 3;
  }
  size_t used = 0;
  for (size_t i = 0; valid && i < count; ++i) {
    SavedRegistrationAccount item;
    std::memcpy(&item, blob + header_size + i * sizeof(item), sizeof(item));
    if (!item.used) continue;
    valid = item.id[sizeof(item.id) - 1] == 0 && item.registrar[sizeof(item.registrar) - 1] == 0 &&
            item.domain[sizeof(item.domain) - 1] == 0 && item.username[sizeof(item.username) - 1] == 0 &&
            item.password[sizeof(item.password) - 1] == 0 &&
            item.auth_username[sizeof(item.auth_username) - 1] == 0 &&
            item.proxy[sizeof(item.proxy) - 1] == 0;
    if (valid && used < SipRegistration::CAPACITY) this->registration_saved_->accounts[used++] = item;
  }
  size_t removed_used = 0;
  for (size_t i = 0; valid && i < removed; ++i) {
    std::array<char, 33> name;
    std::memcpy(name.data(), blob + header_size + count * sizeof(SavedRegistrationAccount) + i * 33, 33);
    valid = name.back() == 0;
    if (valid && name[0] != 0 && removed_used < SipRegistration::CAPACITY)
      this->registration_saved_->removed[removed_used++] = name;
  }
  allocator.deallocate(blob, size);
  if (!valid) {
    for (auto& account : this->registration_saved_->accounts) account = {};
    for (auto& name : this->registration_saved_->removed) name.fill(0);
  }
  return valid;
}

bool VoipStack::write_registration_saved_(const SavedRegistration& next) {
  if (this->registration_nvs_handle_ == 0 || !this->registration_saved_) return false;
  // Dedicated namespace/key avoids the global preferences queue: another
  // component's failed flush cannot make this successfully stored key appear
  // uncommitted. Only the application context reaches this backend; managed
  // SIP work signals that owner before committing. Account preflight still
  // owns the transport mutex here.
  if (nvs_set_blob(this->registration_nvs_handle_, "accounts", &next, sizeof(next)) != ESP_OK) return false;
  if (nvs_commit(this->registration_nvs_handle_) == ESP_OK) return true;
  // Preserve the previous overlay when a backend reports a commit failure.
  nvs_set_blob(this->registration_nvs_handle_, "accounts", this->registration_saved_.get(),
               sizeof(*this->registration_saved_));
  nvs_commit(this->registration_nvs_handle_);
  return false;
}

bool VoipStack::save_registration_override_(const std::string& id, const SipAccountConfig* c, bool removed) {
  if (!this->registration_saved_) return false;
  SavedRegistration next = *this->registration_saved_;
  for (auto& name : next.removed)
    if (name.back() == 0 && id == name.data()) name.fill(0);
  SavedRegistrationAccount* slot = nullptr;
  for (auto& item : next.accounts)
    if (item.used && id == item.id) {
      item = {};
      slot = &item;
      break;
    }
  if (removed) {
    bool baseline = false;
    for (size_t i = 0; i < this->registration_baseline_count_; ++i)
      if (this->registration_baseline_matches_(i, id)) baseline = true;
    if (baseline) {
      bool found = false;
      for (auto& name : next.removed)
        if (name[0] == 0) {
          std::memcpy(name.data(), id.c_str(), id.size() + 1);
          found = true;
          break;
        }
      if (!found) return false;
    }
  } else {
    if (c == nullptr) return false;
    if (slot == nullptr)
      for (auto& item : next.accounts)
        if (!item.used) {
          slot = &item;
          break;
        }
    if (slot == nullptr) return false;
    *slot = {};
    slot->used = 1;
    copy_setting(slot->id, id);
    copy_setting(slot->registrar, c->registrar);
    copy_setting(slot->domain, c->domain);
    copy_setting(slot->username, c->username);
    copy_setting(slot->password, c->password);
    copy_setting(slot->auth_username, c->auth_username);
    copy_setting(slot->proxy, c->proxy);
    slot->port = c->port;
    slot->proxy_port = c->proxy_port;
    slot->tcp = c->tcp;
    slot->expires = c->expires;
    slot->enabled = c->enabled;
  }
  if (!this->write_registration_saved_(next)) return false;
  *this->registration_saved_ = next;
  return true;
}

bool VoipStack::set_account(const SipAccountConfig& config, bool persist) {
  if (this->transport_ == nullptr || !config.valid()) return false;
  SipTransport::AccountCommit commit = nullptr;
  if (persist)
    commit = [](void* ctx, const std::string& id, const SipAccountConfig* c) {
      return static_cast<VoipStack*>(ctx)->save_registration_override_(id, c, false);
    };
  const bool changed = static_cast<SipTransport*>(this->transport_.get())->set_account(config, commit, this);
  if (changed) this->publish_account_changed_(config.id);
  return changed;
}
bool VoipStack::remove_account(const std::string& id, bool persist) {
  if (this->transport_ == nullptr) return false;
  SipTransport::AccountCommit commit = nullptr;
  if (persist)
    commit = [](void* ctx, const std::string& id, const SipAccountConfig*) {
      return static_cast<VoipStack*>(ctx)->save_registration_override_(id, nullptr, true);
    };
  if (!static_cast<SipTransport*>(this->transport_.get())->remove_account(id, commit, this)) return false;
  if (this->default_line_ == id) this->default_line_ = "direct";
  this->publish_account_changed_(id);
  return true;
}
bool VoipStack::reset_account(const std::string& id) {
  if (this->transport_ == nullptr) return false;
  auto* transport = static_cast<SipTransport*>(this->transport_.get());
  SipAccountConfig baseline;
  bool has_baseline = false;
  for (size_t i = 0; i < this->registration_baseline_count_; ++i) {
    if (this->registration_baseline_matches_(i, id)) {
      baseline = this->registration_baseline_at_(i);
      has_baseline = true;
      break;
    }
  }
  SipTransport::AccountCommit commit = [](void* ctx, const std::string& id, const SipAccountConfig*) {
    auto* self = static_cast<VoipStack*>(ctx);
    if (!self->registration_saved_) return false;
    SavedRegistration next = *self->registration_saved_;
    for (auto& s : next.accounts)
      if (s.used && id == s.id) s = {};
    for (auto& name : next.removed)
      if (name.back() == 0 && id == name.data()) name.fill(0);
    if (!self->write_registration_saved_(next)) return false;
    *self->registration_saved_ = next;
    return true;
  };
  const bool changed = has_baseline ? transport->set_account(baseline, commit, this)
                                           : transport->remove_account(id, commit, this, true);
  if (!changed) return false;
  if (!has_baseline && this->default_line_ == id) this->default_line_ = "direct";
  this->publish_account_changed_(id);
  return true;
}
bool VoipStack::get_account_snapshot(const std::string &id, SipAccountSnapshot *out) {
  return this->transport_ != nullptr &&
         static_cast<SipTransport *>(this->transport_.get())->get_account_snapshot(id, out);
}
void VoipStack::publish_account_changed_(const std::string &id) {
  this->defer([this, id]() { this->account_changed_callbacks_.call(id); });
}
bool VoipStack::register_account(const std::string& id) {
  return this->transport_ != nullptr &&
         static_cast<SipTransport*>(this->transport_.get())->register_account(id, true);
}
bool VoipStack::unregister_account(const std::string& id) {
  return this->transport_ != nullptr &&
         static_cast<SipTransport*>(this->transport_.get())->register_account(id, false);
}
bool VoipStack::is_registered(const std::string& id) {
  return this->transport_ != nullptr && static_cast<SipTransport*>(this->transport_.get())->is_registered(id);
}

#ifdef USE_ESPHOME_VOIP_MWI
void VoipStack::message_waiting_callback_(void *ctx, const MessageWaitingState &state) {
  auto *self = static_cast<VoipStack *>(ctx);
  self->publish_message_waiting_(state);
}
#endif

bool VoipStack::select_line(const std::string& id) {
  // Selecting a line changes only the next originate decision, never the
  // identity, authentication context or transport of a running call.
  if (id != "direct" && !this->is_registered(id)) return false;
  this->default_line_ = id;
  return true;
}
void VoipStack::start_call_on_line(const std::string& target, const std::string& line) {
  if (!this->can_start_call_()) return;
  std::string cleaned = Phonebook::trim(target);
  if (cleaned.empty() || cleaned.size() > Phonebook::MAX_ROUTE_BYTES) return;
  for (unsigned char ch : cleaned) if (ch < 32 || ch == 127) return;
  std::string scheme = cleaned.substr(0, cleaned.find(':'));
  std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char ch) { return std::tolower(ch); });
  if (scheme == "sip" || scheme == "sips") {
    cleaned = sip_prefix_target_user(cleaned, "");
    if (cleaned.empty() || cleaned.size() > Phonebook::MAX_ROUTE_BYTES) return;
  } else if (cleaned.find_first_of("|,;\r\n") != std::string::npos || cleaned.find("://") != std::string::npos) return;
  if (line == "direct") {
    this->force_direct_call_ = true;
    this->call_(cleaned, {});
    this->force_direct_call_ = false;
    return;
  }
  if (!this->is_registered(line)) return;
  this->pending_line_ = line;
  this->pending_dialplan_target_ = cleaned;
  this->start_with_id_({});
  this->pending_line_.clear();
}
void VoipStack::registration_state_callback_(void* ctx, const std::string& id, RegistrationState state,
                                             uint16_t status) {
  auto* self = static_cast<VoipStack*>(ctx);
  self->defer([self, id, state, status]() {
    self->registration_state_trigger_.trigger(id, registration_state_name(state), status);
    self->account_changed_callbacks_.call(id);
#ifdef USE_ESPHOME_VOIP_BLF
    self->sync_blf_contacts_();
#endif
  });
}

void VoipStack::registration_main_ready_callback_(void* ctx, SipTransport* owner) {
  auto* self = static_cast<VoipStack*>(ctx);
  // DNS readiness and managed account commits share this main-context bridge.
  // ESPHome Components have application lifetime. The transport can be
  // replaced, so retain only its identity until the main task checks it.
  self->registration_main_owner_.store(owner, std::memory_order_release);
  self->enable_loop_soon_any_context();
}

void VoipStack::pump_registration_wake_() {
  if (this->registration_main_owner_.load(std::memory_order_acquire) == nullptr) return;
  auto* owner = this->registration_main_owner_.exchange(nullptr, std::memory_order_acq_rel);
  if (owner != nullptr && this->transport_.get() == owner) owner->wake_registration_from_main_loop();
}

}  // namespace esphome::voip_stack
#endif
