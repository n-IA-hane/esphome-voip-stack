#include "sip_dialog_host_sdk.h"
#include "sip_types.h"
#include "media_negotiator.h"
#define private public
#include "opus_rtp_codec.h"
#undef private
#include "sip_transport.h"
#include "sip_message.h"

#include <cassert>
#include <cmath>
#include <poll.h>
#include <vector>

using namespace esphome::voip_stack;

static AudioFormat format() { return {48000, PcmFormat::S16LE, 1, 20, AudioCodec::OPUS}; }
static std::string offer(const std::string &limits = "", uint16_t port = 40002) {
  return "v=0\r\nc=IN IP4 127.0.0.1\r\nm=audio " + std::to_string(port) +
      " RTP/AVP 111\r\na=fmtp:111 " + limits + "\r\na=rtpmap:111 opus/48000/2\r\na=ptime:20\r\n";
}

struct Phone : SipTransport {
  using SipTransport::NegotiatedMedia;
  using SipTransport::negotiated_media_;
  using SipTransport::commit_negotiated_media_;
  using SipTransport::exchange_negotiated_media_;
  using SipTransport::parse_remote_media_;
  using SipTransport::build_negotiated_sdp_answer_;
  using SipTransport::build_sdp_offer_;
  using SipTransport::opus_codec_;
  using SipTransport::media_proposal_epoch_;
  Phone() : SipTransport(5060, 40000, 1200, "127.0.0.1", false) {
    AudioFormatList formats; formats.count = 1; formats.formats[0] = format();
    set_audio_formats(formats, formats);
  }
  void prepare_sender() {
    rtp_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    assert(rtp_socket_ >= 0);
    rtp_running_ = true;
  }
};

