#include "sip_mwi.h"
#if defined(USE_ESPHOME_VOIP_REGISTRATION) && (defined(USE_ESPHOME_VOIP_MWI) || defined(USE_ESPHOME_VOIP_BLF))

#include <algorithm>
#include <string_view>
#include <cctype>
#include <cstdio>
#include <limits>
#include <vector>

#include "sip_header_name.h"

namespace esphome::voip_stack {
namespace {
constexpr uint32_t TRANSACTION_MS = 32000;
bool reached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}
std::string lower(std::string text) {
  for (char &c : text)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}
std::string trim(const std::string &text) {
  const size_t start = text.find_first_not_of(" \t\r\n");
  return start == std::string::npos
             ? ""
             : text.substr(start, text.find_last_not_of(" \t\r\n") - start + 1);
}
std::vector<std::string> headers(const std::string &message,
                                 const std::string &wanted) {
  std::vector<std::string> values;
  size_t at = message.find("\r\n");
  bool selected = false;
  while (at != std::string::npos) {
    const size_t begin = at + 2, end = message.find("\r\n", begin);
    if (end == std::string::npos || end == begin)
      break;
    if (message[begin] == ' ' || message[begin] == '\t') {
      if (selected && !values.empty())
        values.back() += ' ' + trim(message.substr(begin, end - begin));
    } else {
      const size_t colon = message.find(':', begin);
      const std::string name =
          colon < end ? trim(message.substr(begin, colon - begin)) : "";
      selected = sip_header_name_equal(name, wanted);
      if (selected)
        values.push_back(trim(message.substr(colon + 1, end - colon - 1)));
    }
    at = end;
  }
  return values;
}
std::string header(const std::string &message, const std::string &name) {
  const auto values = headers(message, name);
  return values.empty() ? "" : values.front();
}
bool decimal(const std::string &text, uint32_t *out, bool saturate = false) {
  if (text.empty())
    return false;
  uint32_t result = 0;
  for (unsigned char c : text) {
    if (c < '0' || c > '9')
      return false;
    if (result > (std::numeric_limits<uint32_t>::max() - (c - '0')) / 10) {
      if (!saturate)
        return false;
      result = std::numeric_limits<uint32_t>::max();
    } else
      result = result * 10 + (c - '0');
  }
  *out = result;
  return true;
}
std::string parameter(const std::string &text, const std::string &name) {
  size_t at = text.find(';');
  while (at != std::string::npos) {
    const size_t next = text.find(';', at + 1);
    const std::string part = trim(
        text.substr(at + 1, next == std::string::npos ? next : next - at - 1));
    const auto equal = part.find('=');
    if (lower(trim(part.substr(0, equal))) == name)
      return equal == std::string::npos ? "" : trim(part.substr(equal + 1));
    at = next;
  }
  return "";
}
std::string tag(const std::string &field) {
  const auto close = field.rfind('>');
  return parameter(close == std::string::npos ? field : field.substr(close + 1),
                   "tag");
}
bool safe_uri(const std::string &value) {
  if (value.size() < 5 || value.size() > 512 ||
      lower(value.substr(0, 4)) != "sip:")
    return false;
  for (unsigned char c : value)
    if (c <= 32 || c == 127 || c == '<' || c == '>' || c == '"')
      return false;
  return true;
}
std::string contact_uri(const std::string &field) {
  const auto begin = field.find('<'), end = field.find('>');
  const std::string value = trim(
      begin == std::string::npos
          ? field
          : field.substr(begin + 1,
                         end == std::string::npos ? end : end - begin - 1));
  return safe_uri(value) ? value : "";
}
bool sequence(const std::string &field, const char *method, uint32_t *out) {
  const auto separator = field.find_first_of(" \t");
  return separator != std::string::npos &&
         lower(trim(field.substr(separator))) == lower(method) &&
         decimal(field.substr(0, separator), out) && *out > 0 &&
         *out <= 2147483647;
}
std::string ip_string(uint32_t ip) {
  return std::to_string((ip >> 24) & 255) + "." +
         std::to_string((ip >> 16) & 255) + "." +
         std::to_string((ip >> 8) & 255) + "." + std::to_string(ip & 255);
}
std::string route_set(const std::string &message) {
  std::string result;
  for (const auto &field : headers(message, "Record-Route")) {
    if (result.size() + field.size() > 1024)
      return "";
    if (!result.empty())
      result += ", ";
    result += field;
  }
  return result;
}
bool same_config(const SipAccountConfig &a, const SipAccountConfig &b) {
  return a.id == b.id && a.registrar == b.registrar && a.domain == b.domain &&
         a.username == b.username && a.password == b.password &&
         a.auth_username == b.auth_username && a.proxy == b.proxy &&
         a.port == b.port && a.proxy_port == b.proxy_port && a.tcp == b.tcp;
}
} // namespace

