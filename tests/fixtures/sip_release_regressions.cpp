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
namespace fixture {
#include "sip_recovery_owner.cpp"
}
#include <csignal>
#include <iostream>
using fixture::Wire;
using namespace esphome::voip_stack;

// Inject one partial write followed by EAGAIN while retaining real TCP bytes.
// The ordinary pressure scenario below fills the actual kernel send buffer.
static int partial_socket = -1;
static unsigned partial_calls = 0;
extern "C" ssize_t __real_send(int, const void *, size_t, int);
extern "C" ssize_t __wrap_send(int fd, const void *data, size_t size, int flags) {
  if (fd == partial_socket) {
    if (partial_calls++ != 0) { errno = EAGAIN; return -1; }
    size = std::min<size_t>(size, 3);
  }
  return __real_send(fd, data, size, flags);
}

static unsigned completions(const Wire &wire) {
  return std::count(wire.events.begin(), wire.events.end(), SipSignalType::TERMINAL_COMPLETE);
}

int main(int argc, char **argv) {
  assert(argc == 3);
  signal(SIGPIPE, SIG_IGN);
  const std::string mode = argv[1];
  Wire wire(std::string(argv[2]) == "tcp");
  if (mode.rfind("tcp-", 0) == 0) {
    assert(wire.tcp);
    const int fd = wire.phone.stream_fd();
    const std::string first = "REGISTER sip:peer SIP/2.0\r\nContent-Length: 0\r\n\r\n";
    const std::string second = "SUBSCRIBE sip:peer SIP/2.0\r\nContent-Length: 0\r\n\r\n";
    if (mode == "tcp-hard-error") {
      assert(shutdown(fd, SHUT_WR) == 0);
      assert(!wire.phone.send_sip_tcp_record_(first, fd));
      return 0;
    }
    size_t padding_size = 0;
    if (mode == "tcp-partial") {
      partial_socket = fd;
    } else {
      int small = 1024;
      assert(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
      const std::string padding(65536, 'x');
      for (;;) {
        const auto count = send(fd, padding.data(), padding.size(), MSG_NOSIGNAL);
        if (count < 0) break;
        padding_size += count;
      }
      assert(errno == EAGAIN || errno == EWOULDBLOCK);
    }
    assert(wire.phone.send_sip_tcp_record_(first, fd));
#ifdef VOIP_HAS_TCP_TX_RESUME
    assert(!wire.phone.tcp_flow_.tx_pending.empty());
    const uint32_t deadline = wire.phone.tcp_flow_.tx_deadline;
    assert(wire.phone.send_sip_tcp_record_(second, fd));
    assert(wire.phone.tcp_flow_.tx_deadline == deadline);
    if (mode == "tcp-deadline") {
      wire.phone.tcp_flow_.tx_deadline = 1;
      assert(!wire.phone.pump_tcp_tx_(wire.phone.tcp_flow_, false));
      assert(wire.phone.tcp_flow_.tx_pending.empty());
      assert(send(fd, "x", 1, MSG_NOSIGNAL) < 0 && errno == EPIPE);
      return 0;
    }
    assert(!wire.phone.send_sip_tcp_record_(std::string(8193, 'y'), fd));
    partial_socket = -1;
    std::string observed;
    const size_t expected_size = padding_size + first.size() + second.size();
    for (unsigned i = 0; i < 2000 && observed.size() < expected_size; ++i) {
      char bytes[8192];
      const auto count = recv(wire.peer.fd, bytes, sizeof(bytes), MSG_DONTWAIT);
      if (count > 0) observed.append(bytes, count);
      pollfd writable{fd, POLLOUT, 0};
      assert(wire.phone.pump_tcp_tx_(wire.phone.tcp_flow_, poll(&writable, 1, 0) > 0));
      if (observed.size() < expected_size) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(observed == std::string(padding_size, 'x') + first + second);
    assert(wire.phone.tcp_flow_.tx_pending.empty() && wire.phone.tcp_flow_.tx_deadline == 0);
    const std::string bye = "BYE sip:peer SIP/2.0\r\nContent-Length: 0\r\n\r\n";
    assert(wire.phone.send_sip_tcp_record_(bye, fd));
    assert(wire.take() == bye);
    return 0;
#else
    assert(false && "baseline has no owner for a retained TCP record");
#endif
  }
  const auto invite = wire.dial();
  if (mode == "cancel-timeout" || mode == "cancel-late-provisional") {
    wire.deliver(wire.reply(invite, 180, "A"));
    assert(wire.phone.send_cancel("recovery"));
    const auto cancel = wire.take();
    assert(cancel.rfind("CANCEL ", 0) == 0);
    wire.deliver(wire.reply(cancel, 200, "A"));
    const uint32_t terminal_deadline = wire.phone.pending_invite_.deadline_ms;
    if (mode == "cancel-late-provisional") wire.deliver(wire.reply(invite, 183, "A"));
    assert(wire.phone.pending_invite_.deadline_ms == terminal_deadline);
    wire.phone.pending_invite_.deadline_ms = 0;
    wire.phone.pump_udp_retransmits_();
    assert(wire.phone.pending_invite_.empty());
    assert(!wire.phone.snapshot().terminal_transaction_pending && wire.phone.idle());
    wire.reuse();
    return 0;
  }
  if (mode == "timer100" || mode == "timer180" || mode == "timer-unanswered" ||
      mode == "cancel-proceeding") {
    if (mode != "timer-unanswered") wire.deliver(wire.reply(invite, mode == "timer100" ? 100 : 180, "A"));
    // The host clock is fixed. Expire Timer B directly, preserving real packet
    // matching, transaction pumping and final-response handling.
    wire.phone.pending_invite_.deadline_ms = 0;
    wire.phone.pump_udp_retransmits_();
    if (mode == "timer-unanswered") {
      assert(wire.phone.pending_invite_.empty());
      assert(!wire.phone.snapshot().call_active);
      wire.deliver(wire.reply(invite, 200, "A", wire.answer(invite)));
      wire.quiet();
      return 0;
    }
    assert(!wire.phone.pending_invite_.empty());
    wire.quiet();
    if (mode == "cancel-proceeding") {
      assert(wire.phone.send_cancel("recovery"));
      const auto cancel = wire.take();
      assert(cancel.rfind("CANCEL ", 0) == 0);
      wire.deliver(wire.reply(cancel, 200, "A"));
    }
    wire.deliver(wire.reply(invite, 200, "A", wire.answer(invite)));
    assert(wire.take().rfind("ACK ", 0) == 0);
    if (mode == "cancel-proceeding") {
      const auto bye = wire.take();
      assert(bye.rfind("BYE ", 0) == 0);
      wire.deliver(fixture::response(bye, 200));
      assert(wire.phone.idle());
      wire.reuse();
    } else {
      assert(wire.phone.snapshot().call_active);
      wire.end();
    }
    return 0;
  }
  wire.established(invite);
  if (mode == "rtp-old-newssrc" || mode == "rtp-clean-newssrc" || mode == "rtp-old-samessrc") {
    wire.phone.activate_media();
    std::atomic<unsigned> received{0};
    wire.phone.set_audio_callback([](void *context, const TransportAudioFrame &) {
      static_cast<std::atomic<unsigned> *>(context)->fetch_add(1);
    }, &received);
    std::thread worker([&] { wire.phone.run_worker(); });
    auto packet = [&](fixture::UdpPeer &source, unsigned sequence, uint32_t ssrc) {
      const auto format = std::get<4>(wire.phone.media_facts());
      std::vector<uint8_t> bytes(12 + format.nominal_frame_bytes(), 0);
      bytes[0] = 0x80; bytes[1] = 96; bytes[2] = sequence >> 8; bytes[3] = sequence;
      bytes[8] = ssrc >> 24; bytes[9] = ssrc >> 16; bytes[10] = ssrc >> 8; bytes[11] = ssrc;
      sockaddr_in to{}; to.sin_family = AF_INET;
      to.sin_addr.s_addr = htonl(INADDR_LOOPBACK); to.sin_port = htons(wire.phone.media_port());
      assert(sendto(source.fd, bytes.data(), bytes.size(), 0,
                    reinterpret_cast<sockaddr *>(&to), sizeof(to)) == static_cast<ssize_t>(bytes.size()));
    };
    auto wait = [&](unsigned count) {
      for (unsigned i = 0; i < 300 && received.load() < count; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      return received.load();
    };
    packet(wire.media, 1, 0x111); assert(wait(1) == 1);
    fixture::UdpPeer next_media;
    auto proposal = wire.answer(invite);
    const auto port = proposal.find("m=audio ") + 8;
    proposal.replace(port, proposal.find(' ', port) - port, std::to_string(ntohs(next_media.address.sin_port)));
    wire.deliver(wire.request("INVITE", 2, invite, proposal));
    assert(wire.take().rfind("SIP/2.0 200", 0) == 0);
    wire.deliver(wire.request("ACK", 2, invite));
    if (mode != "rtp-clean-newssrc") {
      packet(wire.media, 2, 0x111); assert(wait(2) == 2);
    }
    const auto before = received.load();
    packet(next_media, 3, mode == "rtp-old-samessrc" ? 0x111 : 0x222);
    const auto after = wait(before + 1);
    if (mode != "rtp-old-samessrc") {
      packet(wire.media, 4, 0x111);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      assert(received.load() == after);
    }
    wire.phone.stop_worker(); worker.join();
    assert(after == before + 1);
    assert(wire.phone.latched_rtp_port_ == ntohs(next_media.address.sin_port));
    return 0;
  }
  if (mode == "bye-before-ack-timeout" || mode == "bye-before-ack-delayed" || mode == "bye-after-ack") {
    wire.deliver(wire.request("INVITE", 2, invite, wire.answer(invite)));
    assert(wire.take().rfind("SIP/2.0 200", 0) == 0 && wire.phone.waiting_ack());
    if (mode == "bye-after-ack") wire.deliver(wire.request("ACK", 2, invite));
    wire.deliver(wire.request("BYE", 3, invite));
    assert(wire.take().rfind("SIP/2.0 200", 0) == 0);
    assert(wire.phone.snapshot().terminal_transaction_pending == (mode != "bye-after-ack"));
    const auto before = completions(wire);
    if (mode == "bye-before-ack-delayed") wire.deliver(wire.request("ACK", 2, invite));
    else wire.phone.ack_timeout();
    assert(!wire.phone.snapshot().terminal_transaction_pending);
    assert(!wire.phone.snapshot().call_active);
    assert(completions(wire) - before == (mode == "bye-after-ack" ? 0U : 1U));
    const auto after = completions(wire);
    wire.deliver(wire.request("ACK", 2, invite));
    wire.phone.ack_timeout();
    assert(completions(wire) == after);
    wire.reuse();
    return 0;
  }
  assert(false && "unknown regression scenario");
}
