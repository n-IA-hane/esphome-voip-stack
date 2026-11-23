#define main previous_dialog_owner_main
#include "sip_dialog_owner.cpp"
#undef main
#include <fcntl.h>
#include <thread>
#include <chrono>

struct RecoveryPhone : NativePhone {
  bool tcp{false};
  RecoveryPhone(UdpPeer &local, uint16_t port) : NativePhone(local, port) {}
  void stream(int fd) {
    tcp = true;
    tcp_flow_.socket = fd;
    tcp_flow_.peer_ip = INADDR_LOOPBACK;
    remote_sip_tcp_ = true;
  }
  void receive() {
    if (!tcp) return NativePhone::receive();
    sockaddr_in src{}; src.sin_family = AF_INET; src.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    src.sin_port = htons(remote_sip_port_.load());
    handle_sip_stream_(tcp_flow_.socket, src);
  }
  int stream_fd() const { return tcp_flow_.socket; }
  void activate_media() {
    assert(rtp_socket_ >= 0);
    rtp_running_ = true;
    media_lifecycle_phase_ = MediaLifecyclePhase::ACTIVE;
  }
  void run_worker() { rtp_task_(); }
  void stop_worker() { rtp_running_ = false; rtp_task_terminate_ = true; shutdown(rtp_socket_, SHUT_RD); }
  uint16_t media_port() const { return rtp_port_; }
  bool waiting_ack() const { return completed_invite_.awaiting_ack; }
  void retry_fork_byes() {
    for (auto &fork : completed_invite_client_.rejected) if (!fork.bye.empty()) fork.bye.next_ms = 0;
    pump_udp_retransmits_();
  }
  uint32_t fork_bye_deadline() const {
    for (const auto &fork : completed_invite_client_.rejected) if (!fork.bye.empty()) return fork.bye.deadline_ms;
    return 0;
  }
  void ack_timeout() {
    completed_invite_.deadline_ms = 0;
    pump_udp_retransmits_();
  }
  void media_packet() {
    AudioFormat tx;
    get_media_config_(&tx, nullptr, nullptr, nullptr);
    std::vector<uint8_t> pcm(tx.nominal_frame_bytes(), 0x20);
    send_audio_frame(pcm.data(), pcm.size());
  }
};

