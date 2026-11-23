#include "sip_transport.h"
#include "sip_message.h"
#include "transport_host_sdk.h"

#include <poll.h>
#include <tuple>
#include <unistd.h>

using namespace esphome::voip_stack;

// The extra byte must use the existing table's tail padding on this host ABI.
struct PreviousPayloadLayout {
  RtpAudioPayloads::Entry entries[VOIP_STACK_MAX_AUDIO_FORMATS];
  uint8_t count;
};
static_assert(sizeof(RtpAudioPayloads) == sizeof(PreviousPayloadLayout));

struct AliasPhone : SipTransport {
  AliasPhone(uint16_t sip, uint16_t media) : SipTransport(sip, media, 1200, "127.0.0.1", false) {}
  auto stable_facts() const {
    esphome::LockGuard dialog(this->dialog_mutex_);
    esphome::LockGuard media(this->media_lifecycle_mutex_);
    AudioFormat tx, rx;
    uint8_t tx_pt, rx_pt;
    this->get_media_config_(&tx, &rx, &tx_pt, &rx_pt);
    return std::make_tuple(this->call_id_, this->local_uri_, this->local_contact_uri_, this->remote_uri_,
                          this->local_tag_, this->remote_tag_, this->rtp_socket_, this->rtp_task_handle_,
                          this->rtp_ssrc_, this->rtp_sequence_.load(), this->rtp_timestamp_.load(),
                          this->remote_rtp_ip_v4_.load(), this->remote_rtp_port_.load(), tx, rx, tx_pt, rx_pt);
  }
  RtpAudioPayloads payloads() const {
    esphome::LockGuard media(this->media_lifecycle_mutex_);
    RtpAudioPayloads result;
    this->get_media_config_(nullptr, nullptr, nullptr, nullptr, &result);
    return result;
  }
  bool admits(uint8_t pt) const {
    uint8_t pcm[320]{};
    return this->received_audio_format_(pt, nullptr, pcm, sizeof(pcm));
  }
  bool active() const {
    return this->media_lifecycle_phase_.load() == MediaLifecyclePhase::ACTIVE && this->rtp_running_.load();
  }
};