#ifdef USE_ESPHOME_VOIP_MWI
bool SipMwi::parse_summary(const std::string &body,
                           MessageWaitingState *result) {
  if (result == nullptr || body.empty() || body.size() > 4096)
    return false;
  MessageWaitingState parsed;
  parsed.account_id = result->account_id;
  bool status = false, voice = false, account = false;
  size_t at = 0;
  while (at < body.size()) {
    size_t end = body.find('\n', at);
    if (end == std::string::npos)
      end = body.size();
    const std::string line = trim(body.substr(at, end - at));
    at = end + 1;
    if (line.empty())
      break; // Optional RFC 2822 message headers follow the summary.
    const auto colon = line.find(':');
    if (colon == std::string::npos)
      return false;
    const std::string name = lower(trim(line.substr(0, colon))),
                      value = trim(line.substr(colon + 1));
    if (name == "messages-waiting") {
      const std::string waiting = lower(value);
      if (status || (waiting != "yes" && waiting != "no"))
        return false;
      status = true;
      parsed.waiting = waiting == "yes";
    } else if (name == "message-account") {
      if (account || value.empty() || value.size() > 512 || value.find(':') == std::string::npos) return false;
      for (unsigned char ch : value)
        if (ch <= 32 || ch == 127 || ch == '<' || ch == '>' || ch == '"') return false;
      account = true;
      parsed.message_account = value;
    } else if (name == "voice-message") {
      if (voice)
        return false;
      voice = true;
      const auto urgent = value.find('(');
      const std::string normal = trim(value.substr(0, urgent));
      const auto slash = normal.find('/');
      if (slash == std::string::npos ||
          !decimal(trim(normal.substr(0, slash)), &parsed.new_messages, true) ||
          !decimal(trim(normal.substr(slash + 1)), &parsed.old_messages, true))
        return false;
      if (urgent != std::string::npos) {
        if (value.back() != ')')
          return false;
        const std::string counts =
            trim(value.substr(urgent + 1, value.size() - urgent - 2));
        const auto divider = counts.find('/');
        if (divider == std::string::npos ||
            !decimal(trim(counts.substr(0, divider)), &parsed.urgent_new,
                     true) ||
            !decimal(trim(counts.substr(divider + 1)), &parsed.urgent_old,
                     true))
          return false;
      }
    }
  }
  if (!status)
    return false;
  parsed.available = true;
  *result = std::move(parsed);
  return true;
}
#endif

#ifdef USE_ESPHOME_VOIP_BLF
bool SipMwi::configure_blf(uint8_t capacity) {
  if (capacity == 0 || capacity > 64) return false;
  if (this->blf_accounts_ != nullptr) return this->blf_capacity_ == capacity;
  std::unique_ptr<BlfWatch[]> storage(new (std::nothrow) BlfWatch[capacity]);
  if (storage == nullptr) return false;
#ifdef USE_ESPHOME_VOIP_PRESENCE
  std::unique_ptr<Account[]> presence(new (std::nothrow) Account[capacity]);
  if (presence == nullptr) return false;
  this->presence_accounts_ = std::move(presence);
#endif
  this->blf_accounts_ = std::move(storage);
  this->blf_capacity_ = capacity;
  return true;
}

void SipMwi::publish_blf_(size_t index) {
  auto &watch = this->blf_accounts_[this->watch_slot_(index)];
  const bool available = watch.blf.available;
  const auto state = watch.blf.state;
  const uint32_t version = watch.blf.version;
  const auto ringing = watch.blf.ringing_dialogs, active = watch.blf.active_dialogs, held = watch.blf.held_dialogs;
  watch.dialogs.snapshot(&watch.blf);
  if (!watch.blf.contact_id.empty() && this->hooks_.notify_blf != nullptr &&
      (available != watch.blf.available || state != watch.blf.state || version != watch.blf.version ||
       ringing != watch.blf.ringing_dialogs || active != watch.blf.active_dialogs || held != watch.blf.held_dialogs))
    this->hooks_.notify_blf(this->hooks_.context, watch.blf);
}

#ifdef USE_ESPHOME_VOIP_PRESENCE
void SipMwi::publish_presence_(size_t index, PresenceState presence) {
  auto &state = this->blf_accounts_[this->watch_slot_(index)].blf;
  if (state.presence == presence) return;
  state.presence = presence;
  if (!state.contact_id.empty() && this->hooks_.notify_blf != nullptr)
    this->hooks_.notify_blf(this->hooks_.context, state);
}
#endif

bool SipMwi::watch_retired_(size_t slot) const {
  const auto &watch = this->blf_accounts_[slot];
  if (!watch.call_id.empty() || watch.pending || watch.waiting_notify) return false;
#ifdef USE_ESPHOME_VOIP_PRESENCE
  const auto &presence = this->presence_accounts_[slot];
  if (!presence.call_id.empty() || presence.pending || presence.waiting_notify) return false;
#endif
  return true;
}

void SipMwi::update_watch_account_(size_t slot, size_t account_index,
                                  const SipRegistration::Account &account, uint32_t now) {
  this->update_account_(MWI_CAPACITY + slot, account.config, account.config_revision,
      account.target_ip, account.target_port, account.contact, account.bound, account.desired, now);
#ifdef USE_ESPHOME_VOIP_PRESENCE
  auto &presence = this->presence_accounts_[slot];
  presence.package = Account::Package::PRESENCE;
  presence.account_index = account_index;
  this->update_account_(this->presence_index_(slot), account.config, account.config_revision,
      account.target_ip, account.target_port, account.contact, account.bound, account.desired, now);
#else
  (void) account_index;
#endif
}

void SipMwi::begin_blf_watch_(size_t slot, size_t account_index, const SipRegistration::Account &account,
                              std::string_view id, std::string_view target, uint32_t now) {
  auto &watch = this->blf_accounts_[slot];
  watch = BlfWatch{};
  watch.package = Account::Package::DIALOG;
  watch.account_index = account_index;
  watch.watch_target = target;
  watch.blf.account_id = account.config.id;
  watch.blf.contact_id = id;
#ifdef USE_ESPHOME_VOIP_PRESENCE
  this->presence_accounts_[slot] = Account{};
#endif
  this->update_watch_account_(slot, account_index, account, now);
}

void SipMwi::retire_blf_watch_(size_t slot, uint32_t now) {
  const size_t rows[] = {MWI_CAPACITY + slot,
#ifdef USE_ESPHOME_VOIP_PRESENCE
                        this->presence_index_(slot),
#endif
  };
  for (const auto row : rows) {
    auto &watch = this->account_(row);
    watch.desired = false;
    this->unavailable_(row);
    if (!watch.bound || watch.call_id.empty()) {
      this->reset_dialog_(row);
      watch.occupied = false;
    } else if (!watch.pending && !watch.waiting_notify) {
      watch.scheduled = true;
      watch.next_ms = now;
    }
  }
}