int main(int argc, char **argv) {
  assert(argc == 2);
  const std::string mode(argv[1]);
  if (mode == "codec") {
    const auto audio = format();
    OpusRtpCodec codec;
    assert(codec.configure(&audio, &audio, 1200));
    auto *encoder = codec.encoder_; auto *decoder = codec.decoder_;
    std::vector<int16_t> pcm(audio.nominal_frame_samples());
    uint8_t packet[1275];
    for (const auto rate : {8000, 12000, 16000, 24000, 48000}) {
      size_t bytes = 0;
      const OpusReceiveConstraints limits{12000, static_cast<uint16_t>(rate), false};
      for (unsigned frame = 0; frame < 500; ++frame) {
        for (size_t i = 0; i < pcm.size(); ++i)
          pcm[i] = static_cast<int16_t>(15000 * std::sin((frame * pcm.size() + i) * 0.197));
        const auto size = codec.encode(reinterpret_cast<const uint8_t *>(pcm.data()), pcm.size() * 2,
                                       audio, packet, sizeof(packet), limits);
        assert(size > 0);
        bytes += size;
        assert(opus_packet_get_nb_channels(packet) == 1);
        assert(opus_packet_get_nb_samples(packet, size, 48000) == 960);
        const int bandwidth = opus_packet_get_bandwidth(packet);
        assert(bandwidth <= (rate == 8000 ? OPUS_BANDWIDTH_NARROWBAND : rate == 12000 ? OPUS_BANDWIDTH_MEDIUMBAND :
            rate == 16000 ? OPUS_BANDWIDTH_WIDEBAND : rate == 24000 ? OPUS_BANDWIDTH_SUPERWIDEBAND : OPUS_BANDWIDTH_FULLBAND));
      }
      int bitrate; assert(opus_encoder_ctl(encoder, OPUS_GET_BITRATE(&bitrate)) == OPUS_OK && bitrate == 12000);
      // RFC 7587 limits the average, not each VBR packet. Measure ten seconds.
      assert(bytes * 8 / 10 <= 12000);
      assert(codec.encoder_ == encoder && codec.decoder_ == decoder);
    }
    assert(codec.encode(reinterpret_cast<const uint8_t *>(pcm.data()), pcm.size() * 2, audio, packet, sizeof(packet)));
    int bitrate; assert(opus_encoder_ctl(encoder, OPUS_GET_BITRATE(&bitrate)) == OPUS_OK && bitrate == 24000);
    assert(codec.encoder_ == encoder && codec.decoder_ == decoder);
    return 0;
  }
  Phone phone;
  const auto initial = phone.negotiated_media_();
  Phone::NegotiatedMedia selected;
  assert(phone.parse_remote_media_(offer("maxaveragebitrate=12000;maxplaybackrate=8000;stereo=0"), 0, false, initial, &selected));
  assert(selected.opus_tx.max_average_bitrate == 12000 && selected.opus_tx.max_playback_rate == 8000 && !selected.opus_tx.stereo);
  phone.commit_negotiated_media_(selected);
  const auto answer = phone.build_negotiated_sdp_answer_(selected, 1);
  assert(answer.find("maxaveragebitrate=28000") != std::string::npos);
  assert(answer.find("maxaveragebitrate=12000") == std::string::npos && answer.find("maxplaybackrate=8000") == std::string::npos);
  auto *encoder = phone.opus_codec_.encoder_; auto *decoder = phone.opus_codec_.decoder_;
  if (mode == "negotiation") {
    Phone::NegotiatedMedia replacement;
    assert(phone.parse_remote_media_(offer("maxaveragebitrate=16000;maxplaybackrate=24000;stereo=1"), 0, false, selected, &replacement, true));
    assert(replacement.opus_tx.max_average_bitrate == 16000 && replacement.opus_tx.max_playback_rate == 24000 && replacement.opus_tx.stereo);
    // Failed preparation cannot mutate the committed value or encoder.
    assert(!phone.parse_remote_media_(offer("maxaveragebitrate=6000") + "a=maxptime:1\r\n", 0, false, selected, &replacement, true));
    assert(phone.negotiated_media_().opus_tx == selected.opus_tx);
    assert(phone.parse_remote_media_(offer("maxaveragebitrate=5999;maxplaybackrate=7999;stereo=2"), 0, false, selected, &replacement, true));
    assert(replacement.opus_tx == OpusReceiveConstraints{});
    assert(phone.parse_remote_media_(offer("maxaveragebitrate=99999999999999;maxplaybackrate=48001") +
        "a=fmtp:112 maxaveragebitrate=6000\r\n", 0, false, selected, &replacement, true));
    assert(replacement.opus_tx == OpusReceiveConstraints{});
    assert(!phone.build_sdp_offer_().empty());
    auto remote_answer=offer("maxaveragebitrate=16000;maxplaybackrate=16000;stereo=1");
    assert(phone.parse_remote_media_(remote_answer, 0, true, selected, &replacement));
    assert(replacement.opus_tx.max_average_bitrate==16000 && replacement.opus_tx.max_playback_rate==16000 && replacement.opus_tx.stereo);
    assert(phone.parse_remote_media_(offer(), 0, false, selected, &replacement, true));
    assert(replacement.opus_tx == OpusReceiveConstraints{});
    phone.exchange_negotiated_media_(replacement);
    assert(phone.negotiated_media_().opus_tx == OpusReceiveConstraints{} && replacement.opus_tx == selected.opus_tx);
    phone.exchange_negotiated_media_(replacement);
    assert(phone.negotiated_media_().opus_tx == selected.opus_tx && replacement.opus_tx == OpusReceiveConstraints{});
  } else {
    int inbox = socket(AF_INET, SOCK_DGRAM, 0); assert(inbox >= 0);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(inbox, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    socklen_t length = sizeof(address); assert(getsockname(inbox, reinterpret_cast<sockaddr *>(&address), &length) == 0);
    selected.audio_port = ntohs(address.sin_port); phone.commit_negotiated_media_(selected); phone.prepare_sender();
    std::vector<int16_t> pcm(960, 10000); uint8_t packet[1500];
    phone.send_audio_frame(reinterpret_cast<const uint8_t *>(pcm.data()), pcm.size() * 2);
    const auto size = recv(inbox, packet, sizeof(packet), 0); assert(size > 12);
    assert((packet[1] & 127) == 111 && opus_packet_get_bandwidth(packet + 12) == OPUS_BANDWIDTH_NARROWBAND);
    int bitrate; assert(opus_encoder_ctl(encoder, OPUS_GET_BITRATE(&bitrate)) == OPUS_OK && bitrate == 12000);
    // A speculative contract must not alter the encoder or send a packet.
    phone.media_proposal_epoch_++; auto staged = selected; staged.opus_tx = {};
    phone.exchange_negotiated_media_(staged);
    phone.send_audio_frame(reinterpret_cast<const uint8_t *>(pcm.data()), pcm.size() * 2);
    pollfd poller{inbox, POLLIN, 0}; assert(poll(&poller, 1, 0) == 0);
    assert(opus_encoder_ctl(encoder, OPUS_GET_BITRATE(&bitrate)) == OPUS_OK && bitrate == 12000);
    phone.exchange_negotiated_media_(staged); phone.media_proposal_epoch_++;
    phone.send_audio_frame(reinterpret_cast<const uint8_t *>(pcm.data()), pcm.size() * 2);
    assert(recv(inbox, packet, sizeof(packet), 0) > 12);
    auto relaxed = selected; relaxed.opus_tx = {}; phone.commit_negotiated_media_(relaxed);
    phone.send_audio_frame(reinterpret_cast<const uint8_t *>(pcm.data()), pcm.size() * 2);
    assert(recv(inbox, packet, sizeof(packet), 0) > 12);
    assert(opus_encoder_ctl(encoder, OPUS_GET_BITRATE(&bitrate)) == OPUS_OK && bitrate == 24000);
    close(inbox);
  }
  assert(phone.opus_codec_.encoder_ == encoder && phone.opus_codec_.decoder_ == decoder);
}
