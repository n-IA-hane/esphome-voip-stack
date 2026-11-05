// Appended to the existing socket harness before its main().
std::string auth_parameter(const std::string &header, const std::string &key) {
  const auto at = header.find(key + "=");
  assert(at != std::string::npos);
  auto start = at + key.size() + 1;
  if (header[start] == '"') {
    ++start;
    return header.substr(start, header.find('"', start) - start);
  }
  return header.substr(start, header.find(',', start) - start);
}

void verify_digest(const std::string &wire, const std::string &name,
                   const std::string &nonce, unsigned count) {
  const auto auth = field(wire, name);
  assert(!auth.empty());
  assert(auth_parameter(auth, "nonce") == nonce);
  const auto target_end = wire.find(" SIP/2.0");
  const auto target = wire.substr(10, target_end - 10);
  assert(auth_parameter(auth, "uri") == target);
  char nc[9];
  std::snprintf(nc, sizeof(nc), "%08x", count);
  assert(auth_parameter(auth, "nc") == nc);
  const auto ha1 = hash_value("SHA-256", "alice:example.test:test-password");
  const auto ha2 = hash_value("SHA-256", "SUBSCRIBE:" + target);
  const auto expected = hash_value("SHA-256", ha1 + ':' + nonce + ':' + nc + ':' +
      auth_parameter(auth, "cnonce") + ":auth:" + ha2);
  assert(auth_parameter(auth, "response") == expected);
}

std::string challenge(bool proxy, const std::string &nonce) {
  return std::string(proxy ? "Proxy-Authenticate" : "WWW-Authenticate") +
      ": Digest realm=\"example.test\", nonce=\"" + nonce +
      "\", algorithm=SHA-256, qop=\"auth\"\r\n";
}

void changed_request(const std::string &previous, const std::string &next) {
  assert(field(previous, "Call-ID") == field(next, "Call-ID"));
  assert(field(previous, "From") == field(next, "From"));
  assert(std::stoul(field(next, "CSeq")) == std::stoul(field(previous, "CSeq")) + 1);
  assert(field(previous, "Via") != field(next, "Via"));
}

struct TransactionHarness {
  Harness h;
  SipRegistration owner;
  bool blf;
  unsigned notify_seq{0};

  TransactionHarness(bool tcp, bool dialog) : h(tcp), blf(dialog) {
    assert(owner.set_account(h.config, 0));
    auto &a = owner.account(0);
    a.bound = true;
    a.target_ip = INADDR_LOOPBACK;
    a.target_port = h.first.port;
    a.contact = "sip:alice@127.0.0.1:5060";
    if (blf) {
      assert(h.client.configure_blf(1));
      assert(h.client.set_blf_watch(0, 0, a, "peer", "sip:101@example.test", 0));
    } else {
      h.client.update_registered_account(0, a, 0);
    }
  }

  std::string notification(const std::string &request, bool terminal, unsigned lease) {
    const std::string body = terminal ? "" : blf ?
        "<dialog-info xmlns='urn:ietf:params:xml:ns:dialog-info' version='0' "
        "state='full' entity='sip:101@example.test'/>" : "Messages-Waiting: yes\r\n";
    auto wire = notify(request, ++notify_seq, body,
        terminal ? "terminated;reason=timeout" : "active;expires=" + std::to_string(lease));
    if (blf) {
      auto at = wire.find("Event: message-summary");
      assert(at != std::string::npos);
      wire.replace(at, 22, "Event: dialog");
      at = wire.find("application/simple-message-summary");
      assert(at != std::string::npos);
      wire.replace(at, 34, "application/dialog-info+xml");
    }
    return wire;
  }

  std::string active(unsigned lease) {
    h.client.tick(0);
    auto initial = h.first.receive();
    assert(field(initial, "Authorization").empty());
    assert(field(initial, "Proxy-Authorization").empty());
    h.answer(response(initial, 407, challenge(true, "old-proxy")), 1);
    auto authorized = h.first.receive();
    changed_request(initial, authorized);
    verify_digest(authorized, "Proxy-Authorization", "old-proxy", 1);
    h.answer(response(authorized, 202, "Expires: 3600\r\n"), 2);
    assert(h.notification(notification(authorized, false, lease), 3).rfind("SIP/2.0 200", 0) == 0);
    assert(!h.client.account_retired(0));
    return authorized;
  }

