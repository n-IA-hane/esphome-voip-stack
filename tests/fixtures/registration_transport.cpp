std::string SipTransport::registration_hash_(const std::string &algorithm, const std::string &input) {
  const EVP_MD *md = algorithm == "MD5" ? EVP_md5() : EVP_sha256();
  unsigned char digest[EVP_MAX_MD_SIZE]; unsigned n = 0;
  assert(EVP_Digest(input.data(), input.size(), digest, &n, md, nullptr) == 1);
  std::string result;
  for (unsigned i = 0; i < n; ++i) { char item[3]; snprintf(item, sizeof(item), "%02x", digest[i]); result += item; }
  return result;
}
void bind_account(SipTransport &s, Peer &peer) {
  setup(s, peer); s.call_id_.clear(); s.media_active_ = false;
  if (peer.tcp) { s.tcp_flow_.socket = -1; s.registration_flows_[0].socket = peer.esp; }
  SipRegistration::Hooks hooks; hooks.context = &s; hooks.send = SipTransport::registration_send_;
  hooks.resolve = [](void *, size_t, const std::string &) { return uint32_t(INADDR_LOOPBACK); };
  hooks.local_ip = [](void *, uint32_t) { return std::string("127.0.0.1"); };
  hooks.random = esp_random; hooks.hash = SipTransport::registration_hash_;
  s.registration_.set_hooks(hooks);
  SipAccountConfig config; config.id = "office"; config.registrar = "127.0.0.1"; config.domain = "pbx.example";
  config.username = "100"; config.password = "test-only-password"; config.tcp = peer.tcp;
  config.port = ntohs(peer.address.sin_port);
  assert(s.registration_.set_account(config, clock_ms)); s.registration_.tick(clock_ms);
  auto requests = peer.drain(); assert(requests.size() == 1);
  auto answer = response(requests[0], 200);
  answer.insert(answer.find("Content-Length:"), "Contact: " + header_value(requests[0], "Contact") + "\r\n");
  peer.send_to(s, answer); assert(s.registration_.is_registered("office"));
}
void start_account_call(SipTransport &s, Peer &peer) {
  std::string host; uint16_t port; bool tcp;
  assert(s.prepare_account_line("office", &host, &port, &tcp));
  assert(host == "127.0.0.1" && port == ntohs(peer.address.sin_port) && tcp == peer.tcp);
  s.remote_ip_v4_ = INADDR_LOOPBACK; s.remote_sip_port_ = port; s.remote_sip_tcp_ = tcp;
  assert(s.send_invite_unlocked_("account-call", "esp", "ESP", "101", "Peer"));
  assert(s.active_account_ == 0);
}

