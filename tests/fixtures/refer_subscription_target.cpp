int main(int argc, char **argv) {
  assert(argc == 4);
  const bool old_tcp = std::atoi(argv[1]), new_tcp = std::atoi(argv[2]);
  const std::string route = argv[3];
  SipTransport sip;
  Peer original(old_tcp), candidate(new_tcp), refreshed(false);
  bind_account(sip, original);
  confirm_account_call(sip, original);
  if (route == "loose" || route == "strict") {
    sip.dialog_route_count_ = 2;
    sip.dialog_routes_[0] = "<" + original.uri() + (route == "loose" ? ";lr>" : ">");
    sip.dialog_routes_[1] = "<sip:route2@192.0.2.19;lr>";
  }
  const auto routes = sip.dialog_routes_;
  const auto route_count = sip.dialog_route_count_;
  assert(account_refer(sip, original, candidate.uri()).empty());
  complete_direct_connect(sip, candidate);
  const auto invite = pick(candidate.drain(), "INVITE");
  candidate.send_to(sip, response(invite, 180));
  const auto candidate_id = sip.refer_dialog_->call_id_;
  const auto deadline = sip.refer_dialog_->attempt_deadline_ms;
  const auto old_target = sip.remote_target_uri_;
  const auto old_lease = sip.refer_dialog_->subscription_deadline_ms;
  const auto old_cseq = sip.remote_dialog_cseq_;
  const std::string target = refreshed.uri();
  const auto refresh = request(original, "SUBSCRIBE", 10,
      "Event: refer;id=9\r\nExpires: 60\r\nContact: <" + (route == "invalid" ? "" : target) +
      ">\r\nRecord-Route: <sip:ignored@192.0.2.99;lr>\r\n",
      sip.call_id_, sip.local_tag_, sip.remote_tag_);
  original.send_to(sip, refresh);
  const auto original_records = original.drain();
  if (route == "invalid") {
    assert(original_records.size() == 1 && original_records[0].rfind("SIP/2.0 400", 0) == 0);
    assert(sip.remote_target_uri_ == old_target && sip.remote_dialog_cseq_ == old_cseq);
    assert(sip.refer_dialog_->subscription_deadline_ms == old_lease);
    assert(refreshed.drain().empty());
  } else {
    assert(sip.remote_target_uri_ == target);
    assert(!original_records.empty() && original_records[0].rfind("SIP/2.0 200", 0) == 0);
    assert(header_value(original_records[0], "Expires") == "60");
    const bool direct_udp = !old_tcp && route == "none";
    const auto records = direct_udp ? refreshed.drain() : original_records;
    const auto notify = pick(records, "NOTIFY");
    const std::string expected_uri = route == "strict" ? strip_angle_uri(routes[0]) : target;
    assert(notify.rfind("NOTIFY " + expected_uri + " SIP/2.0", 0) == 0);
    std::vector<std::string> actual_routes;
    sip_record_fields(notify, "Route", [&](std::string_view field) {
      actual_routes.push_back(sip_record_unfold(field));
      return true;
    });
    if (route == "loose") {
      assert((actual_routes == std::vector<std::string>{routes[0], routes[1]}));
    } else if (route == "strict") {
      assert((actual_routes == std::vector<std::string>{routes[1], "<" + target + ">"}));
    } else {
      assert(actual_routes.empty());
    }
    if (direct_udp) {
      assert(original_records.size() == 1);
      refreshed.send_to(sip, response(notify, 200));
    } else {
      assert(original_records.size() == 2 && refreshed.drain().empty());
      original.send_to(sip, response(notify, 200));
    }
    assert(sip.refer_dialog_->pending_notify_.empty());
    original.send_to(sip, refresh);
    const auto replay = original.drain();
    assert(replay.size() == 1 && replay[0] == original_records[0]);
    assert(refreshed.drain().empty());
  }
  assert(sip.dialog_route_count_ == route_count && sip.dialog_routes_ == routes);
  assert(sip.refer_dialog_->call_id_ == candidate_id && !sip.refer_retiring_ && sip.media_active_);
  assert(sip.refer_dialog_->attempt_deadline_ms == deadline && candidate.drain().empty());
  sip.release_refer_dialog_();
  for (auto &flow : sip.registration_flows_) sip.close_tcp_flow_(flow);
  sip.close_tcp_flow_(sip.tcp_flow_);
  close(sip.sip_socket_);
}
