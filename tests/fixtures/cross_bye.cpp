// Real protocol methods are injected by test_cross_bye_runtime.py. The initial
// confirmed call and media hardware are fixtures, just as in refer_wire_main.
#include <iostream>

void attach_app(VoipStack &app, SipTransport &wire, const std::string &id) {
  app.t.sip = &wire;
  app.identity = {id, "caller", "Caller", "esp", "ESP", ""};
  app.call_state_ = CallState::IN_CALL;
  wire.signal_context = &app;
  wire.signal_callback = &VoipStack::transport_sip_signal_callback_;
}

void print_requests(const std::vector<std::string> &requests) {
  for (const auto &request : requests) {
    std::cout << "UNEXPECTED_REQUEST bytes=" << request.size()
              << " To=[" << header_value(request, "To") << "]"
              << " From=[" << header_value(request, "From") << "]"
              << " Contact=[" << header_value(request, "Contact") << "]\n"
              << "RAW_REQUEST_BEGIN\n" << request << "RAW_REQUEST_END\n";
  }
}

void close_fixture(SipTransport &wire) {
  wire.signal_callback = nullptr;
  wire.close_tcp_flow_(wire.tcp_flow_);
  if (wire.sip_socket_ >= 0) close(wire.sip_socket_);
  wire.sip_socket_ = -1;
}

int deferred_stop(bool tcp) {
  SipTransport wire;
  Peer remote(tcp);
  setup(wire, remote);
  VoipStack app;
  attach_app(app, wire, "original");

  // The remote BYE is parsed, answered and retires the actual transport.
  // Leave only its application callback queued to model a legitimate race
  // between the SIP task and a native API command in the main loop.
  remote.send_to(wire, request(remote, "BYE", 2));
  const auto replies = remote.drain();
  assert(replies.size() == 1 && replies.front().rfind("SIP/2.0 200", 0) == 0);
  assert(wire.call_id_.empty() && wire.local_tag_.empty() && wire.remote_uri_.empty());
  assert(app.call_state_ == CallState::IN_CALL && app.identity.call_id == "original");
  assert(app.deferred.size() == 1);

  app.stop();  // Actual production public method, not a fabricated SIP request.
  const auto unexpected = remote.drain();
  print_requests(unexpected);
  if (unexpected.empty() && !wire.pending_bye_.empty()) {
    // With an ephemeral UDP peer, the malformed request's fallback URI also
    // loses its port and targets5060. Preserve the actual production-formatted
    // transaction separately instead of pretending it reached the test peer.
    std::cout << "STORED_FORMATTED_BYE target_port=" << wire.pending_bye_.port
              << " To=[" << header_value(wire.pending_bye_.request, "To") << "]"
              << " From=[" << header_value(wire.pending_bye_.request, "From") << "]"
              << " Contact=[" << header_value(wire.pending_bye_.request, "Contact") << "]\n";
  }
  app.drain();  // Normal deferred signal handler and terminal cleanup.

  const bool quiet = app.call_state_ == CallState::IDLE && app.identity.call_id.empty() &&
                     wire.call_id_.empty() && !wire.terminal_transaction_pending_locked_();
  const bool passed = unexpected.empty() && quiet;
  std::cout << "RESULT scenario=deferred-stop tcp=" << tcp
            << " unexpected_requests=" << unexpected.size()
            << " transport_call_id=[" << wire.call_id_ << "]"
            << " bye_pending=" << !wire.pending_bye_.empty()
            << " app_state=" << call_state_to_str(app.call_state_.load())
            << " quiet=" << quiet << '\n';
  close_fixture(wire);
  return passed ? 0 : 1;
}

