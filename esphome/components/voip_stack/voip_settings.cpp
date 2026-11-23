#include "voip_stack.h"

#ifdef USE_ESP32

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "sip_message.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace voip_stack {

bool VoipStack::can_send_dtmf() const {
  return this->call_state_.load(std::memory_order_acquire) == CallState::IN_CALL &&
      this->transport_ != nullptr && this->transport_->can_send_dtmf();
}
bool VoipStack::send_dtmf(char digit, uint16_t duration_ms) {
  return this->can_send_dtmf() && this->transport_->send_dtmf(digit, duration_ms);
}


static const char *const TAG = "voip_stack.settings";

namespace {

struct ParsedPhonebookSlot {
  ContactEntry entry;
};

float db_to_linear(float db) {
  return std::pow(10.0f, db / 20.0f);
}

float clamp_volume_(float volume) {
  if (!std::isfinite(volume)) return 0.0f;
  return std::max(0.0f, std::min(1.0f, volume));
}

float clamp_mic_gain_db_(float db) {
  if (!std::isfinite(db)) return 0.0f;
  return std::max(-20.0f, std::min(20.0f, db));
}

bool parse_slot_for_normalize(const std::string &raw, ParsedPhonebookSlot *slot) {
  if (!Phonebook::parse_entry(raw, &slot->entry)) return false;
  return slot->entry.endpoint_type == ContactEndpointType::UNKNOWN ||
         slot->entry.endpoint_type == ContactEndpointType::SIP;
}

void append_csv(std::string *out, const std::string &entry) {
  if (entry.empty()) return;
  if (!out->empty()) out->push_back(',');
  *out += entry;
}

std::string serialize_endpoint(std::string_view name, std::string_view route,
                               ContactEndpointType endpoint_type,
                               std::string_view ip, uint16_t port,
                               uint16_t rtp_port, bool sip_transport_tcp = false) {
  if (name.empty()) return "";
  if (ip.empty() || port == 0) return std::string(name);
  if (endpoint_type == ContactEndpointType::SIP) {
    std::string result;
    result.reserve(name.size() + route.size() + ip.size() + 24);
    result.append(name.data(), name.size());
    if (!route.empty() && route != name) { result.push_back('|'); result.append(route.data(), route.size()); }
    result.push_back('|'); result.append(ip.data(), ip.size());
    result += "|" + std::to_string(port) + "|" + std::to_string(rtp_port) + "|" + (sip_transport_tcp ? "sip_tcp" : "sip_udp");
    return result;
  }
  return std::string(name);
}

}  // namespace

// === Settings persistence ===

void VoipStack::load_settings_() {
  this->settings_pref_ = global_preferences->make_preference<StoredSettings>(fnv1_hash("voip_stack_settings"));

  StoredSettings stored;
  if (this->settings_pref_.load(&stored) && stored.version == SETTINGS_VERSION) {
    this->suppress_save_ = true;

    // Volume only when voip_stack owns the master_volume number;
    // template-number setups own their own DAC + AEC sync.
    const float volume = clamp_volume_(stored.volume_pct / 100.0f);
#ifdef USE_NUMBER
#ifdef USE_ESPHOME_VOIP_STACK_MASTER_ALIAS
    if (this->has_master_volume_control_()) {
      ESP_LOGD(TAG, "Audio master owns volume; ignored saved Native volume");
    } else
#endif
    if (this->volume_number_ != nullptr) {
      this->volume_.store(volume, std::memory_order_relaxed);
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
      if (this->speaker_ != nullptr) {
        this->speaker_->set_volume(volume);
      }
#endif
      ESP_LOGD(TAG, "Loaded volume: %.0f%%", volume * 100.0f);
    } else if (volume != 1.0f) {
      ESP_LOGI(TAG, "External master owns output; ignored legacy VoIP RX volume %.0f%%", volume * 100.0f);
    }
#else
    (void) volume;
#endif

    // Skip mic_gain when esp_audio_stack owns it (its own persistence).
    this->mic_gain_db_ = clamp_mic_gain_db_(stored.mic_gain_db);
#ifdef USE_NUMBER
    if (this->mic_gain_number_ != nullptr && this->has_microphone_()) {
      if (this->mic_gain_db_ != 0.0f && !this->ensure_mic_processing_buffer_()) {
        ESP_LOGE(TAG, "Stored mic_gain %.1fdB ignored: processing buffer unavailable", this->mic_gain_db_);
        this->mic_gain_db_ = 0.0f;
      }
      this->mic_gain_.store(db_to_linear(this->mic_gain_db_), std::memory_order_relaxed);
      ESP_LOGD(TAG, "Loaded mic_gain: %.1fdB", this->mic_gain_db_);
    }
#endif

    // auto_answer / AEC use switch restore_mode, not this struct.
    this->suppress_save_ = false;
  } else {
    ESP_LOGD(TAG, "No saved settings, using defaults");
  }
}

