// Drive a credential change while the old account still owns event dialogs.
struct HandoverNotifier {
  std::vector<std::string> requests;
  std::string reply;

  void attach(SipMwi &subscriptions) {
    SipMwi::Hooks hooks;
    hooks.context = this;
    hooks.local_ip = [](void *, uint32_t) { return std::string("127.0.0.1"); };
    hooks.random = random_value;
    hooks.send = [](void *ctx, size_t, std::string_view wire, uint32_t, uint16_t, bool) {
      static_cast<HandoverNotifier *>(ctx)->requests.emplace_back(wire);
      return true;
    };
    hooks.reply = [](void *ctx, const std::string &wire, uint32_t, uint16_t, bool, int) {
      static_cast<HandoverNotifier *>(ctx)->reply = wire;
      return true;
    };
    subscriptions.set_hooks(hooks);
  }

  void answer(SipMwi &subscriptions, Peer &peer, int flow, const std::string &request,
              unsigned status, unsigned now) {
    std::string wire = response(request, status, "Expires: " + field(request, "Expires") + "\r\n");
    if (field(request, "To").find(";tag=") == std::string::npos)
      wire.insert(wire.find("\r\nCall-ID:"), ";tag=notifier");
    assert(subscriptions.handle_response(wire, INADDR_LOOPBACK, peer.port, peer.tcp, flow, now));
  }

  void notify(SipMwi &subscriptions, Peer &peer, int flow, const std::string &request,
              unsigned cseq, unsigned now, bool terminated = false, unsigned expected_status = 200) {
    const auto event = field(request, "Event");
    std::string body;
    if (!terminated) {
      if (event == "message-summary") body = "Messages-Waiting: yes\r\n";
      if (event == "dialog") body = "<dialog-info xmlns='urn:ietf:params:xml:ns:dialog-info' "
          "version='0' state='full' entity='sip:peer@example.org'/>";
      if (event == "presence") body = "<presence xmlns='urn:ietf:params:xml:ns:pidf' entity='sip:peer@example.org'>"
          "<tuple id='phone'><status><basic>open</basic></status></tuple></presence>";
    }
    const auto sequence = std::to_string(cseq);
    const auto contact = field(request, "Contact");
    const std::string wire = "NOTIFY " + contact.substr(1, contact.size() - 2) + " SIP/2.0\r\n"
        "Via: SIP/2.0/UDP 127.0.0.1;branch=z9hG4bK-event" + sequence +
        "\r\nFrom: <sip:peer@example.org>;tag=notifier\r\nTo: " + field(request, "From") +
        "\r\nCall-ID: " + field(request, "Call-ID") + "\r\nCSeq: " + sequence + " NOTIFY\r\nEvent: " + event +
        "\r\nSubscription-State: " + (terminated ? "terminated;reason=timeout" : "active;expires=3600") +
        "\r\nContact: <sip:peer@127.0.0.1>\r\nContent-Type: " + field(request, "Accept") +
        "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    reply.clear();
    assert(subscriptions.handle_notify(wire, INADDR_LOOPBACK, peer.port, peer.tcp, flow, now));
    assert(reply.rfind("SIP/2.0 " + std::to_string(expected_status), 0) == 0);
  }
};

