// Established media is a hardware fixture. REFER, SUBSCRIBE, transport I/O,
// response classification, transaction retention and deadlines are production.
uint32_t active_lease(const std::string &notify) {
  const auto state = header_value(notify, "Subscription-State");
  assert(state.rfind("active;expires=", 0) == 0);
  return static_cast<uint32_t>(std::stoul(state.substr(15)));
}

void finish_transfer(SipTransport &sip, Peer &original, Peer &target,
                     const std::string &invite, bool reporting, bool original_ended = false) {
  target.send_to(sip, response(invite, 200));
  const auto candidate = target.drain();
  assert(candidate.size() == 1 && candidate[0].rfind("ACK ", 0) == 0);
  const auto ended = original.drain();
  assert(ended.size() == unsigned(reporting) + unsigned(!original_ended));
  unsigned reports = 0, byes = 0;
  for (const auto &message : ended) {
    if (message.rfind("NOTIFY ", 0) == 0) {
      assert(reporting && message_body(message) == "SIP/2.0 200 OK\r\n");
      assert(header_value(message, "Subscription-State") == "terminated;reason=noresource");
      ++reports;
    } else {
      assert(message.rfind("BYE ", 0) == 0);
      ++byes;
    }
    original.send_to(sip, response(message, 200));
  }
  assert(reports == unsigned(reporting) && byes == unsigned(!original_ended));
  sip.pump_refer_();
  assert(sip.call_id_ != "original" && sip.media_active_);
  assert(sip.delivered.size() == 1 && sip.delivered[0].type == SipSignalType::CONNECTED_IDENTITY);
  cleanup(sip);
}

std::string refresh(SipTransport &sip, Peer &original, unsigned sequence, unsigned expires) {
  auto message = request(original, "SUBSCRIBE", sequence,
      "Event: refer;id=2\r\nExpires: " + std::to_string(expires) + "\r\n");
  original.send_to(sip, message);
  const auto responses = original.drain();
  assert(responses.size() == 2 && responses[0].rfind("SIP/2.0 200", 0) == 0);
  const auto granted = static_cast<uint32_t>(std::stoul(header_value(responses[0], "Expires")));
  if (expires != 0) {
    assert(granted > 0 && granted <= expires && active_lease(responses[1]) == granted);
    assert(message_body(responses[1]) == "SIP/2.0 100 Trying\r\n");
  } else {
    assert(granted == 0 && header_value(responses[1], "Subscription-State") == "terminated;reason=timeout");
  }
  original.send_to(sip, response(responses[1], 200));
  return message;
}

void slow_answer(bool old_tcp, bool new_tcp, unsigned elapsed, bool wrap) {
  if (wrap) clock_ms = UINT32_MAX - 20000U;
  const uint32_t started = clock_ms;
  SipTransport sip; Peer original(old_tcp), target(new_tcp); setup(sip, original);
  sip.set_calling_timeout(120000);
  const auto invite = begin(sip, original, target);
  target.send_to(sip, response(invite, 180));
  const auto deadline = sip.refer_dialog_->attempt_deadline_ms;
  assert(deadline == started + 120000U);
  uint32_t delay = 0;
  assert(sip.next_refer_deadline_(started, &delay) && delay == 120000);
  for (const unsigned interim : {33000U, elapsed}) {
    clock_ms = started + interim;
    target.send_to(sip, response(invite, 183));
    sip.pump_refer_();
    assert(sip.refer_dialog_ && !sip.refer_retiring_ && sip.refer_dialog_->attempt_deadline_ms == deadline);
    assert(target.drain().empty() && original.drain().empty());
    assert(sip.next_refer_deadline_(clock_ms, &delay) && delay == 120000U - interim);
  }
  finish_transfer(sip, original, target, invite, true);
}