void VoipStack::schedule_save_settings_() {
  if (this->suppress_save_ || this->save_scheduled_) {
    return;
  }
  this->save_scheduled_ = true;
  // 250 ms debounce against slider drag.
  this->set_timeout(SCHED_SAVE_SETTINGS, 250, [this]() {
    this->save_scheduled_ = false;
    this->save_settings_();
  });
}

void VoipStack::save_settings_() {
  StoredSettings stored;
  stored.version = SETTINGS_VERSION;
  const float volume = clamp_volume_(this->volume_.load(std::memory_order_relaxed));
  stored.volume_pct = static_cast<uint8_t>(
      std::lround(volume * 100.0f));
  stored.mic_gain_db = static_cast<int8_t>(std::lround(clamp_mic_gain_db_(this->mic_gain_db_)));

  this->settings_pref_.save(&stored);
  ESP_LOGD(TAG, "Saved settings: vol=%d%%, mic=%ddB",
           stored.volume_pct, stored.mic_gain_db);
}

void VoipStack::load_routing_settings_() {
  this->routing_settings_pref_ =
      global_preferences->make_preference<StoredRoutingSettings>(fnv1_hash("voip_stack_routing_settings"));
  StoredRoutingSettings stored;
  if (this->routing_settings_pref_.load(&stored) && stored.version == ROUTING_SETTINGS_VERSION) {
    stored.extension[MAX_EXTENSION_BYTES] = '\0';
    stored.ring_groups[MAX_GROUP_LIST_BYTES] = '\0';
    stored.conference_groups[MAX_GROUP_LIST_BYTES] = '\0';
    this->set_extension(stored.extension);
    this->set_ring_groups(stored.ring_groups);
    this->set_conference_groups(stored.conference_groups);
    ESP_LOGD(TAG, "Loaded routing identity and groups");
  }
#ifdef USE_ESPHOME_VOIP_STACK_QUEUES
  this->queues_settings_pref_ =
      global_preferences->make_preference<StoredQueuesSettings>(fnv1_hash("voip_stack_queues_settings"));
  StoredQueuesSettings queues;
  if (this->queues_settings_pref_.load(&queues)) {
    queues.queues[MAX_GROUP_LIST_BYTES] = '\0';
    this->set_queues(queues.queues);
  }
#endif
  this->routing_settings_loaded_ = true;
}

void VoipStack::schedule_save_routing_settings_() {
  if (!this->routing_settings_loaded_ || this->routing_save_scheduled_) return;
  this->routing_save_scheduled_ = true;
  this->set_timeout("save_routing_settings", 250, [this]() {
    this->routing_save_scheduled_ = false;
    this->save_routing_settings_();
  });
}

void VoipStack::save_routing_settings_() {
  StoredRoutingSettings stored;
  auto copy_bounded = [](char *dest, size_t capacity, const std::string &source) {
    const size_t bytes = std::min(capacity - 1, source.size());
    std::memcpy(dest, source.data(), bytes);
    dest[bytes] = '\0';
  };
  copy_bounded(stored.extension, sizeof(stored.extension), this->extension_);
  copy_bounded(stored.ring_groups, sizeof(stored.ring_groups), this->ring_groups_);
  copy_bounded(stored.conference_groups, sizeof(stored.conference_groups), this->conference_groups_);
  this->routing_settings_pref_.save(&stored);
#ifdef USE_ESPHOME_VOIP_STACK_QUEUES
  StoredQueuesSettings queues;
  copy_bounded(queues.queues, sizeof(queues.queues), this->queues_);
  this->queues_settings_pref_.save(&queues);
#endif
  ESP_LOGD(TAG, "Saved routing identity and groups");
}