  void retransmission(const std::string &wire, uint32_t now) {
    h.client.tick(now);
    if (h.first.tcp) {
      assert(h.first.quiet());
    } else {
      // Exact record comparison includes credentials, CSeq, branch and body framing.
      assert(h.first.receive() == wire);
    }
  }

  void finish(const std::string &wire, uint32_t now, bool terminal, unsigned lease) {
    h.answer(response(wire, 202, "Expires: " + std::string(terminal ? "0" : "3600") + "\r\n"), now);
    assert(!h.client.account_retired(0));
    assert(h.notification(notification(wire, terminal, lease), now + 1).rfind("SIP/2.0 200", 0) == 0);
    assert(h.client.account_retired(0) == terminal);
    if (terminal) {
      // Terminal NOTIFY retransmission is answered even after local dialog retirement.
      const auto end = notify_seq;
      --notify_seq;
      assert(h.notification(notification(wire, true, lease), now + 2).rfind("SIP/2.0 200", 0) == 0);
      assert(notify_seq == end && h.first.quiet());
    }
  }
};

int main(int argc, char **argv) {
  assert(argc == 4);
  const std::string scenario = argv[1], transport = argv[2], package = argv[3];
  TransactionHarness t(transport == "tcp", package == "blf");
  auto &h = t.h;
  if (scenario == "dual") {
    h.client.tick(0);
    auto first = h.first.receive();
    h.answer(response(first, 401, challenge(false, "origin")), 1);
    auto origin = h.first.receive();
    changed_request(first, origin);
    verify_digest(origin, "Authorization", "origin", 1);
    h.answer(response(origin, 407, challenge(true, "proxy")), 2);
    auto both = h.first.receive();
    changed_request(origin, both);
    verify_digest(both, "Authorization", "origin", 2);
    verify_digest(both, "Proxy-Authorization", "proxy", 1);
    t.retransmission(both, 502);
    t.finish(both, 503, false, 3600);
  } else {
    const bool terminal = scenario.rfind("unsubscribe-", 0) == 0;
    const unsigned lease = terminal ? 3600 : scenario == "refresh94" ? 118 : 108;
    const auto previous = t.active(lease);
    const uint32_t now = terminal ? 86003 : 3 + lease * 800;
    if (terminal) {
      auto &account = t.owner.account(0);
      account.desired = false;
      account.bound = scenario != "unsubscribe-unbound";
      if (t.blf) h.client.update_blf_account(0, account, now);
      else h.client.update_registered_account(0, account, now);
    }
    h.client.tick(now);
    auto fresh = h.first.receive();
    changed_request(previous, fresh);
    assert(field(fresh, "To").find(";tag=server") != std::string::npos);
    assert(field(fresh, "Expires") == (terminal ? "0" : "3600"));
    if (scenario == "bare401") {
      h.answer(response(fresh, 401), now + 1);
      h.client.tick(now + 1000);
      // A malformed authentication response cannot confirm remote retirement.
      assert(h.first.quiet());
      assert(!h.client.account_retired(0));
    } else {
      assert(field(fresh, "Authorization").empty());
      assert(field(fresh, "Proxy-Authorization").empty());
      t.retransmission(fresh, now + 500);
      const std::string nonce = scenario == "same-nonce" ? "old-proxy" : "new-proxy";
      h.answer(response(fresh, 407, challenge(true, nonce)), now + 501);
      auto authorized = h.first.receive();
      changed_request(fresh, authorized);
      assert(field(authorized, "To") == field(fresh, "To"));
      assert(field(authorized, "Expires") == (terminal ? "0" : "3600"));
      verify_digest(authorized, "Proxy-Authorization", nonce, scenario == "same-nonce" ? 2 : 1);
      t.retransmission(authorized, now + 1001);
      t.finish(authorized, now + 1002, terminal, lease);
    }
  }
  std::cout << "PASS " << scenario << ' ' << transport << ' ' << package << '\n';
}