void unlimited_policy(bool old_tcp, bool new_tcp, const std::string &mode) {
  SipTransport sip; Peer original(old_tcp), target(new_tcp); setup(sip, original);
  sip.set_calling_timeout(0);
  const auto invite = begin(sip, original, target);
  target.send_to(sip, response(invite, 180));
  assert(sip.refer_dialog_->attempt_timeout_ms == 0);
  const uint32_t lease = sip.refer_dialog_->subscription_interval_ms;
  assert(lease > 0 && sip.refer_dialog_->subscription_deadline_ms == 100 + lease);
  if (mode == "refresh" || mode == "refresh-ended") {
    unsigned sequence = 3;
    const bool ended = mode == "refresh-ended";
    if (ended) {
      original.send_to(sip, request(original, "BYE", sequence++));
      const auto replies = original.drain();
      assert(replies.size() == 1 && replies[0].rfind("SIP/2.0 200", 0) == 0);
      assert(sip.refer_dialog_->owner_invite_ended && !sip.media_active_);
    }
    for (unsigned elapsed : {60000U, 120000U, 180000U, 240000U}) {
      clock_ms = 100 + elapsed;
      const auto message = refresh(sip, original, sequence++, 300);
      const auto expiry = sip.refer_dialog_->subscription_deadline_ms;
      // A retransmitted SUBSCRIBE replays its response and never refreshes twice.
      clock_ms += 100;
      original.send_to(sip, message);
      const auto replay = original.drain();
      assert(replay.size() == 1 && replay[0].rfind("SIP/2.0 200", 0) == 0);
      assert(sip.refer_dialog_->subscription_deadline_ms == expiry);
      sip.pump_refer_();
      assert(sip.refer_dialog_ && !sip.refer_retiring_ && target.drain().empty());
    }
    clock_ms = 285100;
    sip.pump_refer_();
    assert(!sip.refer_retiring_);
    finish_transfer(sip, original, target, invite, true, ended);
    return;
  }
  if (mode == "invalid-refresh") {
    const auto expiry = sip.refer_dialog_->subscription_deadline_ms;
    for (const auto &invalid : {
        std::make_pair("Event: refer;id=99\r\nExpires: 300\r\n", 481),
        std::make_pair("Event: dialog\r\nExpires: 300\r\n", 489),
        std::make_pair("Event: refer;id=2\r\nExpires: no\r\n", 400),
        std::make_pair("Event: refer;id=2\r\nExpires: -1\r\n", 400)}) {
      original.send_to(sip, request(original, "SUBSCRIBE", 3, invalid.first));
      const auto replies = original.drain();
      assert(replies.size() == 1 && replies[0].rfind("SIP/2.0 " + std::to_string(invalid.second), 0) == 0);
      assert(sip.refer_dialog_->subscription_deadline_ms == expiry);
    }
    original.send_to(sip, request(original, "SUBSCRIBE", 3, "Event: refer;id=2\r\nExpires: 300\r\n",
                                  "original", "wrong-tag"));
    const auto rejected = original.drain();
    assert(rejected.size() == 1 && rejected[0].rfind("SIP/2.0 481", 0) == 0);
    assert(sip.refer_dialog_->subscription_deadline_ms == expiry);
    refresh(sip, original, 3, 300);
    finish_transfer(sip, original, target, invite, true);
    return;
  }
  if (mode == "unsubscribe") {
    clock_ms = 1100;
    refresh(sip, original, 3, 0);
  } else {
    assert(mode == "expire" || mode == "silent-cancel");
    clock_ms = 100 + lease;
    sip.pump_refer_();
    const auto expired = original.drain();
    assert(expired.size() == 1 && expired[0].rfind("NOTIFY ", 0) == 0);
    assert(header_value(expired[0], "Subscription-State") == "terminated;reason=timeout");
    original.send_to(sip, response(expired[0], 200));
  }
  assert(sip.refer_dialog_ && !sip.refer_retiring_ && !sip.refer_dialog_->reporter_available);
  assert(sip.call_id_ == "original" && sip.media_active_ && target.drain().empty());
  uint32_t delay = 123;
  assert(!sip.next_refer_deadline_(clock_ms, &delay));
  clock_ms = 3600100;
  sip.pump_refer_();
  assert(sip.refer_dialog_ && !sip.refer_retiring_ && sip.refer_dialog_->outgoing_invite_pending_);
  assert(target.drain().empty() && original.drain().empty());
  assert(!sip.next_refer_deadline_(clock_ms, &delay));
  if (mode != "silent-cancel") {
    finish_transfer(sip, original, target, invite, false);
    return;
  }
  sip.cancel_refer_();
  assert(sip.refer_retiring_ && sip.next_refer_deadline_(clock_ms, &delay) && delay == SIP_TRANSACTION_TIMEOUT_MS);
  const auto cancelled = target.drain();
  assert(cancelled.size() == 1 && cancelled[0].rfind("CANCEL ", 0) == 0);
  assert(original.drain().empty());
  clock_ms += SIP_TRANSACTION_TIMEOUT_MS;
  sip.pump_refer_();
  assert(!sip.refer_dialog_ && sip.call_id_ == "original" && sip.media_active_);
  cleanup(sip);
}

void custom_timeout(bool old_tcp, bool new_tcp, uint32_t budget) {
  SipTransport sip; Peer original(old_tcp), target(new_tcp); setup(sip, original);
  sip.set_calling_timeout(budget);
  const auto invite = begin(sip, original, target);
  target.send_to(sip, response(invite, 180));
  clock_ms = 100 + budget - 1;
  sip.pump_refer_();
  assert(!sip.refer_retiring_ && target.drain().empty() && original.drain().empty());
  ++clock_ms;
  sip.pump_refer_();
  const auto cancelled = target.drain();
  const auto final = original.drain();
  assert(cancelled.size() == 1 && cancelled[0].rfind("CANCEL ", 0) == 0);
  assert(final.size() == 1 && message_body(final[0]) == "SIP/2.0 408 Transfer Failed\r\n");
  assert(header_value(final[0], "Subscription-State") == "terminated;reason=noresource");
  original.send_to(sip, response(final[0], 200));
  target.send_to(sip, response(cancelled[0], 200));
  target.send_to(sip, response(invite, 487));
  const auto ack = target.drain();
  assert(ack.size() == 1 && ack[0].rfind("ACK ", 0) == 0);
  sip.pump_refer_();
  assert(!sip.refer_dialog_ && sip.call_id_ == "original" && sip.media_active_ && sip.delivered.empty());
  assert(original.drain().empty());
  original.send_to(sip, request(original, "BYE", 3));
  const auto ended = original.drain();
  assert(ended.size() == 1 && ended[0].rfind("SIP/2.0 200", 0) == 0);
  assert(sip.call_id_.empty() && !sip.media_active_);
  cleanup(sip);
}

int main(int argc, char **argv) {
  assert(argc == 5);
  const std::string mode = argv[1];
  const bool old_tcp = std::atoi(argv[2]), new_tcp = std::atoi(argv[3]);
  if (mode == "slow-answer" || mode == "wrapped-answer")
    slow_answer(old_tcp, new_tcp, std::stoul(argv[4]), mode == "wrapped-answer");
  else if (mode == "custom-timeout") custom_timeout(old_tcp, new_tcp, std::stoul(argv[4]));
  else unlimited_policy(old_tcp, new_tcp, mode);
}