// === User-facing setters ===

void VoipStack::set_volume(float volume) {
  volume = clamp_volume_(volume);
#ifdef USE_ESPHOME_VOIP_STACK_MASTER_ALIAS
  if (this->has_master_volume_control_()) {
#ifdef USE_ESPHOME_VOIP_STACK_MASTER_NUMBER_ALIAS
    if (this->master_volume_control_.number != nullptr) {
      auto call = this->master_volume_control_.number->make_call();
      call.set_value(volume * 100.0f);
      call.perform();
      return;
    }
#endif
    this->master_volume_control_.set(this->master_volume_control_.owner, volume);
    return;
  }
#endif
  this->volume_.store(volume, std::memory_order_relaxed);
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
  if (this->speaker_ != nullptr) {
    this->speaker_->set_volume(volume);
  }
#endif
  this->schedule_save_settings_();
}

#ifdef USE_ESPHOME_VOIP_STACK_MASTER_ALIAS
void VoipStack::sync_master_volume_() {
#ifdef USE_NUMBER
  if (this->has_master_volume_control_() && this->volume_number_ != nullptr) {
    const float percent = this->get_volume_control_percent();
    if (!this->volume_number_->has_state() || this->volume_number_->state != percent)
      this->volume_number_->publish_state(percent);
  }
#endif
}
#endif

void VoipStack::set_auto_answer(bool enabled) {
  this->auto_answer_ = enabled;
  ESP_LOGI(TAG, "Auto-answer set to %s", enabled ? "ON" : "OFF");
  // Persisted via switch restore_mode, not save_settings_.
}

void VoipStack::set_do_not_disturb(bool enabled) {
  this->do_not_disturb_ = enabled;
  ESP_LOGI(TAG, "Do-not-disturb set to %s", enabled ? "ON" : "OFF");
  // Persisted via switch restore_mode, not save_settings_.
}

void VoipStack::set_mic_gain_db(float db) {
  if (!this->has_microphone_()) {
    ESP_LOGW(TAG, "Ignoring mic_gain: this voip endpoint has no microphone");
    return;
  }
  // gain = 10^(dB/20); clamp to -20..+20 dB.
  db = clamp_mic_gain_db_(db);
  if (db != 0.0f && !this->ensure_mic_processing_buffer_()) {
    ESP_LOGE(TAG, "Mic gain %.1f dB ignored: processing buffer unavailable", db);
    return;
  }
  this->mic_gain_db_ = db;
  const float gain = db_to_linear(db);
  this->mic_gain_.store(gain, std::memory_order_relaxed);
  ESP_LOGD(TAG, "Mic gain set to %.1f dB (%.2fx)", db, gain);
  this->schedule_save_settings_();
}

// === Contacts ===
// Phonebook owns merge logic. add/set route through the same idempotent
// merge: same shape = noop, missing endpoint = upgrade, mismatch = replace.
// Slot order is stable; only flush_contacts wipes.

void VoipStack::add_contact(const std::string &entry, const std::string &id, ContactKind kind,
                            const std::string &account, bool auto_blf) {
  const std::string normalized_entry = this->normalize_phonebook_for_transport_(entry);
  ContactEntry contact;
  if (!Phonebook::parse_entry(normalized_entry, &contact)) return;
#ifdef USE_ESPHOME_VOIP_CONTACT_METADATA
  if ((!id.empty() && !sip_valid_contact_identity(id)) || !sip_valid_text(account, 32)) return;
  contact.id = id; contact.kind = kind; contact.account_id = account; contact.auto_blf = auto_blf;
#else
  if (!id.empty() || !account.empty() || auto_blf || kind != ContactKind::UNKNOWN) return;
#endif
  const AddResult r = this->phonebook_.add_entry(contact);
  switch (r) {
    case AddResult::Added:
      ESP_LOGI(TAG, "Contact added: %s", normalized_entry.c_str());
      this->publish_phonebook_changed_();
      break;
    case AddResult::Upgraded:
    case AddResult::EndpointReplaced:
      ESP_LOGI(TAG, "Contact endpoint updated: %s", normalized_entry.c_str());
      this->publish_phonebook_changed_();
      break;
    case AddResult::Noop:
    case AddResult::Rejected:
      break;
  }
}

