#pragma once

#include "esphome/core/defines.h"
#if defined(USE_ESPHOME_VOIP_REGISTRATION) && (defined(USE_ESPHOME_VOIP_MWI) || defined(USE_ESPHOME_VOIP_BLF))

#include "sip_registration.h"
#include "message_waiting.h"
#ifdef USE_ESPHOME_VOIP_BLF
#include "sip_blf.h"
#ifdef USE_ESP32
#include <esp_heap_caps.h>
#endif
#endif

#include <array>
#include <cstdint>
#include <memory>
#include <new>
#include <string>

namespace esphome::voip_stack {

// RFC 3842 message-summary client. The SIP worker owns this bounded pool and
// drives its deadlines. Registration, calls, REFER and PCM retain their owners.
class SipMwi {
public:
  static constexpr size_t CAPACITY = SipRegistration::CAPACITY;
#ifdef USE_ESPHOME_VOIP_MWI
  static constexpr size_t MWI_CAPACITY = CAPACITY;
#else
  static constexpr size_t MWI_CAPACITY = 0;
#endif
  struct Hooks {
    void *context{nullptr};
    bool (*send)(void *, size_t, const std::string &, uint32_t, uint16_t,
                 bool){nullptr};
    // Reply on the received transport/flow, never a new connection.
    bool (*reply)(void *, const std::string &, uint32_t, uint16_t, bool,
                  int){nullptr};
    std::string (*local_ip)(void *, uint32_t){nullptr};
    void (*notify)(void *, const MessageWaitingState &){nullptr};
#ifdef USE_ESPHOME_VOIP_BLF
    void (*notify_blf)(void *, const BlfStateInfo &){nullptr};
#endif
    uint32_t (*random)(){nullptr};
    SipDigest::Hash hash{nullptr};
    uint16_t local_port{5060};
  };
  void set_hooks(const Hooks &hooks) { this->hooks_ = hooks; }
  void update_account(size_t index, const SipAccountConfig &config,
                      uint32_t target_ip, uint16_t target_port,
                      const std::string &contact, bool bound, bool desired,
                      uint32_t now);
  // The registration pool owns stable slots. Invalidate before replacing a
  // slot under the transport's dialog lock, then borrow its current revision.
  void update_registered_account(size_t index, const SipRegistration::Account &account, uint32_t now);
  void on_registration_accepted(size_t index, const SipRegistration::Account &account, uint32_t now);
  void invalidate_account(size_t index);
  void tick(uint32_t now);
  bool next_deadline(uint32_t now, uint32_t *remaining) const;
  bool handle_response(const std::string &message, uint32_t source_ip,
                       uint16_t source_port, bool tcp, int flow_index,
                       uint32_t now);
  bool handle_notify(const std::string &message, uint32_t source_ip,
                     uint16_t source_port, bool tcp, int flow_index,
                     uint32_t now);
  void connection_lost(size_t index, uint32_t now);
#ifdef USE_ESPHOME_VOIP_BLF
  bool configure_blf(uint8_t capacity);
  bool set_blf_watch(size_t index, size_t account_index, const SipRegistration::Account &account,
                     const std::string &id, const std::string &target, uint32_t now);
  void update_blf_account(size_t index, const SipRegistration::Account &account, uint32_t now);
  void clear_blf_watch(size_t index, uint32_t now);
  void invalidate_blf_account(size_t account_index);
  const BlfStateInfo *blf_state(size_t index) const;
  const std::string &blf_watch_id(size_t index) const;
  bool blf_watch_requested(size_t index) const;
  uint8_t blf_capacity() const { return this->blf_capacity_; }
#endif
  const MessageWaitingState &state(size_t index) const {
    return this->account_(index).state;
  }
  static bool parse_summary(const std::string &body,
                            MessageWaitingState *result);

protected:
  struct Account {
#ifdef USE_ESPHOME_VOIP_BLF
    bool dialog{false};
    uint8_t account_index{0};
#endif
    const SipAccountConfig *config_{nullptr};
    // Only standalone C++ callers of update_account need an owned copy.
    std::unique_ptr<SipAccountConfig> owned_config;
    uint32_t config_revision{0};
    const SipAccountConfig &config() const { return *this->config_; }
    MessageWaitingState state;
    SipDigest digest, proxy_digest;
    RegistrationMessage request;
    std::string contact, call_id, tag, remote_tag, branch, remote_target,
        routes;
    std::string last_notify_id, last_notify_tag, last_notify_remote_tag,
        last_notify_branch;
    uint32_t last_notify_cseq{0}, last_notify_until{0}, last_notify_ip{0};
    uint16_t last_notify_port{0};
    bool last_notify_tcp{false};
    int last_notify_flow{-1};
    uint32_t target_ip{0}, cseq{0}, notify_cseq{0}, next_ms{0}, deadline_ms{0},
        interval_ms{500}, notify_deadline_ms{0}, expires_ms{0},
        requested_expires{3600}, expires{3600};
    uint16_t target_port{0};
    uint8_t auth_attempts{0}, proxy_auth_attempts{0}, failures{0};
    bool occupied{false}, desired{false}, bound{false}, pending{false},
        scheduled{false}, subscribed{false}, waiting_notify{false},
        notify_seen{false}, supported{true}, request_in_dialog{false}, refresh_after_register{false};
  };
  std::array<Account, MWI_CAPACITY> accounts_{};
#ifdef USE_ESPHOME_VOIP_BLF
  struct BlfWatch : Account {
#ifdef USE_ESP32
    static void *operator new[](size_t size, const std::nothrow_t &) noexcept {
#ifdef USE_ESPHOME_VOIP_BLF_PSRAM
      return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
      return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
    }
    static void operator delete[](void *pointer) noexcept { heap_caps_free(pointer); }
    static void operator delete[](void *pointer, const std::nothrow_t &) noexcept { heap_caps_free(pointer); }
#endif
    std::string watch_target;
    BlfStateInfo blf;
    BlfDialogCache dialogs;
    std::string pending_account, pending_id, pending_target;
    uint8_t pending_account_index{0};
    bool replace_pending{false};
    bool needs_full{false}, full_refresh_requested{false};
  };
  std::unique_ptr<BlfWatch[]> blf_accounts_;
  uint8_t blf_capacity_{0};
  void publish_blf_(size_t index);
  void begin_blf_watch_(size_t slot, size_t account_index, const SipRegistration::Account &account,
                         const std::string &id, const std::string &target, uint32_t now);
  void retire_blf_watch_(size_t slot, uint32_t now);
#endif
  size_t total_capacity_() const {
#ifdef USE_ESPHOME_VOIP_BLF
    return MWI_CAPACITY + this->blf_capacity_;
#else
    return MWI_CAPACITY;
#endif
  }
  size_t owner_index_(size_t index) const {
#ifdef USE_ESPHOME_VOIP_BLF
    return this->account_(index).account_index;
#else
    return index;
#endif
  }
  Account &account_(size_t index) {
#ifdef USE_ESPHOME_VOIP_BLF
    if (index >= MWI_CAPACITY) return this->blf_accounts_[index - MWI_CAPACITY];
#endif
    return this->accounts_[index];
  }
  const Account &account_(size_t index) const {
#ifdef USE_ESPHOME_VOIP_BLF
    if (index >= MWI_CAPACITY) return this->blf_accounts_[index - MWI_CAPACITY];
#endif
    return this->accounts_[index];
  }
  Hooks hooks_{};
  void update_account_(size_t index, const SipAccountConfig &config, uint32_t revision,
                       uint32_t target_ip, uint16_t target_port, const std::string &contact,
                       bool bound, bool desired, uint32_t now);
  bool begin_(size_t index, uint32_t now);
  static bool registration_refresh_ready_(const Account &account, uint32_t now);
  void retry_(size_t index, uint32_t now, uint32_t delay_ms = 0,
              bool new_dialog = false);
  void publish_(size_t index, const MessageWaitingState &state);
  void unavailable_(size_t index);
  void reset_dialog_(size_t index);
  bool reply_(const std::string &request, uint16_t status, uint32_t ip,
              uint16_t port, bool tcp, int flow);
  std::string token_();
};

} // namespace esphome::voip_stack
#endif
