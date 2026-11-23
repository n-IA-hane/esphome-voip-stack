#include "transport_host_sdk.h"
#include "sip_types.h"
#ifdef USE_ESPHOME_VOIP_STACK_OPUS
#define private public
#include "opus_rtp_codec.h"
#undef private
#endif
#include "sip_transport.h"
#include "sip_message.h"

#include <cmath>
#include <poll.h>
#include <tuple>
#include <vector>

using namespace esphome::voip_stack;

struct Wire {
  bool tcp;
  int fd, listener{-1};
  sockaddr_in address{};
  std::string buffered;
  explicit Wire(bool stream = false) : tcp(stream), fd(socket(AF_INET, stream ? SOCK_STREAM : SOCK_DGRAM, 0)) {
    assert(fd >= 0);
    address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    socklen_t size = sizeof(address);
    assert(getsockname(fd, reinterpret_cast<sockaddr *>(&address), &size) == 0);
    if (tcp) { listener = fd; fd = -1; assert(listen(listener, 1) == 0); }
  }
  ~Wire() { if (fd >= 0) close(fd); if (listener >= 0) close(listener); }
  uint16_t port() const { return ntohs(address.sin_port); }
  void accept_phone() {
    pollfd pending{listener, POLLIN, 0}; assert(poll(&pending, 1, 2000) == 1);
    fd = accept(listener, nullptr, nullptr); assert(fd >= 0);
  }
  void connect_phone(uint16_t port) {
    assert(tcp); fd = socket(AF_INET, SOCK_STREAM, 0); assert(fd >= 0);
    auto target = address; target.sin_port = htons(port);
    assert(connect(fd, reinterpret_cast<sockaddr *>(&target), sizeof(target)) == 0);
    socklen_t size = sizeof(address);
    assert(getsockname(fd, reinterpret_cast<sockaddr *>(&address), &size) == 0);
  }
  void send_to(const std::string &message, uint16_t port) const {
    if (tcp) {
      size_t sent = 0;
      while (sent < message.size()) {
        const auto count = send(fd, message.data() + sent, message.size() - sent, 0);
        assert(count > 0); sent += count;
      }
    } else {
      auto target = address; target.sin_port = htons(port);
      assert(sendto(fd, message.data(), message.size(), 0, reinterpret_cast<sockaddr *>(&target),
                    sizeof(target)) == static_cast<ssize_t>(message.size()));
    }
  }
  std::string take() {
    for (;;) {
      if (tcp) {
        const auto end = buffered.find("\r\n\r\n");
        if (end != std::string::npos) {
          const auto at = buffered.find("Content-Length: "); assert(at < end);
          const auto bytes = std::stoul(buffered.substr(at + 16));
          if (buffered.size() >= end + 4 + bytes) {
            const auto message = buffered.substr(0, end + 4 + bytes);
            buffered.erase(0, message.size()); return message;
          }
        }
      }
      pollfd ready{fd, POLLIN, 0}; assert(poll(&ready, 1, 2000) == 1);
      char packet[8192]; const auto bytes = recv(fd, packet, sizeof(packet), 0); assert(bytes > 0);
      if (!tcp) return {packet, static_cast<size_t>(bytes)};
      buffered.append(packet, bytes);
    }
  }
};