void VoipStack::remove_contact(const std::string &name) {
  if (!this->phonebook_.remove_one(name)) {
    return;
  }
  ESP_LOGI(TAG, "Contact removed: %s", name.c_str());
  this->publish_phonebook_changed_();
}

void VoipStack::set_contacts(const std::string &contacts_csv) {
  const std::string normalized_csv = this->normalize_phonebook_for_transport_(contacts_csv);
  if (normalized_csv.empty()) return;
  const bool changed = this->phonebook_.add_batch(normalized_csv);
  const bool selected = this->maybe_auto_select_ha_first_();
  if (changed || selected) {
    ESP_LOGI(TAG, "Contacts batch applied: %zu total", this->phonebook_.size());
    this->publish_phonebook_changed_();
  }
}

std::string VoipStack::normalize_phonebook_for_transport_(const std::string &contacts_csv) {
  const auto raw_entries = Phonebook::split(contacts_csv, ',');
  std::vector<ParsedPhonebookSlot> slots;
  slots.reserve(raw_entries.size());

  for (const auto &raw : raw_entries) {
    ParsedPhonebookSlot slot;
    if (!parse_slot_for_normalize(raw, &slot)) {
      continue;
    }
    slots.push_back(slot);
  }

  if (slots.empty()) return contacts_csv;

  std::string out;
  for (const auto &slot : slots) {
    const auto &entry = slot.entry;

    if (entry.endpoint_type == ContactEndpointType::SIP) {
      append_csv(&out, serialize_endpoint(entry.name, entry.route,
                                          entry.endpoint_type, entry.ip,
                                          entry.port, entry.rtp_port,
                                          entry.sip_transport_tcp));
      continue;
    }

    append_csv(&out, entry.name);
  }

  return out;
}

bool VoipStack::maybe_auto_select_ha_first_() {
  if (!this->use_ha_as_first_contact_) return false;
  if (this->first_contacts_batch_committed_) return false;
  if (this->ha_peer_name_.empty()) return false;
  if (this->call_state_.load(std::memory_order_acquire) != CallState::IDLE) return false;
  if (this->phonebook_.find(this->ha_peer_name_) == nullptr) return false;
  if (!this->phonebook_.select(this->ha_peer_name_)) return false;

  this->first_contacts_batch_committed_ = true;
  ESP_LOGI(TAG, "Selected HA peer '%s' as initial destination", this->ha_peer_name_.c_str());
  return true;
}

void VoipStack::flush_contacts() {
  this->phonebook_.clear();
  this->publish_phonebook_changed_();
  ESP_LOGI(TAG, "Contacts flushed (empty list)");
}

#ifdef USE_VOIP_HA_AUTO_REGISTER
void VoipStack::set_ha_display_name(const std::string &name) {
  const std::string cleaned = Phonebook::trim(name);
  if (cleaned.empty() || cleaned.size() > Phonebook::MAX_NAME_BYTES) return;
  for (const unsigned char ch : cleaned)
    if (ch < 32 || ch == 127) return;
  this->ha_display_name_ = cleaned;
}
#endif