std::string pick(const std::vector<std::string> &messages, const std::string &method,
                 const std::string &call = "") {
  for (const auto &message : messages)
    if (message.rfind(method + " ", 0) == 0 && (call.empty() || header_value(message, "Call-ID") == call))
      return message;
  assert(false && "expected SIP record missing"); return "";
}
void confirm_account_call(SipTransport &s, Peer &pbx) {
  start_account_call(s, pbx);
  auto invite = pick(pbx.drain(), "INVITE");
  pbx.send_to(s, response(invite, 401));
  auto records = pbx.drain(); invite = pick(records, "INVITE");
  assert(!header_value(invite, "Authorization").empty());
  pbx.send_to(s, response(invite, 200));
  assert(!pick(pbx.drain(), "ACK").empty());
  s.media_active_ = true;
}
std::string account_refer(SipTransport &s, Peer &pbx, const std::string &target) {
  const auto refer = request(pbx, "REFER", 9, "Refer-To: <" + target + ">\r\n",
                            s.call_id_, s.local_tag_, s.remote_tag_);
  pbx.send_to(s, refer);
  auto records = pbx.drain();
  assert(!pick(records, "SIP/2.0", s.call_id_).empty());
  auto notify = pick(records, "NOTIFY", s.call_id_);
  pbx.send_to(s, response(notify, 200));
  assert(s.refer_dialog_);
  for (const auto &message : records)
    if (message.rfind("INVITE ", 0) == 0) return message;
  return "";
}
void complete_direct_connect(SipTransport &s, Peer &target) {
  if (!target.tcp) return;
  auto &flow = s.refer_dialog_->tcp_flow_;
  flow.socket = target.esp; flow.connect_requested = false;
  assert(s.send_sip_tcp_record_(flow.pending.data(), flow.pending.size(), target.esp));
  flow.pending.clear();
}
void account_refer_auth_case(SipTransport &s, Peer &pbx) {
  confirm_account_call(s, pbx);
  const std::string original = s.call_id_;
  auto invite = account_refer(s, pbx, "sip:102@pbx.example");
  assert(invite.rfind("INVITE sip:102@pbx.example ", 0) == 0);
  assert(header_value(invite, "From").find("100@pbx.example") != std::string::npos);
  const auto candidate = header_value(invite, "Call-ID");
  pbx.send_to(s, response(invite, 401));
  auto records = pbx.drain();
  assert(!pick(records, "ACK", candidate).empty()); invite = pick(records, "INVITE", candidate);
  assert(!header_value(invite, "Authorization").empty());
  assert(s.active_account_ == 0 && s.refer_dialog_->active_account_ == 0);
  pbx.send_to(s, response(invite, 200)); records = pbx.drain();
  assert(!pick(records, "ACK", candidate).empty());
  const auto notify = pick(records, "NOTIFY", original);
  const auto old_bye = pick(records, "BYE", original);
  assert(s.call_id_ == candidate && s.active_account_ == 0);
  // A challenge for the retiring leg must not change the new leg's context.
  pbx.send_to(s, response(old_bye, 407)); records = pbx.drain();
  const auto authenticated_bye = pick(records, "BYE", original);
  assert(!header_value(authenticated_bye, "Proxy-Authorization").empty());
  assert(s.dialog_proxy_digest_.empty() && !s.refer_dialog_->dialog_proxy_digest_.empty());
  pbx.send_to(s, response(authenticated_bye, 200)); pbx.send_to(s, response(notify, 200));
  s.pump_refer_(); assert(!s.refer_dialog_);
  assert(s.call_id_ == candidate && s.registration_.is_registered("office"));
  assert(s.send_bye_unlocked_(candidate)); auto bye = pick(pbx.drain(), "BYE", candidate);
  pbx.send_to(s, response(bye, 200));
  assert(s.call_id_.empty() && s.active_account_ == -1 && s.registration_.is_registered("office"));
}
void account_refer_direct_case(SipTransport &s, Peer &pbx, bool target_tcp, bool lose_owner) {
  confirm_account_call(s, pbx);
  const std::string original = s.call_id_;
  Peer target(target_tcp);
  assert(account_refer(s, pbx, target.uri()).empty());
  assert(s.refer_dialog_->active_account_ == -1);
  complete_direct_connect(s, target);
  auto invite = pick(target.drain(), "INVITE");
  assert(header_value(invite, "Authorization").empty());
  const auto candidate = header_value(invite, "Call-ID");
  if (lose_owner) {
    assert(pbx.tcp);
    s.handle_tcp_peer_loss_(&s.registration_flows_[0]);
    assert(s.refer_dialog_ && s.refer_dialog_->owner_invite_ended);
    assert(s.call_id_ == original && s.connection_losses == 0);
  }
  target.send_to(s, response(invite, 200));
  assert(!pick(target.drain(), "ACK", candidate).empty());
  assert(s.call_id_ == candidate && s.active_account_ == -1);
  assert(s.dialog_account_socket_ == -1 && s.dialog_digest_.empty());
  if (!lose_owner) {
    auto records = pbx.drain();
    const auto notify = pick(records, "NOTIFY", original), bye = pick(records, "BYE", original);
    assert(s.refer_dialog_ && s.refer_dialog_->active_account_ == 0);
    // The retiring dialog is still an account consumer, even after unregister.
    assert(s.register_account("office", false)); s.registration_.tick(clock_ms);
    auto registration = pick(pbx.drain(), "REGISTER"); pbx.send_to(s, response(registration, 200));
    assert(!s.registration_.account(0).bound);
    auto replacement = s.registration_.account(0).config; replacement.password = "replacement-test-value";
    assert(!s.set_account(replacement));
    pbx.send_to(s, response(bye, 200)); pbx.send_to(s, response(notify, 200));
    s.pump_refer_(); assert(!s.refer_dialog_);
    assert(s.set_account(replacement));
  } else {
    s.pump_refer_(); assert(!s.refer_dialog_);
  }
  // New dialog requests must use the new peer, not the previous account socket.
  assert(s.send_bye_unlocked_(candidate)); auto bye = pick(target.drain(), "BYE", candidate);
  target.send_to(s, response(bye, 200)); assert(s.call_id_.empty());
}
#ifdef USE_ESPHOME_VOIP_MWI
void notify_on_new_control_flow(SipTransport &s, Peer &pbx) {
  SipMwi::Hooks hooks;
  hooks.context = &s; hooks.send = SipTransport::registration_send_; hooks.reply = SipTransport::registration_mwi_reply_;
  hooks.local_ip = [](void *, uint32_t) { return std::string("127.0.0.1"); };
  hooks.random = esp_random; hooks.hash = SipTransport::registration_hash_; hooks.local_port = s.sip_port_;
  s.mwi_.set_hooks(hooks);
  const auto &account = s.registration_.account(0);
  s.mwi_.update_account(0, account.config, account.target_ip, account.target_port, account.contact, true, true, clock_ms);
  s.mwi_.tick(clock_ms); const auto subscribe = pbx.drain(); assert(subscribe.size() == 1);
  Peer direct(true); setup(s, direct);
  const int original_socket = s.tcp_flow_.socket;
  int listener = socket(AF_INET, SOCK_STREAM, 0); assert(listener >= 0);
  sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
  socklen_t length = sizeof(address); assert(getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length) == 0);
  assert(listen(listener, 1) == 0);
  int notifier = socket(AF_INET, SOCK_STREAM, 0); assert(notifier >= 0);
  assert(connect(notifier, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
  sockaddr_in source{}; length = sizeof(source);
  int incoming = accept(listener, reinterpret_cast<sockaddr *>(&source), &length); assert(incoming >= 0); close(listener);
  assert(fcntl(incoming, F_SETFL, O_NONBLOCK) == 0);
  assert(s.park_inbound_control_(incoming, source));
  std::string body = "Messages-Waiting: yes\r\nVoice-Message: 2/1 (0/0)\r\n";
  const auto &sub = subscribe[0];
  std::string message = "NOTIFY " + strip_angle_uri(header_value(sub, "Contact")) + " SIP/2.0\r\n" +
      "Via: SIP/2.0/TCP 127.0.0.1;branch=z9hG4bKnotify\r\nFrom: " + header_value(sub, "To") + ";tag=notifier\r\n" +
      "To: " + header_value(sub, "From") + "\r\nCall-ID: " + header_value(sub, "Call-ID") + "\r\n" +
      "CSeq: 1 NOTIFY\r\nEvent: message-summary\r\nSubscription-State: active;expires=60\r\n" +
      "Contact: <" + pbx.uri() + ">\r\nContent-Type: application/simple-message-summary\r\nContent-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;
  assert(send(notifier, message.data(), message.size(), 0) == static_cast<ssize_t>(message.size()));
  pollfd ready{incoming, POLLIN, 0}; assert(poll(&ready, 1, 1000) == 1);
  s.handle_sip_stream_(incoming, source);
  char answer[4096]; const int n = recv(notifier, answer, sizeof(answer), 0); assert(n > 0);
  assert(std::string(answer, n).rfind("SIP/2.0 200", 0) == 0);
  assert(s.mwi_.state(0).available && s.mwi_.state(0).waiting && s.mwi_.state(0).new_messages == 2);
  assert(s.call_id_ == "original" && s.tcp_flow_.socket == original_socket && s.media_active_);
  assert(s.registration_flows_[0].socket == pbx.esp && s.inbound_control_flow_.socket == -1);
  close(notifier);
}
#endif

int main(int argc, char **argv) {
  assert(argc == 3); const std::string scenario = argv[1]; const bool tcp = std::string(argv[2]) == "tcp";
  SipTransport s; Peer pbx(tcp); bind_account(s, pbx);
#ifdef USE_ESPHOME_VOIP_MWI
  if (scenario == "notify-control-flow") {
    notify_on_new_control_flow(s, pbx);
    for (auto &flow : s.registration_flows_) s.close_tcp_flow_(flow);
    s.close_tcp_flow_(s.tcp_flow_); close(s.sip_socket_); return 0;
  }
#endif
  if (scenario == "contact-next-hop" || scenario == "proxy-next-hop" || scenario == "route-next-hop") {
    assert(!tcp);Peer target(false);Peer router(false);
    if(scenario=="proxy-next-hop")s.registration_.account(0).config.proxy="127.0.0.1";
    start_account_call(s,pbx);auto invite=pick(pbx.drain(),"INVITE");
    auto answer=response(invite,200);
    std::string headers="Contact: <"+target.uri()+">\r\n";
    if(scenario=="route-next-hop")headers+="Record-Route: <"+router.uri()+";lr>\r\n";
    answer.insert(answer.find("Content-Length:"),headers);pbx.send_to(s,answer);
    Peer& hop=scenario=="contact-next-hop"?target:scenario=="proxy-next-hop"?pbx:router;
    auto records=hop.drain();auto ack=pick(records,"ACK");assert(ack.find("ACK "+target.uri()+" ")==0);
    assert(s.send_bye_unlocked_(s.call_id_));auto bye=pick(hop.drain(),"BYE");
    assert(bye.find("BYE "+target.uri()+" ")==0);hop.send_to(s,response(bye,200));
    assert(s.call_id_.empty()&&s.registration_.is_registered("office"));close(s.sip_socket_);return 0;
  }
  if (scenario == "retired-flow-late-loss") {
    assert(tcp);auto& account=s.registration_.account(0);
    account.bound=false;account.pending=false;account.state=RegistrationState::FAILED;
    auto replacement=account.config;replacement.password="replacement-test-password";
    assert(s.set_account(replacement));const auto generation=account.call_id;
    assert(account.state==RegistrationState::DISABLED&&account.next_ms==clock_ms);
    s.handle_tcp_peer_loss_(&s.registration_flows_[0]);
    assert(account.call_id==generation&&account.state==RegistrationState::DISABLED&&account.next_ms==clock_ms);
    s.registration_.tick(clock_ms);assert(s.registration_flows_[0].connect_requested);
    assert(!s.registration_flows_[0].pending.empty());
    s.close_tcp_flow_(s.registration_flows_[0]);close(s.sip_socket_);return 0;
  }
  if (scenario == "failed-flow-retry" || scenario == "failed-flow-replace" || scenario == "failed-flow-remove") {
    assert(tcp);
    auto& flow = s.registration_flows_[0];
    const std::string obsolete = "REGISTER sip:obsolete.invalid SIP/2.0\r\nContent-Length: 0\r\n\r\n";
    flow.pending = obsolete; flow.connect_requested = true;
    if (scenario == "failed-flow-retry") {
      s.handle_tcp_peer_loss_(&flow);
      assert(flow.pending.empty() && !flow.connect_requested);
      const auto next = s.registration_.account(0).next_ms;
      s.registration_.tick(next);
      assert(flow.connect_requested && flow.pending != obsolete && !flow.pending.empty());
      assert(flow.pending.find("REGISTER sip:127.0.0.1:") == 0);
    } else {
      s.registration_.account(0).bound = false;
      s.registration_.account(0).state = RegistrationState::FAILED;
      s.registration_.account(0).pending = false;
      if (scenario == "failed-flow-replace") {
        auto config = s.registration_.account(0).config; config.enabled = false;
        assert(s.set_account(config));
      } else assert(s.remove_account("office"));
      assert(flow.pending.empty() && !flow.connect_requested && flow.close_requested);
    }
    s.close_tcp_flow_(flow); close(s.sip_socket_); return 0;
  }
  if (scenario == "reset-missing") {
    unsigned commits = 0;
    auto commit = [](void *p, const std::string &, const SipAccountConfig *) { ++*static_cast<unsigned *>(p); return true; };
    assert(s.remove_account("already-removed", commit, &commits, true));
    assert(commits == 1 && s.registration_.is_registered("office"));
    for (auto &flow : s.registration_flows_) s.close_tcp_flow_(flow);
    close(s.sip_socket_); return 0;
  }
  if (scenario == "refer-authenticated") {
    account_refer_auth_case(s, pbx);
  } else if (scenario == "refer-direct-udp" || scenario == "refer-direct-tcp") {
    account_refer_direct_case(s, pbx, scenario == "refer-direct-tcp", false);
  } else if (scenario == "refer-owner-loss-udp" || scenario == "refer-owner-loss-tcp") {
    account_refer_direct_case(s, pbx, scenario == "refer-owner-loss-tcp", true);
  } else if (scenario == "atomic-change") {
    unsigned writes = 0;
    auto commit = [](void *ctx, const std::string &, const SipAccountConfig *) { ++*static_cast<unsigned *>(ctx); return true; };
    auto changed = s.registration_.account(0).config; changed.password = "new-test-password";
    assert(!s.set_account(changed, commit, &writes)); assert(writes == 0);
    assert(s.register_account("office", false)); s.registration_.tick(clock_ms);
    auto messages = pbx.drain(); assert(messages.size() == 1);
    pbx.send_to(s, response(messages[0], 200));
    assert(!s.registration_.account(0).bound);
    auto reject = [](void *, const std::string &, const SipAccountConfig *) { return false; };
    assert(!s.set_account(changed, reject, nullptr));
    assert(s.registration_.account(0).config.password == "test-only-password");
    assert(s.set_account(changed, commit, &writes)); assert(writes == 1);
  } else {
    start_account_call(s, pbx); auto sent = pbx.drain(); assert(sent.size() == 1);
    auto invite = sent[0]; assert(header_value(invite, "From").find("100@pbx.example") != std::string::npos);
    if (scenario == "authenticated-call") {
      pbx.send_to(s, response(invite, 401)); sent = pbx.drain();
      assert(sent.size() == 2 && sent[0].rfind("ACK ", 0) == 0 && sent[1].rfind("INVITE ", 0) == 0);
      assert(header_value(sent[0], "CSeq") == "1 ACK");
      assert(via_branch(header_value(sent[0], "Via")) == via_branch(header_value(invite, "Via")));
      invite = sent[1]; assert(header_value(invite, "CSeq") == "2 INVITE");
      assert(!header_value(invite, "Authorization").empty());
      pbx.send_to(s, response(invite, 200)); sent = pbx.drain();
      assert(sent.size() == 1 && sent[0].rfind("ACK ", 0) == 0);
      assert(header_value(sent[0], "Authorization") == header_value(invite, "Authorization"));
      assert(s.send_bye_unlocked_("account-call")); sent = pbx.drain(); assert(sent.size() == 1);
      pbx.send_to(s, response(sent[0], 407)); sent = pbx.drain(); assert(sent.size() == 1);
      assert(sent[0].rfind("BYE ", 0) == 0 && !header_value(sent[0], "Proxy-Authorization").empty());
      assert(!header_value(sent[0], "Authorization").empty());
      pbx.send_to(s, response(sent[0], 200)); assert(s.call_id_.empty() && s.active_account_ == -1);
      assert(s.registration_.is_registered("office"));
      if (tcp) assert(s.registration_flows_[0].socket == pbx.esp);
    } else if (scenario == "foreign-flow") {
      Peer foreign(true); s.tcp_flow_.socket = foreign.esp;
      s.handle_tcp_peer_loss_(&s.tcp_flow_);
      assert(s.call_id_ == "account-call" && s.active_account_ == 0 && s.connection_losses == 0);
      if (tcp) {
        Peer datagram(false); datagram.send_to(s, request(datagram, "INVITE", 7, "", "foreign", "", "remote"));
        auto rejected = datagram.drain(); assert(rejected.size() == 1 && rejected[0].rfind("SIP/2.0 486", 0) == 0);
        assert(s.registration_flows_[0].socket == pbx.esp && s.remote_sip_tcp_);
      }
      assert(s.call_id_ == "account-call");
    } else assert(false);
  }
  for (auto &flow : s.registration_flows_) s.close_tcp_flow_(flow);
  s.close_tcp_flow_(s.tcp_flow_); close(s.sip_socket_);
}