static uint16_t free_port() { Wire reservation; return reservation.port(); }
static uint16_t free_sip_port() {
  for (unsigned attempt = 0; attempt < 100; ++attempt) {
    Wire udp; int tcp = socket(AF_INET, SOCK_STREAM, 0); assert(tcp >= 0);
    const bool available = bind(tcp, reinterpret_cast<sockaddr *>(&udp.address), sizeof(udp.address)) == 0;
    close(tcp); if (available) return udp.port();
  }
  std::abort();
}
static uint16_t free_pair() {
  for (unsigned attempt = 0; attempt < 100; ++attempt) {
    Wire first; if (first.port() == 65535) continue;
    int next = socket(AF_INET, SOCK_DGRAM, 0); assert(next >= 0);
    auto address = first.address; address.sin_port = htons(first.port() + 1);
    const bool available = bind(next, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0;
    close(next); if (available) return first.port();
  }
  std::abort();
}
static std::string response(const std::string &request, const std::string &body, const std::string &contact) {
  return "SIP/2.0 200 OK\r\nVia: " + header_value(request, "Via") + "\r\nFrom: " +
      header_value(request, "From") + "\r\nTo: " + header_value(request, "To") +
      ";tag=peer\r\nCall-ID: " + header_value(request, "Call-ID") + "\r\nCSeq: " +
      header_value(request, "CSeq") + "\r\nContact: <" + contact +
      ">\r\nContent-Type: application/sdp\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}
static uint8_t pcm_payload(const std::string &sdp, unsigned rate, const std::string &name = "L16") {
  const auto encoding = sdp.find(" " + name + "/" + std::to_string(rate)); assert(encoding != std::string::npos);
  const auto start = sdp.rfind("a=rtpmap:", encoding); assert(start != std::string::npos);
  return static_cast<uint8_t>(std::stoul(sdp.substr(start + 9, encoding - start - 9)));
}

struct Phone : SipTransport {
  using SipTransport::SipTransport;
  auto audio_facts() const {
    esphome::LockGuard dialog(dialog_mutex_);
    AudioFormat tx, rx; uint8_t tx_pt, rx_pt;
    get_media_config_(&tx, &rx, &tx_pt, &rx_pt);
    return std::make_tuple(call_id_, local_tag_, remote_tag_, local_uri_, remote_uri_, remote_target_uri_,
                          rtp_socket_, rtp_task_handle_, rtp_ssrc_, rtp_sequence_.load(), rtp_timestamp_.load(),
                          audio_tx_clock_rate_, audio_tx_clock_generation_, rtp_running_.load(),
                          remote_rtp_ip_v4_.load(), remote_rtp_port_.load(), tx, rx, tx_pt, rx_pt);
  }
  auto rtp_facts() const {
    esphome::LockGuard lock(rtp_socket_mutex_);
    return std::make_tuple(rtp_sequence_.load(), rtp_timestamp_.load(), rtp_ssrc_);
  }
  RtpAudioPayloads payloads() const {
    esphome::LockGuard dialog(dialog_mutex_); RtpAudioPayloads result;
    get_media_config_(nullptr, nullptr, nullptr, nullptr, &result); return result;
  }
  bool active() const {
    return media_lifecycle_phase_.load() == MediaLifecyclePhase::ACTIVE && rtp_running_.load();
  }
  bool media_idle() const {
    esphome::LockGuard media(media_lifecycle_mutex_);
    return media_lifecycle_phase_.load() == MediaLifecyclePhase::IDLE &&
           rtp_task_quiesced_.load() && !rtp_running_.load();
  }
  bool acked() const { esphome::LockGuard dialog(dialog_mutex_); return !completed_invite_.awaiting_ack; }
  bool idle() const { esphome::LockGuard dialog(dialog_mutex_); return call_id_.empty(); }
#ifdef USE_ESPHOME_VOIP_STACK_OPUS
  auto constraints() const { esphome::LockGuard dialog(dialog_mutex_); return negotiated_media_().opus_tx; }
  auto encoder() { return opus_codec_.encoder_; }
  auto decoder() { return opus_codec_.decoder_; }
#endif
};
struct Received {
  std::mutex mutex;
  std::vector<uint8_t> pcm;
  uint32_t format{0};
  static void capture(void *context, const TransportAudioFrame &frame) {
    auto &self = *static_cast<Received *>(context);
    std::lock_guard<std::mutex> lock(self.mutex);
    self.pcm.assign(frame.pcm, frame.pcm + frame.bytes); self.format = frame.format_packed;
  }
};
template<class Predicate> static void settled(Predicate predicate) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!predicate() && std::chrono::steady_clock::now() < end) std::this_thread::yield();
  assert(predicate());
}
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
struct Sink : EncodedVideoSink {
  VideoCapability get_receive_video_capability() const override {
    VideoCapability value; value.encoding = "JPEG"; value.payload_type = 26;
    value.width = 320; value.height = 240; value.max_fps = 10; return value;
  }
  size_t max_encoded_access_unit_bytes() const override { return 4096; }
  bool start_video(const VideoCapability &) override { return true; }
  bool set_video_active(bool) override { return true; }
  void stop_video() override {}
  bool consume_video_access_unit(const EncodedVideoAccessUnit &) override { return true; }
};
#endif

