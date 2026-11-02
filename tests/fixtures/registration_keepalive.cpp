// Production registration timers and real socket records, independent of calls.
#define main registration_fixture_main
#include "registration_engine.cpp"
#undef main
#include <cstdlib>
#include <new>

static bool count_allocations = false;
static size_t allocations = 0;
void* operator new(size_t size) {
  if (count_allocations) ++allocations;
  if (void* value = std::malloc(size ? size : 1)) return value;
  throw std::bad_alloc();
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, size_t) noexcept { std::free(value); }
void operator delete[](void* value, size_t) noexcept { std::free(value); }

std::string mapping(std::string message, unsigned port = 41000) {
  const auto start = message.find(";rport\r\n");
  assert(start != std::string::npos);
  message.replace(start, 8, ";received=198.51.100.1;rport=" + std::to_string(port) + "\r\n");
  return message;
}
void no_record(Peer& peer) {
  char bytes[8192];
  assert(recv(peer.peer, bytes, sizeof(bytes), MSG_DONTWAIT) < 0);
}

int main(int argc, char** argv) {
  assert(argc >= 3);
  const std::string scenario = argv[1];
  const bool tcp = std::string(argv[2]) == "tcp";
  Harness h(tcp);
  h.hooks.random = []() -> uint32_t { static uint32_t value = 4000000000U; return ++value; };
  h.manager = SipRegistration();  // Exercise the production 15s default.
  h.manager.set_hooks(h.hooks);
  if (scenario != "default") h.manager.set_keepalive_interval(scenario == "disabled" ? 0 : 2000);
  auto config = h.config();
  if (scenario == "priority") h.manager.set_keepalive_interval(44000);
  assert(h.manager.set_account(config, 0));
  h.manager.tick(0);
  auto initial = h.first.receive();
  if (scenario == "credentials") {
    h.feed(h.first, response(initial, 401,
        "WWW-Authenticate: Digest realm=\"registered\",nonce=\"retained\",algorithm=MD5,qop=\"auth\"\r\n"), 0);
    initial = h.first.receive();
    assert(field(initial, "Authorization").find("nonce=\"retained\"") != std::string::npos);
  }
  h.feed(h.first, mapping(h.success(initial, scenario == "priority" ? 60 : 3600)), 1);
  const auto registration_deadline = h.manager.account(0).next_ms;
  const auto expiry = h.manager.account(0).binding_expires_ms;
  uint32_t due = scenario == "default" ? 15001 : scenario == "priority" ? 44001 : 2001;
  if (scenario == "disabled") {
    h.manager.tick(15001); no_record(h.first);
    uint32_t remaining = 0;
    assert(h.manager.next_deadline(15001, &remaining));
    assert(remaining == registration_deadline - 15001);
    assert(h.manager.account(0).negotiated_expires == 3600);
    std::cout << "PASS disabled\n"; return 0;
  }
  h.manager.tick(due - 1); no_record(h.first);
  h.manager.tick(due);
  auto request = h.first.receive();
  assert(request.rfind("OPTIONS sip:pbx.example:", 0) == 0);
  assert(field(request, "Call-ID") == field(initial, "Call-ID"));
  assert(field(request, "CSeq") == (scenario == "credentials" ? "3 OPTIONS" : "2 OPTIONS"));
  assert(field(request, "Content-Length") == "0");
  assert(field(request, "Authorization").empty() && field(request, "Contact").empty());
  assert(field(request, "Expires").empty() && field(request, "Content-Type").empty());
  assert(h.manager.is_registered(config.id));
  assert(h.manager.account(0).binding_expires_ms == expiry);

  if (scenario == "identity") {
    const auto valid = mapping(response(request, 200));
    for (const char* name : {"Call-ID", "CSeq", "Via"}) {
      auto altered = valid;
      const auto start = std::string(name) == "Via" ? altered.find("branch=") + 7 :
          altered.find(std::string("\r\n") + name + ": ") + std::string(name).size() + 4;
      altered.insert(start, "invalid");
      h.manager.handle_response(altered, INADDR_LOOPBACK, h.first.port, tcp, tcp ? 0 : -1, due + 1);
      assert((h.manager.account(0).pending && h.manager.account(0).request_kind==SipRegistration::RequestKind::KEEPALIVE));
    }
    h.manager.handle_response(valid, INADDR_LOOPBACK + 1, h.first.port, tcp, tcp ? 0 : -1, due + 1);
    h.manager.handle_response(valid, INADDR_LOOPBACK, h.first.port + 1, tcp, tcp ? 0 : -1, due + 1);
    if (tcp) h.manager.handle_response(valid, INADDR_LOOPBACK, h.first.port, true, 1, due + 1);
    assert((h.manager.account(0).pending && h.manager.account(0).request_kind==SipRegistration::RequestKind::KEEPALIVE));
    h.feed(h.first, valid, due + 2);
    assert(!h.manager.account(0).pending);
  } else if (scenario == "timer" || scenario == "timeout") {
    for (uint32_t delta : {500, 1500, 3500, 7500}) {
      h.manager.tick(due + delta);
      if (tcp) no_record(h.first);
      else assert(h.first.receive() == request);
    }
    if (scenario == "timer") {
      h.feed(h.first, mapping(response(request, 100)), due + 7501);
      h.manager.tick(due + 11500); no_record(h.first);
      h.manager.tick(due + 11501);
      if (tcp) no_record(h.first); else assert(h.first.receive() == request);
      h.feed(h.first, mapping(response(request, 405)), due + 11502);
      assert(h.manager.is_registered(config.id) && !h.manager.account(0).pending);
    } else {
      h.manager.tick(due + 32000);
      auto refresh = h.first.receive();
      assert(refresh.rfind("REGISTER ", 0) == 0 && field(refresh, "Expires") == "3600");
      assert(h.manager.is_registered(config.id));
      h.feed(h.first, mapping(h.success(refresh)), due + 32001);
    }
  } else if (scenario == "priority") {
    uint32_t remaining = 0;
    if (!tcp) {
      while (h.manager.account(0).next_ms < registration_deadline) {
        h.manager.tick(h.manager.account(0).next_ms);
        assert(h.first.receive() == request);
      }
    }
    assert(h.manager.next_deadline(registration_deadline - 1, &remaining) && remaining == 1);
    h.manager.tick(registration_deadline);
    auto refresh = h.first.receive();
    assert(refresh.rfind("REGISTER ", 0) == 0 && !(h.manager.account(0).pending && h.manager.account(0).request_kind==SipRegistration::RequestKind::KEEPALIVE));
    h.feed(h.first, mapping(h.success(refresh)), registration_deadline + 1);
  } else if (scenario == "unregister") {
    assert(h.manager.request_registration(config.id, false, due + 1));
    h.manager.tick(due + 1);
    auto unregister = h.first.receive();
    assert(unregister.rfind("REGISTER ", 0) == 0 && field(unregister, "Expires") == "0");
    h.manager.handle_response(mapping(response(request, 200)), INADDR_LOOPBACK, h.first.port, tcp, tcp ? 0 : -1, due + 2);
    assert(h.manager.account(0).pending && !(h.manager.account(0).pending && h.manager.account(0).request_kind==SipRegistration::RequestKind::KEEPALIVE));
    h.feed(h.first, h.success(unregister, 0), due + 3);
    assert(!h.manager.is_registered(config.id));
  } else if (scenario == "mapping" || scenario == "credentials") {
    h.feed(h.first, mapping(response(request, 401,
        "WWW-Authenticate: Digest realm=\"probe\",nonce=\"unrelated\",algorithm=MD5\r\n"), 42000), due + 1);
    auto refresh = h.first.receive();
    assert(refresh.rfind("REGISTER ", 0) == 0 && field(refresh, "Expires") == "3600");
    assert(field(refresh, "Contact") == field(initial, "Contact"));
    if (scenario == "credentials") {
      assert(field(refresh, "Authorization").find("nonce=\"retained\"") != std::string::npos);
      assert(h.manager.account(0).digest.nonce == "retained" && h.manager.account(0).digest.nonce_count == 2);
    }
    h.feed(h.first, mapping(h.success(refresh), 42000), due + 2);
    assert(h.manager.is_registered(config.id));
  } else if (scenario == "loss") {
    h.manager.connection_lost(0, due + 1);
    assert(h.manager.account(0).next_ms == due + 1);
    h.manager.tick(due + 1);
    auto refresh = h.first.receive();
    assert(refresh.rfind("REGISTER ", 0) == 0);
    h.feed(h.first, mapping(h.success(refresh)), due + 2);
  } else {
    const unsigned status = argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 200;
    const std::string challenge = status == 401 || status == 407 ?
        std::string(status == 401 ? "WWW-Authenticate" : "Proxy-Authenticate") +
        ": Digest realm=\"probe\",nonce=\"unrelated\",algorithm=MD5\r\n" : "";
    h.feed(h.first, mapping(response(request, status, challenge)), due + 1);
    assert(h.manager.account(0).state == RegistrationState::REGISTERED);
    assert(h.manager.account(0).last_status == 200 && h.manager.account(0).negotiated_expires == 3600);
    assert(h.manager.account(0).next_ms == registration_deadline);
    assert(h.manager.account(0).binding_expires_ms == expiry);
    no_record(h.first);  // A non-INVITE final response has no ACK.
    if (scenario == "allocation") {
      const char* retained = h.manager.account(0).request.data();
#ifdef USE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM
      const auto psram_before = allocated;
#endif
      for (unsigned cycle = 0; cycle < 3; ++cycle) {
        due += 2001;
        allocations = 0; count_allocations = true;
        h.manager.tick(due);
        count_allocations = false;
        assert(allocations == 0);
        auto next = h.first.receive();
        auto answer = mapping(response(next, 405));
        count_allocations = true;
        h.manager.handle_response(answer, INADDR_LOOPBACK, h.first.port, tcp, tcp ? 0 : -1, due + 1);
        count_allocations = false;
        assert(allocations == 0 && !h.manager.account(0).pending);
        assert(h.manager.account(0).request.data() == retained);
#ifdef USE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM
        assert(allocated == psram_before);
#endif
      }
    }
  }
  std::cout << "PASS " << scenario << " " << (tcp ? "tcp" : "udp") << "\n";
}