bool SipMwi::set_blf_watch(size_t index, size_t account_index, const SipRegistration::Account &account,
                           const std::string &id, const std::string &target, uint32_t now) {
  if (index >= this->blf_capacity_ || account_index >= CAPACITY || !account.occupied ||
      id.empty() || id.size() > 96 || !safe_uri(target)) return false;
  auto &watch = this->blf_accounts_[index];
  if (watch.package == Account::Package::DIALOG && watch.desired && watch.blf.account_id == account.config.id &&
      watch.blf.contact_id == id && watch.watch_target == target) {
    this->update_watch_account_(index, account_index, account, now);
    return true;
  }
  if (!this->watch_retired_(index)) {
    watch.pending_account = account.config.id;
    watch.pending_account_index = account_index;
    watch.pending_id = id; watch.pending_target = target;
    watch.replace_pending = true;
    this->retire_blf_watch_(index, now);
    return true;
  }
  this->begin_blf_watch_(index, account_index, account, id, target, now);
  return true;
}

void SipMwi::clear_blf_watch(size_t index, uint32_t now) {
  if (index >= this->blf_capacity_) return;
  auto &watch = this->blf_accounts_[index];
  watch.replace_pending = false;
  watch.pending_account.clear(); watch.pending_id.clear(); watch.pending_target.clear();
  this->retire_blf_watch_(index, now);
}

void SipMwi::invalidate_blf_account(size_t account_index) {
  for (size_t slot = 0; slot < this->blf_capacity_; ++slot) {
    auto &watch = this->blf_accounts_[slot];
    if (watch.package != Account::Package::DIALOG || watch.account_index != account_index) continue;
    const size_t rows[] = {MWI_CAPACITY + slot,
#ifdef USE_ESPHOME_VOIP_PRESENCE
                          this->presence_index_(slot),
#endif
    };
    for (const auto row : rows) {
      this->unavailable_(row);
      this->reset_dialog_(row);
      auto &entry = this->account_(row);
      entry.bound = entry.occupied = false;
      entry.config_ = nullptr;
    }
  }
}

void SipMwi::update_blf_account(size_t index, const SipRegistration::Account &account, uint32_t now) {
  for (size_t slot = 0; slot < this->blf_capacity_; ++slot) {
    auto &watch = this->blf_accounts_[slot];
    if (watch.replace_pending && watch.pending_account_index == index && account.occupied &&
        watch.pending_account == account.config.id && this->watch_retired_(slot)) {
      const RegistrationMessage id = std::move(watch.pending_id), target = std::move(watch.pending_target);
      this->begin_blf_watch_(slot, index, account, id, target, now);
    }
    if (watch.package != Account::Package::DIALOG || watch.account_index != index || !watch.desired) continue;
    if (!account.occupied || watch.blf.account_id != account.config.id) {
      this->invalidate_blf_account(index);
      continue;
    }
    this->update_watch_account_(slot, index, account, now);
  }
}

const BlfStateInfo *SipMwi::blf_state(size_t index) const {
  return index < this->blf_capacity_ ? &this->blf_accounts_[index].blf : nullptr;
}
std::string_view SipMwi::blf_watch_id(size_t index) const {
  const auto &watch = this->blf_accounts_[index];
  if (watch.replace_pending) return {watch.pending_id.data(), watch.pending_id.size()};
  return {watch.blf.contact_id.data(), watch.blf.contact_id.size()};
}
bool SipMwi::blf_watch_requested(size_t index) const {
  return index < this->blf_capacity_ &&
      (this->blf_accounts_[index].replace_pending || this->blf_accounts_[index].desired);
}
#endif

std::string SipMwi::token_() {
  static uint32_t fallback = 1;
  char text[24];
  const uint32_t first =
      this->hooks_.random != nullptr ? this->hooks_.random() : ++fallback;
  const uint32_t second =
      this->hooks_.random != nullptr ? this->hooks_.random() : ++fallback;
  std::snprintf(text, sizeof(text), "%08lx%08lx",
                static_cast<unsigned long>(first),
                static_cast<unsigned long>(second));
  return text;
}

#ifdef USE_ESPHOME_VOIP_MWI
void SipMwi::publish_(size_t index, MessageWaitingState state) {
  auto &old = this->accounts_[index].state;
  if (old.account_id == state.account_id && old.available == state.available &&
      old.waiting == state.waiting && old.new_messages == state.new_messages &&
      old.old_messages == state.old_messages &&
      old.urgent_new == state.urgent_new && old.urgent_old == state.urgent_old &&
      old.message_account == state.message_account)
    return;
  old = std::move(state);
  if (this->hooks_.notify != nullptr)
    this->hooks_.notify(this->hooks_.context, old);
}
#endif
void SipMwi::unavailable_(size_t index) {
#ifdef USE_ESPHOME_VOIP_BLF
#ifdef USE_ESPHOME_VOIP_PRESENCE
  if (this->account_(index).package == Account::Package::PRESENCE) {
    this->publish_presence_(index, PresenceState::UNKNOWN);
    return;
  }
#endif
  if (this->account_(index).package == Account::Package::DIALOG) {
    this->blf_accounts_[this->watch_slot_(index)].dialogs.invalidate();
    this->publish_blf_(index);
    return;
  }
#endif
#ifdef USE_ESPHOME_VOIP_MWI
  MessageWaitingState state;
  const auto &account = this->account_(index);
  const auto &previous = this->accounts_[index].state;
  if (account.config_ == nullptr && previous.account_id.empty()) return;
  state.account_id = previous.account_id.empty() ? account.config().id : previous.account_id;
  if (state.account_id.empty())
    return;
  this->publish_(index, std::move(state));
#endif
}
void SipMwi::reset_dialog_(size_t index) {
  auto &a = this->account_(index);
#ifdef USE_ESPHOME_VOIP_BLF
  if (a.package == Account::Package::DIALOG) {
    auto &watch = this->blf_accounts_[this->watch_slot_(index)];
    watch.dialogs.invalidate();
    watch.needs_full = watch.full_refresh_requested = false;
  }
#endif
  a.call_id.clear();
  a.tag.clear();
  a.remote_tag.clear();
  a.remote_target.clear();
  a.routes.clear();
  a.request.clear();
  a.pending = false;
  a.scheduled = false;
  a.subscribed = false;
  a.waiting_notify = false;
  a.notify_seen = false;
  a.request_in_dialog = false;
  a.refresh_after_register = false;
  a.notify_cseq = 0;
  a.expires_ms = 0;
  a.auth_attempts = a.proxy_auth_attempts = 0;
}
void SipMwi::invalidate_account(size_t index) {
  if (index >= MWI_CAPACITY) return;
  auto &a = this->accounts_[index];
  if (a.config_ != nullptr) this->unavailable_(index);
  auto state = std::move(a.state);
  a = MwiAccount{};
  a.state = std::move(state);
}

