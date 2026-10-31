#include <arpa/inet.h>
#include <openssl/evp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <iostream>
#include <vector>

#include "sip_registration.h"
using namespace esphome::voip_stack;

std::string hash_value(const std::string& algorithm, const std::string& value) {
  const EVP_MD* md = algorithm == "MD5" ? EVP_md5() : EVP_sha256();
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned size = 0;
  assert(EVP_Digest(value.data(), value.size(), digest, &size, md, nullptr) == 1);
  std::string result;
  for (unsigned i = 0; i < size; ++i) {
    char byte[3];
    std::snprintf(byte, sizeof(byte), "%02x", digest[i]);
    result += byte;
  }
  return result;
}
uint32_t random_value() {
  static uint32_t value = 99;
  return ++value;
}
std::string field(const std::string& message, const std::string& name) {
  const std::string key = "\r\n" + name + ": ";
  const auto start = message.find(key);
  if (start == std::string::npos) return "";
  const auto begin = start + key.size();
  return message.substr(begin, message.find("\r\n", begin) - begin);
}
struct Peer {
  bool tcp;
  int esp{-1}, peer{-1};
  uint16_t port{0};
  explicit Peer(bool stream) : tcp(stream) {
    int server = socket(AF_INET, tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
    assert(server >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    assert(getsockname(server, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    this->port = ntohs(address.sin_port);
    this->esp = socket(AF_INET, tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
    assert(esp >= 0);
    if (tcp) assert(listen(server, 1) == 0);
    assert(connect(esp, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    this->peer = tcp ? accept(server, nullptr, nullptr) : server;
    assert(peer >= 0);
    if (tcp) close(server);
    timeval timeout{2, 0};
    assert(setsockopt(esp, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    assert(setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
  }
  ~Peer() {
    close(esp);
    close(peer);
  }
  std::string receive() {
    char buffer[8192];
    int n = recv(peer, buffer, sizeof(buffer), 0);
    assert(n > 0);
    return std::string(buffer, n);
  }
  std::string answer(const std::string& message) {
    sockaddr_in source{};
    socklen_t length = sizeof(source);
    if (!tcp) assert(getsockname(esp, reinterpret_cast<sockaddr*>(&source), &length) == 0);
    const ssize_t n = tcp ? send(peer, message.data(), message.size(), 0)
                          : sendto(peer, message.data(), message.size(), 0,
                                   reinterpret_cast<sockaddr*>(&source), sizeof(source));
    assert(n == static_cast<ssize_t>(message.size()));
    char buffer[8192];
    const int read = recv(esp, buffer, sizeof(buffer), 0);
    assert(read > 0);
    return std::string(buffer, read);
  }
};
std::string response(const std::string& request, unsigned status, const std::string& extra = "") {
  std::string result = "SIP/2.0 " + std::to_string(status) + " Test\r\n";
  for (const char* key : {"Via", "From", "To", "Call-ID", "CSeq"})
    result += std::string(key) + ": " + field(request, key) + "\r\n";
  return result + extra + "Content-Length: 0\r\n\r\n";
}
std::string response_headers(std::string message, const std::string& style) {
  for (const auto& names : {std::pair<const char*, const char*>{"Via", "v"}, {"From", "f"}, {"To", "t"},
                            {"Call-ID", "i"}, {"Contact", "m"}, {"Content-Length", "l"}}) {
    std::string replacement;
    if (style == "mixed") {
      replacement = names.second;
      if (replacement == "f" || replacement == "i" || replacement == "m") replacement[0] -= 'a' - 'A';
    } else if (style == "mixed-long") {
      replacement = names.first;
      for (size_t i = 0; i < replacement.size(); ++i)
        if (i % 2 == 0 && replacement[i] >= 'a' && replacement[i] <= 'z') replacement[i] -= 'a' - 'A';
    } else if (style.size() == 1 && (style[0] | 32) == names.second[0]) {
      replacement = style;
    }
    if (replacement.empty()) continue;
    const std::string original = std::string("\r\n") + names.first + ":";
    const auto start = message.find(original);
    if (start != std::string::npos) message.replace(start + 2, std::string(names.first).size(), replacement);
  }
  return message;
}
struct Harness {
  Peer first, second;
  SipRegistration manager;
  SipRegistration::Hooks hooks;
  std::vector<RegistrationState> states;
  explicit Harness(bool tcp) : first(tcp), second(tcp) {
    hooks.context = this;
    hooks.send = [](void* ctx, size_t i, const std::string& s, uint32_t ip, uint16_t port, bool tcp) {
      auto& self = *static_cast<Harness*>(ctx);
      auto& peer = i == 0 ? self.first : self.second;
      assert(ip == INADDR_LOOPBACK && port == peer.port && tcp == peer.tcp);
      return send(peer.esp, s.data(), s.size(), 0) == static_cast<ssize_t>(s.size());
    };
    hooks.resolve = [](void*, size_t, const std::string&) { return static_cast<uint32_t>(INADDR_LOOPBACK); };
    hooks.local_ip = [](void*, uint32_t) { return std::string("127.0.0.1"); };
    hooks.notify = [](void* ctx, const std::string&, RegistrationState state, uint16_t) {
      static_cast<Harness*>(ctx)->states.push_back(state);
    };
    hooks.random = random_value;
    hooks.hash = hash_value;
    manager.set_hooks(hooks);
  }
  SipAccountConfig config(const std::string& id = "office", unsigned i = 0) {
    SipAccountConfig c;
    c.id = id;
    c.registrar = "pbx.example";
    c.domain = "example.org";
    c.username = i == 0 ? "100" : "101";
    c.password = "test-only-password";
    c.port = i == 0 ? first.port : second.port;
    c.tcp = first.tcp;
    return c;
  }
  void feed(Peer& peer, const std::string& s, uint32_t now = 1, int index = 0) {
    const auto received = peer.answer(s);
    assert(
        manager.handle_response(received, INADDR_LOOPBACK, peer.port, peer.tcp, peer.tcp ? index : -1, now));
  }
  std::string success(const std::string& request, unsigned expires = 3600) {
    auto contact = field(request, "Contact");
    const auto suffix = contact.find(">;");
    if (suffix != std::string::npos) contact.resize(suffix + 1);
    return response(request, 200, "Contact: " + contact + ";expires=" + std::to_string(expires) + "\r\n");
  }
};

int main(int argc, char** argv) {
  assert(argc >= 2);
  std::string scenario = argv[1];
  if (scenario == "digest") {
    assert(argc == 3);
    SipDigest digest;
    SipAccountConfig c;
    c.username = "Mufasa";
    c.password = "Circle Of Life";
    assert(digest.parse(
        std::string("Digest realm=\"testrealm@host.com\", nonce=\"dcd98b7102dd2f0e8b11d0f600bfb0c093\", "
                    "qop=\"auth,auth-int\", algorithm=") +
            argv[2],
        false));
    std::cout << digest.authorization(c, "GET", "/dir/index.html", "0a4f113b", hash_value);
    return 0;
  }
  const bool tcp = argc > 2 && std::string(argv[2]) == "tcp";
  Harness h(tcp);
  auto c = h.config();
  assert(c.valid());
  if (scenario == "async") {
    h.hooks.resolve = [](void*, size_t, const std::string&) { return SipRegistration::RESOLUTION_PENDING; };
    h.manager.set_hooks(h.hooks);
    assert(h.manager.set_account(c, 0));
    h.manager.tick(0);
    assert(h.manager.account(0).resolving && !h.manager.account(0).pending);
    uint32_t wait;
    assert(!h.manager.next_deadline(0, &wait));
    h.manager.resolution_complete(0, 0x7f000002, 1, true);
    h.manager.tick(1);
    assert(h.manager.account(0).resolving && h.manager.account(0).target_ip == 0);
    h.manager.resolution_complete(0, INADDR_LOOPBACK, 2);
    h.manager.tick(2);
    auto request = h.first.receive();
    h.feed(h.first, h.success(request), 3);
    assert(h.manager.is_registered("office"));
    std::cout << "PASS async " << (tcp ? "tcp" : "udp") << "\n";
    return 0;
  }
  if (scenario == "send-rejected") {
    h.hooks.send = [](void*, size_t, const std::string&, uint32_t, uint16_t, bool) { return false; };
    h.manager.set_hooks(h.hooks);
    assert(h.manager.set_account(c, 0)); h.manager.tick(0);
    const auto& account = h.manager.account(0);
    assert(!account.pending && account.request.empty());
    assert(account.state == RegistrationState::RETRY && account.last_status == 503);
    uint32_t remaining = 0; assert(h.manager.next_deadline(0, &remaining));
    assert(remaining == 30000);
    h.hooks.send = [](void* ctx, size_t, const std::string& message, uint32_t, uint16_t, bool) {
      auto& self = *static_cast<Harness*>(ctx);
      return send(self.first.esp, message.data(), message.size(), 0) == static_cast<ssize_t>(message.size());
    };
    h.manager.set_hooks(h.hooks); h.manager.tick(30000);
    auto request = h.first.receive(); h.feed(h.first, h.success(request), 30001);
    assert(h.manager.is_registered("office"));
    std::cout << "PASS send-rejected " << (tcp ? "tcp" : "udp") << "\n"; return 0;
  }
  assert(h.manager.set_account(c, 0));
  h.manager.tick(0);
  auto initial = h.first.receive();
  assert(initial.rfind("REGISTER sip:pbx.example:", 0) == 0);
  assert(field(initial, "From").find("100@example.org") != std::string::npos);
  if (scenario == "headers") {
    assert(argc == 5);
    const std::string style = argv[3];
    const auto check_identity = [&](const std::string& message, uint32_t now) {
      for (const std::string& identity : {h.manager.account(0).branch, h.manager.account(0).call_id}) {
        auto wrong = message;
        const auto start = wrong.find(identity);
        assert(start != std::string::npos);
        wrong.replace(start, identity.size(), identity + "-wrong");
        const auto sequence = h.manager.account(0).cseq;
        h.feed(h.first, response_headers(wrong, style), now);
        assert(!h.manager.is_registered("office") && h.manager.account(0).pending);
        assert(h.manager.account(0).cseq == sequence);
      }
    };
    if (std::string(argv[4]) == "auth") {
      const auto challenge = response(initial, 401,
          "WWW-Authenticate: Digest realm=\"pbx\",nonce=\"headers\",algorithm=SHA-256,qop=\"auth\"\r\n");
      check_identity(challenge, 1);
      h.feed(h.first, response_headers(challenge, style), 2);
      initial = h.first.receive();
      assert(field(initial, "Authorization").find("algorithm=SHA-256") != std::string::npos);
      assert(field(initial, "CSeq") == "2 REGISTER");
    }
    const auto accepted = h.success(initial, 90);
    check_identity(accepted, 3);
    h.feed(h.first, response_headers(accepted, style), 4);
    assert(h.manager.is_registered("office") && !h.manager.account(0).pending);
    assert(h.manager.account(0).negotiated_expires == 90);
  } else if (scenario == "auth") {
    const bool proxy = argc > 3 && std::string(argv[3]) == "proxy";
    h.feed(
        h.first,
        response(
            initial, proxy ? 407 : 401,
            std::string(proxy ? "Proxy-Authenticate" : "WWW-Authenticate") +
                ": Digest "
                "realm=\"pbx\",nonce=\"nonce-one\",algorithm=SHA-256,qop=\"auth\",opaque=\"one,two\"\r\n"));
    const auto authenticated = h.first.receive();
    assert(field(authenticated, proxy ? "Proxy-Authorization" : "Authorization").find("opaque=\"one,two\"") !=
           std::string::npos);
    assert(field(authenticated, "Call-ID") == field(initial, "Call-ID"));
    assert(field(authenticated, "CSeq") == "2 REGISTER");
    h.feed(h.first, h.success(authenticated), 2);
    assert(h.manager.is_registered("office"));
    uint32_t wait = 0;
    assert(h.manager.next_deadline(2, &wait) && wait >= 2700000 && wait <= 2880000);
    h.manager.tick(2880002);
    auto refresh = h.first.receive();
    assert(field(refresh, "CSeq") == "3 REGISTER");
    h.feed(h.first, h.success(refresh), 2880003);
    assert(h.manager.request_registration("office", false, 2880004));
    h.manager.tick(2880004);
    auto unregister = h.first.receive();
    assert(field(unregister, "Expires") == "0");
    assert(field(unregister, "Contact").find("*:") == std::string::npos);
    h.feed(h.first, h.success(unregister, 0), 2880005);
    assert(h.manager.account(0).state == RegistrationState::DISABLED);
  } else if (scenario == "identity") {
    auto wrong = response(initial, 200, "Contact: " + field(initial, "Contact") + "\r\n");
    wrong.replace(wrong.find("1 REGISTER"), 10, "9 REGISTER");
    h.feed(h.first, wrong);
    assert(!h.manager.is_registered("office") && h.manager.account(0).pending);
    assert(h.manager.handle_response(h.success(initial), 0x7f000002, h.first.port, tcp, tcp ? 0 : -1, 2));
    assert(!h.manager.is_registered("office"));
    h.feed(h.first, h.success(initial), 3);
    assert(h.manager.is_registered("office"));
    assert(h.manager.match_incoming("esphome-office", INADDR_LOOPBACK, tcp, tcp ? 0 : -1) == 0);
    assert(h.manager.match_incoming("101", INADDR_LOOPBACK, tcp, tcp ? 0 : -1) < 0);
  } else if (scenario == "minimum") {
    h.feed(h.first, response(initial, 423, "Min-Expires: 7200\r\n"));
    auto next = h.first.receive();
    assert(field(next, "Expires") == "7200");
    h.feed(h.first, h.success(next, 7200), 2);
    assert(h.manager.is_registered("office"));
  } else if (scenario == "contacts") {
    std::string own = field(initial, "Contact");
    own.resize(own.find(">;") + 1);
    const auto at = own.find(";transport=");
    own.replace(at, 14, tcp ? ";transport=TCP" : ";transport=UDP");
    h.feed(h.first, response(initial, 200,
                             "Contact: <sip:another@127.0.0.1:9999>;expires=300\r\nContact: " + own +
                                 ";expires=60\r\n"));
    assert(h.manager.is_registered("office") && h.manager.account(0).negotiated_expires == 60);
  } else if (scenario == "retry") {
    h.feed(h.first, response(initial, 503, "Retry-After: 120\r\n"));
    assert(h.manager.account(0).state == RegistrationState::RETRY);
    uint32_t wait;
    assert(h.manager.next_deadline(1, &wait) && wait == 120000);
    h.manager.tick(120001);
    assert(h.first.receive().rfind("REGISTER ", 0) == 0);
  } else if (scenario == "timeout") {
    if (!tcp) {
      h.manager.tick(500);
      assert(h.first.receive() == initial);
    }
    h.manager.tick(32000);
    assert(h.manager.account(0).state == RegistrationState::RETRY);
  } else if (scenario == "deadline") {
    if (!tcp) {
      for (unsigned when : {500, 1500, 3500, 7500, 11500, 15500, 19500, 23500, 27500, 31500}) {
        h.manager.tick(when);
        h.first.receive();
      }
    }
    uint32_t wait;
    assert(h.manager.next_deadline(31500, &wait) && wait == 500);
    h.manager.tick(32000);
    assert(h.manager.account(0).state == RegistrationState::RETRY);
  } else if (scenario == "dual-auth") {
    h.feed(
        h.first,
        response(initial, 407,
                 "Proxy-Authenticate: Digest realm=\"proxy\",nonce=\"one\",algorithm=MD5,qop=\"auth\"\r\n"));
    auto next = h.first.receive();
    assert(!field(next, "Proxy-Authorization").empty());
    h.feed(
        h.first,
        response(
            next, 401,
            "WWW-Authenticate: Digest realm=\"registrar\",nonce=\"two\",algorithm=MD5\r\nWWW-Authenticate: "
            "Digest realm=\"registrar\",nonce=\"three\",algorithm=SHA-256,qop=\"auth\"\r\n"),
        2);
    next = h.first.receive();
    assert(field(next, "Authorization").find("algorithm=SHA-256") != std::string::npos);
    assert(!field(next, "Proxy-Authorization").empty());
    h.feed(h.first, h.success(next), 3);
    assert(h.manager.is_registered("office"));
  } else if (scenario == "multiple") {
    assert(h.manager.set_account(h.config("home", 1), 0));
    h.manager.tick(0);
    auto other = h.second.receive();
    h.feed(h.second, h.success(other), 1, 1);
    h.feed(h.first, h.success(initial), 2);
    assert(h.manager.is_registered("home") && h.manager.is_registered("office"));
    if (tcp) {
      h.manager.connection_lost(0, 3);
      assert(h.manager.account(0).state == RegistrationState::RETRY);
      assert(h.manager.is_registered("office") && h.manager.is_registered("home"));
    }
  } else if (scenario == "same-user") {
    auto other_config = h.config("home", 1); other_config.username = c.username;
    assert(h.manager.set_account(other_config, 0)); h.manager.tick(0); auto other = h.second.receive();
    h.feed(h.first, h.success(initial), 1); h.feed(h.second, h.success(other), 2, 1);
    assert(h.manager.is_registered("office") && h.manager.is_registered("home"));
    assert(h.manager.match_incoming("esphome-office", INADDR_LOOPBACK, tcp, tcp ? 0 : -1) == 0);
    assert(h.manager.match_incoming("esphome-home", INADDR_LOOPBACK, tcp, tcp ? 1 : -1) == 1);
    assert(h.manager.match_incoming(c.username, INADDR_LOOPBACK, tcp, tcp ? 0 : -1) < 0);
  } else if (scenario == "refresh-binding") {
    h.feed(h.first, h.success(initial, 60), 1);
    h.manager.tick(48001);
    auto refresh = h.first.receive();
    assert(h.manager.is_registered("office") && h.manager.account(0).pending);
    assert(h.manager.match_incoming("esphome-office", INADDR_LOOPBACK, tcp, tcp ? 0 : -1) == 0);
    h.feed(h.first, response(refresh, 503), 48002);
    assert(h.manager.is_registered("office"));
    uint32_t wait;
    assert(h.manager.next_deadline(48002, &wait) && wait == 11999);
    h.manager.tick(60001);
    assert(!h.manager.is_registered("office"));
    assert(h.manager.match_incoming("esphome-office", INADDR_LOOPBACK, tcp, tcp ? 0 : -1) < 0);
  } else if (scenario == "intent") {
    assert(h.manager.request_registration("office", false, 1));
    h.feed(h.first, h.success(initial), 2);
    h.manager.tick(2);
    auto last = h.first.receive();
    assert(field(last, "Expires") == "0");
    h.feed(h.first, h.success(last, 0), 3);
    assert(h.manager.account(0).state == RegistrationState::DISABLED);
  } else if (scenario == "unauthorized") {
    auto challenge = "WWW-Authenticate: Digest realm=\"x\", nonce=\"bad\", qop=\"auth\"\r\n";
    for (unsigned i = 0; i < 3; ++i) {
      h.feed(h.first, response(initial, 401, challenge), i + 1);
      if (i != 2) initial = h.first.receive();
    }
    assert(h.manager.account(0).state == RegistrationState::FAILED);
    assert(h.manager.request_registration("office", true, 9));
    h.manager.tick(9);
    h.first.receive();
  } else if (scenario == "invalid") {
    for (auto field : {std::string("a\r\nb"), std::string("direct"), std::string(33, 'x')}) {
      auto bad = c;
      bad.id = field;
      assert(!bad.valid());
    }
    c.domain = "invalid:5060";
    assert(!c.valid());
    SipDigest digest;
    assert(!digest.parse("Digest nonce=\"x\",qop=\"auth-int\"", false));
  } else
    assert(false);
  std::cout << "PASS " << scenario << " " << (tcp ? "tcp" : "udp") << "\n";
  return 0;
}