static uint16_t reserve_udp(int *fd) {
  *fd = socket(AF_INET, SOCK_DGRAM, 0);
  assert(*fd >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(*fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
  socklen_t length = sizeof(address);
  assert(getsockname(*fd, reinterpret_cast<sockaddr *>(&address), &length) == 0);
  return ntohs(address.sin_port);
}
static std::string take(int fd) {
  pollfd ready{fd, POLLIN, 0};
  assert(poll(&ready, 1, 2000) == 1);
  char bytes[8192];
  const auto count = recv(fd, bytes, sizeof(bytes), 0);
  assert(count > 0);
  return {bytes, static_cast<size_t>(count)};
}

int main(int argc, char **argv) {
  assert(argc == 2);
  const std::string mode = argv[1];
  int peer, reserved, remote_media;
  const auto peer_port = reserve_udp(&peer);
  const auto remote_media_port = reserve_udp(&remote_media);
  const auto sip_port = reserve_udp(&reserved);
  close(reserved);
  const auto media_port = reserve_udp(&reserved);
  close(reserved);
  {
    AliasPhone phone(sip_port, media_port);
    AudioFormat pcm{16000, PcmFormat::S16LE, 1, 10, AudioCodec::PCM};
    AudioFormatList formats;
    formats.count = 1;
    formats.formats[0] = pcm;
    phone.set_audio_formats(formats, formats);
    phone.set_accept_direct_calls(true);
    phone.set_remote("127.0.0.1", peer_port, remote_media_port);
    assert(phone.start());
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    endpoint.sin_port = htons(sip_port);
    const auto deliver = [&](const std::string &message) {
      assert(sendto(peer, message.data(), message.size(), 0, reinterpret_cast<sockaddr *>(&endpoint),
                    sizeof(endpoint)) == static_cast<ssize_t>(message.size()));
    };
    std::string tagged_to = "<sip:100@127.0.0.1>";
    const auto request = [&](const std::string &method, unsigned cseq, const std::string &body = "",
                             bool failure_ack = false) {
      return method + " sip:100@127.0.0.1 SIP/2.0\r\nVia: SIP/2.0/UDP 127.0.0.1:" +
          std::to_string(peer_port) + ";branch=z9hG4bK-alias-" + std::to_string(cseq) +
          (failure_ack ? "INVITE" : method) +
          "\r\nFrom: <sip:101@127.0.0.1>;tag=peer\r\nTo: " + tagged_to +
          "\r\nCall-ID: alias-history\r\nCSeq: " + std::to_string(cseq) + " " + method +
          "\r\nContent-Type: application/sdp\r\nContent-Length: " + std::to_string(body.size()) +
          "\r\n\r\n" + body;
    };
    const auto offer = [&](bool alias, const std::string &direction, uint8_t alias_pt = 97,
                          unsigned alias_rate = 16000, unsigned primary_rate = 16000) {
      std::string body = "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=Alias hold\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n"
          "m=audio " + std::to_string(remote_media_port) + " RTP/AVP 96" + (alias ? " " + std::to_string(alias_pt) : "") +
          "\r\na=rtpmap:96 L16/" + std::to_string(primary_rate) + "/1\r\n";
      if (alias) body += "a=rtpmap:" + std::to_string(alias_pt) + " L16/" + std::to_string(alias_rate) + "/1\r\n";
      return body + "a=ptime:10\r\na=" + direction + "\r\n";
    };
    deliver(request("INVITE", 1, offer(true, "sendrecv")));
    assert(take(peer).rfind("SIP/2.0 100", 0) == 0);
    if (mode != "unprepared-owner") assert(phone.prepare_media_path());
    assert(phone.send_answer("alias-history", pcm, pcm));
    const auto accepted = take(peer);
    assert(accepted.rfind("SIP/2.0 200", 0) == 0);
    tagged_to = header_value(accepted, "To");
    deliver(request("ACK", 1));
    if (mode != "unprepared-owner") assert(phone.commit_media_path() && phone.active());
    const auto original = phone.stable_facts();
    const auto initial = phone.payloads();
    assert(initial.count == 2 && initial.find(96, nullptr) && initial.find(97, nullptr));
    const auto allocated = video_probe::allocated;
    unsigned cseq = 2;
    const auto negotiate = [&](const std::string &sdp, unsigned expected) {
      const auto number = cseq++;
      deliver(request("INVITE", number, sdp));
      const auto answer = take(peer);
      assert(answer.rfind("SIP/2.0 " + std::to_string(expected), 0) == 0);
      // Taking the owner's lock waits for its complete publish/commit section.
      assert(phone.stable_facts() == original && video_probe::allocated == allocated);
      deliver(request("ACK", number, "", expected >= 300));
      return answer;
    };
    if (mode == "unprepared-owner") {
      assert(!phone.active());
      negotiate(offer(false, "sendonly"), 488);
      assert(phone.payloads() == initial);
    } else {
      const auto held = negotiate(offer(false, "sendonly"), 200);
      const auto map = phone.payloads();
      assert(map.count == 1 && map.known_count == 2 && map.find(96, nullptr) && !map.find(97, nullptr));
      assert(map.find_known(97, nullptr) && phone.admits(97));  // In-flight old mapping remains prepared.
      assert(message_body(held).find(" RTP/AVP 96\r\n") != std::string::npos);
      assert(message_body(held).find("a=rtpmap:97 ") == std::string::npos);
      if (mode == "shrink-restore") {
        for (unsigned cycle = 0; cycle < 3; ++cycle) {
          negotiate(offer(true, "sendrecv"), 200);
          assert(phone.payloads() == initial && phone.payloads().known_count == 2);
          negotiate(offer(false, "sendonly"), 200);
          assert(phone.payloads().count == 1 && phone.payloads().known_count == 2);
        }
      } else if (mode == "retired-remap") {
        negotiate(offer(true, "sendrecv", 97, 48000), 488);
        assert(phone.payloads() == map && phone.admits(97));
        negotiate(offer(true, "sendrecv"), 200);  // Failure did not retire the original meaning.
        assert(phone.payloads() == initial);
      } else if (mode == "unprepared-alias") {
        negotiate(offer(true, "sendrecv", 98), 488);  // Same geometry, never committed PT.
        assert(phone.payloads() == map && !phone.admits(98));
      } else {
        assert(mode == "primary-change");
        negotiate(offer(false, "sendonly", 97, 16000, 48000), 488);
        assert(phone.payloads() == map);
      }
      assert(phone.active());
    }
    phone.stop();
    reap_host_tasks();
    assert(video_probe::tasks == 0);
  }
  close(peer);
  close(remote_media);
  assert(video_probe::allocated == video_probe::freed && video_probe::allocations.empty());
}
