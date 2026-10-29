#pragma once

#include "esphome/core/defines.h"
#ifdef USE_ESPHOME_VOIP_REGISTRATION

#include <array>
#include <cstdint>
#include <string>
#ifdef USE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM
#include "sip_signaling_string.h"
#endif

#ifndef VOIP_REGISTRATION_CAPACITY
#define VOIP_REGISTRATION_CAPACITY 2
#endif

namespace esphome::voip_stack {
#ifdef USE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM
using RegistrationMessage = SipSignalingString;
#else
using RegistrationMessage = std::string;
#endif

// REGISTER owns a binding, never a call/dialog or a media lifecycle.
struct SipAccountConfig {
  std::string id, registrar, domain, username, password, auth_username, proxy;
  uint16_t port{5060}, proxy_port{5060};
  uint32_t expires{3600};
  bool tcp{false}, enabled{true};
  bool valid() const;
};

// Immutable YAML baseline. Strings and rows have firmware lifetime in Flash;
// runtime account state and persisted overrides continue to own their values.
struct SipAccountConfigView {
  const char *id, *registrar, *domain, *username, *password, *auth_username, *proxy;
  uint16_t port, proxy_port;
  uint32_t expires;
  bool tcp, enabled;

  SipAccountConfig materialize() const {
    return {id, registrar, domain, username, password, auth_username, proxy,
            port, proxy_port, expires, tcp, enabled};
  }
};

enum class RegistrationState : uint8_t { DISABLED, REGISTERING, REGISTERED, UNREGISTERING, RETRY, FAILED };
const char* registration_state_name(RegistrationState state);

struct SipDigest {
  using Hash = std::string (*)(const std::string& algorithm, const std::string& input);
  std::string realm, nonce, opaque, algorithm{"MD5"}, qop;
  uint32_t nonce_count{0};
  bool proxy{false}, stale{false};
  bool parse(const std::string& header, bool proxy_auth);
  bool parse_response(const std::string& message, bool proxy_auth);
  std::string authorization(const SipAccountConfig& account, const std::string& method,
                            const std::string& uri, const std::string& cnonce, Hash hash);
  bool empty() const { return this->nonce.empty(); }
};

class SipRegistration {
 public:
  static constexpr size_t CAPACITY = VOIP_REGISTRATION_CAPACITY;
  static_assert(CAPACITY >= 1 && CAPACITY <= 4, "Registration capacity must be between one and four");
  struct Hooks {
    void* context{nullptr};
    bool (*send)(void*, size_t, const std::string&, uint32_t, uint16_t, bool){nullptr};
    uint32_t (*resolve)(void*, size_t, const std::string&){nullptr};
    std::string (*local_ip)(void*, uint32_t){nullptr};
    void (*notify)(void*, const std::string&, RegistrationState, uint16_t){nullptr};
    uint32_t (*random)(){nullptr};
    SipDigest::Hash hash{nullptr};
    uint16_t local_port{5060};
  };
  struct Account {
    SipAccountConfig config;
#ifdef USE_ESPHOME_VOIP_MWI
    uint32_t config_revision{0};
#endif
    RegistrationState state{RegistrationState::DISABLED};
    SipDigest digest, proxy_digest;
    std::string call_id, tag, branch, contact;
    RegistrationMessage request;
    uint32_t cseq{0}, target_ip{0}, next_ms{0}, deadline_ms{0}, interval_ms{500}, negotiated_expires{0},
        requested_expires{0};
    uint32_t binding_expires_ms{0}, binding_ip{0};
    uint16_t target_port{0}, last_status{0};
    uint8_t auth_attempts{0}, proxy_auth_attempts{0}, failures{0};
    bool occupied{false}, desired{false}, pending{false}, scheduled{false}, resolving{false}, bound{false};
  };

  void set_hooks(const Hooks& hooks) { this->hooks_ = hooks; }
  int find(const std::string& id) const;
  bool set_account(const SipAccountConfig& config, uint32_t now);
  bool remove_account(const std::string& id);
  bool request_registration(const std::string& id, bool enabled, uint32_t now);
  bool is_registered(const std::string& id) const;
  void tick(uint32_t now);
  bool next_deadline(uint32_t now, uint32_t* remaining) const;
  bool handle_response(const std::string& message, uint32_t source_ip, uint16_t source_port, bool tcp,
                       int flow_index, uint32_t now);
  void connection_lost(size_t index, uint32_t now);
  static constexpr uint32_t RESOLUTION_PENDING = UINT32_MAX;
  void resolution_complete(size_t index, uint32_t ip, uint32_t now, bool obsolete = false);
  int match_incoming(const std::string& to_user, uint32_t source_ip, bool tcp, int flow_index) const;
  const Account& account(size_t index) const { return this->accounts_[index]; }
  Account& account(size_t index) { return this->accounts_[index]; }

 protected:
  std::array<Account, CAPACITY> accounts_{};
  Hooks hooks_{};
  bool begin_(size_t index, uint32_t now);
  void retry_(size_t index, uint16_t status, uint32_t now, uint32_t delay_ms = 0);
  void state_(size_t index, RegistrationState state, uint16_t status);
  std::string token_();
};

}  // namespace esphome::voip_stack
#endif