bool VoipStack::set_contact(const std::string &name) {
  if (this->phonebook_.empty()) {
    ESP_LOGW(TAG, "set_contact('%s') failed: contacts list is empty", name.c_str());
    std::string msg = std::string("Contact not found: ") + name;
    this->defer([this, name, msg]() { this->call_failed_trigger_.trigger(name, msg); });
    return false;
  }
  if (this->phonebook_.select(name)) {
    this->publish_destination_();
    ESP_LOGI(TAG, "Selected contact: %s", name.c_str());
    return true;
  }
  ESP_LOGW(TAG, "set_contact('%s') failed: not found in %zu contacts",
           name.c_str(), this->phonebook_.size());
  std::string msg = std::string("Contact not found: ") + name;
  this->defer([this, name, msg]() { this->call_failed_trigger_.trigger(name, msg); });
  return false;
}

bool VoipStack::call_contact(const std::string &id, std::string_view account) {
  if (!this->can_start_call_()) return false;
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  if (!account.empty() && account != "direct" && !this->is_registered(std::string(account))) return false;
#else
  if (!account.empty() && account != "direct") return false;
#endif
  const auto account_for = [this](const ContactEntry &entry) { return this->get_contact_account_view(entry); };
  const auto *entry = this->phonebook_.find_id(id, account, account_for);
  if (entry == nullptr) return false;
  if (!this->phonebook_.select(id, account, account_for)) return false;
  this->pending_dialplan_target_.clear();
  this->publish_destination_();
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  this->pending_line_ = this->get_contact_account(id, account);
#endif
  this->start();
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  this->pending_line_.clear();
#endif
  return true;
}

std::string_view VoipStack::get_contact_account_view(const ContactEntry &entry) const {
  (void) entry;
#ifdef USE_ESPHOME_VOIP_REGISTRATION
#ifdef USE_ESPHOME_VOIP_CONTACT_METADATA
  if (!entry.account_id.empty()) return {entry.account_id.data(), entry.account_id.size()};
#endif
#ifdef USE_ESPHOME_VOIP_DIRECTORY
  if (entry.provider != nullptr)
    for (const auto &provider : this->directory_providers_)
      if (provider.owner == entry.provider && !provider.account_id.empty()) return provider.account_id;
#endif
#ifdef USE_ESPHOME_VOIP_BLF
  return this->blf_account_id_;
#else
  return this->default_line_;
#endif
#else
  return "direct";
#endif
}

std::string VoipStack::get_contact_account(std::string_view id, std::string_view account) const {
  const auto *entry = this->phonebook_.find_id(id, account,
      [this](const ContactEntry &candidate) { return this->get_contact_account_view(candidate); });
  if (entry == nullptr) return "";
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  const auto selected = this->get_contact_account_view(*entry);
  return selected.empty() ? this->default_line_ : std::string(selected);
#else
  return "direct";
#endif
}

#ifdef USE_ESPHOME_VOIP_DIRECTORY
DirectoryCommitResult VoipStack::commit_contact_directory(const void *provider, std::vector<ContactEntry> entries,
                                                          const std::string &account_id) {
  return this->commit_contact_directory(provider, entries.data(), entries.size(), account_id);
}

DirectoryCommitResult VoipStack::commit_contact_directory(const void *provider, ContactEntry *entries, size_t count,
                                                          const std::string &account_id, DirectoryCommitInfo *info) {
  if (entries == nullptr && count != 0) return DirectoryCommitResult::INVALID;
  if (count > Phonebook::MAX_CONTACTS) return DirectoryCommitResult::CAPACITY;
  if (account_id.size() > 32) return DirectoryCommitResult::INVALID;
  DirectoryProvider *binding = nullptr;
  for (auto &item : this->directory_providers_)
    if (item.owner == provider) { binding = &item; break; }
  if (binding == nullptr && count != 0)
    for (auto &item : this->directory_providers_)
      if (item.owner == nullptr) { binding = &item; break; }
  if (binding == nullptr && count != 0) return DirectoryCommitResult::CAPACITY;
  SipTargetParts own;
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  SipAccountSnapshot account;
  std::string own_uri;
  if (!account_id.empty() && this->get_account_snapshot(account_id, &account)) {
    own_uri = account.config.address_of_record();
    if (sip_parse_target_uri(own_uri, own)) own.port = account.config.port;
  }
#endif
  const auto self = [this, &own](const ContactEntry &entry) {
    return (!this->device_route_id_.empty() &&
            (entry.route == this->device_route_id_ || entry.stable_id() == this->device_route_id_)) ||
           sip_matches_aor(own, entry.route);
  };
  size_t retained_count = 0;
  for (size_t i = 0; i < count; ++i) {
    if (self(entries[i])) continue;
    if (retained_count != i) entries[retained_count] = std::move(entries[i]);
    ++retained_count;
  }
  const auto result = this->phonebook_.replace_directory(provider, entries, retained_count, info);
  if (result == DirectoryCommitResult::APPLIED || result == DirectoryCommitResult::UNCHANGED) {
    const bool route_changed = binding != nullptr && (binding->owner != provider || binding->account_id != account_id);
    bool retained = false;
    for (size_t i = 0; i < this->phonebook_.size(); ++i) retained |= this->phonebook_.at(i)->provider == provider;
    if (binding != nullptr) {
      binding->owner = retained ? provider : nullptr;
      binding->account_id = retained ? account_id : "";
    }
    if (result == DirectoryCommitResult::APPLIED || route_changed) this->publish_phonebook_changed_();
  }
  return result;
}
#endif