void SipMwi::update_account(size_t index, const SipAccountConfig &config,
                            uint32_t ip, uint16_t port,
                            const std::string &contact, bool bound,
                            bool desired, uint32_t now) {
  if (index >= MWI_CAPACITY) return;
  auto &a = this->account_(index);
  if (a.owned_config == nullptr || !same_config(*a.owned_config, config)) {
    auto owned = std::make_unique<SipAccountConfig>(config);
    this->invalidate_account(index);
    a.owned_config = std::move(owned);
  }
#ifdef USE_ESPHOME_VOIP_BLF
  a.account_index = index;
#endif
  this->update_account_(index, *a.owned_config, 0, ip, port, contact, bound, desired, now);
}

void SipMwi::update_registered_account(size_t index, const SipRegistration::Account &account, uint32_t now) {
  if (index >= MWI_CAPACITY) return;
  if (!account.occupied) {
    if (this->account_(index).config_ != nullptr) this->invalidate_account(index);
    return;
  }
#ifdef USE_ESPHOME_VOIP_BLF
  this->account_(index).account_index = index;
#endif
  this->update_account_(index, account.config, account.config_revision, account.target_ip,
                        account.target_port, account.contact, account.bound, account.desired, now);
}

void SipMwi::on_registration_accepted(size_t index, const SipRegistration::Account &account, uint32_t now) {
  if (index >= CAPACITY || !account.occupied || !account.bound || !account.desired) return;
#ifdef USE_ESPHOME_VOIP_MWI
  this->update_registered_account(index, account, now);
#endif
#ifdef USE_ESPHOME_VOIP_BLF
  this->update_blf_account(index, account, now);
#endif
  for (size_t row = 0; row < this->total_capacity_(); ++row) {
    auto &subscription = this->account_(row);
    if (subscription.occupied && this->owner_index_(row) == index && subscription.bound &&
        subscription.desired && subscription.supported)
      subscription.refresh_after_register = true;
  }
}

bool SipMwi::registration_refresh_ready_(const Account &account, uint32_t now) {
  return account.refresh_after_register && account.bound && account.desired && account.supported &&
         !account.pending && !account.waiting_notify &&
         !(account.scheduled && account.failures != 0 && !reached(now, account.next_ms));
}

void SipMwi::update_account_(size_t index, const SipAccountConfig &config, uint32_t revision,
                             uint32_t ip, uint16_t port, const std::string &contact,
                             bool bound, bool desired, uint32_t now) {
  auto &a = this->account_(index);
  const bool changed = !a.occupied || a.config_ != &config || a.config_revision != revision ||
                       a.target_ip != ip || a.target_port != port || a.contact != contact;
  if (changed) {
    if (a.occupied) this->unavailable_(index);
    this->reset_dialog_(index);
    a.digest = SipDigest{};
    a.proxy_digest = SipDigest{};
    a.config_ = &config;
    a.config_revision = revision;
    if (a.owned_config.get() != &config) a.owned_config.reset();
    a.contact = contact;
    a.target_ip = ip;
    a.target_port = port;
    a.occupied = config.valid() && safe_uri(contact) && ip != 0 && port != 0;
    a.supported = true;
    a.failures = 0;
    a.expires = 3600;
#ifdef USE_ESPHOME_VOIP_MWI
    if (index < MWI_CAPACITY) {
      MessageWaitingState state;
      state.account_id = config.id;
      this->publish_(index, std::move(state));
    }
#endif
#ifdef USE_ESPHOME_VOIP_BLF
    if (a.package == Account::Package::DIALOG) this->publish_blf_(index);
#endif
  }
  const bool old_desired = a.desired;
  const bool rebound = bound && !a.bound;
  a.desired = desired;
  a.bound = bound;
  if (rebound)
    a.supported = true;
  if (!a.occupied)
    return;
  if (!bound) {
    this->unavailable_(index);
    this->reset_dialog_(index);
    return;
  }
  if (!a.desired) {
    this->unavailable_(index);
    if (old_desired && !a.call_id.empty() && !a.pending && !a.waiting_notify &&
        !a.remote_tag.empty()) {
      a.pending = false;
      a.scheduled = true;
      a.next_ms = now;
    }
  } else if (a.supported && !a.pending && !a.scheduled && !a.subscribed) {
    a.scheduled = true;
    a.next_ms = now;
  }
}