void managed_subscription_handover(Harness &lab, SipTransport &transport, const SipAccountConfig &old,
                                  Saved &saved, const std::string &scenario) {
  HandoverNotifier notifier;
  auto &subscriptions = transport.mwi_;
  notifier.attach(subscriptions);
  assert(subscriptions.configure_blf(1));
  assert(subscriptions.set_blf_watch(0, 0, lab.manager.account(0), "Peer", "sip:peer@example.org", 2));
  auto second = lab.config("desk", 1);
  assert(lab.manager.set_account(second, 2));
  transport.step(2);
  lab.feed(lab.second, lab.success(lab.second.receive()), 3, 1);
  transport.step(3);
  const auto watches = std::move(notifier.requests);
  assert(watches.size() == 4);  // Messages on both lines, dialog and presence on the managed one.
  std::string independent, previous;
  for (const auto &request : watches) {
    const bool other = field(request, "From").find("sip:101@") != std::string::npos;
    auto &peer = other ? lab.second : lab.first;
    notifier.answer(subscriptions, peer, other, request, 200, 4);
    notifier.notify(subscriptions, peer, other, request, 1, 5);
    if (other) independent = request;
    else if (field(request, "Event") == "dialog") previous = request;
  }
  assert(!independent.empty() && !previous.empty() && subscriptions.blf_state(0)->available);

  if (scenario == "watch-refresh") {
    subscriptions.on_registration_accepted(0, lab.manager.account(0), 6);
    subscriptions.tick(6);
    assert(notifier.requests.size() == 3);
    for (const auto &refresh : notifier.requests) {
      assert(field(refresh, "Expires") != "0");
      notifier.answer(subscriptions, lab.first, 0, refresh, 403, 7);
    }
    assert(!subscriptions.account_retired(0));
    assert(subscriptions.state(0).available && subscriptions.blf_state(0)->available);
    notifier.notify(subscriptions, lab.first, 0, previous, 2, 8);
    return;
  }

  auto replacement = old;
  replacement.password = "test-recommissioned";
  if (scenario == "watch-remove") assert(transport.remove_account(old.id, persist, &saved));
  else assert(transport.set_account(replacement, persist, &saved));
  transport.step(6);
  auto unregister = lab.first.receive();
  assert(field(unregister, "Expires") == "0");
  // A new PBX account already owns this login. The old credential fails Digest.
  const std::string challenge = "WWW-Authenticate: Digest realm=\"example.org\", nonce=\"fixture\"\r\n";
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    std::thread registrar([&] { lab.feed(lab.first, response(unregister, 401, challenge), 7); });
    registrar.join();
    if (attempt != 2) unregister = lab.first.receive();
  }
  transport.step(8);
  const auto retirements = std::move(notifier.requests);
  assert(retirements.size() == 3);
  for (const auto &request : retirements) {
    assert(field(request, "Expires") == "0");
    if (scenario != "watch-timeout")
      notifier.answer(subscriptions, lab.first, 0, request,
                      scenario == "watch-notify" ? 200 : scenario == "watch-missing" ? 481 : 403, 9);
  }
  unsigned now = 10;
  if (scenario == "watch-notify" || scenario == "watch-timeout") {
    transport.step(now);
    assert(saved.count == 0 && transport.managed_update_->active);
    if (scenario == "watch-notify") {
      for (const auto &request : retirements)
        notifier.notify(subscriptions, lab.first, 0, request, 2, ++now, true);
    } else {
      now += 32000;
    }
  }
  transport.step(++now);
  assert(saved.count == 1 && !transport.managed_update_->active);
  assert(subscriptions.state(1).waiting && lab.manager.is_registered(second.id));
  notifier.notify(subscriptions, lab.second, 1, independent, 2, ++now);
  notifier.notify(subscriptions, lab.first, 0, previous, 3, ++now, true, 481);
  if (scenario == "watch-remove") {
    assert(saved.removed && lab.manager.find(old.id) < 0 && quiet(lab.first));
    return;
  }
  assert(saved.config.username == old.username && saved.config.password == replacement.password);
  auto registration = lab.first.receive();
  assert(field(registration, "Expires") == "3600");
  assert(field(registration, "Call-ID") != field(unregister, "Call-ID"));
  std::thread registrar([&] { lab.feed(lab.first, response(registration, 401, challenge), ++now); });
  registrar.join();
  auto authorized = lab.first.receive();
  const auto auth = field(authorized, "Authorization");
  const std::string target = "sip:" + replacement.registrar + ":" + std::to_string(replacement.port);
  const auto expected = hash_value("MD5", hash_value("MD5", replacement.username + ":example.org:" +
      replacement.password) + ":fixture:" + hash_value("MD5", "REGISTER:" + target));
  assert(auth.find("username=\"Waveshare S3 Audio\"") != std::string::npos);
  assert(auth.find("response=\"" + expected + "\"") != std::string::npos);
  lab.feed(lab.first, lab.success(authorized), ++now);
  assert(lab.manager.is_registered(old.id));
}
