#include "sip_transport.h"
#include "sip_message.h"
#include "transport_host_sdk.h"
#include "socket_reservations.h"
#include <poll.h>

using namespace esphome::voip_stack;

struct Phone : SipTransport {
  using SipTransport::SipTransport;
  struct ObservedAccount { uint32_t revision; SipAccountConfig config; };
  ObservedAccount account() {
    esphome::LockGuard lock(dialog_mutex_);
    return {registration_.account(0).config_revision, registration_.account(0).config};
  }
  bool retrying() {
    esphome::LockGuard lock(dialog_mutex_);
    const auto &account = registration_.account(0);
    return account.state == RegistrationState::RETRY && !account.pending && !account.bound;
  }
};

struct Peer {
  bool tcp;
  int listener, fd{-1};
  sockaddr_in address{}, sender{};
  std::string buffered;
  explicit Peer(bool stream) : tcp(stream), listener(socket(AF_INET, stream ? SOCK_STREAM : SOCK_DGRAM, 0)) {
    assert(listener >= 0);
    address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    assert(getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length) == 0);
    if (tcp) assert(listen(listener, 2) == 0); else fd = listener;
  }
  ~Peer() { if (tcp && fd >= 0) close(fd); close(listener); }
  void accept_generation() {
    if (!tcp) return;
    if (fd >= 0) close(fd);
    pollfd ready{listener, POLLIN, 0}; assert(poll(&ready, 1, 2000) == 1);
    fd = accept(listener, nullptr, nullptr); assert(fd >= 0); buffered.clear();
  }
  std::string take() {
    for (;;) {
      const auto end = buffered.find("\r\n\r\n");
      if (end != buffered.npos) {
        const size_t size = end + 4 + std::stoul(header_value(buffered, "Content-Length"));
        if (buffered.size() >= size) {
          auto record = buffered.substr(0, size); buffered.erase(0, size); return record;
        }
      }
      pollfd ready{fd, POLLIN, 0}; assert(poll(&ready, 1, 2000) == 1);
      char bytes[8192]; socklen_t length = sizeof(sender);
      const auto count = tcp ? recv(fd, bytes, sizeof(bytes), 0) :
          recvfrom(fd, bytes, sizeof(bytes), 0, reinterpret_cast<sockaddr *>(&sender), &length);
      assert(count > 0); buffered.append(bytes, count);
    }
  }
  void reply(const std::string &request, unsigned status) {
    std::string message = "SIP/2.0 " + std::to_string(status) + " Reply\r\nVia: " + header_value(request, "Via") +
        "\r\nFrom: " + header_value(request, "From") + "\r\nTo: " + header_value(request, "To") +
        "\r\nCall-ID: " + header_value(request, "Call-ID") + "\r\nCSeq: " + header_value(request, "CSeq") + "\r\n";
    if (status == 401 || status == 407) message += std::string(status == 407 ? "Proxy-Authenticate" : "WWW-Authenticate") +
        ": Digest realm=\"pbx\", nonce=\"fixture\", algorithm=MD5, qop=\"auth\"\r\n";
    if (status == 200) message += "Contact: " + header_value(request, "Contact") + "\r\n";
    else message += "Retry-After: 47\r\n";
    message += "Content-Length: 0\r\n\r\n";
    const auto count = tcp ? send(fd, message.data(), message.size(), 0) :
        sendto(fd, message.data(), message.size(), 0, reinterpret_cast<sockaddr *>(&sender), sizeof(sender));
    assert(count == static_cast<ssize_t>(message.size()));
  }
};

template<class F> static void eventually(F predicate) {
  for (unsigned i = 0; i < 400; ++i) { if (predicate()) return; delay(5); }
  assert(predicate());
}

int main(int argc, char **argv) {
  assert(argc == 3);
  Peer peer(std::string(argv[1]) == "tcp");
  const unsigned rejection = std::stoul(argv[2]);
  ReservedPorts sip(ReservedPorts::Kind::SIP), media(ReservedPorts::Kind::UDP);
  Phone phone(sip.port(), media.port(), 1200, "127.0.0.1", false);
  phone.initialize_registration();
  SipAccountConfig config;
  config.id = "office"; config.registrar = "127.0.0.1"; config.domain = "pbx.example";
  config.port = ntohs(peer.address.sin_port); config.tcp = peer.tcp;
  config.username = "100"; config.password = "old-test-password";
  assert(phone.set_account(config));
  sip.release(); media.release(); assert(phone.start());
  peer.accept_generation();
  auto request = peer.take();
  auto replacement = config; replacement.password = "corrected-test-password";
  assert(!phone.set_account(replacement));  // In-flight REGISTER still owns its credentials.
  for (unsigned i = 0; i < (rejection == 403 ? 1U : 3U); ++i) {
    peer.reply(request, rejection);
    if (i < 2 && rejection != 403) request = peer.take();
  }
  eventually([&] { return phone.retrying(); });
  const auto before = phone.account();
  assert(!phone.set_account(replacement, [](void *, const std::string &, const SipAccountConfig *) { return false; }, nullptr));
  auto after = phone.account();
  assert(before.revision == after.revision && after.config.password == config.password);
  assert(phone.set_account(replacement));
  peer.accept_generation();
  const auto fresh = peer.take();
  assert(header_value(fresh, "Call-ID") != header_value(request, "Call-ID"));
  peer.reply(fresh, 401);
  const auto authenticated = peer.take();
  assert(!header_value(authenticated, "Authorization").empty());
  peer.reply(authenticated, 200);
  eventually([&] { return phone.is_registered("office"); });
  after = phone.account();
  assert(after.revision != before.revision && after.config.password == replacement.password);
  assert(!phone.set_account(config));  // A live binding must still unregister first.
  phone.stop(); reap_host_tasks();
  assert(video_probe::tasks == 0 && video_probe::allocated == video_probe::freed);
}