bool SipMwi::begin_(size_t index, uint32_t now) {
  auto &a = this->account_(index);
  if (!a.occupied || !a.bound || this->hooks_.send == nullptr)
    return false;
  const std::string local =
      this->hooks_.local_ip == nullptr
          ? ""
          : this->hooks_.local_ip(this->hooks_.context, a.target_ip);
  if (local.empty()) {
    this->retry_(index, now);
    return false;
  }
  if (a.call_id.empty()) {
    a.call_id = "mwi-" + this->token_() + "@" + local;
    a.tag = this->token_();
    a.cseq = 0;
  }
  if (a.cseq >= 2147483647) {
    this->retry_(index, now);
    return false;
  }
  ++a.cseq;
  a.branch = "z9hG4bK" + this->token_();
  const std::string aor = "sip:" + a.config().username + "@" + a.config().domain;
  std::string observed = aor;
  const char *event_name = "message-summary";
  const char *content_type = "application/simple-message-summary";
#ifdef USE_ESPHOME_VOIP_BLF
  if (a.package != Account::Package::MESSAGE_SUMMARY) {
    observed = this->blf_accounts_[this->watch_slot_(index)].watch_target;
#ifdef USE_ESPHOME_VOIP_PRESENCE
    event_name = a.package == Account::Package::DIALOG ? "dialog" : "presence";
    content_type = a.package == Account::Package::DIALOG ? "application/dialog-info+xml" : "application/pidf+xml";
#else
    event_name = "dialog";
    content_type = "application/dialog-info+xml";
#endif
  }
#endif
  std::string target = a.remote_target.empty() ? observed : a.remote_target;
  std::string routes = a.routes;
  if (!routes.empty()) {
    const auto close = routes.find('>');
    const std::string first = contact_uri(
        routes.substr(0, close == std::string::npos ? close : close + 1));
    if (!first.empty() && lower(first).find(";lr") == std::string::npos) {
      const auto comma = routes.find(',', close);
      routes = comma == std::string::npos ? "" : trim(routes.substr(comma + 1));
      if (!routes.empty())
        routes += ", ";
      routes += '<' + target + '>';
      target = first;
    }
  }
  std::string message =
      "SUBSCRIBE " + target + " SIP/2.0\r\nVia: SIP/2.0/" +
      (a.config().tcp ? "TCP " : "UDP ") + local + ':' +
      std::to_string(this->hooks_.local_port) + ";branch=" + a.branch +
      ";rport\r\nMax-Forwards: 70\r\nFrom: <" + aor + ">;tag=" + a.tag +
      "\r\nTo: <" + observed + '>' +
      (a.remote_tag.empty() ? "" : ";tag=" + a.remote_tag) +
      "\r\nCall-ID: " + a.call_id + "\r\nCSeq: " + std::to_string(a.cseq) +
      " SUBSCRIBE\r\nContact: <" + a.contact +
      ">\r\nEvent: " + event_name + "\r\nAccept: " + content_type + "\r\nExpires: ";
  a.requested_expires = a.desired ? a.expires : 0;
  message += std::to_string(a.requested_expires) + "\r\n";
  if (!routes.empty())
    message += "Route: " + routes + "\r\n";
  for (SipDigest *digest : {&a.digest, &a.proxy_digest}) {
    if (!digest->empty()) {
      const std::string auth = digest->authorization(
          a.config(), "SUBSCRIBE", target, this->token_(), this->hooks_.hash);
      if (auth.empty()) {
        this->retry_(index, now);
        return false;
      }
      message += auth;
    }
  }
  message += "Content-Length: 0\r\n\r\n";
  a.request_in_dialog = !a.remote_tag.empty();
  a.refresh_after_register = false;
  a.request = message;
  a.pending = true;
  a.scheduled = false;
  a.interval_ms = 500;
  a.next_ms = now + 500;
  a.deadline_ms = now + TRANSACTION_MS;
  a.waiting_notify = true;
  a.notify_deadline_ms = now + TRANSACTION_MS;
  if (!this->hooks_.send(this->hooks_.context, this->owner_index_(index), message, a.target_ip,
                         a.target_port, a.config().tcp)) {
    this->retry_(index, now);
    return false;
  }
  return true;
}
void SipMwi::retry_(size_t index, uint32_t now, uint32_t delay_ms,
                    bool new_dialog) {
  auto &a = this->account_(index);
  const bool keep = !new_dialog && a.bound && a.subscribed &&
                    a.expires_ms != 0 && !reached(now, a.expires_ms);
  if (!keep) {
    this->unavailable_(index);
    this->reset_dialog_(index);
  } else {
    a.pending = false;
    // A rejected refresh does not revoke the previous subscription lease.
    a.waiting_notify = false;
    a.request.clear();
  }
  a.failures = std::min<uint8_t>(a.failures + 1, 6);
  a.scheduled = a.desired && a.bound && a.supported;
  a.next_ms =
      now + (delay_ms != 0 ? delay_ms
                           : std::min<uint32_t>(60000, 1000U << a.failures));
}
void SipMwi::connection_lost(size_t index, uint32_t now) {
  for (size_t row = 0; row < this->total_capacity_(); ++row)
    if (this->account_(row).occupied && this->owner_index_(row) == index) {
      this->account_(row).bound = false;
      this->retry_(row, now);
    }
}
void SipMwi::tick(uint32_t now) {
  for (size_t i = 0; i < this->total_capacity_(); ++i) {
    auto &a = this->account_(i);
    if (!a.occupied)
      continue;
#ifdef USE_ESPHOME_VOIP_BLF
    if (a.package == Account::Package::DIALOG) {
      auto &watch = this->blf_accounts_[this->watch_slot_(i)];
      if (watch.needs_full && a.desired && a.subscribed && !a.pending && !a.waiting_notify) {
        watch.needs_full = false;
        a.scheduled = true;
        a.next_ms = now;
      }
    }
#endif
    if (a.waiting_notify && reached(now, a.notify_deadline_ms)) {
      this->retry_(i, now, 0, true);
      continue;
    }
    if (a.expires_ms != 0 && reached(now, a.expires_ms)) {
      const uint32_t remaining_backoff =
          a.scheduled && a.failures != 0 && !reached(now, a.next_ms)
              ? a.next_ms - now
              : 0;
      this->retry_(i, now, remaining_backoff, true);
      continue;
    }
    if (registration_refresh_ready_(a, now)) {
      a.refresh_after_register = false;
      a.scheduled = true;
      a.next_ms = now;
    }
    if (a.pending) {
      if (reached(now, a.deadline_ms)) {
        this->retry_(i, now);
        continue;
      }
      if (!a.config().tcp && reached(now, a.next_ms)) {
        this->hooks_.send(this->hooks_.context, this->owner_index_(i),
                          static_cast<std::string>(a.request), a.target_ip,
                          a.target_port, false);
        a.interval_ms = std::min<uint32_t>(a.interval_ms * 2, 4000);
        a.next_ms = now + a.interval_ms;
      }
    } else if (a.scheduled && reached(now, a.next_ms))
      this->begin_(i, now);
  }
}
bool SipMwi::next_deadline(uint32_t now, uint32_t *remaining) const {
  if (remaining == nullptr)
    return false;
  bool found = false;
  uint32_t best = 0;
  for (size_t index = 0; index < this->total_capacity_(); ++index) {
    const auto &a = this->account_(index);
    if (!a.occupied)
      continue;
    auto consider = [&](uint32_t deadline) {
      const uint32_t delta = reached(now, deadline) ? 0 : deadline - now;
      if (!found || delta < best)
        best = delta;
      found = true;
    };
    if (registration_refresh_ready_(a, now)) consider(now);
    if (a.pending) {
      consider(a.deadline_ms);
      if (!a.config().tcp)
        consider(a.next_ms);
    } else if (a.scheduled)
      consider(a.next_ms);
    if (a.waiting_notify)
      consider(a.notify_deadline_ms);
    if (a.expires_ms != 0)
      consider(a.expires_ms);
  }
  if (found)
    *remaining = best;
  return found;
}

