#include "sip_mwi.h"
#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <iostream>
#include <openssl/evp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace esphome::voip_stack;
std::string hash_value(const std::string &algorithm, const std::string &input) {
  const EVP_MD *md = algorithm == "MD5" ? EVP_md5() : EVP_sha256();
  unsigned char bytes[EVP_MAX_MD_SIZE];
  unsigned size = 0;
  assert(EVP_Digest(input.data(), input.size(), bytes, &size, md, nullptr) ==
         1);
  std::string result;
  for (unsigned n = 0; n < size; ++n) {
    char hex[3];
    std::snprintf(hex, sizeof(hex), "%02x", bytes[n]);
    result += hex;
  }
  return result;
}
std::string field(const std::string &text, const std::string &key) {
  const auto at = text.find("\r\n" + key + ": ");
  if (at == std::string::npos)
    return "";
  const auto start = at + key.size() + 4;
  return text.substr(start, text.find("\r\n", start) - start);
}
uint32_t random_value() {
  static uint32_t n = 123;
  return ++n;
}
struct Peer {
  bool tcp;
  int client{-1}, remote{-1};
  uint16_t port{0};
  explicit Peer(bool stream, bool incoming = false) : tcp(stream) {
    int listener = socket(AF_INET, tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
    assert(listener >= 0);
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(listener, reinterpret_cast<sockaddr *>(&dst), sizeof(dst)) ==
           0);
    socklen_t size = sizeof(dst);
    assert(getsockname(listener, reinterpret_cast<sockaddr *>(&dst), &size) ==
           0);
    port = ntohs(dst.sin_port);
    if (tcp && incoming) {
      assert(listen(listener, 1) == 0);
      remote = socket(AF_INET, SOCK_STREAM, 0);
      assert(connect(remote, reinterpret_cast<sockaddr *>(&dst), sizeof(dst)) ==
             0);
      client = accept(listener, nullptr, nullptr);
      close(listener);
      assert(client >= 0 && remote >= 0);
      size = sizeof(dst);
      assert(getsockname(remote, reinterpret_cast<sockaddr *>(&dst), &size) ==
             0);
      port = ntohs(dst.sin_port);
      timeval timeout{1, 0};
      setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      setsockopt(remote, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      return;
    }
    if (tcp)
      assert(listen(listener, 1) == 0);
    client = socket(AF_INET, tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
    assert(client >= 0);
    assert(connect(client, reinterpret_cast<sockaddr *>(&dst), sizeof(dst)) ==
           0);
    remote = tcp ? accept(listener, nullptr, nullptr) : listener;
    if (tcp)
      close(listener);
    assert(remote >= 0);
    timeval timeout{1, 0};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(remote, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  }
  ~Peer() {
    close(client);
    close(remote);
  }
  std::string receive() {
    char buf[8192];
    int n = recv(remote, buf, sizeof(buf), 0);
    assert(n > 0);
    return {buf, size_t(n)};
  }
  std::string deliver(const std::string &text) {
    sockaddr_in dst{};
    socklen_t len = sizeof(dst);
    assert(getsockname(client, reinterpret_cast<sockaddr *>(&dst), &len) == 0);
    const auto n = tcp ? send(remote, text.data(), text.size(), 0)
                       : sendto(remote, text.data(), text.size(), 0,
                                reinterpret_cast<sockaddr *>(&dst), len);
    assert(n == ssize_t(text.size()));
    char buf[8192];
    int got = recv(client, buf, sizeof(buf), 0);
    assert(got > 0);
    return {buf, size_t(got)};
  }
  bool quiet() {
    char c;
    return recv(remote, &c, 1, MSG_DONTWAIT) < 0 &&
           (errno == EAGAIN || errno == EWOULDBLOCK);
  }
};
std::string response(const std::string &request, unsigned status,
                     const std::string &extra = "",
                     const std::string &remote_tag = "server") {
  std::string r = "SIP/2.0 " + std::to_string(status) + " Test\r\n";
  for (const char *name : {"Via", "From", "To", "Call-ID", "CSeq"}) {
    auto value = field(request, name);
    if (std::string(name) == "To" && value.find(";tag=") == std::string::npos)
      value += ";tag=" + remote_tag;
    r += std::string(name) + ": " + value + "\r\n";
  }
  return r + extra + "Content-Length: 0\r\n\r\n";
}
std::string
notify(const std::string &request, unsigned cseq = 1,
       const std::string &body =
           "Messages-Waiting: yes\r\nVoice-Message: 2/4 (1/2)\r\n",
       const std::string &substate = "active;expires=60",
       const std::string &type = "application/simple-message-summary",
       const std::string &server_tag = "server",
       const std::string &extra = "") {
  return "NOTIFY " +
         field(request, "Contact")
             .substr(1, field(request, "Contact").size() - 2) +
         " SIP/2.0\r\nVia: SIP/2.0/UDP 127.0.0.1;branch=z9hG4bKnotify" +
         std::to_string(cseq) +
         ";rport\r\nFrom: <sip:mailbox@example.test>;tag=" + server_tag +
         "\r\nTo: " + field(request, "From") +
         "\r\nCall-ID: " + field(request, "Call-ID") +
         "\r\nCSeq: " + std::to_string(cseq) +
         " NOTIFY\r\nEvent: message-summary\r\nSubscription-State: " +
         substate + "\r\nContact: <sip:mailbox@127.0.0.1>\r\n" + extra +
         "Content-Type: " + type +
         "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" +
         body;
}
struct Harness {
  Peer first, second;
  SipMwi client;
  SipMwi::Hooks hooks;
  std::vector<MessageWaitingState> updates;
  unsigned replies = 0;
  Peer *reply_peer{nullptr};
  SipAccountConfig config;
  explicit Harness(bool tcp) : first(tcp), second(tcp) {
    hooks.context = this;
    hooks.local_port = 5060;
    hooks.random = random_value;
    hooks.hash = hash_value;
    hooks.local_ip = [](void *, uint32_t) { return std::string("127.0.0.1"); };
    hooks.send = [](void *ctx, size_t index, const std::string &msg,
                    uint32_t ip, uint16_t port, bool stream) {
      auto &h = *static_cast<Harness *>(ctx);
      auto &p = index == 0 ? h.first : h.second;
      assert(ip == INADDR_LOOPBACK && port == p.port && stream == p.tcp);
      return send(p.client, msg.data(), msg.size(), 0) == ssize_t(msg.size());
    };
    hooks.reply = [](void *ctx, const std::string &msg, uint32_t, uint16_t port,
                     bool, int) {
      auto &h = *static_cast<Harness *>(ctx);
      auto &p = h.reply_peer != nullptr
                    ? *h.reply_peer
                    : (port == h.second.port ? h.second : h.first);
      h.replies++;
      return send(p.client, msg.data(), msg.size(), 0) == ssize_t(msg.size());
    };
    hooks.notify = [](void *ctx, const MessageWaitingState &s) {
      static_cast<Harness *>(ctx)->updates.push_back(s);
    };
    client.set_hooks(hooks);
    config.id = "desk";
    config.registrar = "127.0.0.1";
    config.domain = "example.test";
    config.username = "alice";
    config.password = "test-password";
    config.tcp = tcp;
    config.port = first.port;
  }
  std::string start(uint32_t now = 0, bool boot_enabled = true) {
    config.enabled = boot_enabled;
    client.update_account(0, config, INADDR_LOOPBACK, first.port,
                          "sip:alice@127.0.0.1:5060", true, true, now);
    client.tick(now);
    return first.receive();
  }
  void answer(const std::string &r, uint32_t now = 1) {
    assert(client.handle_response(first.deliver(r), INADDR_LOOPBACK, first.port,
                                  first.tcp, 0, now));
  }
  std::string notification(const std::string &n, uint32_t now = 2,
                           uint32_t ip = INADDR_LOOPBACK, uint16_t port = 0,
                           int flow = 0) {
    assert(client.handle_notify(first.deliver(n), ip, port ? port : first.port,
                                first.tcp, flow, now));
    return first.receive();
  }
  std::string active() {
    auto sub = start();
    answer(response(sub, 200, "Expires: 60\r\n"));
    assert(notification(notify(sub)).rfind("SIP/2.0 200", 0) == 0);
    return sub;
  }
};
int main(int argc, char **argv) {
  assert(argc >= 2);
  const std::string scenario = argv[1];
  bool tcp = argc > 2 && std::string(argv[2]) == "tcp";
  if (scenario == "parse") {
    std::string body{std::istreambuf_iterator<char>(std::cin), {}};
    MessageWaitingState s;
    bool ok = SipMwi::parse_summary(body, &s);
    std::cout << ok << ' ' << s.waiting << ' ' << s.new_messages << ' '
              << s.old_messages << ' ' << s.urgent_new << ' ' << s.urgent_old
              << '\n';
    return 0;
  }
  Harness h(tcp);
  if (scenario == "legacy-temporary") {
    h.client.connection_lost(0, 0);
    h.client.update_account(0, SipAccountConfig(h.config), INADDR_LOOPBACK, h.first.port,
                            "sip:alice@127.0.0.1:5060", true, true, 0);
    h.config.username = "mutated";
    h.config.password.assign(512, 'x');
    h.client.tick(0);
    auto sub = h.first.receive();
    assert(field(sub, "From").find("sip:alice@") != std::string::npos);
    h.answer(response(sub, 200, "Expires: 60\r\n"));
    assert(h.notification(notify(sub)).rfind("SIP/2.0 200", 0) == 0);
  } else if (scenario == "borrowed-refresh" || scenario == "borrowed-password" ||
             scenario == "borrowed-remove-reuse" || scenario == "borrowed-revision") {
    SipRegistration owner;
    assert(owner.set_account(h.config, 0));
    const auto bind = [&](uint32_t now) {
      auto &account = owner.account(0);
      account.bound = true;
      account.target_ip = INADDR_LOOPBACK;
      account.target_port = h.first.port;
      account.contact = "sip:" + account.config.username + "@127.0.0.1:5060";
      h.client.update_registered_account(0, account, now);
      h.client.tick(now);
      return h.first.receive();
    };
    auto sub = bind(0);
    if (scenario == "borrowed-refresh") {
      h.answer(response(sub, 200, "Expires: 60\r\n"));
      assert(h.notification(notify(sub)).rfind("SIP/2.0 200", 0) == 0);
      h.client.tick(48002);
      auto refresh = h.first.receive();
      assert(field(refresh, "Call-ID") == field(sub, "Call-ID"));
      h.answer(response(refresh, 200, "Expires: 60\r\n"), 48003);
      assert(h.notification(notify(refresh, 2), 48004).rfind("SIP/2.0 200", 0) == 0);
      owner.account(0).desired = false;
      h.client.update_registered_account(0, owner.account(0), 48005);
      h.client.tick(48005);
      auto unsubscribe = h.first.receive();
      assert(field(unsubscribe, "Expires") == "0");
      assert(h.notification(notify(unsubscribe, 3, "", "terminated;reason=timeout"), 48006)
                 .rfind("SIP/2.0 200", 0) == 0);
      assert(!h.client.state(0).available);
    } else {
      if (scenario == "borrowed-remove-reuse") {
        h.answer(response(sub, 200, "Expires: 60\r\n"));
        assert(h.notification(notify(sub)).rfind("SIP/2.0 200", 0) == 0);
      }
      const auto revision = owner.account(0).config_revision;
      // This is the same ordering used by SipTransport under dialog_mutex_.
      // An advanced caller may update the stable pool and then reconcile in
      // the same critical section. Revision comparison must also detect it.
      if (scenario != "borrowed-revision") h.client.invalidate_account(0);
      assert(!h.client.state(0).available);
      if (scenario == "borrowed-password" || scenario == "borrowed-revision") {
        h.config.password = "new-test-password";
        assert(owner.set_account(h.config, 10));
      } else {
        assert(owner.remove_account(h.config.id));
        h.client.update_registered_account(0, owner.account(0), 10);
        h.client.tick(10);
        assert(h.first.quiet());
        h.config.id = "replacement";
        h.config.username = "bob";
        assert(owner.set_account(h.config, 11));
      }
      assert(owner.account(0).config_revision != revision);
      auto fresh = bind(12);
      assert(field(fresh, "Call-ID") != field(sub, "Call-ID"));
      // A delayed challenge and a replay of the old NOTIFY cannot touch the
      // replacement generation, including the cached successful NOTIFY case.
      assert(!h.client.handle_response(h.first.deliver(response(sub, 401,
          "WWW-Authenticate: Digest realm=\"example.test\", nonce=\"old\", qop=\"auth\"\r\n")),
          INADDR_LOOPBACK, h.first.port, tcp, 0, 13));
      assert(h.notification(notify(sub), 14).rfind("SIP/2.0 481", 0) == 0);
      h.answer(response(fresh, 401,
          "WWW-Authenticate: Digest realm=\"example.test\", nonce=\"new\", qop=\"auth\"\r\n"), 15);
      auto authorized = h.first.receive();
      const auto auth = field(authorized, "Authorization");
      assert(!auth.empty());
      const auto cnonce_at = auth.find("cnonce=\"");
      assert(cnonce_at != std::string::npos);
      const auto cnonce_end = auth.find('"', cnonce_at + 8);
      const auto cnonce = auth.substr(cnonce_at + 8, cnonce_end - cnonce_at - 8);
      const auto ha1 = hash_value("MD5", h.config.username + ":example.test:" + h.config.password);
      const auto ha2 = hash_value("MD5", "SUBSCRIBE:sip:" + h.config.username + "@example.test");
      const auto expected = hash_value("MD5", ha1 + ":new:00000001:" + cnonce + ":auth:" + ha2);
      assert(auth.find("response=\"" + expected + '"') != std::string::npos);
      assert(field(authorized, "From").find("sip:" + h.config.username + "@") != std::string::npos);
      h.answer(response(authorized, 200, "Expires: 60\r\n"), 16);
      assert(h.notification(notify(authorized), 17).rfind("SIP/2.0 200", 0) == 0);
      assert(h.client.state(0).available && h.client.state(0).account_id == h.config.id);
    }
  } else if (scenario == "auth" || scenario == "digest" || scenario == "wire") {
    auto sub = h.start();
    assert(field(sub, "Event") == "message-summary" &&
           field(sub, "Accept") == "application/simple-message-summary");
    const std::string algorithm =
        scenario == "digest" && argc > 2 ? argv[2] : "MD5";
    h.answer(response(sub, 401,
                      "WWW-Authenticate: Digest realm=\"example.test\", "
                      "nonce=\"nonce-one\", qop=\"auth\", algorithm=" +
                          algorithm + "\r\n"));
    auto authorized = h.first.receive();
    assert(field(authorized, "Authorization").find("Digest ") == 0);
    assert(field(authorized, "Call-ID") == field(sub, "Call-ID"));
    assert(field(authorized, "CSeq") != field(sub, "CSeq"));
    if (scenario == "digest") {
      std::cout << field(authorized, "Authorization") << '\n';
      return 0;
    }
    h.answer(
        response(authorized, 407,
                 "Proxy-Authenticate: Digest realm=\"proxy.test\", "
                 "nonce=\"nonce-two\", qop=\"auth\", algorithm=SHA-256\r\n"));
    auto both = h.first.receive();
    assert(!field(both, "Authorization").empty() &&
           !field(both, "Proxy-Authorization").empty());
    if (scenario == "wire") {
      std::cout << both;
      return 0;
    }
    h.answer(response(both, 200, "Expires: 60\r\n"));
    assert(!h.client.state(0).available);
    assert(h.notification(notify(both)).rfind("SIP/2.0 200", 0) == 0);
    assert(h.client.state(0).waiting && h.client.state(0).new_messages == 2);
  } else if (scenario == "early") {
    auto sub = h.start();
    assert(h.notification(
                notify(sub, 1,
                       "Messages-Waiting: yes\r\nVoice-Message: 3/2 (1/0)\r\n"))
               .rfind("SIP/2.0 200", 0) == 0);
    assert(h.client.state(0).new_messages == 3);
    h.answer(response(sub, 200, "Expires: 3600\r\n"));
    h.client.tick(48002);
    auto refreshed = h.first.receive();
    assert(field(refreshed, "To").find(";tag=server") != std::string::npos);
    assert(refreshed.find("SUBSCRIBE sip:mailbox@127.0.0.1 ") == 0);
  } else if (scenario == "identity") {
    auto sub = h.active();
    auto n = notify(sub, 2);
    const auto count = h.updates.size();
    assert(h.notification(n, 3, 0x7f000002).rfind("SIP/2.0 481", 0) == 0);
    if (tcp)
      assert(
          h.notification(n, 4, INADDR_LOOPBACK, 0, 1).rfind("SIP/2.0 481", 0) ==
          0);
    auto wrong = n;
    auto at = wrong.find(";tag=", wrong.find("\r\nTo:"));
    wrong.insert(at + 5, "wrong");
    assert(h.notification(wrong, 5).rfind("SIP/2.0 481", 0) == 0);
    assert(h.notification(notify(sub, 2, "Messages-Waiting: no\r\n",
                                 "active;expires=60", "text/plain"),
                          6)
               .rfind("SIP/2.0 415", 0) == 0);
    auto foreign = n;
    at = foreign.find(field(sub, "Call-ID"));
    foreign.replace(at, field(sub, "Call-ID").size(), "unsolicited");
    assert(h.notification(foreign, 7).rfind("SIP/2.0 481", 0) == 0);
    assert(h.updates.size() == count);
  } else if (scenario == "duplicates") {
    auto sub = h.active();
    auto n =
        notify(sub, 2, "Messages-Waiting: no\r\nVoice-Message: 0/6 (0/0)\r\n");
    assert(h.notification(n, 3).rfind("SIP/2.0 200", 0) == 0);
    const auto count = h.updates.size();
    assert(h.notification(n, 4).rfind("SIP/2.0 200", 0) == 0);
    assert(h.updates.size() == count);
    assert(h.notification(notify(sub, 1), 5).rfind("SIP/2.0 500", 0) == 0);
    assert(h.updates.size() == count && !h.client.state(0).waiting);
  } else if (scenario == "minimum") {
    auto sub = h.start();
    h.answer(response(sub, 423, "Min-Expires: 7200\r\n"));
    auto next = h.first.receive();
    assert(field(next, "Expires") == "7200");
  } else if (scenario == "unsupported") {
    auto sub = h.start();
    h.answer(response(sub, 489));
    uint32_t wait = 0;
    assert(!h.client.next_deadline(1000, &wait));
    h.client.tick(100000);
    assert(h.first.quiet() && !h.client.state(0).available);
  } else if (scenario == "deadline") {
    auto sub = h.start();
    h.answer(response(sub, 200, "Expires: 3600\r\n"));
    uint32_t wait = 0;
    assert(h.client.next_deadline(31999, &wait) && wait == 1);
    h.client.tick(32000);
    assert(h.client.next_deadline(32000, &wait) && wait >= 1000);
    h.client.tick(34000);
    auto fresh = h.first.receive();
    assert(field(fresh, "Call-ID") != field(sub, "Call-ID"));
  } else if (scenario == "retransmit") {
    auto sub = h.start();
    h.client.tick(500);
    if (tcp)
      assert(h.first.quiet());
    else
      assert(h.first.receive() == sub);
  } else if (scenario == "unsubscribe") {
    auto sub = h.active();
    h.client.update_account(0, h.config, INADDR_LOOPBACK, h.first.port,
                            "sip:alice@127.0.0.1:5060", true, false, 20);
    h.client.tick(20);
    auto stop = h.first.receive();
    assert(field(stop, "Expires") == "0");
    h.answer(response(stop, 200, "Expires: 0\r\n"), 21);
    auto end = notify(stop, 2, "", "terminated;reason=timeout");
    assert(h.notification(end, 22).rfind("SIP/2.0 200", 0) == 0);
    assert(!h.client.state(0).available);
    assert(h.notification(end, 23).rfind("SIP/2.0 200", 0) == 0);
    uint32_t wait = 0;
    assert(!h.client.next_deadline(23, &wait));
  } else if (scenario == "runtime-enable") {
    auto sub = h.start(0, false);
    assert(field(sub, "Event") == "message-summary");
  } else if (scenario == "routes") {
    auto sub = h.start();
    h.answer(response(
        sub, 200, "Expires: 60\r\nRecord-Route: <sip:wrong.example;lr>\r\n"));
    assert(h.notification(notify(sub, 1, "Messages-Waiting: no\r\n",
                                 "active;expires=60",
                                 "application/simple-message-summary", "server",
                                 "Record-Route: <sip:proxy.example;lr>\r\n"))
               .rfind("SIP/2.0 200", 0) == 0);
    h.client.tick(48002);
    auto next = h.first.receive();
    assert(field(next, "Route") == "<sip:proxy.example;lr>");
    assert(next.find("wrong.example") == std::string::npos);
  } else if (scenario == "terminated") {
    auto sub = h.active();
    assert(
        h.notification(notify(sub, 2, "",
                              "terminated;reason=deactivated;retry-after=120"),
                       10)
            .rfind("SIP/2.0 200", 0) == 0);
    h.client.tick(10);
    auto next = h.first.receive();
    assert(field(next, "Call-ID") != field(sub, "Call-ID"));
  } else if (scenario == "fork-response") {
    auto sub = h.start();
    assert(h.notification(
                notify(sub, 1, "Messages-Waiting: no\r\n", "active;expires=60",
                       "application/simple-message-summary", "winner"),
                2)
               .rfind("SIP/2.0 200", 0) == 0);
    h.answer(response(sub, 200, "Expires: 3600\r\n", "other-fork"), 3);
    uint32_t wait = 0;
    assert(h.client.next_deadline(3, &wait) && wait < 60000);
    h.client.tick(48002);
    auto refreshed = h.first.receive();
    assert(field(refreshed, "To").find(";tag=winner") != std::string::npos);
  } else if (scenario == "request-line") {
    auto sub = h.start();
    auto bad = notify(sub);
    bad.insert(7, "<");
    bad.insert(bad.find(" SIP/2.0"), ">");
    assert(h.notification(bad).rfind("SIP/2.0 400", 0) == 0);
    assert(!h.client.state(0).available);
  } else if (scenario == "refresh-response-tag") {
    auto sub = h.active();
    h.client.tick(48002);
    auto refresh = h.first.receive();
    auto bad = response(refresh, 200, "Expires: 300\r\n");
    auto at = bad.find(";tag=server", bad.find("\r\nTo:"));
    assert(at != std::string::npos);
    bad.replace(at, 11, ";tag=wrong");
    h.answer(bad, 48003);
    uint32_t remaining = 0;
    assert(h.client.next_deadline(48003, &remaining));
    assert(remaining == (tcp ? 11999U : 499U));
    h.answer(response(refresh, 200, "Expires: 300\r\n"), 48004);
    assert(h.notification(notify(refresh, 2, "Messages-Waiting: no\r\n",
                                 "active;expires=300"),
                          48005)
               .rfind("SIP/2.0 200", 0) == 0);
  } else if (scenario == "refresh-error") {
    auto sub = h.active();
    h.client.tick(48002);
    auto refresh = h.first.receive();
    h.answer(response(refresh, 503, "Retry-After: 120\r\n"), 48003);
    assert(h.client.state(0).available && h.client.state(0).waiting);
    assert(
        h.notification(notify(sub, 2,
                              "Messages-Waiting: yes\r\nVoice-Message: 3/1\r\n",
                              "active;expires=10"),
                       49000)
            .rfind("SIP/2.0 200", 0) == 0);
    assert(h.client.state(0).new_messages == 3);
    h.client.tick(60000);
    assert(!h.client.state(0).available && h.first.quiet());
    h.client.tick(168003);
    auto fresh = h.first.receive();
    assert(field(fresh, "Call-ID") != field(sub, "Call-ID"));
  } else if (scenario == "refresh-terminal") {
    auto sub = h.active();
    h.client.tick(48002);
    auto refresh = h.first.receive();
    h.answer(response(refresh, 481), 48003);
    assert(!h.client.state(0).available);
    assert(h.notification(notify(sub, 2), 49000).rfind("SIP/2.0 481", 0) == 0);
    h.client.tick(50003);
    assert(field(h.first.receive(), "Call-ID") != field(sub, "Call-ID"));
  } else if (scenario == "notify-new-connection") {
    auto sub = h.active();
    if (tcp) {
      Peer incoming(true, true);
      h.reply_peer = &incoming;
      assert(h.client.handle_notify(incoming.deliver(notify(sub, 2)),
                                    INADDR_LOOPBACK, incoming.port, true, -1,
                                    10));
      assert(incoming.receive().rfind("SIP/2.0 200", 0) == 0);
      assert(h.client.handle_notify(incoming.deliver(notify(sub, 3)),
                                    INADDR_LOOPBACK, incoming.port, true, 1,
                                    11));
      assert(incoming.receive().rfind("SIP/2.0 481", 0) == 0);
      h.reply_peer = nullptr;
    } else {
      assert(
          h.notification(notify(sub, 2), 10, INADDR_LOOPBACK, h.first.port + 1)
              .rfind("SIP/2.0 481", 0) == 0);
    }
  } else if (scenario == "multiple") {
    auto first = h.start();
    auto second = h.config;
    second.id = "other";
    second.username = "bob";
    h.client.update_account(1, second, INADDR_LOOPBACK, h.second.port,
                            "sip:bob@127.0.0.1:5060", true, true, 0);
    h.client.tick(0);
    auto request = h.second.receive();
    assert(field(request, "Call-ID") != field(first, "Call-ID"));
    h.answer(response(first, 200, "Expires: 60\r\n"));
    assert(h.notification(notify(first)).rfind("SIP/2.0 200", 0) == 0);
    assert(h.client.state(0).waiting && !h.client.state(1).available);
  } else
    return 2;
  std::cout << "PASS " << scenario << ' ' << (tcp ? "tcp" : "udp") << '\n';
}
