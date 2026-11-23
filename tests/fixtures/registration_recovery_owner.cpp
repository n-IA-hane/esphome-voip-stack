#define main previous_registration_main
#include "registration_engine.cpp"
#undef main

int main(int argc, char **argv) {
  assert(argc == 4);
  const unsigned status = std::stoul(argv[1]);
  const std::string mode = argv[3];
  Harness h(std::string(argv[2]) == "tcp");
  auto c = h.config();
  assert(h.manager.set_account(c, 0)); h.manager.tick(0);
  auto attempt = h.first.receive();
  const auto challenge = status == 407 ? "Proxy-Authenticate" : "WWW-Authenticate";
  uint32_t now = 1;
  for (unsigned count = 0; count < (status == 401 || status == 407 ? 3U : 1U); ++count) {
    h.feed(h.first, response(attempt, status,
        (status == 401 || status == 407 ? std::string(challenge) + ": Digest realm=\"pbx\", nonce=\"disabled\"\r\n" : "") +
        "Retry-After: 47\r\n"), now++);
    if (count < 2 && (status == 401 || status == 407)) attempt = h.first.receive();
  }
  auto a = h.manager.account(0);
  assert(!a.pending && a.scheduled && a.desired);
  assert(a.next_ms == now - 1 + 47000 && "rejected enabled accounts retain bounded Retry-After recovery");
  h.manager.tick(a.next_ms - 1);
  char byte; assert(recv(h.first.peer, &byte, 1, MSG_DONTWAIT) < 0);
  if (mode == "disable" || mode == "remove") {
    if (mode == "remove") assert(h.manager.remove_account(c.id));
    else {
      assert(h.manager.request_registration(c.id, false, now)); h.manager.tick(now);
      auto unregister = h.first.receive(); assert(field(unregister, "Expires") == "0");
      h.feed(h.first, response(unregister, 403), now + 1);
    }
    h.manager.tick(a.next_ms + 900000);
    assert(recv(h.first.peer, &byte, 1, MSG_DONTWAIT) < 0);
    return 0;
  }
  if (mode == "credentials") {
    c.password = "corrected-test-password";
    assert(h.manager.set_account(c, now)); h.manager.tick(now);
  } else h.manager.tick(a.next_ms);
  auto recovered = h.first.receive();
  assert(field(recovered, "Call-ID") != "" && (field(recovered, "Call-ID") != field(attempt, "Call-ID") || field(recovered, "CSeq") != field(attempt, "CSeq")));
  const unsigned auth_status = status == 407 ? 407 : 401;
  h.feed(h.first, response(recovered, auth_status, std::string(challenge) +
      ": Digest realm=\"pbx\", nonce=\"enabled\"\r\n"), a.next_ms + 1);
  auto authorized = h.first.receive();
  assert(!field(authorized, auth_status == 407 ? "Proxy-Authorization" : "Authorization").empty());
  h.feed(h.first, h.success(authorized), a.next_ms + 2);
  assert(h.manager.is_registered(c.id) && h.manager.account(0).auth_attempts == 0 && h.manager.account(0).failures == 0);
}