bool SipMwi::handle_response(const std::string &message, uint32_t ip,
                             uint16_t port, bool tcp, int flow, uint32_t now) {
  uint32_t seq = 0;
  if (message.rfind("SIP/2.0 ", 0) != 0 ||
      !sequence(header(message, "CSeq"), "SUBSCRIBE", &seq))
    return false;
  const std::string id = header(message, "Call-ID");
  for (size_t i = 0; i < this->total_capacity_(); ++i) {
    auto &a = this->account_(i);
    if (!a.occupied || id != a.call_id)
      continue;
    if (!a.pending || ip != a.target_ip || port != a.target_port ||
        tcp != a.config().tcp || (tcp && flow != static_cast<int>(this->owner_index_(i))) ||
        seq != a.cseq || tag(header(message, "From")) != a.tag ||
        parameter(header(message, "Via"), "branch") != a.branch)
      return true;
    uint32_t status = 0;
    if (message.size() < 12 || !decimal(message.substr(8, 3), &status) ||
        status < 100 || status > 699)
      return true;
    if (status < 200) {
      a.interval_ms = 4000;
      a.next_ms = now + 4000;
      return true;
    }
    // Fork selection applies to the original SUBSCRIBE, never an in-dialog
    // refresh. Ignore a wrong-tag response without consuming the transaction.
    if (a.request_in_dialog && tag(header(message, "To")) != a.remote_tag)
      return true;
    a.pending = false;
    a.request.clear();
    if (status == 401 || status == 407) {
      SipDigest challenge;
      auto &attempts = status == 401 ? a.auth_attempts : a.proxy_auth_attempts;
      if (!challenge.parse_response(message, status == 407) ||
          ++attempts > (challenge.stale ? 3 : 2)) {
        a.supported = false;
        this->retry_(i, now);
        return true;
      }
      (status == 401 ? a.digest : a.proxy_digest) = std::move(challenge);
      this->begin_(i, now);
      return true;
    }
    if (status == 423 && a.desired && a.requested_expires != 0) {
      uint32_t minimum = 0;
      if (decimal(header(message, "Min-Expires"), &minimum) &&
          minimum > a.requested_expires && minimum <= 604800) {
        a.expires = minimum;
        this->begin_(i, now);
        return true;
      }
    }
    if (status >= 200 && status < 300) {
      a.auth_attempts = a.proxy_auth_attempts = a.failures = 0;
      const std::string remote = tag(header(message, "To"));
      // RFC 6665: the first accepted NOTIFY establishes the dialog, even when
      // its From tag differs from the SUBSCRIBE response's To tag (forking).
      if (!a.remote_tag.empty() && !remote.empty() && remote != a.remote_tag) {
        if (a.subscribed) {
          const uint32_t remaining =
              reached(now, a.expires_ms) ? 0 : a.expires_ms - now;
          a.scheduled = true;
          a.next_ms = !a.desired ? now : now + remaining * 4 / 5;
        }
        return true;
      }
      uint32_t seconds = a.requested_expires;
      const auto expires = header(message, "Expires");
      if (!expires.empty() &&
          (!decimal(expires, &seconds) || seconds > 604800)) {
        this->retry_(i, now);
        return true;
      }
      if (a.requested_expires == 0) {
        a.expires_ms =
            0; // Wait for the terminal NOTIFY, not an immediate local expiry.
        a.scheduled = false;
      } else if (seconds == 0) {
        this->retry_(i, now);
        return true;
      } else {
        if (!a.notify_seen)
          a.expires_ms = now + seconds * 1000;
        a.scheduled = true;
        const uint32_t refresh = now + std::max<uint32_t>(1000, seconds * 800);
        a.next_ms =
            a.notify_seen && a.expires_ms != 0
                ? now + std::min<uint32_t>(
                            refresh - now,
                            std::max<uint32_t>(1, (a.expires_ms - now) * 4 / 5))
                : refresh;
        if (!a.desired && !a.remote_tag.empty())
          a.next_ms = now;
      }
      return true;
    }
    if (status == 403 || status == 405 || status == 489 ||
        status == 501)
      a.supported = false;
    uint32_t retry = 0;
    decimal(header(message, "Retry-After"), &retry);
    const bool terminal = status == 404 || status == 405 || status == 410 ||
                          status == 416 || (status >= 480 && status <= 485) ||
                          status == 489 || status == 501 || status == 604;
    this->retry_(i, now, std::min<uint32_t>(retry, 604800) * 1000, terminal);
    return true;
  }
  return false;
}

