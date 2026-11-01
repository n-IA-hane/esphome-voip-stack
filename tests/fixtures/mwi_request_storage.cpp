// Host C++ new/delete accounting; external is the signaling allocator policy.
// Fixed-size hash isolates request serialization, not crypto implementation
// costs.

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
namespace allocation_probe {
thread_local bool external = false;
static bool active = false;
static size_t live_i = 0, live_e = 0, base_i = 0, base_e = 0, peak_i = 0,
              peak_e = 0, alloc_i = 0, alloc_e = 0;
struct alignas(std::max_align_t) Header {
  size_t bytes;
  bool external;
};
void start() {
  active = true;
  base_i = peak_i = live_i;
  base_e = peak_e = live_e;
  alloc_i = alloc_e = 0;
}
void observe() {
  if (active) {
    if (live_i > peak_i)
      peak_i = live_i;
    if (live_e > peak_e)
      peak_e = live_e;
  }
}
} // namespace allocation_probe
void *operator new(size_t n) {
  auto *h = static_cast<allocation_probe::Header *>(
      std::malloc(sizeof(allocation_probe::Header) + n));
  if (!h)
    throw std::bad_alloc();
  h->bytes = n;
  h->external = allocation_probe::external;
  if (h->external) {
    allocation_probe::live_e += n;
    if (allocation_probe::active)
      ++allocation_probe::alloc_e;
  } else {
    allocation_probe::live_i += n;
    if (allocation_probe::active)
      ++allocation_probe::alloc_i;
  }
  allocation_probe::observe();
  return h + 1;
}
void operator delete(void *p) noexcept {
  if (!p)
    return;
  auto *h = static_cast<allocation_probe::Header *>(p) - 1;
  if (h->external)
    allocation_probe::live_e -= h->bytes;
  else
    allocation_probe::live_i -= h->bytes;
  std::free(h);
}
void *operator new[](size_t n) { return ::operator new(n); }
void operator delete[](void *p) noexcept { ::operator delete(p); }
void operator delete(void *p, size_t) noexcept { ::operator delete(p); }
void operator delete[](void *p, size_t) noexcept { ::operator delete(p); }
#include "sip_mwi.h"
using namespace esphome::voip_stack;
struct Client;
struct Capture {
  Client *owner = nullptr;
  char data[8192]{};
  size_t size = 0;
  unsigned sends = 0;
  bool fail = false;
};
struct Client : SipMwi {
  using SipMwi::begin_;
  auto &row() { return accounts_[0]; }
};
std::string field(std::string_view data, std::string_view name) {
  const std::string key = "\r\n" + std::string(name) + ": ";
  auto p = data.find(key);
  assert(p != std::string_view::npos);
  p += key.size();
  return std::string(data.substr(p, data.find("\r\n", p) - p));
}
std::string response(std::string_view req) {
  std::string s = "SIP/2.0 200 OK\r\n";
  for (const char *name : {"Via", "From", "To", "Call-ID", "CSeq"}) {
    auto v = field(req, name);
    if (std::string_view(name) == "To" && v.find(";tag=") == std::string::npos)
      v += ";tag=server";
    s += std::string(name) + ": " + v + "\r\n";
  }
  return s + "Expires: 3600\r\nContent-Length: 0\r\n\r\n";
}
uint64_t hash_wire(const Capture &c) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < c.size; ++i) {
    h ^= static_cast<unsigned char>(c.data[i]);
    h *= 1099511628211ULL;
  }
  return h;
}
void run(unsigned auth, bool refresh, bool fail) {
  SipRegistration::Account owner;
  owner.config.id = "ha";
  owner.config.username = "waveshare-s3-touch-lcd-185c-v2";
  owner.config.registrar = owner.config.domain = "192.168.1.200";
  owner.config.password = "fixture-only-not-a-real-secret";
  owner.config.port = 5060;
  owner.config.expires = 3600;
  owner.config_revision = 1;
  owner.occupied = owner.desired = owner.bound = true;
  owner.target_ip = 0xc0a801c8;
  owner.target_port = 5060;
  owner.contact = "sip:waveshare-s3-touch-lcd-185c-v2@192.168.1.192:5060";
  Client c;
  Capture sent;
  sent.owner = &c;
  sent.fail = fail;
  SipMwi::Hooks hooks;
  hooks.context = &sent;
  hooks.local_port = 5060;
  hooks.random = []() { return uint32_t(0x12345678); };
  hooks.hash = [](const std::string &, const std::string &) {
    return std::string(32, 'a');
  };
  hooks.local_ip = [](void *, uint32_t) {
    return std::string("192.168.1.192");
  };
  hooks.send = [](void *ctx, size_t, std::string_view msg, uint32_t, uint16_t,
                  bool) {
    auto &s = *static_cast<Capture *>(ctx);
    assert(msg.size() == s.owner->row().request.size());
    if (s.sends == 0)
      assert(msg.data() == s.owner->row().request.data());
    else
      assert(std::memcmp(s.data, msg.data(), msg.size()) == 0);
    assert(msg.size() < sizeof(s.data));
    s.size = msg.size();
    ++s.sends;
    std::memcpy(s.data, msg.data(), s.size);
    return !s.fail;
  };
  c.set_hooks(hooks);
  c.update_registered_account(0, owner, 0);
  auto &row = c.row();
  const size_t empty_capacity = row.request.capacity();
  if (refresh) {
    row.call_id = "mwi-stable-usage@192.168.1.192";
    row.tag = "stable-local-tag";
    row.remote_tag = "server";
    row.remote_target = "sip:mailbox@192.168.1.200";
    row.subscribed = true;
    row.notify_seen = true;
    row.expires_ms = 100000;
  }
  if (auth & 1)
    assert(row.digest.parse(
        "Digest realm=\"fixture-realm\", nonce=\"fixture-nonce-original\", "
        "algorithm=MD5, qop=\"auth\"",
        false));
  if (auth & 2)
    assert(row.proxy_digest.parse(
        "Digest realm=\"fixture-proxy\", nonce=\"fixture-nonce-proxy\", "
        "algorithm=MD5, qop=\"auth\"",
        true));
  allocation_probe::start();
  const bool ok = c.begin_(0, 1000);
  allocation_probe::active = false;
  assert(ok == !fail);
  const size_t pi = allocation_probe::peak_i - allocation_probe::base_i,
               pe = allocation_probe::peak_e - allocation_probe::base_e,
               ai = allocation_probe::alloc_i, ae = allocation_probe::alloc_e,
               cap = row.request.capacity();
  if (ok) {
    // The existing retransmit path materializes a synchronous temporary.
    // Its bytes remain identical and the transaction store remains owned.
    const char *owned = row.request.data();
    c.tick(1500);
    assert(sent.sends == 2 && row.request.data() == owned);
    auto r = response(std::string_view(sent.data, sent.size));
    assert(c.handle_response(r, owner.target_ip, 5060, false, -1, 1501));
  }
  assert(row.request.empty() && row.request.capacity() == empty_capacity);
  c.invalidate_account(0);
  assert(row.request.capacity() == empty_capacity);
  std::printf(
      "{\"auth\":%u,\"refresh\":%s,\"send_error\":%s,\"wire_bytes\":%zu,\"wire_"
      "hash\":\"%llu\",\"peak_internal_cpp_bytes_above_entry\":%zu,\"peak_"
      "external_cpp_bytes_above_entry\":%zu,\"internal_cpp_allocations\":%zu,"
      "\"external_cpp_allocations\":%zu,\"pending_request_capacity\":%zu,"
      "\"capacity_after_completion\":%zu,\"empty_capacity\":%zu}\n",
      auth, refresh ? "true" : "false", fail ? "true" : "false", sent.size,
      static_cast<unsigned long long>(hash_wire(sent)), pi, pe, ai, ae, cap,
      row.request.capacity(), empty_capacity);
}
int main() {
  for (unsigned auth = 0; auth < 4; ++auth)
    for (bool refresh : {false, true})
      run(auth, refresh, false);
  run(3, true, true);
}
