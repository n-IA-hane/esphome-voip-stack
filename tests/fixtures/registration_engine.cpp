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
    hooks.send = [](void* ctx, size_t i, std::string_view s, uint32_t ip, uint16_t port, bool tcp) {
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
    manager.set_keepalive_interval(0);  // Isolate REGISTER cases; dedicated tests cover 15s and discovery.
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
  if (scenario == "nat-contact-owner") {
    assert(!tcp);
    // Wire tuple from the office hairpin: local .63, NAT .1, registrar VIP.
    constexpr uint32_t vip = 0x0afffffe;
    const std::string mode = argc > 3 ? argv[3] : "basic";
    c.registrar = "fusionpbx.tecnodata.net";
    c.port = 5060;
    h.hooks.resolve = [](void*, size_t, const std::string&) { return uint32_t{0x0afffffe}; };
    h.hooks.local_ip = [](void*, uint32_t) { return std::string("192.168.1.63"); };
    h.hooks.send = [](void* ctx, size_t index, std::string_view message, uint32_t ip, uint16_t port, bool stream) {
      assert(ip == 0x0afffffe && port == 5060 && !stream);
      auto& fixture = *static_cast<Harness*>(ctx);
      auto& peer = index == 0 ? fixture.first : fixture.second;
      return send(peer.esp, message.data(), message.size(), 0) == static_cast<ssize_t>(message.size());
    };
    h.manager.set_hooks(h.hooks);
    const auto accept = [&](const std::string& request, uint32_t now, const std::string& received = "192.168.1.1",
                            uint16_t port = 5060) {
      auto message = h.success(request, 60);
      const auto at = message.find(";rport");
      message.replace(at, 6, ";received=" + received + ";rport=" + std::to_string(port));
      if (mode == "via-lws") {
        const auto protocol_end = message.find("UDP ");
        message.replace(protocol_end, 4, "udp\t ");
        const auto parameters = message.find(";branch=");
        message.insert(parameters, " \t\r\n ");
      }
      assert(h.manager.handle_response(message, vip, 5060, false, -1, now));
    };
    assert(h.manager.set_account(c, 0)); h.manager.tick(0);
    auto initial = h.first.receive();
    assert(field(initial, "Contact") == "<sip:esphome-office@192.168.1.63:5060;transport=udp>;expires=3600");
    if (mode == "uncorrelated") {
      auto mapped_response = h.success(initial, 60);
      mapped_response.replace(mapped_response.find(";rport"), 6, ";received=192.168.1.1;rport=5060");
      for (const std::string name : {"Call-ID", "CSeq", "Via"}) {
        auto bad = mapped_response;
        const auto value = field(mapped_response, name);
        const auto start = bad.find(value);
        assert(start != std::string::npos);
        bad.insert(start, "wrong");
        h.manager.handle_response(bad, vip, 5060, false, -1, 1);
        assert(h.manager.account(0).pending && h.manager.account(0).observed_ip == 0);
      }
      h.manager.handle_response(mapped_response, vip + 1, 5060, false, -1, 1);
      h.manager.handle_response(mapped_response, vip, 5061, false, -1, 1);
      h.manager.handle_response(mapped_response, vip, 5060, true, 0, 1);
      assert(h.manager.account(0).pending && h.manager.account(0).observed_ip == 0);
    }
    if (mode == "local-ip-change") {
      accept(initial, 1, "192.168.1.63");
      assert(h.manager.is_registered("office") && h.manager.account(0).cseq == 1);
      h.hooks.local_ip = [](void*, uint32_t) { return std::string("192.168.1.64"); };
      h.manager.set_hooks(h.hooks);
      h.manager.connection_lost(0, 2); h.manager.tick(2);
      auto refresh = h.first.receive();
      assert(field(refresh, "Via").find("192.168.1.64:5060") != std::string::npos);
      accept(refresh, 3, "192.168.1.64");
      auto corrected = h.first.receive();
      assert(field(corrected, "Contact") == "<sip:esphome-office@192.168.1.64:5060;transport=udp>;expires=3600");
      assert(corrected.find("Contact: <sip:esphome-office@192.168.1.63:5060;transport=udp>;expires=0\r\n") != std::string::npos);
      accept(corrected, 4, "192.168.1.64");
      assert(h.manager.is_registered("office") && !h.manager.account(0).pending);
      assert(h.manager.account(0).observed_ip == 0xc0a80140 && h.manager.account(0).retiring_contact.empty());
      std::cout << "PASS nat-contact-owner udp\n"; return 0;
    }
    if (mode == "no-nat") {
      accept(initial, 1, "192.168.1.63");
      assert(h.manager.is_registered("office") && !h.manager.account(0).pending);
      assert(h.manager.account(0).cseq == 1 && h.manager.account(0).retiring_contact.empty());
      assert(h.manager.account(0).contact == "sip:esphome-office@192.168.1.63:5060;transport=udp");
      std::cout << "PASS nat-contact-owner udp\n"; return 0;
    }
    if (mode == "malformed") {
      for (const std::string mapping : {";received=bad;rport=5060", ";received=192.168.1.1;rport=0",
          ";received=192.168.1.1;rport=65536", ";received=224.0.0.1;rport=5060",
          ";received=192.168.1.1;rport=5060;rport=5061"}) {
        auto bad = h.success(initial, 60);
        bad.replace(bad.find(";rport"), 6, mapping);
        assert(h.manager.handle_response(bad, vip, 5060, false, -1, 1));
        assert(h.manager.account(0).observed_ip == 0 && !h.manager.account(0).pending);
        assert(h.manager.request_registration("office", true, 1)); h.manager.tick(1);
        initial = h.first.receive();
      }
      auto wrong = h.success(initial, 60);
      wrong.replace(wrong.find("192.168.1.63:5060"), std::string("192.168.1.63:5060").size(), "192.168.1.99:5060");
      assert(h.manager.handle_response(wrong, vip, 5060, false, -1, 1));
      assert(h.manager.account(0).pending && h.manager.account(0).observed_ip == 0);
      // Fresh account restores CSeq 1 for the common original witness below.
      assert(h.manager.set_account(c, 1)); h.manager.tick(1); initial = h.first.receive();
    }
    accept(initial, 1);
    assert(h.manager.account(0).observed_ip == 0xc0a80101);
    // Original failing witness: accepting the local Contact leaves PBX routing around NAT.
    assert(h.manager.account(0).pending);
    auto rewrite = h.first.receive();
    const std::string mapped = "<sip:esphome-office@192.168.1.1:5060;transport=udp>";
    assert(field(rewrite, "Contact") == mapped + ";expires=3600");
    assert(rewrite.find("Contact: <sip:esphome-office@192.168.1.63:5060;transport=udp>;expires=0\r\n") != std::string::npos);
    assert(field(rewrite, "Call-ID") == field(initial, "Call-ID"));
    assert(field(rewrite, "CSeq") == "2 REGISTER");
    assert(rewrite.find("Contact: *") == std::string::npos);
    assert(rewrite.find("another-phone") == std::string::npos);
    if (mode == "auth" || mode == "retry") {
      auto challenge = response(rewrite, mode == "auth" ? 401 : 503,
          "WWW-Authenticate: Digest realm=\"pbx\",nonce=\"rewrite\",algorithm=MD5,qop=\"auth\"\r\n");
      assert(h.manager.handle_response(challenge, vip, 5060, false, -1, 2));
      if (mode == "retry") h.manager.tick(30002);
      rewrite = h.first.receive();
      assert(field(rewrite, "Contact") == mapped + ";expires=3600");
      assert(rewrite.find("Contact: <sip:esphome-office@192.168.1.63:5060;transport=udp>;expires=0\r\n") != std::string::npos);
      if (mode == "auth") assert(!field(rewrite, "Authorization").empty());
    }
    if (mode == "intent") assert(h.manager.request_registration("office", false, 2));
    accept(rewrite, 2);
    if (mode == "mapping") {
      h.manager.tick(48002);
      auto refresh = h.first.receive();
      accept(refresh, 48003, "192.168.1.2", 5070);
      auto changed = h.first.receive();
      assert(field(changed, "Contact") == "<sip:esphome-office@192.168.1.2:5070;transport=udp>;expires=3600");
      assert(changed.find("Contact: " + mapped + ";expires=0\r\n") != std::string::npos);
      assert(changed.find("192.168.1.63:5060;transport=udp>;expires=") == std::string::npos);
      accept(changed, 48004, "192.168.1.2", 5070);
      assert(h.manager.account(0).retiring_contact.empty() && !h.manager.account(0).pending);
      std::cout << "PASS nat-contact-owner udp\n"; return 0;
    }
    if (mode == "other-line") {
      auto other = c;
      other.id = "ha";  // Same AOR on a distinct account-owned registration.
      assert(h.manager.set_account(other, 3)); h.manager.tick(3);
      auto request = h.second.receive();
      accept(request, 4, "192.168.1.63");
      assert(h.manager.is_registered("ha") && h.manager.account(1).cseq == 1);
      const auto other_contact = h.manager.account(1).contact;
      assert(h.manager.request_registration("office", false, 5)); h.manager.tick(5);
      auto unregister = h.first.receive();
      assert(unregister.find("Contact: " + mapped + ";expires=0\r\n") != std::string::npos);
      assert(unregister.find("esphome-ha@") == std::string::npos && unregister.find("Contact: *") == std::string::npos);
      assert(h.manager.is_registered("ha") && h.manager.account(1).cseq == 1);
      assert(h.manager.account(1).contact == other_contact && h.manager.account(1).retiring_contact.empty());
      std::cout << "PASS nat-contact-owner udp\n"; return 0;
    }
    if (mode == "reset") {
      assert(h.manager.set_account(c, 3));
      assert(h.manager.account(0).observed_ip == 0 && h.manager.account(0).retiring_contact.empty());
      h.manager.tick(3);
      auto fresh = h.first.receive();
      assert(field(fresh, "Contact") == field(initial, "Contact"));
      assert(h.manager.remove_account("office"));
      assert(h.manager.account(0).observed_ip == 0 && h.manager.account(0).contact.empty());
      std::cout << "PASS nat-contact-owner udp\n"; return 0;
    }
    if (mode != "intent") assert(h.manager.is_registered("office") && !h.manager.account(0).pending);
    assert(h.manager.match_incoming("418", vip, false, -1) == (mode == "intent" ? -1 : 0));
    assert(h.manager.match_incoming("418", 0xc0a8013d, false, -1) < 0);
    assert(h.manager.request_registration("office", false, 3)); h.manager.tick(3);
    auto unregister = h.first.receive();
    assert(field(unregister, "Contact") == mapped + ";expires=0");
    assert(field(unregister, "Expires") == "0");
    assert(unregister.find("192.168.1.63:5060;transport=udp>;expires=") == std::string::npos);
    std::cout << "PASS nat-contact-owner udp\n";
    return 0;
  }
  if (scenario == "managed-literal") {
    c.username = "Caffè 20%@Casa";
    assert(c.valid() && c.address_of_record() == "sip:Caff%C3%A8%2020%25%40Casa@example.org");
    // Manual and managed accounts both contain literal, unescaped usernames.
    c.username = "+123%2A";
    assert(c.valid() && c.address_of_record() == "sip:%2B123%252A@example.org");
    c.id = "ha";
    for (const char *invalid : {"", " bad", "bad ", "bad\r\nname", "\xC0\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80"}) {
      c.username = invalid;
      assert(!c.valid());
    }
    c.username = std::string(64, 'a'); assert(c.valid());
    c.username += 'a'; assert(!c.valid());
    c.username = std::string(32, '\xc3'); assert(!c.valid());
    c.username = "Caffè 20%@Casa";
    const std::string aor = "<sip:Caff%C3%A8%2020%25%40Casa@example.org>";
    h.manager.set_keepalive_interval(2000);
    assert(h.manager.set_account(c, 0));
    h.manager.tick(0);
    const auto first = h.first.receive();
    assert(field(first, "To") == aor && field(first, "From").find(aor + ";tag=") == 0);
    h.feed(h.first, response(first, 401, "WWW-Authenticate: Digest realm=\"pbx\", nonce=\"fixed\"\r\n"));
    const auto retry = h.first.receive();
    const auto target = "sip:" + c.registrar + ":" + std::to_string(c.port);
    const auto digest = hash_value("MD5", hash_value("MD5", c.username + ":pbx:" + c.password) + ":fixed:" +
                                          hash_value("MD5", "REGISTER:" + target));
    const auto auth = field(retry, "Authorization");
    assert(auth.find("username=\"Caffè 20%@Casa\"") != std::string::npos);
    assert(auth.find("response=\"" + digest + "\"") != std::string::npos);
    h.feed(h.first, h.success(retry), 2);
    assert(h.manager.is_registered("ha"));
    h.manager.tick(2002);
    const auto options = h.first.receive();
    assert(options.rfind("OPTIONS ", 0) == 0 && field(options, "To") == aor && field(options, "From") == field(first, "From"));
    h.feed(h.first, response(options, 200), 2003);
    assert(h.manager.request_registration("ha", false, 2004));
    h.manager.tick(2004);
    const auto retire = h.first.receive();
    assert(field(retire, "To") == aor && field(retire, "Expires") == "0");
    h.feed(h.first, h.success(retire, 0), 2005);
    assert(!h.manager.is_registered("ha") && !h.manager.account(0).pending);
    std::cout << "PASS managed-literal " << (tcp ? "tcp" : "udp") << "\n";
    return 0;
  }
  if (scenario == "digest-syntax") {
    assert(argc == 5);
    const std::string style = argv[3];
    const bool proxy = std::string(argv[4]) == "proxy";
    const std::string name = proxy ? "Proxy-Authenticate" : "WWW-Authenticate";
    const auto challenge = [&](const std::string &algorithm, const std::string &nonce) {
      const auto colon = style == "hcolon" ? " \t: " : ": ";
      const auto fold = style == "fold" ? "\r\n\t" : " ";
      return name + colon + "Digest realm=\"pbx,office\", " + fold +
          "nonce=\"" + nonce + "\", algorithm=" + algorithm + ", " + fold +
          "qop=\"auth-int,auth\", opaque=\"one,\\\"two\\\"\"\r\n";
    };
    assert(h.manager.set_account(c, 0));
    h.manager.tick(0);
    const auto initial = h.first.receive();
    std::string challenges = challenge("unsupported", "ignored");
    if (style == "reverse") challenges += challenge("SHA-256-sess", "selected") + challenge("MD5", "weak");
    else challenges += challenge("MD5", "weak") + challenge("SHA-256-sess", "selected");
    challenges += challenge("SHA-256", "later");
    h.feed(h.first, response(initial, proxy ? 407 : 401, challenges));
    const auto authenticated = h.first.receive();
    const auto auth = field(authenticated, proxy ? "Proxy-Authorization" : "Authorization");
    assert(auth.find("algorithm=SHA-256-sess") != std::string::npos);
    assert(auth.find("nonce=\"selected\"") != std::string::npos);
    assert(auth.find("realm=\"pbx,office\"") != std::string::npos);
    assert(auth.find("opaque=\"one,\\\"two\\\"\"") != std::string::npos);
    assert(auth.find("qop=auth") != std::string::npos);
    assert(field(authenticated, "CSeq") == "2 REGISTER");
    h.feed(h.first, h.success(authenticated), 2);
    assert(h.manager.is_registered("office"));
    std::cout << "PASS digest-syntax " << (tcp ? "tcp" : "udp") << "\n";
    return 0;
  }
  if (scenario == "rfc-syntax") {
    assert(argc == 5);
    const std::string style = argv[3];
    const bool auth = std::string(argv[4]) == "auth";
    assert(h.manager.set_account(c, 0));
    h.manager.tick(0);
    auto request = h.first.receive();
    const auto rewrite = [&](std::string message) {
      const auto replace = [&](const std::string& from, const std::string& to) {
        const auto at = message.find(from);
        if (at != std::string::npos) message.replace(at, from.size(), to);
      };
      if (style == "contact-hcolon") replace("\r\nContact:", "\r\nContact \t:");
      if (style == "contact-fold") replace("\r\nContact: ", "\r\nContact:\r\n\t");
      if (style == "cseq-hcolon") replace("\r\nCSeq:", "\r\nCSeq \t:");
      if (style == "via-hcolon") replace("\r\nVia:", "\r\nVia \t:");
      if (style == "via-fold") replace(";branch=", "\r\n\t;branch=");
      const auto sequence = field(request, "CSeq");
      const auto separator = sequence.find(' ');
      const std::string whitespace = style == "cseq-tab" ? "\t" :
          style == "cseq-spaces" ? "  \t " : style == "cseq-fold" ? "\r\n\t" : " ";
      replace("CSeq: " + sequence, "CSeq: " + sequence.substr(0, separator) + whitespace + "REGISTER");
      return message;
    };
    if (auth) {
      h.feed(h.first, rewrite(response(request, 401,
          "WWW-Authenticate: Digest realm=\"syntax\",nonce=\"syntax\",algorithm=SHA-256,qop=\"auth\"\r\n")), 1);
      request = h.first.receive();
      assert(!field(request, "Authorization").empty());
    }
    auto accepted = h.success(request, 60);
    const auto own_uri = h.manager.account(0).contact;
    std::string contact;
    if (style == "quoted-angle") contact = "\"Phone <door>\" <" + own_uri + ">;expires=60";
    if (style == "escaped-angle") contact = "\"Phone \\\"<door>\\\"\" <" + own_uri + ">;expires=60";
    if (style == "quoted-only") contact = "\"<" + own_uri + ">\"";
    if (style == "wrong-real-uri") contact = "\"<" + own_uri + ">\" <sip:other@example.test:5060>;expires=60";
    if (style == "suffix-junk") contact = "<" + own_uri + ">junk;expires=60";
    if (style == "two-uris") contact = "<" + own_uri + "><sip:other@example.test:5060>;expires=60";
    if (!contact.empty()) accepted = response(request, 200, "Contact: " + contact + "\r\n");
    const bool bare = style.find("bare") != std::string::npos;
    if (bare) {
      assert(!tcp);
      auto contact = h.manager.account(0).contact;
      contact.resize(contact.find(';'));
      const auto port = contact.substr(contact.rfind(':') + 1);
      if (style == "wrong-bare-binding") contact.replace(4, 3, "999");
      const bool expired = style == "expired-bare-binding";
      accepted = response(request, 200, "Contact: " + contact +
          (style == "bare-expiry-fallback" ? "" : expired ? ";expires=0" : ";expires=60") +
          "\r\nExpires: 60\r\n");
      const auto via = field(request, "Via");
      const auto at = accepted.find("Via: " + via);
      accepted.replace(at, 5 + via.size(), "Via: " + via + ";received=127.0.0.1;rport=" + port);
    }
    h.feed(h.first, rewrite(accepted), 2);
    const bool expected = style != "wrong-bare-binding" && style != "expired-bare-binding" &&
        style != "quoted-only" && style != "wrong-real-uri" && style != "suffix-junk" && style != "two-uris";
    assert(h.manager.is_registered("office") == expected);
    if (expected) assert(h.manager.account(0).negotiated_expires == 60);
    std::cout << "PASS rfc-syntax " << (tcp ? "tcp" : "udp") << "\n";
    return 0;
  }
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
    h.hooks.send = [](void*, size_t, std::string_view, uint32_t, uint16_t, bool) { return false; };
    h.manager.set_hooks(h.hooks);
    assert(h.manager.set_account(c, 0)); h.manager.tick(0);
    const auto& account = h.manager.account(0);
    assert(!account.pending && account.request.empty());
    assert(account.state == RegistrationState::RETRY && account.last_status == 503);
    uint32_t remaining = 0; assert(h.manager.next_deadline(0, &remaining));
    assert(remaining == 30000);
    h.hooks.send = [](void* ctx, size_t, std::string_view message, uint32_t, uint16_t, bool) {
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
    // The registrar may address its registered line by extension or name.
    assert(h.manager.match_incoming("101", INADDR_LOOPBACK, tcp, tcp ? 0 : -1) == 0);
    assert(h.manager.match_incoming("Kitchen Panel", INADDR_LOOPBACK, tcp, tcp ? 0 : -1) == 0);
    assert(h.manager.match_incoming("101", 0x7f000002, tcp, tcp ? 0 : -1) < 0);
  } else if (scenario == "registrar-target") {
    // Request-URI users exactly as a PBX resolved them: extension, username or text name.
    assert(argc >= 4);
    h.feed(h.first, h.success(initial), 1);
    assert(h.manager.is_registered("office"));
    for (int i = 3; i < argc; ++i) {
      assert(h.manager.match_incoming(argv[i], INADDR_LOOPBACK, tcp, tcp ? 0 : -1) == 0);
      assert(h.manager.match_incoming(argv[i], INADDR_LOOPBACK, tcp, -1) == 0);
      assert(h.manager.match_incoming(argv[i], 0x7f000002, tcp, -1) < 0);
    }
  } else if (scenario == "incoming-transport") {
    h.feed(h.first, h.success(initial), 1);
    assert(h.manager.is_registered("office"));
    assert(h.manager.match_incoming("esphome-office", INADDR_LOOPBACK, !tcp, -1) == 0);
    assert(h.manager.match_incoming("esphome-office", 0x7f000002, !tcp, -1) < 0);
    assert(h.manager.match_incoming("101", INADDR_LOOPBACK, !tcp, -1) == 0);
    assert(h.manager.match_incoming("101", 0x7f000002, !tcp, -1) < 0);
    assert(h.manager.match_incoming("esphome-office", INADDR_LOOPBACK, true, 1) < 0);
    assert(h.manager.match_incoming("101", INADDR_LOOPBACK, true, 1) < 0);
    auto replacement = c;
    replacement.password = "new-fixture-value";
    assert(h.manager.set_account(replacement, 2));
    assert(h.manager.match_incoming("esphome-office", INADDR_LOOPBACK, !tcp, -1) < 0);
    assert(h.manager.match_incoming("101", INADDR_LOOPBACK, !tcp, -1) < 0);
    h.manager.tick(2);
    auto request = h.first.receive();
    h.feed(h.first, h.success(request), 3);
    assert(h.manager.match_incoming("esphome-office", INADDR_LOOPBACK, !tcp, -1) == 0);
    assert(h.manager.request_registration("office", false, 4));
    assert(h.manager.match_incoming("esphome-office", INADDR_LOOPBACK, !tcp, -1) < 0);
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
  } else if (scenario == "mapped-contact") {
    assert(argc == 4);
    const std::string mode = argv[3];
    h.feed(h.first, response(initial, 401,
        "WWW-Authenticate: Digest realm=\"registrar\",nonce=\"mapped\",algorithm=MD5,qop=\"auth\"\r\n"));
    const auto authenticated = h.first.receive();
    assert(!field(authenticated, "Authorization").empty());
    const std::string host = mode == "asterisk" ? "127.0.0.1" : "198.51.100.44";
    const unsigned port = mode == "asterisk" && !tcp ? 5060 : 62000;
    std::string contact = "sip:esphome-office@" + host + ":" + std::to_string(port);
    if (tcp) contact += ";transport=TCP";
    if (mode == "preserve-udp" && !tcp) contact += ";transport=udp";
    if (mode == "wrong-host") contact.replace(contact.find(host), host.size(), "198.51.100.45");
    if (mode == "wrong-port") contact.replace(contact.find(":62000"), 6, ":62001");
    if (mode == "wrong-user") contact.replace(contact.find("esphome-office"), 14, "another-phone");
    if (mode == "wrong-transport") {
      if (tcp) contact.replace(contact.find(";transport=TCP"), 14, ";transport=udp");
      else contact += ";transport=tcp";
    }
    const bool expected = mode == "asterisk" || mode == "nat" || mode == "preserve-udp" || mode == "multiple";
    std::string headers;
    if (mode == "multiple") headers += "Contact: <sip:another@198.51.100.44:62000>;expires=900\r\n";
    if (mode != "missing-contact") headers += "Contact: <" + contact + ">;expires=60\r\n";
    headers += "Expires: 120\r\n";
    auto accepted = response(authenticated, 200, headers);
    if (mode != "no-flow") {
      const auto marker = accepted.find(";rport");
      assert(marker != std::string::npos);
      accepted.replace(marker, 6, ";received=" + host + ";rport=" + std::to_string(port));
    }
    h.feed(h.first, accepted, 3);
    assert(h.manager.is_registered("office") == expected);
    if (expected) assert(h.manager.account(0).negotiated_expires == 60);
    else assert(h.manager.account(0).state == RegistrationState::RETRY);
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
    // Two lines with distinct usernames on one peer: the target must name its line.
    const int any_flow = -1;
    assert(h.manager.match_incoming("esphome-home", INADDR_LOOPBACK, tcp, any_flow) == 1);
    assert(h.manager.match_incoming("101", INADDR_LOOPBACK, tcp, any_flow) == 1);
    assert(h.manager.match_incoming("100", INADDR_LOOPBACK, tcp, any_flow) == 0);
    assert(h.manager.match_incoming("Kitchen Panel", INADDR_LOOPBACK, tcp, any_flow) < 0);
    assert(h.manager.match_incoming("101", 0x7f000002, tcp, any_flow) < 0);
    // A disabled line keeps its names: its calls and answer policy do not move to the other line.
    assert(h.manager.request_registration("home", false, 5));
    assert(h.manager.match_incoming("esphome-home", INADDR_LOOPBACK, tcp, any_flow) < 0);
    assert(h.manager.match_incoming("101", INADDR_LOOPBACK, tcp, any_flow) < 0);
    assert(h.manager.match_incoming("100", INADDR_LOOPBACK, tcp, any_flow) == 0);
    assert(h.manager.match_incoming("Kitchen Panel", INADDR_LOOPBACK, tcp, any_flow) == 0);
  } else if (scenario == "same-user") {
    auto other_config = h.config("home", 1); other_config.username = c.username;
    assert(h.manager.set_account(other_config, 0)); h.manager.tick(0); auto other = h.second.receive();
    h.feed(h.first, h.success(initial), 1); h.feed(h.second, h.success(other), 2, 1);
    assert(h.manager.is_registered("office") && h.manager.is_registered("home"));
    assert(h.manager.match_incoming("esphome-office", INADDR_LOOPBACK, tcp, tcp ? 0 : -1) == 0);
    assert(h.manager.match_incoming("esphome-home", INADDR_LOOPBACK, tcp, tcp ? 1 : -1) == 1);
    // Two lines share the peer: only an owned TCP flow or the Contact user selects one.
    assert(h.manager.match_incoming(c.username, INADDR_LOOPBACK, tcp, tcp ? 0 : -1) == (tcp ? 0 : -1));
    assert(h.manager.match_incoming(c.username, INADDR_LOOPBACK, tcp, tcp ? 1 : -1) == (tcp ? 1 : -1));
    // A shared username names both lines, so disabling one leaves the other selectable.
    assert(h.manager.request_registration("home", false, 3));
    assert(h.manager.match_incoming("esphome-home", INADDR_LOOPBACK, tcp, -1) < 0);
    assert(h.manager.match_incoming(c.username, INADDR_LOOPBACK, tcp, -1) == 0);
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
    assert(h.manager.match_incoming("101", INADDR_LOOPBACK, tcp, tcp ? 0 : -1) < 0);
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
    assert(h.manager.account(0).state == RegistrationState::RETRY && h.manager.account(0).scheduled);
    assert(h.manager.account(0).next_ms >= 30003);
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
    assert(!digest.parse("Digest nonce=\"x\",qop=\"\"", false));
    assert(!digest.parse("Digest nonce=\"x\",qop=\"auth", false));
  } else
    assert(false);
  std::cout << "PASS " << scenario << " " << (tcp ? "tcp" : "udp") << "\n";
  return 0;
}
