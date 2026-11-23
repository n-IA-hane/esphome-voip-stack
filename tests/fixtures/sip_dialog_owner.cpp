#include "sip_transport.h"
#include "sip_message.h"

#include <cassert>
#include <poll.h>
#include <tuple>
#include <vector>

using namespace esphome::voip_stack;

struct UdpPeer {
  int fd;
  sockaddr_in address{};
  UdpPeer() {
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    socklen_t size = sizeof(address);
    assert(getsockname(fd, reinterpret_cast<sockaddr *>(&address), &size) == 0);
  }
  ~UdpPeer() {
    if (fd >= 0) close(fd);
  }
  std::string take() const {
    pollfd poller{fd, POLLIN, 0};
    assert(poll(&poller, 1, 700) == 1);
    char bytes[8192];
    const ssize_t count = recv(fd, bytes, sizeof(bytes), 0);
    assert(count > 0);
    return {bytes, static_cast<size_t>(count)};
  }
};

struct NativePhone : SipTransport {
  NativePhone(UdpPeer &local, uint16_t rtp_port)
      : SipTransport(ntohs(local.address.sin_port), rtp_port, 1200, "127.0.0.1", false) {
    // Feed ready datagrams explicitly. No RTOS tasks are started on the host.
    sip_socket_ = local.fd;
    local.fd = -1;
    running_ = true;
  }
  void receive() {
    char data[8192];
    sockaddr_in src{};
    socklen_t n = sizeof(src);
    const auto count = recvfrom(sip_socket_, data, sizeof(data), 0, reinterpret_cast<sockaddr *>(&src), &n);
    assert(count > 0);
    handle_sip_udp_datagram_(data, count, src);
  }
  bool invite_waiting_for_final() const {
    return !pending_invite_.empty() && pending_invite_.completed &&
           pending_invite_.next_ms == pending_invite_.deadline_ms;
  }
  bool idle() const {
    return call_id_.empty() && pending_invite_.empty() && pending_bye_.empty() && rtp_socket_ < 0 && !media_active_;
  }
  auto identity_facts() const {
    return std::make_tuple(call_id_, local_uri_, local_contact_uri_, local_tag_, remote_tag_);
  }
  auto media_facts() const {
    AudioFormat tx, rx;
    get_media_config_(&tx, &rx, nullptr, nullptr);
    return std::make_tuple(rtp_socket_, remote_rtp_ip_v4_.load(), remote_rtp_port_.load(), tx, rx);
  }
};