int redial(const std::string &operation, bool old_tcp, bool new_tcp) {
  SipTransport wire;
  Peer old_peer(old_tcp);
  setup(wire, old_peer);
  VoipStack app;
  attach_app(app, wire, "original");
  const auto old_bye = request(old_peer, "BYE", 2);
  old_peer.send_to(wire, old_bye);
  const auto old_replies = old_peer.drain();
  assert(old_replies.size() == 1 && old_replies.front().rfind("SIP/2.0 200", 0) == 0);
  app.drain();
  assert(app.call_state_ == CallState::IDLE && wire.call_id_.empty());

  // A new connection is a socket fixture. The new INVITE, response, ACK and
  // dialog tags are produced/processed by the actual transport methods.
  wire.close_tcp_flow_(wire.tcp_flow_);
  Peer new_peer(new_tcp);
  wire.remote_ip_v4_ = INADDR_LOOPBACK;
  wire.remote_sip_port_ = ntohs(new_peer.address.sin_port);
  wire.remote_sip_tcp_ = new_tcp;
  wire.tcp_flow_.socket = new_peer.esp;
  app.identity = {"redial-new", "esp", "ESP", "target", "Target", "Target"};
  app.call_state_ = CallState::CALLING;
  assert(wire.send_invite_unlocked_("redial-new", "esp", "ESP", "target", "Target"));
  const auto invites = new_peer.drain();
  assert(invites.size() == 1 && invites.front().rfind("INVITE ", 0) == 0);
  new_peer.send_to(wire, response(invites.front(), 200));
  const auto acks = new_peer.drain();
  assert(acks.size() == 1 && acks.front().rfind("ACK ", 0) == 0);
  app.drain();
  assert(wire.call_id_ == "redial-new" && wire.media_active_ && app.call_state_ == CallState::IN_CALL);

  const auto current_id = wire.call_id_;
  const auto local_tag = wire.local_tag_;
  const auto remote_tag = wire.remote_tag_;
  const auto request_cseq = wire.cseq_;
  const auto socket = wire.tcp_flow_.socket.load();
  const auto delivered = wire.delivered.size();
  bool command_sent = false;
  if (operation == "stale-transport") {
    command_sent = wire.send_bye("original");
  } else if (operation == "stale-api") {
    app.hangup_call_if_id("original");
  } else {
    assert(operation == "late-bye");
    if (!old_tcp) {
      old_peer.send_to(wire, old_bye);
      const auto replay = old_peer.drain();
      assert(replay.size() == 1 && replay.front() == old_replies.front());
    } else if (new_tcp) {
      // A delayed request can arrive on the surviving/current TCP flow.
      // Never try to write to the old connection after closing its ESP end.
      new_peer.send_to(wire, old_bye);
      const auto replay = new_peer.drain();
      assert(replay.size() == 1 && replay.front() == old_replies.front());
    } else {
      // After changing transport, a new UDP transaction with the retired
      // dialog tags must get481, rather than replaying an unrelated TCP cache.
      auto delayed = request(new_peer, "BYE", 3);
      new_peer.send_to(wire, delayed);
      const auto rejected = new_peer.drain();
      assert(rejected.size() == 1 && rejected.front().rfind("SIP/2.0 481", 0) == 0);
    }
  }
  const auto unexpected = new_peer.drain();
  print_requests(unexpected);
  const bool unchanged = wire.call_id_ == current_id && wire.local_tag_ == local_tag &&
                         wire.remote_tag_ == remote_tag && wire.cseq_ == request_cseq &&
                         wire.media_active_ && wire.remote_sip_tcp_ == new_tcp &&
                         wire.tcp_flow_.socket == socket && wire.delivered.size() == delivered &&
                         !wire.terminal_transaction_pending_locked_() &&
                         app.call_state_ == CallState::IN_CALL && app.identity.call_id == current_id;
  std::cout << "RESULT scenario=" << operation << " old_tcp=" << old_tcp << " new_tcp=" << new_tcp
            << " command_sent=" << command_sent << " unexpected_requests=" << unexpected.size()
            << " current_call_id=[" << wire.call_id_ << "] unchanged=" << unchanged << '\n';
  const bool passed = !command_sent && unexpected.empty() && unchanged;
  close_fixture(wire);
  return passed ? 0 : 1;
}

int valid_stop(bool tcp, const std::string &identity) {
  SipTransport wire;
  Peer remote(tcp);
  setup(wire, remote);
  std::string requested = "original";
  if (identity == "implicit") requested.clear();
  if (identity == "logical") {
    wire.logical_call_id_ = "logical-original";
    wire.logical_wire_id_ = "original";
    requested = wire.logical_call_id_;
  }
  assert(wire.send_bye(requested));
  const auto requests = remote.drain();
  assert(requests.size() == 1 && requests.front().rfind("BYE ", 0) == 0);
  assert(header_value(requests.front(), "Call-ID") == "original");
  assert(!name_addr_uri(header_value(requests.front(), "From")).empty());
  assert(!name_addr_uri(header_value(requests.front(), "To")).empty());
  assert(!tag_from_header(header_value(requests.front(), "From")).empty());
  assert(!tag_from_header(header_value(requests.front(), "To")).empty());
  assert(wire.terminal_transaction_pending_locked_());
  remote.send_to(wire, response(requests.front(), 200));
  assert(wire.call_id_.empty() && !wire.terminal_transaction_pending_locked_());
  close_fixture(wire);
  return 0;
}

int main(int argc, char **argv) {
  assert(argc >= 3);
  if (std::string(argv[1]) == "deferred-stop") return deferred_stop(std::atoi(argv[2]));
  assert(argc >= 4);
  if (std::string(argv[1]) == "valid-stop") return valid_stop(std::atoi(argv[2]), argv[3]);
  return redial(argv[1], std::atoi(argv[2]), std::atoi(argv[3]));
}