void VoipStack::call(const std::string &value) { this->call_(value); }

void VoipStack::start_call_on_line(const std::string &target, const std::string &line,
                                  bool send_video, bool receive_video) {
  if (!this->can_start_call_()) return;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  if (this->transport_ != nullptr) this->transport_->set_outgoing_video_intent({true, send_video, receive_video});
#else
  (void) send_video;
  (void) receive_video;
#endif
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  this->start_call_on_line_(target, line);
#else
  if (line.empty() || line == "direct") this->call(target);
#endif
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  if (this->call_state_.load(std::memory_order_acquire) == CallState::IDLE && this->transport_ != nullptr)
    this->transport_->set_outgoing_video_intent({});
#endif
}

void VoipStack::hangup_call_if_id(const std::string &call_id) {
  if (!call_id.empty() && call_id == this->get_current_call_id_()) this->stop();
}

void VoipStack::call_(const std::string &value) {
  if (!this->can_start_call_()) return;
  const std::string target = Phonebook::trim(value);
  if (target.empty()) {
    ESP_LOGW(TAG, "call failed: empty target");
    this->defer([this]() { this->call_failed_trigger_.trigger("", "Empty target"); });
    return;
  }
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  if (!this->force_direct_call_ && this->default_line_ != "direct") {
    this->start_call_on_line_(target, this->default_line_); return;
  }
#endif
  if (sip_normalize_dial_target(value, Phonebook::MAX_ROUTE_BYTES).empty()) {
    ESP_LOGW(TAG, "call failed: invalid target label");
    this->defer([this, target]() { this->call_failed_trigger_.trigger(target, "Invalid target"); });
    return;
  }

  if (this->phonebook_.select(target)) {
    this->pending_dialplan_target_.clear();
    this->publish_destination_();
    ESP_LOGI(TAG, "Selected contact: %s", target.c_str());
    this->start();
    return;
  }

  if (this->ha_peer_name_.empty() || this->phonebook_.find(this->ha_peer_name_) == nullptr ||
      !this->phonebook_.select(this->ha_peer_name_)) {
    ESP_LOGW(TAG, "call('%s') failed: not in local phonebook and HA peer unavailable",
             target.c_str());
    std::string msg = std::string("Contact not found: ") + target;
    this->defer([this, target, msg]() { this->call_failed_trigger_.trigger(target, msg); });
    return;
  }

  this->pending_dialplan_target_ = target;
  this->publish_destination_();
  ESP_LOGI(TAG, "Routing target '%s' through HA peer '%s'",
           target.c_str(), this->ha_peer_name_.c_str());
  this->start_call_();
}

void VoipStack::next_contact() {
  if (this->phonebook_.empty()) return;
  this->phonebook_.next();
  this->publish_last_reason_("");
  this->clear_terminal_call_snapshot_();
  this->publish_destination_();
  ESP_LOGD(TAG, "Selected contact: %s", this->get_current_destination().c_str());
}

