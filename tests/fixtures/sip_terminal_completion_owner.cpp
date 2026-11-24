// Keep the real transport, peer socket and SDP fixtures. No FSM is reconstructed.
#include "sip_transport.h"
#include "sip_message.h"
#include <cassert>
#include <fcntl.h>
#include <poll.h>
#include <thread>
#include <chrono>
#include <tuple>
#include <vector>
#include <iostream>
namespace recovery_fixture {
#include "sip_recovery_owner.cpp"
}
using recovery_fixture::Wire;
using recovery_fixture::response;
using esphome::voip_stack::SipSignal;
using esphome::voip_stack::SipSignalType;

int main(int argc, char **argv) {
  assert(argc == 3);
  const std::string mode = argv[1];
  Wire wire(std::string(argv[2]) == "tcp");
  const auto invite = wire.dial();
  wire.established(invite);
  const auto original_media = wire.phone.media_facts();
  const auto original_identity = wire.phone.identity_facts();
  const std::string incompatible =
      "v=0\r\no=- 991 2 IN IP4 127.0.0.1\r\ns=terminal-boundary\r\n"
      "c=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio 49000 RTP/AVP 96\r\n"
      "a=rtpmap:96 L16/48001/1\r\na=ptime:20\r\n";
  wire.deliver(wire.request("INVITE", 100, invite, incompatible));
  assert(wire.take().rfind("SIP/2.0 488", 0) == 0);
  assert(wire.phone.waiting_ack());
  assert(wire.phone.media_facts() == original_media);
  if (mode == "ack") {
    wire.deliver(wire.request("ACK", 100, invite));
    assert(!wire.phone.waiting_ack());
  }
  if (mode != "live") {
    assert(wire.phone.send_bye("recovery"));
    const auto bye = wire.take();
    assert(bye.rfind("BYE ", 0) == 0);
    wire.deliver(response(bye, 200));
    assert(!wire.phone.snapshot().call_active);
  }

  std::vector<SipSignal> completions;
  wire.phone.set_sip_signal_callback([](void *opaque, SipSignal signal) {
    if (signal.type == SipSignalType::TERMINAL_COMPLETE)
      static_cast<std::vector<SipSignal> *>(opaque)->push_back(std::move(signal));
  }, &completions);
  const bool before = wire.phone.snapshot().terminal_transaction_pending;
  wire.phone.ack_timeout();  // The existing host clock expires the real Timer H.
  const auto emitted = completions.size();
  wire.phone.ack_timeout();
  assert(completions.size() == emitted);
  assert(completions.empty() || completions.front().call_id == "recovery");
  if (mode == "reuse") {
    const auto next = wire.dial("next-live");
    wire.established(next);
    const auto next_identity = wire.phone.identity_facts();
    const auto next_media = wire.phone.media_facts();
    wire.phone.ack_timeout();
    assert(wire.phone.snapshot().call_active && wire.phone.identity_facts() == next_identity &&
           wire.phone.media_facts() == next_media && completions.size() == emitted);
  }
  const auto after = wire.phone.snapshot();
  const bool preserved = mode != "live" ||
      (after.call_active && wire.phone.media_facts() == original_media &&
       wire.phone.identity_facts() == original_identity);
  const bool expected_event = mode != "ack";
  std::cout << "RESULT mode=" << mode << " before_pending=" << before
            << " after_pending=" << after.terminal_transaction_pending
            << " completion_events=" << completions.size()
            << " live_media_preserved=" << preserved << std::endl;
  const bool pass = !after.terminal_transaction_pending && preserved &&
      completions.size() == (expected_event ? 1U : 0U) &&
      (completions.empty() || !completions.front().terminal_transaction_pending);
  wire.phone.set_sip_signal_callback(nullptr, nullptr);
  return pass ? 0 : 1;
}