struct Wire {
  UdpPeer endpoint, peer, media, media_port;
  RecoveryPhone phone;
  sockaddr_in receiver;
  bool tcp;
  std::string pending;
  std::vector<SipSignalType> events;
  Wire(bool stream) : phone(endpoint, ntohs(media_port.address.sin_port)), receiver(endpoint.address), tcp(stream) {
    close(media_port.fd); media_port.fd = -1;
    if (tcp) {
      int server = socket(AF_INET, SOCK_STREAM, 0);
      assert(server >= 0);
      auto address = peer.address;
      assert(bind(server, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
      assert(listen(server, 1) == 0);
      int client = socket(AF_INET, SOCK_STREAM, 0);
      assert(connect(client, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
      int accepted = accept(server, nullptr, nullptr);
      assert(accepted >= 0); close(server); close(peer.fd); peer.fd = accepted;
      assert(fcntl(client, F_SETFL, O_NONBLOCK) == 0);
      phone.stream(client);
    }
    phone.set_remote("127.0.0.1", ntohs(peer.address.sin_port), 40000);
    phone.set_sip_signal_callback([](void *ctx, SipSignal signal) {
      static_cast<Wire *>(ctx)->events.push_back(signal.type);
    }, this);
  }
  std::string take() {
    if (!tcp) return peer.take();
    while (true) {
      auto end = pending.find("\r\n\r\n");
      if (end != pending.npos) {
        size_t count = std::stoul(header_value(pending, "Content-Length"));
        if (pending.size() >= end + 4 + count) {
          auto result = pending.substr(0, end + 4 + count);
          pending.erase(0, result.size());
          return result;
        }
      }
      pending += peer.take();
    }
  }
  void quiet() {
    assert(pending.empty());
    pollfd p{peer.fd, POLLIN, 0};
    assert(poll(&p, 1, 15) == 0);
  }
  void deliver(const std::string &message) {
    const auto n = tcp ? send(peer.fd, message.data(), message.size(), 0) :
        sendto(peer.fd, message.data(), message.size(), 0, reinterpret_cast<sockaddr *>(&receiver), sizeof(receiver));
    assert(n == static_cast<ssize_t>(message.size()));
    if (tcp) {
      pollfd ready{phone.stream_fd(), POLLIN, 0};
      assert(poll(&ready, 1, 700) == 1);
    }
    phone.receive();
  }
  std::string dial(const char *id = "recovery") {
    assert(phone.send_invite(id, "100", "Phone", "101", "Peer"));
    const auto request = take();
    assert(request.rfind("INVITE ", 0) == 0);
    return request;
  }
  std::string answer(const std::string &inv) {
    auto sdp = message_body(inv);
    const auto begin = sdp.find("m=audio ") + 8;
    sdp.replace(begin, sdp.find(' ', begin) - begin, std::to_string(ntohs(media.address.sin_port)));
    return sdp;
  }
  std::string reply(const std::string &inv, unsigned status, const std::string &tag, const std::string &body = "") {
    auto msg = response(inv, status, body);
    auto to = header_value(msg, "To");
    auto pos = to.find(";tag=");
    if (pos != to.npos) to.erase(pos);
    msg.replace(msg.find("\r\nTo: ") + 6, header_value(msg, "To").size(), to + ";tag=" + tag);
    if (!body.empty()) msg.insert(msg.find("Content-Length:"), "Content-Type: application/sdp\r\n");
    msg.insert(msg.find("Content-Length:"), "Contact: <sip:peer@127.0.0.1:" + std::to_string(ntohs(peer.address.sin_port)) + ">\r\n");
    return msg;
  }
  std::string request(const std::string &method, unsigned seq, const std::string &inv, const std::string &body = "", const std::string &mime = "application/sdp") {
    std::string msg = method + " " + name_addr_uri(header_value(inv, "From")) + " SIP/2.0\r\nVia: SIP/2.0/" +
        (tcp ? "TCP" : "UDP") + " 127.0.0.1:" + std::to_string(ntohs(peer.address.sin_port)) + ";branch=z9hG4bK-peer-" + std::to_string(seq) +
        "\r\nFrom: " + header_value(inv, "To") + ";tag=A\r\nTo: " + header_value(inv, "From") +
        "\r\nCall-ID: " + header_value(inv, "Call-ID") + "\r\nCSeq: " + std::to_string(seq) + " " + method + "\r\n";
    if (!body.empty()) msg += "Content-Type: " + mime + "\r\n";
    return msg + "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
  }
  void established(const std::string &inv) {
    deliver(reply(inv, 200, "A", answer(inv)));
    assert(take().rfind("ACK ", 0) == 0 && phone.snapshot().call_active);
  }
  void reuse() {
    const auto next = dial("next");
    deliver(reply(next, 486, "next"));
    assert(take().rfind("ACK ", 0) == 0 && phone.idle());
  }
  void end() {
    assert(phone.send_bye("recovery"));
    auto bye = take(); assert(bye.rfind("BYE ", 0) == 0);
    deliver(response(bye, 200)); assert(phone.idle());
    reuse();
  }
};

int main(int argc, char **argv) {
  assert(argc == 3);
  const std::string mode = argv[1];
  const bool rx_mode = mode == "rtp-rx-ip" || mode == "rtp-rx-port";
  const bool ip_change = mode == "rtp-ip" || mode == "rtp-rx-ip";
  const bool target_change = rx_mode || mode == "rtp-port" || mode == "rtp-ip";
  Wire w(std::string(argv[2]) == "tcp");
  const auto inv = w.dial();
  if (mode == "same-tag" || mode == "early-fork" || mode == "cancel-fork" || mode == "negative-fork") {
    w.deliver(w.reply(inv, 180, "A"));
    assert(w.phone.invite_waiting_for_final());
    if (mode == "cancel-fork") {
      assert(w.phone.send_cancel("recovery"));
      auto cancel = w.take(); assert(cancel.rfind("CANCEL ", 0) == 0);
      w.deliver(w.reply(cancel, 200, "A"));
    }
    w.deliver(w.reply(inv, mode == "negative-fork" ? 486 : 200, mode == "same-tag" ? "A" : "B", w.answer(inv)));
    auto ack = w.take(); assert(ack.rfind("ACK ", 0) == 0);
    assert(tag_from_header(header_value(ack, "To")) == (mode == "same-tag" ? "A" : "B"));
    if (mode == "negative-fork") assert(w.phone.idle());
    else if (mode == "cancel-fork") {
      auto bye = w.take(); assert(bye.rfind("BYE ", 0) == 0 && tag_from_header(header_value(bye, "To")) == "B");
      w.deliver(response(bye, 200)); assert(w.phone.idle());
    } else w.end();
    return 0;
  }
  if (mode == "late-2xx") {
    w.deliver(w.reply(inv, 486, "A")); assert(w.take().rfind("ACK ", 0) == 0 && w.phone.idle());
  } else w.established(inv);
  if (mode == "two-2xx" || mode == "late-2xx" || mode == "fork-cleanup-reuse") {
    const auto before = w.phone.media_facts(); const auto count = w.events.size();
    auto late = w.reply(inv, 200, "B", w.answer(inv));
    w.deliver(late);
    auto ack = w.take(); assert(ack.rfind("ACK ", 0) == 0 && tag_from_header(header_value(ack, "To")) == "B");
    auto bye = w.take(); assert(bye.rfind("BYE ", 0) == 0 && tag_from_header(header_value(bye, "To")) == "B");
    if (mode == "fork-cleanup-reuse") {
      const auto deadline = w.phone.fork_bye_deadline(); assert(deadline != 0);
      w.end();
      assert(w.phone.fork_bye_deadline() == deadline);
      w.phone.retry_fork_byes();
      if (w.tcp) w.quiet();  // Reliable transports retain deadlines, without retransmission.
      else assert(w.take() == bye);
      w.deliver(response(bye, 200));
      assert(w.phone.idle());
      w.phone.retry_fork_byes(); w.quiet();
      return 0;
    }
    w.deliver(response(bye, 200));
    assert(w.events.size() == count && w.phone.media_facts() == before);
    w.deliver(late); assert(w.take() == ack); w.quiet();
    if (mode == "two-2xx") w.end();
    else assert(w.phone.idle());
    return 0;
  }
  w.phone.activate_media();
  w.phone.media_packet(); assert(w.media.take().size() > 12);
  std::atomic<unsigned> received{0};
  std::thread worker;
  auto wait_received = [&](unsigned n) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
    while (received.load(std::memory_order_acquire) < n && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(received.load() == n);
  };
  auto packet = [&](UdpPeer &source, unsigned sequence) {
    const auto fmt = std::get<4>(w.phone.media_facts());
    std::vector<uint8_t> bytes(12 + fmt.nominal_frame_bytes(), 0);
    bytes[0] = 0x80; bytes[1] = 96; bytes[2] = sequence >> 8; bytes[3] = sequence;
    bytes[8] = 0x12; bytes[11] = 0x34;
    for (size_t i = 12; i + 1 < bytes.size(); i += 2) { bytes[i] = 0x12; bytes[i+1] = 0x34; }
    sockaddr_in target{}; target.sin_family = AF_INET; target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    target.sin_port = htons(w.phone.media_port());
    assert(sendto(source.fd, bytes.data(), bytes.size(), 0, reinterpret_cast<sockaddr *>(&target), sizeof(target)) == static_cast<ssize_t>(bytes.size()));
  };
  if (rx_mode) {
    w.phone.set_audio_callback([](void *ctx, const TransportAudioFrame &frame) {
      assert(frame.bytes >= 2 && frame.pcm[0] == 0x34 && frame.pcm[1] == 0x12);
      static_cast<std::atomic<unsigned> *>(ctx)->fetch_add(1, std::memory_order_release);
    }, &received);
    worker = std::thread([&] { w.phone.run_worker(); });
    packet(w.media, 1); wait_received(1);
  }
  const auto before = w.phone.media_facts();
  if (mode.rfind("offerless", 0) == 0) {
    w.deliver(w.request("INVITE", 2, inv));
    const auto ok = w.take(); assert(ok.rfind("SIP/2.0 200", 0) == 0 && !message_body(ok).empty());
    assert(w.phone.media_facts() == before && w.phone.snapshot().call_active);
    w.phone.media_packet(); assert(w.media.take().size() > 12);
    if (mode == "offerless-glare") {
      w.deliver(w.request("INVITE", 3, inv)); assert(w.take().rfind("SIP/2.0 500", 0) == 0);
    }
    if (mode == "offerless-cancel") {
      auto cancel = w.request("CANCEL", 2, inv);
      w.deliver(cancel); assert(w.take().rfind("SIP/2.0 200", 0) == 0);
      assert(w.phone.snapshot().call_active && w.phone.waiting_ack());
    }
    if (mode == "offerless-foreign-ack") {
      auto foreign = w.request("ACK", 2, inv, w.answer(ok));
      foreign.replace(foreign.find(";tag=A"), 6, ";tag=foreign");
      w.deliver(foreign); w.quiet();
      assert(w.phone.waiting_ack() && w.phone.media_facts() == before);
    }
    if (mode == "offerless-timeout") w.phone.ack_timeout();
    else {
      auto answer = mode == "offerless-missing" ? "" : mode == "offerless-invalid" ? "v=0\r\n" : w.answer(ok);
      w.deliver(w.request("ACK", 2, inv, answer, mode == "offerless-mime" ? "text/plain" : "application/sdp"));
    }
    if (mode == "offerless-missing" || mode == "offerless-invalid" || mode == "offerless-timeout" || mode == "offerless-mime") {
      auto bye = w.take(); assert(bye.rfind("BYE ", 0) == 0);
      w.deliver(response(bye, 200)); assert(w.phone.idle());
    } else { assert(w.phone.snapshot().call_active); w.end(); }
    return 0;
  }
  auto proposal = w.answer(inv);
  UdpPeer new_media;
  if (ip_change) {
    close(new_media.fd); new_media.fd = socket(AF_INET, SOCK_DGRAM, 0);
    new_media.address.sin_addr.s_addr = htonl(0x7f000002);
    assert(bind(new_media.fd, reinterpret_cast<sockaddr *>(&new_media.address), sizeof(new_media.address)) == 0);
  }
  if (target_change) {
    const auto start = proposal.find("m=audio ") + 8;
    proposal.replace(start, proposal.find(' ', start) - start, std::to_string(ntohs(new_media.address.sin_port)));
    if (ip_change) {
      const auto pos = proposal.find("c=IN IP4 127.0.0.1"); assert(pos != proposal.npos);
      proposal.replace(pos, 18, "c=IN IP4 127.0.0.2");
    }
  }
  if (mode == "rtp-payload") {
    auto pt = proposal.find("RTP/AVP 96"); assert(pt != proposal.npos); proposal.replace(pt, 10, "RTP/AVP 97");
    pt = proposal.find("a=rtpmap:96"); assert(pt != proposal.npos); proposal.replace(pt, 11, "a=rtpmap:97");
  }
  if (mode == "rtp-invalid") proposal = "v=0\r\nc=IN IP4 127.0.0.1\r\nm=audio 40000 RTP/AVP 96\r\na=rtpmap:96 L16/48001/1\r\na=ptime:20\r\n";
  if (mode == "offer-malformed") proposal = "v=0\r\n";
  w.deliver(w.request("INVITE", 2, inv, proposal, mode == "offer-mime" ? "text/plain" : "application/sdp"));
  const auto result = w.take();
  if (target_change) {
    assert(result.rfind("SIP/2.0 200", 0) == 0);
    const auto after = w.phone.media_facts();
    assert(std::get<0>(after) == std::get<0>(before) && std::get<3>(after) == std::get<3>(before) && std::get<4>(after) == std::get<4>(before));
    assert(std::get<2>(after) == ntohs(new_media.address.sin_port));
    assert(std::get<1>(after) == (ip_change ? 0x7f000002U : INADDR_LOOPBACK));
    w.deliver(w.request("ACK", 2, inv));
    w.phone.media_packet(); assert(new_media.take().size() > 12);
    pollfd old{w.media.fd, POLLIN, 0}; assert(poll(&old, 1, 15) == 0);
  } else {
    assert(result.rfind(mode == "offer-mime" ? "SIP/2.0 415" : "SIP/2.0 488", 0) == 0);
    assert(w.phone.media_facts() == before && w.phone.snapshot().call_active);
    w.phone.media_packet(); assert(w.media.take().size() > 12);
    // A rejected re-INVITE is still an INVITE server transaction. Its peer
    // ACKs the final response before finishing this dialog and starting next.
    w.deliver(w.request("ACK", 2, inv));
  }
  if (rx_mode) {
    packet(new_media, 2); wait_received(2);
    packet(w.media, 3); std::this_thread::sleep_for(std::chrono::milliseconds(15));
    if (ip_change) assert(received.load() == 2);
    w.phone.stop_worker(); worker.join();
  }
  w.end();
}
