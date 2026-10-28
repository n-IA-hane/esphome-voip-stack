#include "sip_transport.h"
#if defined(USE_ESP32) && defined(USE_ESPHOME_VOIP_SIP_TRANSPORT) && defined(USE_ESPHOME_VOIP_REGISTRATION)
#include <esp_system.h>
#include <lwip/dns.h>
#include <lwip/tcpip.h>
#include <mbedtls/md.h>

#include <algorithm>
#include <cstring>

#include "esphome/core/hal.h"
#include "sip_message.h"

namespace esphome::voip_stack {
namespace {
// lwIP may complete a lookup after the transport has stopped. These bounded
// contexts therefore have static lifetime; the guarded owner is detached at
// stop, and a generation prevents a removed/reconfigured account receiving an
// earlier lookup. No lookup ever waits inside the SIP task.
struct RegistrationDnsContext {
  SipTransport* owner{nullptr};
  void (*wake)(SipTransport*){nullptr};
  char hostname[254]{};
  uint32_t generation{0}, request_generation{0}, result_ip{0};
  bool pending{false}, ready{false}, obsolete{false};
};
std::array<RegistrationDnsContext, SipRegistration::CAPACITY> dns_contexts;
StaticSemaphore_t dns_mutex_storage;
SemaphoreHandle_t dns_mutex{nullptr};

void registration_dns_found(const char*, const ip_addr_t* address, void* arg) {
  auto& ctx = *static_cast<RegistrationDnsContext*>(arg);
  xSemaphoreTake(dns_mutex, portMAX_DELAY);
  ctx.pending = false;
  if (ctx.owner != nullptr) {
    ctx.result_ip = address != nullptr && IP_IS_V4(address) ? ntohl(ip4_addr_get_u32(ip_2_ip4(address))) : 0;
    ctx.obsolete = ctx.request_generation != ctx.generation;
    ctx.ready = true;
    // This callback runs under lwIP's TCPIP core lock. Publish to the component
    // owner only: BSD socket calls here would recursively acquire that lock.
    ctx.wake(ctx.owner);
  }
  xSemaphoreGive(dns_mutex);
}

void registration_dns_start(void* arg) {
  auto& ctx = *static_cast<RegistrationDnsContext*>(arg);
  xSemaphoreTake(dns_mutex, portMAX_DELAY);
  const bool current = ctx.owner != nullptr && ctx.request_generation == ctx.generation;
  xSemaphoreGive(dns_mutex);
  if (!current) {
    registration_dns_found(nullptr, nullptr, arg);
    return;
  }
  ip_addr_t result;
  const err_t error =
      dns_gethostbyname_addrtype(ctx.hostname, &result, registration_dns_found, arg, LWIP_DNS_ADDRTYPE_IPV4);
  if (error == ERR_OK)
    registration_dns_found(ctx.hostname, &result, arg);
  else if (error != ERR_INPROGRESS)
    registration_dns_found(ctx.hostname, nullptr, arg);
}
}  // namespace

void SipTransport::initialize_registration() {
  if (dns_mutex == nullptr) dns_mutex = xSemaphoreCreateMutexStatic(&dns_mutex_storage);
  xSemaphoreTake(dns_mutex, portMAX_DELAY);
  for (auto& ctx : dns_contexts) {
    ++ctx.generation;
    ctx.owner = this;
    ctx.ready = false;
    ctx.wake = [](SipTransport* owner) {
      if (owner->registration_wake_callback_ != nullptr)
        owner->registration_wake_callback_(owner->registration_context_, owner);
    };
  }
  xSemaphoreGive(dns_mutex);
  SipRegistration::Hooks hooks;
  hooks.context = this;
  hooks.send = registration_send_;
  hooks.resolve = registration_resolve_;
  hooks.local_ip = registration_local_ip_;
  hooks.notify = registration_notify_;
  hooks.random = esp_random;
  hooks.hash = registration_hash_;
  hooks.local_port = this->sip_port_;
  this->registration_.set_hooks(hooks);
#ifdef USE_ESPHOME_VOIP_MWI
  SipMwi::Hooks mwi;
  mwi.context = this; mwi.send = registration_send_; mwi.reply = registration_mwi_reply_;
  mwi.local_ip = registration_local_ip_; mwi.notify = registration_mwi_notify_;
  mwi.random = esp_random; mwi.hash = registration_hash_; mwi.local_port = this->sip_port_;
  this->mwi_.set_hooks(mwi);
#endif
}

#ifdef USE_ESPHOME_VOIP_MWI
bool SipTransport::park_inbound_control_(int socket, const sockaddr_in &source) {
  if (this->inbound_control_flow_.socket.load(std::memory_order_acquire) >= 0) return false;
  bool trusted = false;
  {
    LockGuard lock(this->dialog_mutex_);
    for (size_t i = 0; i < SipRegistration::CAPACITY; ++i) {
      const auto &a = this->registration_.account(i);
      trusted |= a.occupied && a.bound && a.config.tcp && a.binding_ip == ntohl(source.sin_addr.s_addr);
    }
  }
  if (!trusted) return false;
  this->inbound_control_flow_.socket.store(socket, std::memory_order_release);
  this->inbound_control_flow_.peer_ip.store(ntohl(source.sin_addr.s_addr), std::memory_order_release);
  this->inbound_control_flow_.rx_buffer.clear();
  this->inbound_control_deadline_ = millis() + SIP_TRANSACTION_TIMEOUT_MS;
  return true;
}

void SipTransport::pump_registration_mwi_(uint32_t now) {
  for (size_t i = 0; i < SipRegistration::CAPACITY; ++i) {
    const auto &account = this->registration_.account(i);
    this->mwi_.update_account(i, account.config, account.target_ip, account.target_port, account.contact,
        account.occupied && account.bound, account.occupied && account.desired, now);
  }
  this->mwi_.tick(now);
}

void SipTransport::registration_mwi_notify_(void *ctx, const MessageWaitingState &state) {
  auto *self = static_cast<SipTransport *>(ctx);
  if (self->message_waiting_callback_ != nullptr) self->message_waiting_callback_(self->registration_context_, state);
}

bool SipTransport::registration_mwi_reply_(void *ctx, const std::string &message, uint32_t ip, uint16_t port,
                                          bool tcp, int) {
  auto *self = static_cast<SipTransport *>(ctx);
  if (!tcp) return self->send_sip_udp_bytes_(message.data(), message.size(), ip, port);
  LockGuard send_lock(self->tcp_send_mutex_);
  return self->send_sip_tcp_record_(message, self->dispatch_socket_);
}

bool SipTransport::get_message_waiting(const std::string &id, MessageWaitingState *state) {
  LockGuard lock(this->dialog_mutex_);
  const int index = this->registration_.find(id);
  if (index < 0 || this->mwi_.state(index).account_id != id) return false;
  *state = this->mwi_.state(index); return true;
}

bool SipTransport::has_waiting_messages(const std::string &id) {
  LockGuard lock(this->dialog_mutex_);
  for (size_t i = 0; i < SipRegistration::CAPACITY; ++i) {
    const auto &account = this->registration_.account(i);
    const auto &state = this->mwi_.state(i);
    if (account.occupied && (id.empty() || id == account.config.id) && state.account_id == account.config.id &&
        state.available && state.waiting) return true;
  }
  return false;
}

#endif

std::string SipTransport::registration_hash_(const std::string& algorithm, const std::string& input) {
  const auto type = algorithm == "MD5"       ? MBEDTLS_MD_MD5
                    : algorithm == "SHA-256" ? MBEDTLS_MD_SHA256
                                             : MBEDTLS_MD_NONE;
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(type);
  if (info == nullptr) return "";
  unsigned char digest[MBEDTLS_MD_MAX_SIZE];
  if (mbedtls_md(info, reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest) != 0)
    return "";
  static constexpr char HEX[] = "0123456789abcdef";
  std::string result;
  result.reserve(mbedtls_md_get_size(info) * 2);
  for (size_t i = 0; i < mbedtls_md_get_size(info); ++i) {
    result += HEX[digest[i] >> 4];
    result += HEX[digest[i] & 15];
  }
  return result;
}

uint32_t SipTransport::registration_resolve_(void* owner, size_t index, const std::string& host) {
  in_addr address{};
  if (inet_aton(host.c_str(), &address) != 0) return ntohl(address.s_addr);
  if (index >= dns_contexts.size() || host.size() >= sizeof(dns_contexts[0].hostname)) return 0;
  auto& ctx = dns_contexts[index];
  xSemaphoreTake(dns_mutex, portMAX_DELAY);
  if (ctx.owner != owner) {
    xSemaphoreGive(dns_mutex);
    return 0;
  }
  if (ctx.pending) {
    xSemaphoreGive(dns_mutex);
    return SipRegistration::RESOLUTION_PENDING;
  }
  std::memcpy(ctx.hostname, host.c_str(), host.size() + 1);
  ctx.request_generation = ctx.generation;
  ctx.pending = true;
  ctx.ready = false;
  xSemaphoreGive(dns_mutex);
  if (tcpip_try_callback(registration_dns_start, &ctx) != ERR_OK) {
    xSemaphoreTake(dns_mutex, portMAX_DELAY);
    ctx.pending = false;
    xSemaphoreGive(dns_mutex);
    return 0;
  }
  return SipRegistration::RESOLUTION_PENDING;
}

void SipTransport::invalidate_registration_dns_(size_t index) {
  if (dns_mutex == nullptr || index >= dns_contexts.size()) return;
  xSemaphoreTake(dns_mutex, portMAX_DELAY);
  ++dns_contexts[index].generation;
  dns_contexts[index].ready = false;
  xSemaphoreGive(dns_mutex);
}
void SipTransport::cancel_registration_dns_() {
  if (dns_mutex == nullptr) return;
  xSemaphoreTake(dns_mutex, portMAX_DELAY);
  for (auto& ctx : dns_contexts)
    if (ctx.owner == this) {
      ctx.owner = nullptr;
      ++ctx.generation;
      ctx.ready = false;
    }
  xSemaphoreGive(dns_mutex);
}
void SipTransport::pump_registration_dns_() {
  xSemaphoreTake(dns_mutex, portMAX_DELAY);
  for (size_t i = 0; i < dns_contexts.size(); ++i) {
    auto& ctx = dns_contexts[i];
    if (ctx.owner != this || !ctx.ready) continue;
    this->registration_.resolution_complete(i, ctx.result_ip, millis(), ctx.obsolete);
    ctx.ready = false;
  }
  xSemaphoreGive(dns_mutex);
}

std::string SipTransport::registration_local_ip_(void* ctx, uint32_t ip) {
  std::string result;
  static_cast<SipTransport*>(ctx)->local_ip_for_peer_(ip, &result);
  return result;
}
void SipTransport::registration_notify_(void* ctx, const std::string& id, RegistrationState state,
                                        uint16_t status) {
  auto* self = static_cast<SipTransport*>(ctx);
  if (self->registration_callback_ != nullptr)
    self->registration_callback_(self->registration_context_, id, state, status);
}

bool SipTransport::registration_send_(void* ctx, size_t index, const std::string& message, uint32_t ip,
                                      uint16_t port, bool tcp) {
  auto* self = static_cast<SipTransport*>(ctx);
  if (!tcp) return self->send_sip_udp_bytes_(message.data(), message.size(), ip, port);
  auto& flow = self->registration_flows_[index];
  LockGuard send_lock(self->tcp_send_mutex_);
  const int socket = flow.socket.load(std::memory_order_acquire);
  if (socket >= 0 && !flow.close_requested.load(std::memory_order_acquire))
    return self->send_sip_tcp_record_(message, socket);
  LockGuard pending_lock(self->tcp_tx_pending_mutex_);
  if (!flow.pending.empty()) return false;
  flow.pending.assign(message.data(), message.size());
  if (flow.connecting_socket < 0) {
    flow.target_ip.store(ip, std::memory_order_release);
    flow.target_port.store(port, std::memory_order_release);
    flow.connect_requested.store(true, std::memory_order_release);
  }
  self->wake_sip_task_();
  return true;
}

std::array<SipTransport::SipTcpFlow*, 2 + SipRegistration::CAPACITY> SipTransport::signaling_flows_() {
  std::array<SipTcpFlow*, 2 + SipRegistration::CAPACITY> result{};
  result[0] = &this->tcp_flow_;
  result[1] = this->refer_dialog_ ? &this->refer_dialog_->tcp_flow_ : nullptr;
  for (size_t i = 0; i < SipRegistration::CAPACITY; ++i) result[2 + i] = &this->registration_flows_[i];
  return result;
}

int SipTransport::registration_flow_index_(const SipTcpFlow* flow) const {
  for (size_t i = 0; i < SipRegistration::CAPACITY; ++i)
    if (flow == &this->registration_flows_[i]) return i;
  return -1;
}
int SipTransport::registration_socket_index_(int socket) const {
  if (socket < 0) return -1;
  for (size_t i = 0; i < SipRegistration::CAPACITY; ++i)
    if (socket == this->registration_flows_[i].socket.load(std::memory_order_acquire)) return i;
  return -1;
}
bool SipTransport::account_in_use_(int i) const {
  return i >= 0 && (this->active_account_ == i || this->next_account_ == i ||
                    (this->refer_dialog_ && this->refer_dialog_->active_account_ == i) ||
                    this->terminal_transaction_pending_locked_());
}
void SipTransport::retire_registration_flow_(size_t index) {
  // Retire only records owned by the failed/replaced account generation.
  // close_tcp_flow_ also runs before a new connect and must preserve its record.
  auto& flow = this->registration_flows_[index];
  LockGuard send_lock(this->tcp_send_mutex_);
  flow.connect_requested.store(false, std::memory_order_release);
  flow.close_requested.store(true, std::memory_order_release);
  LockGuard pending_lock(this->tcp_tx_pending_mutex_);
  flow.pending.clear();
}
bool SipTransport::set_account(const SipAccountConfig& config, AccountCommit commit, void* context) {
  LockGuard lock(this->dialog_mutex_);
  const int i = this->registration_.find(config.id);
  if (this->account_in_use_(i) || !config.valid()) return false;
  // Retire a registered binding explicitly before replacing credentials or
  // registrar. A successful unregister leaves the same slot reusable.
  if (i >= 0 && this->registration_.account(i).bound) return false;
  if (i >= 0 && this->registration_.account(i).state != RegistrationState::DISABLED &&
      this->registration_.account(i).state != RegistrationState::FAILED)
    return false;
  if (i < 0) {
    bool space = false;
    for (size_t j = 0; j < SipRegistration::CAPACITY; ++j)
      if (!this->registration_.account(j).occupied) space = true;
    if (!space) return false;
  }
  if (commit != nullptr && !commit(context, config.id, &config)) return false;
  if (i >= 0) this->retire_registration_flow_(i);
  if (i >= 0) this->invalidate_registration_dns_(i);
  const bool result = this->registration_.set_account(config, millis());
  this->wake_sip_task_();
  return result;
}
bool SipTransport::remove_account(const std::string& id, AccountCommit commit, void* context, bool allow_missing) {
  LockGuard lock(this->dialog_mutex_);
  const int i = this->registration_.find(id);
  if (i < 0) return allow_missing && commit != nullptr && commit(context, id, nullptr);
  if (this->account_in_use_(i) || this->registration_.account(i).pending ||
      this->registration_.account(i).bound)
    return false;
  if (commit != nullptr && !commit(context, id, nullptr)) return false;
  const bool result = this->registration_.remove_account(id);
  this->invalidate_registration_dns_(i);
  this->retire_registration_flow_(i);
  this->wake_sip_task_();
  return result;
}
bool SipTransport::register_account(const std::string& id, bool enabled) {
  LockGuard lock(this->dialog_mutex_);
  const int i = this->registration_.find(id);
  if (i < 0) return false;
  const bool result = this->registration_.request_registration(id, enabled, millis());
  this->wake_sip_task_();
  return result;
}
bool SipTransport::is_registered(const std::string& id) {
  LockGuard lock(this->dialog_mutex_);
  return this->registration_.is_registered(id);
}
bool SipTransport::prepare_account_line(const std::string& id, std::string* host, uint16_t* port, bool* tcp) {
  LockGuard lock(this->dialog_mutex_);
  const int i = this->registration_.find(id);
  if (i < 0 || !this->call_id_.empty() || this->terminal_transaction_pending_locked_() ||
      !this->registration_.is_registered(id))
    return false;
  const auto& a = this->registration_.account(i);
  if (a.target_ip == 0) return false;
  this->next_account_ = i;
  in_addr addr{};
  addr.s_addr = htonl(a.target_ip);
  char text[16];
  inet_ntoa_r(addr, text, sizeof(text));
  *host = text;
  *port = a.target_port;
  *tcp = a.config.tcp;
  return true;
}
void SipTransport::clear_account_line() {
  LockGuard lock(this->dialog_mutex_);
  this->next_account_ = -1;
}

bool SipTransport::registration_ingress_(const std::string& message, const sockaddr_in& src, int socket) {
  size_t declared_length = 0;
  const size_t separator = message.find("\r\n\r\n");
  if (!sip_content_length(message, &declared_length) || separator == std::string::npos ||
      declared_length != message.size() - separator - 4) return false;
  const int flow = this->registration_socket_index_(socket);
#ifdef USE_ESPHOME_VOIP_MWI
  const uint32_t ip = ntohl(src.sin_addr.s_addr); const uint16_t port = ntohs(src.sin_port);
  if (this->mwi_.handle_response(message, ip, port, socket >= 0, flow, millis()) ||
      this->mwi_.handle_notify(message, ip, port, socket >= 0, flow, millis())) return true;
#endif
  if (message.rfind("SIP/2.0 ", 0) == 0 && cseq_method(header_value(message, "CSeq")) == "REGISTER") {
    return this->registration_.handle_response(message, ntohl(src.sin_addr.s_addr), ntohs(src.sin_port),
                                               socket >= 0, flow, millis());
  }
  return false;
}

bool SipTransport::retry_authenticated_request_(const std::string& message, const std::string& method,
                                                uint16_t status) {
  if (this->active_account_ < 0 || (status != 401 && status != 407))
    return false;
  // The challenged INVITE is a completed transaction. ACK it before starting
  // a new CSeq/branch, retaining the dialog identity but not its remote tag.
  SipDigest challenge;
  uint8_t& attempts = status == 401 ? this->dialog_auth_attempts_ : this->dialog_proxy_auth_attempts_;
  if (!challenge.parse_response(message, status == 407) || ++attempts > (challenge.stale ? 3 : 2))
    return false;
  (status == 401 ? this->dialog_digest_ : this->dialog_proxy_digest_) = std::move(challenge);
  std::string body;
  SipRequestOptions options;
  if (method == "INVITE") {
    this->remote_tag_.clear();
    this->clear_invite_transaction_();
    this->branch_ = "z9hG4bK" + make_token("");
    this->invite_cseq_ = this->cseq_++;
    options.cseq_number = this->invite_cseq_;
    body = this->authenticated_invite_body_;
  } else if (method == "BYE")
    this->pending_bye_.clear();
  else if (method == "UPDATE") {
    body = message_body(this->pending_update_.request.str());
    this->pending_update_.clear();
  } else
    return false;
  if (!this->send_request_(method, body, options)) {
    SipSignal signal;
    signal.type = SipSignalType::PROTOCOL_ERROR;
    signal.status_code = 503;
    signal.call_id = this->call_id_;
    this->emit_sip_signal_(std::move(signal));
    this->reset_dialog_();
  }
  return true;
}

void SipTransport::remember_registration_routes_(const std::string& message, bool reverse) {
  this->dialog_route_count_ = 0;
  size_t pos = message.find("\r\n");
  while (pos != std::string::npos) {
    const size_t end = message.find("\r\n", pos + 2);
    if (end == std::string::npos || end == pos + 2) break;
    const std::string line = message.substr(pos + 2, end - pos - 2);
    const size_t colon = line.find(':');
    if (colon != std::string::npos) {
      std::string key = line.substr(0, colon);
      for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (key == "record-route") {
        size_t offset = colon + 1;
        while (offset < line.size() && this->dialog_route_count_ < this->dialog_routes_.size()) {
          const size_t close = line.find('>', offset);
          const size_t comma = line.find(',', close == std::string::npos ? offset : close);
          const std::string route =
              trim_copy(line.substr(offset, comma == std::string::npos ? comma : comma - offset));
          if (!route.empty()) this->dialog_routes_[this->dialog_route_count_++] = route;
          if (comma == std::string::npos) break;
          offset = comma + 1;
        }
      }
    }
    pos = end;
  }
  if (reverse)
    std::reverse(this->dialog_routes_.begin(), this->dialog_routes_.begin() + this->dialog_route_count_);
}

}  // namespace esphome::voip_stack
#endif