void VoipStack::prev_contact() {
  if (this->phonebook_.empty()) return;
  this->phonebook_.prev();
  this->publish_last_reason_("");
  this->clear_terminal_call_snapshot_();
  this->publish_destination_();
  ESP_LOGD(TAG, "Selected contact: %s", this->get_current_destination().c_str());
}

#ifdef USE_ESPHOME_VOIP_DIRECTORY
std::string VoipStack::get_current_destination() const {
  return std::string(this->phonebook_.current_name());
}
#else
const std::string &VoipStack::get_current_destination() const {
  return this->phonebook_.current_name();
}
#endif

#ifdef USE_ESPHOME_VOIP_DIRECTORY
std::string VoipStack::get_current_destination_route() const {
  return std::string(this->phonebook_.current_route());
}
#else
const std::string &VoipStack::get_current_destination_route() const {
  return this->phonebook_.current_route();
}
#endif

#ifdef USE_ESPHOME_VOIP_DIRECTORY
std::string VoipStack::get_current_contact_ip() const {
  return std::string(this->phonebook_.current_ip());
}
#else
const std::string &VoipStack::get_current_contact_ip() const {
  return this->phonebook_.current_ip();
}
#endif

uint16_t VoipStack::get_current_contact_port() const {
  return this->phonebook_.current_port();
}

uint16_t VoipStack::get_current_contact_rtp_port() const {
  const auto *c = this->phonebook_.current();
  return c ? c->rtp_port : 0;
}

bool VoipStack::get_current_contact_sip_transport_tcp() const {
  const auto *c = this->phonebook_.current();
  return c != nullptr && c->endpoint_type == ContactEndpointType::SIP && c->sip_transport_tcp;
}

void VoipStack::publish_destination_() {
  std::string current = this->get_current_destination();
  const CallState state = this->call_state_.load(std::memory_order_acquire);
  const CallSnapshot call = this->snapshot_call_identity_();
  if (!call.call_id.empty() && !call.dialed_dest_name.empty() && !call.dest_name.empty()) {
    current = call.dest_name;
  } else if (!this->pending_dialplan_target_.empty()) {
    current = this->pending_dialplan_target_;
  } else if (state == CallState::IDLE && !this->last_reason_.empty() &&
             this->last_terminal_direction_ == "outgoing" &&
             !this->last_terminal_dest_name_.empty()) {
    current = this->last_terminal_dest_name_;
  }
#ifdef USE_TEXT_SENSOR
  if (this->destination_sensor_ != nullptr) {
    this->destination_sensor_->publish_state(current);
  }
#endif
  if (current != this->last_published_destination_) {
    this->last_published_destination_ = current;
    this->destination_changed_trigger_.trigger(current);
  }
  this->publish_sip_snapshot_();
}

void VoipStack::publish_caller_(const std::string &caller_name) {
#ifdef USE_TEXT_SENSOR
  if (this->caller_sensor_ != nullptr) {
    this->caller_sensor_->publish_state(caller_name);
  }
#endif
  this->publish_sip_snapshot_();
}

void VoipStack::publish_contacts_() {
#ifdef USE_TEXT_SENSOR
  if (this->contacts_sensor_ != nullptr) {
    // Count only ("3 contacts"); the CSV is on demand via get_contacts_csv().
    char buf[32];
    snprintf(buf, sizeof(buf), "%zu contact%s",
             this->phonebook_.size(),
             this->phonebook_.size() == 1 ? "" : "s");
    this->contacts_sensor_->publish_state(buf);
  }
#endif
}

void VoipStack::publish_phonebook_changed_() {
#ifdef USE_ESPHOME_VOIP_BLF
  this->sync_blf_contacts_();
#endif
  this->publish_destination_();
  this->publish_contacts_();
  this->phonebook_update_trigger_.trigger(this->get_current_destination());
}

std::string VoipStack::get_contacts_csv() const {
  // Names only (no endpoints) - for UI / diagnostic display.
  return this->phonebook_.names_csv();
}

}  // namespace voip_stack
}  // namespace esphome

#endif  // USE_ESP32