bool SipMwi::reply_(const std::string &request, uint16_t status, uint32_t ip,
                    uint16_t port, bool tcp, int flow) {
  if (this->hooks_.reply == nullptr)
    return false;
  const char *reason = status == 200   ? "OK"
                       : status == 400 ? "Bad Request"
                       : status == 415 ? "Unsupported Media Type"
                       : status == 481 ? "Call/Transaction Does Not Exist"
                       : status == 489 ? "Bad Event"
                                       : "Server Internal Error";
  std::string response =
      "SIP/2.0 " + std::to_string(status) + ' ' + reason + "\r\n";
  const auto vias = headers(request, "Via");
  for (size_t i = 0; i < vias.size(); ++i) {
    std::string value = vias[i];
    if (i == 0) {
      const auto rport = lower(value).find(";rport");
      if (rport != std::string::npos &&
          (rport + 6 == value.size() || value[rport + 6] == ';' ||
           value[rport + 6] == '=')) {
        const auto end = value.find(';', rport + 1);
        value.replace(rport,
                      end == std::string::npos ? value.size() - rport
                                               : end - rport,
                      ";rport=" + std::to_string(port));
      }
      if (parameter(value, "received").empty())
        value += ";received=" + ip_string(ip);
    }
    response += "Via: " + value + "\r\n";
  }
  for (const char *name : {"From", "To", "Call-ID", "CSeq"})
    response += std::string(name) + ": " + header(request, name) + "\r\n";
  if (status == 415)
    response += "Accept: application/simple-message-summary\r\n";
  if (status == 489)
    response += "Allow-Events: message-summary\r\n";
  if (status == 500)
    response += "Retry-After: 0\r\n";
  response += "Content-Length: 0\r\n\r\n";
  return this->hooks_.reply(this->hooks_.context, response, ip, port, tcp,
                            flow);
}