static std::string response(const std::string &request, unsigned status, const std::string &body = "") {
  std::string to = header_value(request, "To");
  if (to.find(";tag=") == to.npos && status > 100) to += ";tag=desk";
  return "SIP/2.0 " + std::to_string(status) + " Reply\r\nVia: " + header_value(request, "Via") +
         "\r\nFrom: " + header_value(request, "From") + "\r\nTo: " + to +
         "\r\nCall-ID: " + header_value(request, "Call-ID") + "\r\nCSeq: " + header_value(request, "CSeq") +
         "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

int main(int argc, char **argv) {
  assert(argc == 2);
  UdpPeer endpoint, peer, remote_media, media_port;
  const auto receiver = endpoint.address;
  const auto local_media_port = ntohs(media_port.address.sin_port);
  close(media_port.fd);
  media_port.fd = -1;
  NativePhone phone(endpoint, local_media_port);
  phone.set_remote("127.0.0.1", ntohs(peer.address.sin_port), 40000);
  std::vector<SipSignalType> events;
  std::vector<SipSignal> identity_events;
  std::pair<std::vector<SipSignalType> *, std::vector<SipSignal> *> observed{&events, &identity_events};
  phone.set_sip_signal_callback(
      [](void *opaque, SipSignal sig) {
        auto &output = *static_cast<decltype(observed) *>(opaque);
        output.first->push_back(sig.type);
        if (sig.type == SipSignalType::CONNECTED_IDENTITY) output.second->push_back(std::move(sig));
      },
      &observed);
  const auto deliver = [&](const std::string &msg) {
    assert(sendto(peer.fd, msg.data(), msg.size(), 0, reinterpret_cast<const sockaddr *>(&receiver),
                  sizeof(receiver)) == static_cast<ssize_t>(msg.size()));
    phone.receive();
  };
  const auto dial = [&](const char *id) {
    assert(phone.send_invite(id, "100", "Phone", "101", std::string(argv[1]) == "identity-number" ? "" : "Peer"));
    const auto inv = peer.take();
    assert(inv.rfind("INVITE ", 0) == 0);
    assert(message_body(inv).find("m=audio " + std::to_string(local_media_port) + " RTP/AVP ") != std::string::npos);
    return inv;
  };
  if (std::string(argv[1]) == "incoming") {
    const std::string peer_uri = "sip:101@127.0.0.1:" + std::to_string(ntohs(peer.address.sin_port));
    const auto request = [&](const std::string &method, const std::string &id, const std::string &to,
                             int cseq, const std::string &body = "") {
      return method + " sip:100@127.0.0.1 SIP/2.0\r\nVia: SIP/2.0/UDP 127.0.0.1:" +
             std::to_string(ntohs(peer.address.sin_port)) + ";branch=z9hG4bK-" + id +
             "\r\nFrom: <" + peer_uri + ">;tag=desk\r\nTo: " + to + "\r\nContact: <" + peer_uri +
             ">\r\nCall-ID: " + id + "\r\nCSeq: " + std::to_string(cseq) + " " + method +
             "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    };
    const std::string destination = "<sip:100@127.0.0.1>";
    deliver(request("INVITE", "unbound", destination, 1));
    assert(peer.take().rfind("SIP/2.0 403", 0) == 0 && events.empty() && phone.idle());
    phone.set_accept_direct_calls(true);

    deliver(request("INVITE", "inbound-cancel", destination, 1));
    assert(peer.take().rfind("SIP/2.0 100", 0) == 0 && events.back() == SipSignalType::INVITE);
    deliver(request("CANCEL", "inbound-cancel", destination, 1));
    assert(peer.take().rfind("SIP/2.0 200", 0) == 0);
    const auto terminated = peer.take();
    assert(terminated.rfind("SIP/2.0 487", 0) == 0 && events.back() == SipSignalType::CANCEL);
    deliver(request("ACK", "inbound-cancel", header_value(terminated, "To"), 1));
    assert(events.back() == SipSignalType::TERMINAL_COMPLETE && phone.idle());

    deliver(request("INVITE", "inbound-offer", destination, 1));
    assert(peer.take().rfind("SIP/2.0 100", 0) == 0 && events.back() == SipSignalType::INVITE);
    assert(phone.send_answer("inbound-offer", DEFAULT_AUDIO_FORMAT, DEFAULT_AUDIO_FORMAT));
    const auto accepted = peer.take();
    assert(accepted.rfind("SIP/2.0 200", 0) == 0);
    const auto tagged_to = header_value(accepted, "To");
    const auto answer = message_body(accepted);
    assert(answer.find("m=audio ") != answer.npos);
    deliver(request("ACK", "inbound-offer", tagged_to, 1, answer));
    assert(events.back() == SipSignalType::MEDIA_READY && phone.snapshot().call_active);
    deliver(request("BYE", "inbound-offer", tagged_to, 2));
    assert(peer.take().rfind("SIP/2.0 200", 0) == 0 && events.back() == SipSignalType::BYE && phone.idle());
    return 0;
  }
  const auto inv = dial("call-a");
  deliver(response(inv, 180));
  assert(phone.invite_waiting_for_final());
  assert(events.size() == 1 && events.back() == SipSignalType::STATUS_180_RINGING);
  auto wrong = response(inv, 486);
  wrong.replace(wrong.find("branch=") + 7, 0, "old-");
  deliver(wrong);
  assert(events.size() == 1 && phone.invite_waiting_for_final());
  if (std::string(argv[1]) == "decline") {
    auto final = response(inv, 486);
    deliver(final);
    const auto ack = peer.take();
    assert(ack.rfind("ACK ", 0) == 0 && phone.idle());
    assert(events.back() == SipSignalType::FINAL_RESPONSE);
    const auto count = events.size();
    deliver(final);
    assert(peer.take() == ack && events.size() == count);
  } else {
    const std::string mode = argv[1];
    const bool cancel = mode == "crossed";
    if (cancel) {
      assert(phone.send_cancel("call-a"));
      const auto req = peer.take();
      assert(req.rfind("CANCEL ", 0) == 0);
      deliver(response(req, 200));
    }
    auto body = message_body(inv);
    auto offset = body.find("m=audio ");
    assert(offset != body.npos);
    offset += 8;
    body.replace(offset, body.find(' ', offset) - offset, std::to_string(ntohs(remote_media.address.sin_port)));
    if (mode == "incompatible") body = "v=0\r\nc=IN IP4 127.0.0.1\r\nm=audio 40000 RTP/AVP 0\r\n";
    auto accepted = response(inv, 200, body);
    const bool identity_case = mode == "identity-name" || mode == "identity-bare" || mode == "identity-number";
    if (identity_case) {
      const auto old_to = header_value(accepted, "To");
      const auto new_to = sip_name_addr(name_addr_uri(old_to),
          mode == "identity-name" ? "Connected reception" : "") + ";tag=desk";
      accepted.replace(accepted.find("\r\nTo: ") + 6, old_to.size(), new_to);
      accepted.insert(accepted.find("Content-Length:"),
          "Contact: <sip:reachable@127.0.0.1:" + std::to_string(ntohs(peer.address.sin_port)) + ">\r\n");
    }
    deliver(accepted);
    assert(peer.take().rfind("ACK ", 0) == 0);
    if (identity_case) {
      if (mode != "identity-bare") {
        assert(identity_events.size() == 1 && events[events.size() - 2] == SipSignalType::STATUS_200_OK);
        const auto &identity = identity_events.back();
        assert(identity.call_id == "call-a" && identity.connected_route == "101" && !identity.media_source_changed);
        assert(identity.connected_name == (mode == "identity-name" ? "Connected reception" : ""));
      } else {
        assert(identity_events.empty() && events.back() == SipSignalType::STATUS_200_OK);
      }
      assert(phone.snapshot().call_active);
      const auto identity_before = phone.identity_facts();
      const auto media_before = phone.media_facts();
      const auto count = events.size();
      deliver(accepted);
      assert(peer.take().rfind("ACK ", 0) == 0);
      assert(events.size() == count && phone.identity_facts() == identity_before && phone.media_facts() == media_before);
      assert(phone.send_bye("call-a"));
      const auto bye = peer.take();
      assert(name_addr_uri(header_value(bye, "From")) == name_addr_uri(header_value(inv, "From")));
      assert(header_value(bye, "Contact") == header_value(inv, "Contact"));
      assert(header_value(bye, "Call-ID") == header_value(inv, "Call-ID"));
      assert(name_addr_uri(header_value(bye, "To")) == name_addr_uri(header_value(inv, "To")));
      assert(sip_display_name_from_header(header_value(bye, "To"), 160) ==
          (mode == "identity-name" ? "Connected reception" : mode == "identity-bare" ? "Peer" : ""));
      assert(sip_request_uri(bye) == "sip:reachable@127.0.0.1:" + std::to_string(ntohs(peer.address.sin_port)));
      deliver(response(bye, 200));
      assert(phone.idle());
      return 0;
    }
    if (mode == "incompatible") {
      assert(events.back() == SipSignalType::MEDIA_INCOMPATIBLE);
    } else if (!cancel) {
      assert(events.back() == SipSignalType::STATUS_200_OK && phone.snapshot().call_active);
      assert(phone.send_bye("call-a"));
    } else
      assert(events.back() == SipSignalType::STATUS_180_RINGING);
    const auto bye = peer.take();
    assert(bye.rfind("BYE ", 0) == 0);
    deliver(response(bye, 200));
    assert(phone.idle() && events.back() == SipSignalType::TERMINAL_COMPLETE);
  }
  const auto next = dial("call-b");
  assert(header_value(next, "Call-ID") == "call-b");
  deliver(response(next, 486));
  assert(peer.take().rfind("ACK ", 0) == 0 && phone.idle());
}