int main(int argc, char **argv) {
  assert(argc == 3); const bool tcp = std::string(argv[1]) == "tcp"; const std::string mode(argv[2]);
  Wire peer(tcp), media, video_media; const auto sip_port = free_sip_port(), audio_port = free_port();
  {
    Phone phone(sip_port, audio_port, 1200, "127.0.0.1", false);
    phone.set_accept_direct_calls(true); phone.set_remote("127.0.0.1", peer.port(), media.port());
    phone.set_sip_signaling_transport(tcp);
#ifdef USE_ESPHOME_VOIP_STACK_OPUS
    assert(mode == "opus-uas");
    AudioFormat opus{48000, PcmFormat::S16LE, 1, 20, AudioCodec::OPUS};
    AudioFormatList formats; formats.count = 1; formats.formats[0] = opus;
    phone.set_audio_formats(formats, formats); assert(phone.start());
    if (tcp) peer.connect_phone(sip_port);
    auto *encoder = phone.encoder(); auto *decoder = phone.decoder(); assert(encoder && decoder);
    for (unsigned leg = 0; leg < 2; ++leg) {
      const std::string id = "opus-uas-" + std::to_string(leg);
      const std::string body = "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=Opus peer\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio " + std::to_string(media.port()) +
          " RTP/AVP 111\r\na=rtpmap:111 opus/48000/2\r\na=ptime:20\r\na=sendrecv\r\n" +
          (leg == 0 ? "a=fmtp:111 maxaveragebitrate=12000;maxplaybackrate=8000;stereo=0\r\n" : "");
      std::string to = "<sip:100@127.0.0.1>";
      const auto request = [&](const std::string &method, unsigned cseq, const std::string &payload = "") {
        return method + " sip:100@127.0.0.1 SIP/2.0\r\nVia: SIP/2.0/" + (tcp ? "TCP" : "UDP") +
            " 127.0.0.1:" + std::to_string(peer.port()) + ";branch=z9hG4bK-" + id + method +
            "\r\nFrom: <sip:101@127.0.0.1>;tag=peer\r\nTo: " + to + "\r\nCall-ID: " + id +
            (method == "INVITE" ? "\r\nContact: <sip:101@127.0.0.1:" + std::to_string(peer.port()) + ">" : "") +
            "\r\nCSeq: " + std::to_string(cseq) + " " + method + "\r\nContent-Type: application/sdp\r\nContent-Length: " +
            std::to_string(payload.size()) + "\r\n\r\n" + payload;
      };
      peer.send_to(request("INVITE", 1, body), sip_port); assert(peer.take().rfind("SIP/2.0 100", 0) == 0);
      const OpusReceiveConstraints limits = leg == 0 ? OpusReceiveConstraints{12000, 8000, false} : OpusReceiveConstraints{};
      assert(phone.constraints() == limits); assert(phone.prepare_media_path());
      assert(phone.send_answer(id, opus, opus)); const auto answer = peer.take();
      assert(answer.rfind("SIP/2.0 200", 0) == 0); to = header_value(answer, "To");
      peer.send_to(request("ACK", 1), sip_port); settled([&] { return phone.acked(); });
      assert(phone.commit_media_path() && phone.active());
      assert(phone.constraints() == limits && phone.encoder() == encoder && phone.decoder() == decoder);
      size_t encoded_bytes = 0; std::vector<int16_t> pcm(960);
      for (unsigned frame = 0; frame < 500; ++frame) {
        for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<int16_t>(15000 * std::sin((frame * pcm.size() + i) * .197));
        const auto context = phone.audio_tx_frame_context(opus); assert(context.valid);
        phone.send_audio_frame(reinterpret_cast<uint8_t *>(pcm.data()), pcm.size() * sizeof(int16_t), context);
        const auto packet = media.take(); assert(packet.size() > 12 && (packet[1] & 127) == 111);
        encoded_bytes += packet.size() - 12;
        assert(opus_packet_get_nb_channels(reinterpret_cast<const uint8_t *>(packet.data()) + 12) == 1);
        if (leg == 0) assert(opus_packet_get_bandwidth(reinterpret_cast<const uint8_t *>(packet.data()) + 12) == OPUS_BANDWIDTH_NARROWBAND);
      }
      int bitrate; assert(opus_encoder_ctl(encoder, OPUS_GET_BITRATE(&bitrate)) == OPUS_OK);
      assert(bitrate == (leg == 0 ? 12000 : 24000)); if (leg == 0) assert(encoded_bytes * 8 / 10 <= 12000);
      assert(phone.encoder() == encoder && phone.decoder() == decoder);
      peer.send_to(request("BYE", 2), sip_port); assert(peer.take().rfind("SIP/2.0 200", 0) == 0);
      settled([&] { return phone.idle(); });
      settled([&] { return phone.media_idle(); });
      assert(phone.constraints() == OpusReceiveConstraints{});
      assert(!phone.audio_tx_frame_context(opus).valid);
    }
#else
    const bool l24 = mode == "l24-storage";
    AudioFormatList tx, rx; tx.count = 3; rx.count = 4;
    tx.formats[0] = {16000, PcmFormat::S16LE, 1, 20, AudioCodec::PCM};
    tx.formats[1] = tx.formats[0]; tx.formats[1].frame_ms = 10;
    tx.formats[2] = {48000, PcmFormat::S16LE, 1, 10, AudioCodec::PCM};
    rx.formats[0] = tx.formats[2]; rx.formats[1] = rx.formats[0]; rx.formats[1].frame_ms = 20;
    rx.formats[2] = tx.formats[0]; rx.formats[3] = tx.formats[1];
    if (l24) {
      tx.count = rx.count = 1;
      tx.formats[0] = {16000, PcmFormat::S24LE, 1, 20, AudioCodec::PCM};
      rx.formats[0] = {16000, PcmFormat::S24LE_IN_S32, 1, 10, AudioCodec::PCM};
      assert(audio_formats_share_rtp_mapping(tx.formats[0], rx.formats[0]));
      assert(!same_decoded_audio_geometry(tx.formats[0], rx.formats[0]));
    }
    Received received;
    if (l24) phone.set_audio_callback(Received::capture, &received);
    phone.set_audio_formats(tx, rx);
    Sink sink; phone.set_video_endpoints(nullptr, &sink); phone.set_video_config(VideoCodec::JPEG, free_pair(), 26, 1200);
    assert(phone.start()); assert(phone.originate("127.0.0.1", peer.port()));
    assert(phone.send_invite("rx-preference", "100", "Phone", "101", "Peer"));
    if (tcp) peer.accept_phone(); const auto invite = peer.take();
    const uint8_t p16 = pcm_payload(message_body(invite), 16000, l24 ? "L24" : "L16");
    const uint8_t p48 = l24 ? p16 : pcm_payload(message_body(invite), 48000);
    const bool offered_video = message_body(invite).find("m=video ") != std::string::npos;
    unsigned session_version = 1;
    const auto audio = [&](const std::string &direction, bool wrong_pt = false, bool wrong_rate = false, unsigned video = 3) {
      const auto secondary = wrong_pt ? uint8_t(112) : p16;
      std::string body = "v=0\r\no=- 1 " + std::to_string(session_version) +
          " IN IP4 127.0.0.1\r\ns=Independent peer\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n"
          "m=audio " + std::to_string(media.port()) + " RTP/AVP ";
      if (l24) body += std::to_string(p16) + "\r\na=rtpmap:" + std::to_string(p16) + " L24/16000/1\r\n";
      else body += std::to_string(p48) + " " + std::to_string(secondary) +
          "\r\na=rtpmap:" + std::to_string(p48) + " L16/48000/1\r\na=rtpmap:" + std::to_string(secondary) +
          " L16/" + std::to_string(wrong_rate ? 32000 : 16000) + "/1\r\n";
      body += "a=ptime:20\r\na=" + direction + "\r\n";
      if (video == 1 || video == 2 || offered_video) body += "m=video " + std::to_string(video == 1 ? video_media.port() : 0) +
          " RTP/AVP 26\r\na=rtpmap:26 JPEG/90000\r\na=sendonly\r\n";
      return body;
    };
    const auto initial = audio("sendrecv");
    peer.send_to(response(invite, initial, "sip:101@127.0.0.1:" + std::to_string(peer.port())), sip_port);
    assert(peer.take().rfind("ACK ", 0) == 0);
    assert(phone.prepare_media_path() && phone.commit_media_path() && phone.active());
    const auto initial_map = phone.payloads();
    AudioFormat alias; assert(initial_map.find(p16, &alias) && alias.sample_rate == 16000 && alias.frame_ms == 10);
    if (l24) assert(alias.pcm_format == PcmFormat::S24LE_IN_S32);
    unsigned cseq = 1;
    const auto request = [&](const std::string &method, const std::string &body = "", bool failure_ack = false) {
      return method + " sip:100@127.0.0.1 SIP/2.0\r\nVia: SIP/2.0/" + (tcp ? "TCP" : "UDP") +
          " 127.0.0.1:" + std::to_string(peer.port()) + ";branch=z9hG4bK-refresh" + std::to_string(cseq) +
          (method == "ACK" && !failure_ack ? "-ack" : "") +
          "\r\nFrom: " + header_value(response(invite, "", "sip:101@127.0.0.1"), "To") +
          "\r\nTo: " + header_value(invite, "From") + "\r\nCall-ID: rx-preference\r\nCSeq: " +
          std::to_string(cseq) + " " + method +
          (method == "INVITE" ? "\r\nContact: <sip:101@127.0.0.1:" + std::to_string(peer.port()) + ">" : "") +
          "\r\nContent-Type: application/sdp\r\nContent-Length: " +
          std::to_string(body.size()) + "\r\n\r\n" + body;
    };
    const auto reoffer = [&](const std::string &body, unsigned status) {
      const auto before = phone.audio_facts(); ++cseq; peer.send_to(request("INVITE", body), sip_port);
      const auto answer = peer.take(); assert(answer.rfind("SIP/2.0 " + std::to_string(status), 0) == 0);
      assert(phone.audio_facts() == before && phone.payloads() == initial_map);
      peer.send_to(request("ACK", "", status >= 300), sip_port); settled([&] { return phone.acked(); });
    };
    reoffer(initial, 200);
    if (mode == "hold") {
      ++session_version; reoffer(audio("inactive"), 200);
      ++session_version; reoffer(audio("sendrecv"), 200);
    } else if (mode == "video") {
      ++session_version; reoffer(audio("sendrecv", false, false, 1), 200);
      ++session_version; reoffer(audio("sendrecv", false, false, 2), 200);
    } else if (mode == "bad-pt") { ++session_version; reoffer(audio("sendrecv", true), 488); }
    else if (mode == "bad-rate") { ++session_version; reoffer(audio("sendrecv", false, true), 488); }
    else assert(mode == "refresh" || l24);
    std::vector<uint8_t> pcm(tx.formats[0].nominal_frame_bytes());
    for (unsigned i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<uint8_t>(i * 7);
    const auto before = phone.rtp_facts();
    assert(phone.active());
    const auto context = phone.audio_tx_frame_context(tx.formats[0]); assert(context.valid);
    phone.send_audio_frame(pcm.data(), pcm.size(), context); const auto packet = media.take();
    assert(packet.size() == pcm.size() + 12 && (packet[1] & 127) == p16);
    const auto u32 = [&](size_t at) {
      return (uint32_t(uint8_t(packet[at])) << 24) | (uint32_t(uint8_t(packet[at + 1])) << 16) |
             (uint32_t(uint8_t(packet[at + 2])) << 8) | uint8_t(packet[at + 3]);
    };
    assert((uint16_t(uint8_t(packet[2])) << 8 | uint8_t(packet[3])) == std::get<0>(before));
    assert(u32(4) == std::get<1>(before) && u32(8) == std::get<2>(before));
    if (l24) {
      auto decoded_format = rx.formats[0]; decoded_format.frame_ms = 20;
      std::vector<uint8_t> decoded(decoded_format.nominal_frame_bytes());
      assert(rtp_payload_to_pcm(reinterpret_cast<const uint8_t *>(packet.data()) + 12, packet.size() - 12,
                                decoded_format, decoded.data(), decoded.size()) == decoded.size());
      for (size_t i = 0, j = 0; i < pcm.size(); i += 3, j += 4) {
        assert(decoded[j] == pcm[i] && decoded[j + 1] == pcm[i + 1] && decoded[j + 2] == pcm[i + 2]);
        assert(decoded[j + 3] == ((pcm[i + 2] & 0x80) ? 0xFF : 0));
      }
      std::string inbound = packet.substr(0, 12 + 480);
      const uint32_t source = htonl(0x51515151); std::memcpy(inbound.data() + 8, &source, sizeof(source));
      for (uint16_t sequence : {1, 2}) {
        const auto number = htons(sequence); const auto time = htonl(uint32_t(sequence - 1) * 160);
        std::memcpy(inbound.data() + 2, &number, sizeof(number)); std::memcpy(inbound.data() + 4, &time, sizeof(time));
        media.send_to(inbound, audio_port);
      }
      settled([&] { std::lock_guard<std::mutex> lock(received.mutex); return !received.pcm.empty(); });
      std::lock_guard<std::mutex> lock(received.mutex);
      const auto actual = unpack_audio_format(received.format);
      assert(actual.pcm_format == PcmFormat::S24LE_IN_S32 && actual.frame_ms == 10 && received.pcm.size() == 640);
      assert(std::equal(received.pcm.begin(), received.pcm.end(), decoded.begin()));
    } else {
      for (unsigned i = 0; i < pcm.size(); i += 2)
        assert(uint8_t(packet[12 + i]) == pcm[i + 1] && uint8_t(packet[13 + i]) == pcm[i]);
    }
    assert(std::get<0>(phone.rtp_facts()) == uint16_t(std::get<0>(before) + 1));
    assert(std::get<1>(phone.rtp_facts()) == std::get<1>(before) + 320);
    ++cseq; peer.send_to(request("BYE"), sip_port); assert(peer.take().rfind("SIP/2.0 200", 0) == 0);
    settled([&] { return phone.idle(); });
#endif
    phone.stop(); reap_host_tasks(); assert(video_probe::tasks == 0);
  }
  assert(video_probe::allocated == video_probe::freed && video_probe::allocations.empty());
}