bool SipMwi::handle_notify(const std::string &message, uint32_t ip,
                           uint16_t port, bool tcp, int flow, uint32_t now) {
  if (message.rfind("NOTIFY ", 0) != 0)
    return false;
  if (message.size() > 8192) {
    this->reply_(message, 400, ip, port, tcp, flow);
    return true;
  }
  const std::string id = header(message, "Call-ID"),
                    event = header(message, "Event");
  bool is_mwi = false;
#ifdef USE_ESPHOME_VOIP_MWI
  is_mwi = lower(trim(event.substr(0, event.find(';')))) == "message-summary";
#endif
  bool is_dialog = false;
  bool is_presence = false;
#ifdef USE_ESPHOME_VOIP_BLF
  is_dialog = lower(trim(event.substr(0, event.find(';')))) == "dialog";
#ifdef USE_ESPHOME_VOIP_PRESENCE
  is_presence = lower(trim(event.substr(0, event.find(';')))) == "presence";
#endif
#endif
  int selected = -1;
  for (size_t i = 0; i < this->total_capacity_(); ++i)
    if (this->account_(i).occupied && !id.empty() &&
        id == this->account_(i).call_id) {
      selected = i;
      break;
    }
  if (!is_mwi && !is_dialog && !is_presence) {
    if (selected < 0)
      return false; // REFER's implicit subscription belongs to its existing
                    // owner.
    this->reply_(message, 489, ip, port, tcp, flow);
    return true;
  }
  const auto line_end = message.find("\r\n");
  const auto uri_end = message.find(' ', 7);
  if (line_end == std::string::npos || uri_end == std::string::npos ||
      uri_end >= line_end ||
      message.substr(uri_end, line_end - uri_end) != " SIP/2.0" ||
      !safe_uri(message.substr(7, uri_end - 7))) {
    this->reply_(message, 400, ip, port, tcp, flow);
    return true;
  }
  uint32_t seq = 0;
  const std::string local_tag = tag(header(message, "To")),
                    remote_tag = tag(header(message, "From"));
  const std::string branch = parameter(header(message, "Via"), "branch");
  if (id.empty() || id.size() > 255 || local_tag.empty() ||
      local_tag.size() > 128 || remote_tag.empty() || remote_tag.size() > 128 ||
      branch.empty() || branch.size() > 255 ||
      !sequence(header(message, "CSeq"), "NOTIFY", &seq)) {
    this->reply_(message, 400, ip, port, tcp, flow);
    return true;
  }
  for (size_t cached = 0; cached < this->total_capacity_(); ++cached) {
    const auto &a = this->account_(cached);
    if (id == a.last_notify_id && seq == a.last_notify_cseq &&
        local_tag == a.last_notify_tag &&
        remote_tag == a.last_notify_remote_tag &&
        branch == a.last_notify_branch && ip == a.last_notify_ip &&
        tcp == a.last_notify_tcp &&
        (tcp ? (flow < 0 || flow == static_cast<int>(this->owner_index_(cached)))
             : port == a.last_notify_port) &&
        !reached(now, a.last_notify_until)) {
      this->reply_(message, 200, ip, port, tcp, flow);
      return true;
    }
  }
  if (selected < 0) {
    this->reply_(message, 481, ip, port, tcp, flow);
    return true;
  }
  const size_t index = static_cast<size_t>(selected);
  auto &a = this->account_(index);
#ifdef USE_ESPHOME_VOIP_BLF
  if ((a.package == Account::Package::DIALOG) != is_dialog ||
      (a.package == Account::Package::PRESENCE) != is_presence) {
    this->reply_(message, 489, ip, port, tcp, flow);
    return true;
  }
#endif
  if (!a.bound || (!a.supported && !a.subscribed) || ip != a.target_ip ||
      tcp != a.config().tcp ||
      (tcp ? (flow >= 0 && flow != static_cast<int>(this->owner_index_(index))) : port != a.target_port) ||
      local_tag != a.tag ||
      (!a.remote_tag.empty() && remote_tag != a.remote_tag) ||
      (!a.subscribed &&
       (!a.waiting_notify || reached(now, a.notify_deadline_ms)))) {
    this->reply_(message, 481, ip, port, tcp, flow);
    return true;
  }
  if (!parameter(event, "id").empty()) {
    this->reply_(message, 489, ip, port, tcp, flow);
    return true;
  }
  if (a.notify_seen && seq <= a.notify_cseq) {
    this->reply_(message, 500, ip, port, tcp, flow);
    return true;
  }
  const std::string subscription = header(message, "Subscription-State");
  const std::string state =
      lower(trim(subscription.substr(0, subscription.find(';'))));
  if (state != "active" && state != "pending" && state != "terminated") {
    this->reply_(message, 400, ip, port, tcp, flow);
    return true;
  }
  const auto body_at = message.find("\r\n\r\n");
  if (body_at == std::string::npos) {
    this->reply_(message, 400, ip, port, tcp, flow);
    return true;
  }
  const std::string_view body(message.data() + body_at + 4, message.size() - body_at - 4);
  const auto content_length = header(message, "Content-Length");
  uint32_t length = 0;
  if (body.size() > 4096 ||
      (!content_length.empty() &&
       (!decimal(content_length, &length) || length != body.size()))) {
    this->reply_(message, 400, ip, port, tcp, flow);
    return true;
  }
#ifdef USE_ESPHOME_VOIP_MWI
  MessageWaitingState next;
  if (is_mwi) next.account_id = a.config().id;
#endif
#ifdef USE_ESPHOME_VOIP_PRESENCE
  PresenceState presence = PresenceState::UNKNOWN;
#endif
  if (!body.empty()) {
    const std::string type = header(message, "Content-Type");
    const char *expected_type = is_dialog ? "application/dialog-info+xml" :
        is_presence ? "application/pidf+xml" : "application/simple-message-summary";
    if (lower(trim(type.substr(0, type.find(';')))) != expected_type) {
      this->reply_(message, 415, ip, port, tcp, flow);
      return true;
    }
#ifdef USE_ESPHOME_VOIP_BLF
    if (is_dialog) {
      auto &watch = this->blf_accounts_[this->watch_slot_(index)];
      const auto parsed = watch.dialogs.apply(body);
      if (parsed == BlfDocumentResult::INVALID) {
        this->reply_(message, 400, ip, port, tcp, flow);
        return true;
      }
      if (parsed == BlfDocumentResult::NEED_FULL || parsed == BlfDocumentResult::LIMIT) {
        watch.dialogs.invalidate();
        if (!watch.full_refresh_requested) {
          watch.needs_full = watch.full_refresh_requested = true;
        }
      } else if (parsed == BlfDocumentResult::APPLIED) {
        watch.needs_full = watch.full_refresh_requested = false;
      }
    } else
#ifdef USE_ESPHOME_VOIP_PRESENCE
    if (is_presence) {
      if (!parse_presence(body, &presence)) {
        this->reply_(message, 400, ip, port, tcp, flow);
        return true;
      }
    } else
#endif
#endif
#ifdef USE_ESPHOME_VOIP_MWI
    if (!this->parse_summary(std::string(body), &next)) {
      this->reply_(message, 400, ip, port, tcp, flow);
      return true;
    }
#else
    { return false; }
#endif
  } else if (state == "active") {
    this->reply_(message, 400, ip, port, tcp, flow);
    return true;
  }
  uint32_t seconds = a.expires;
  const auto expires = parameter(subscription, "expires");
  if (state != "terminated" && !expires.empty() &&
      (!decimal(expires, &seconds) || seconds > 604800)) {
    this->reply_(message, 400, ip, port, tcp, flow);
    return true;
  }
  const std::string contact = contact_uri(header(message, "Contact"));
  if (state != "terminated" && a.remote_tag.empty() && contact.empty()) {
    this->reply_(message, 400, ip, port, tcp, flow);
    return true;
  }
  // Complete the NOTIFY transaction promptly, before invoking any UI callback.
  if (!this->reply_(message, 200, ip, port, tcp, flow))
    return true;
  a.last_notify_id = id;
  a.last_notify_tag = local_tag;
  a.last_notify_remote_tag = remote_tag;
  a.last_notify_branch = branch;
  a.last_notify_cseq = seq;
  a.last_notify_until = now + TRANSACTION_MS;
  a.last_notify_ip = ip;
  a.last_notify_port = port;
  a.last_notify_tcp = tcp;
  a.last_notify_flow = flow;
  if (a.remote_tag.empty()) {
    a.remote_tag = remote_tag;
    a.routes = route_set(message);
  }
  if (!contact.empty())
    a.remote_target = contact;
  a.notify_seen = true;
  a.notify_cseq = seq;
  a.waiting_notify = false;
  if (state == "terminated") {
    this->unavailable_(index);
    const std::string reason = lower(parameter(subscription, "reason"));
    uint32_t retry = 0;
    decimal(parameter(subscription, "retry-after"), &retry);
    if (reason == "rejected" || reason == "noresource" || reason == "invariant")
      a.supported = false;
    this->retry_(index, now, std::min<uint32_t>(retry, 604800) * 1000, true);
    if (a.scheduled && (reason == "deactivated" || reason == "timeout"))
      a.next_ms = now;
  } else {
    const bool retry_pending =
        a.scheduled && a.failures != 0 && !reached(now, a.next_ms);
    const uint32_t retry_deadline = a.next_ms;
    a.subscribed = true;
    a.expires_ms = now + seconds * 1000;
    a.scheduled = !a.pending && (!a.desired || a.supported);
    a.next_ms =
        !a.desired ? now : now + std::max<uint32_t>(1000, seconds * 800);
    if (retry_pending && a.scheduled &&
        static_cast<int32_t>(retry_deadline - a.next_ms) > 0)
      a.next_ms = retry_deadline;
    if (state == "active" && a.desired) {
#ifdef USE_ESPHOME_VOIP_PRESENCE
      if (is_presence) this->publish_presence_(index, presence);
      else
#endif
#ifdef USE_ESPHOME_VOIP_BLF
      if (is_dialog) this->publish_blf_(index);
      else
#endif
#ifdef USE_ESPHOME_VOIP_MWI
        this->publish_(index, std::move(next));
#else
        { return false; }
#endif
    } else
      this->unavailable_(index);
  }
  return true;
}

} // namespace esphome::voip_stack
#endif
