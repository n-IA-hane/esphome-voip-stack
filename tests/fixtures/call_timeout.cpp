#define protected public
#include "sip_transport.h"
#undef protected
#include "sip_message.h"
#include <cassert>
#include <fcntl.h>
#include <poll.h>
#include <thread>
#include <chrono>
#include <tuple>
#include <vector>
#include <iostream>

uint32_t clock_ms = 100;
namespace fixture {
#include "sip_recovery_owner.cpp"
}

// PRODUCTION_APPLICATION_OWNER

int main(int argc, char **argv) {
  assert(argc == 4);
  const bool tcp = std::string(argv[1]) == "tcp";
  const unsigned provisional = std::stoul(argv[2]);
  const std::string scenario = argv[3];
  fixture::Wire wire(tcp);
  VoipStack app;
  app.t.sip = &wire.phone;
  app.calling_start_time_ = clock_ms;
  app.identity = {"recovery", "100", "Phone", "101", "Peer", "Peer"};
  app.call_state_ = CallState::CALLING;
  wire.phone.set_sip_signal_callback([](void *context, SipSignal signal) {
    auto *self = static_cast<VoipStack *>(context);
    self->defer([self, signal]() { self->on_sip_signal_received_(signal); });
  }, &app);

  if (scenario == "incoming-default") {
    app.call_state_ = CallState::RINGING;
    app.ringing_start_time_ = clock_ms;
    clock_ms += 3600000;
    app.timeout_loop(clock_ms);
    app.drain();
    assert(app.call_state_ == CallState::RINGING);
    wire.quiet();
    return 0;
  }

  uint32_t expected_timeout = 120000;
  if (scenario.rfind("configured-", 0) == 0 || scenario == "answer-after32") app.set_calling_timeout(expected_timeout);
  if (scenario == "calling-zero") {
    app.set_calling_timeout(0);
    app.set_ringing_timeout(30000);
    expected_timeout = 0;
  } else if (scenario == "calling-zero-last") {
    app.set_ringing_timeout(30000);
    app.set_calling_timeout(0);
    expected_timeout = 0;
  } else if (scenario == "ringing-zero" || scenario == "outgoing-omitted") {
    app.set_ringing_timeout(0);
    expected_timeout = 0;
  } else if (scenario == "ringing-independent") {
    app.set_ringing_timeout(60000);
    expected_timeout = 0;
  } else if (scenario == "calling-precedence") {
    app.set_calling_timeout(90000);
    app.set_ringing_timeout(30000);
    expected_timeout = 90000;
  }

  const auto invite = wire.dial();
  wire.deliver(wire.reply(invite, provisional, "A"));
  app.drain();
  const CallState progress = provisional == 100 ? CallState::CALLING : CallState::REMOTE_RINGING;
  assert(app.call_state_ == progress);
  const auto pump = [&](uint32_t elapsed) {
    clock_ms = 100 + elapsed;
    wire.phone.pump_udp_retransmits_();
    app.timeout_loop(clock_ms);
    app.drain();
  };

  // The transaction's former 32s Timer B must not truncate active progress.
  pump(33000);
  assert(app.call_state_ == progress && !wire.phone.pending_invite_.empty());
  assert(!app.can_start_call_());
  wire.quiet();

  if (scenario == "answer-after32") {
    pump(45000);
    wire.deliver(wire.reply(invite, 200, "A", wire.answer(invite)));
    assert(wire.take().rfind("ACK ", 0) == 0);
    app.drain();
    assert(app.call_state_ == CallState::IN_CALL && app.became_active == 1);
    assert(wire.phone.snapshot().call_active);
    wire.end();
    return 0;
  }

  if (expected_timeout == 0) {
    pump(3600000);
    assert(app.call_state_ == progress && !wire.phone.pending_invite_.empty());
    wire.quiet();
    app.fire_timeout_decline_();  // The explicit opt-out still permits hangup.
    app.drain();
  } else {
    if (scenario == "configured-repeated") {
      clock_ms = 60100;
      for (const unsigned status : {100, 180, 183}) {
        wire.deliver(wire.reply(invite, status, "A"));
        app.drain();
        assert(app.calling_start_time_ == 100);
      }
      clock_ms = 110100;
      wire.deliver(wire.reply(invite, 183, "A"));
      app.drain();
      assert(app.calling_start_time_ == 100);
    }
    pump(expected_timeout - 1);
    assert(app.call_state_ == (scenario == "configured-repeated" ? CallState::REMOTE_RINGING : progress));
    wire.quiet();
    pump(expected_timeout);
    assert(app.call_state_ == CallState::TERMINATING);
  }

  const auto cancel = wire.take();
  assert(cancel.rfind("CANCEL ", 0) == 0);
  assert(wire.phone.snapshot().terminal_transaction_pending && !app.can_start_call_());
  const auto terminal_deadline = wire.phone.pending_invite_.deadline_ms;
  if (scenario == "configured-silent") {
    clock_ms = terminal_deadline;
    wire.phone.pump_udp_retransmits_();
    app.drain();
  } else {
    wire.deliver(wire.reply(cancel, 200, "A"));
    if (scenario == "configured-late200") {
      wire.deliver(wire.reply(invite, 183, "A"));
      assert(wire.phone.pending_invite_.deadline_ms == terminal_deadline);
      wire.deliver(wire.reply(invite, 200, "A", wire.answer(invite)));
      assert(wire.take().rfind("ACK ", 0) == 0);
      const auto bye = wire.take();
      assert(bye.rfind("BYE ", 0) == 0);
      wire.deliver(fixture::response(bye, 200));
    } else {
      wire.deliver(wire.reply(invite, 487, "A"));
      assert(wire.take().rfind("ACK ", 0) == 0);
    }
    app.drain();
  }
  assert(app.became_active == 0 && app.call_state_ == CallState::IDLE);
  assert(wire.phone.idle() && app.can_start_call_());
  wire.reuse();
}
