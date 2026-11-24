#include "sip_sdp_audio_negotiation.h"
#include "sip_sdp_audio_offer.h"
#include "sip_sdp_video_selection.h"

#include <cassert>
#include <fstream>
#include <iterator>

using namespace esphome::voip_stack;

static VideoCapability jpeg_capability() {
  VideoCapability result;
  result.encoding = "JPEG";
  result.payload_type = 26;
  result.width = 320;
  result.height = 240;
  result.max_fps = 10;
  return result;
}

static void disabled_video() {
  const auto jpeg = jpeg_capability();
  for (const auto port : {"0", "40008"}) {
    VideoOfferAttributes offer;
    read_video_offer_attributes(std::string("m=video ") + port + " RTP/AVP 113 118\r\na=rtcp-mux-only\r\n", 0xC0000201,
                                jpeg, jpeg, offer);
    uint8_t payload = 26;
    bool zero = false;
    const auto expected = std::string(port) == "0" ? VideoOfferDisposition::DISABLED : VideoOfferDisposition::REJECTED;
    assert(validate_video_offer(offer, 0, 0, false, payload, zero) == expected);
    assert(offer.offered && payload == 113 && !zero);
  }
}

static void audio_section(const char *fixture_path) {
  std::ifstream fixture(fixture_path, std::ios::binary);
  assert(fixture);
  const std::string captured{std::istreambuf_iterator<char>(fixture), {}};
  AudioOfferAttributes captured_offer;
  assert(read_audio_offer_attributes(captured, captured_offer));
  assert(captured_offer.port == 21414 && captured_offer.payload_rank[9] == 0 && captured_offer.ptime == 10);

  const AudioFormat g722{16000, PcmFormat::S16LE, 1, 10, AudioCodec::G722};
  AudioFormatList formats;
  formats.count = 1;
  formats.formats[0] = g722;
  RtpAudioPayloads previously_offered;
  assert(previously_offered.add(9, g722));
  const std::string local_sdp = "v=0\r\nc=IN IP4 192.0.2.1\r\nm=audio 40000 RTP/AVP 9 120\r\n"
      "a=rtpmap:9 G722/8000/1\r\na=rtpmap:120 telephone-event/8000\r\na=fmtp:120 0-15\r\na=ptime:10\r\na=sendrecv\r\n";
  AudioFormatList offered_tx;
  DtmfPayloads offered_dtmf;
  uint8_t offered_clocks = 0;
  assert(capture_audio_offer(local_sdp, formats, formats, 1200, &offered_tx,
                             &previously_offered, &offered_clocks, &offered_dtmf));
  const LocalAudioOffer local{formats, formats, offered_tx, previously_offered, 1200,
                              offered_clocks, RTP_AUDIO_RECEIVE_PAYLOAD_BYTES, &offered_dtmf};
  const SdpAudioMedia retained;
  SdpAudioMedia negotiated;
  DtmfConfig dtmf;
  unsigned choices = 0;
  const auto trace = [&](uint8_t pt, const AudioFormat &, const AudioPayloadSelection &, const AudioMappingChanges &c) {
    assert(pt == 9 && c.rx && c.tx);
    ++choices;
  };
  const auto no_rejection = [](size_t, uint16_t, uint32_t, bool, bool) { assert(false); };
  assert(negotiate_remote_audio(captured, 0, true, false, local, retained, negotiated, dtmf, trace, no_rejection));
  assert(choices == 1 && negotiated.tx == g722 && negotiated.rx == g722);
  assert(negotiated.tx_pt == 9 && negotiated.rx_pt == 9 && negotiated.audio_port == 21414);
  assert(negotiated.audio_ip == 0xC0000202 && negotiated.send && negotiated.receive);
  assert(dtmf.tx.valid() && dtmf.tx.clock == 8000 && dtmf.tx.payload_type == 120);

  // The first malformed candidate cannot donate payloads to the next one.
  const std::string body =
      "a=sendonly\r\nm=audio 8000 RTP/SAVP 111\r\nm=audio 8001 RTP/AVP 115 broken\r\n"
      "m=video 8100 RTP/AVP 26\r\na=inactive\r\nm=audio 8200 RTP/AVP 96 9 96\r\n"
      "a=maxptime:32.5\r\na=maxptime:20.5\r\na=recvonly\r\nm=audio 8300 RTP/AVP 115\r\na=inactive";
  AudioOfferAttributes offer;
  assert(read_audio_offer_attributes(body, offer));
  assert(offer.section == body.find("m=audio 8200") && offer.port == 8200 && offer.flow == 2);
  assert(offer.offered(96) && offer.offered(9) && !offer.offered(115));
  assert(offer.payload_rank[96] == 0 && offer.payload_rank[9] == 1 && offer.maxptime == 20);
  uint8_t pt;
  AudioFormat mapped;
  assert(!parse_rtpmap_format("a=rtpmap:97 PCM/16000", &mapped, &pt));
  assert(parse_rtpmap_format("a=rtpmap:9 G722/8000", &mapped, &pt));
  assert(mapped.sample_rate == 16000 && audio_format_rtp_clock_rate(mapped) == 8000);
  assert(sdp_direction("a=inactive-suffix") == -1 && sdp_direction("a=inactive-suffix", true) == 0);
}

static void inactive_video() {
  const auto jpeg = jpeg_capability();
  const std::string lines =
      "c=IN IP4 192.0.2.3\r\na=sendonly\r\nm=video 41000 RTP/AVP 112 broken\r\n"
      "m=video 42000 RTP/AVP 26\r\na=inactive\r\na=rtcp:42001 IN IP4 192.0.2.4\r\n";
  const auto inspect = [&](const std::string &body, bool compatible) {
    VideoOfferAttributes offer;
    read_video_offer_attributes(body, 0, jpeg, jpeg, offer);
    assert(offer.offered && !offer.offered_payloads[112]);
    assert(offer.media_port == 42000 && offer.media_ip == 0xC0000203 && offer.media_rtcp_ip == 0xC0000204);
    assert(offer.media_max_fps == 0);
    bool zero = false;
    VideoCapability output;
    assert(validate_video_offer(offer, 0, 0, false, output.payload_type, zero) ==
           VideoOfferDisposition::SELECT_PAYLOAD);
    VideoPayloadSelection selected{output};
    assert(select_video_payload(offer, jpeg, jpeg, true, false, 26, selected) == compatible);
    assert(!selected.send && !selected.receive);
    if (compatible) assert(output.is_jpeg() && output.payload_type == 26);
  };
  inspect(lines + "a=framerate:29.97\r\n", true);
  inspect(lines + "a=rtpmap:26 H264/90000\r\n", false);
  inspect(lines + "a=rtpmap:26 H264/90000\r\na=rtpmap:26 JPEG/90000\r\n", true);
}

int main(int argc, char **argv) {
  assert(argc == 3);
  const std::string which = argv[1];
  if (which == "disabled")
    disabled_video();
  else if (which == "audio")
    audio_section(argv[2]);
  else {
    assert(which == "inactive");
    inactive_video();
  }
}
