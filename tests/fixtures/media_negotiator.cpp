#include "media_negotiator.h"
#include "sip_sdp_audio_offer.h"

#include <cassert>
#include <string>

using namespace esphome::voip_stack;

AudioFormat pcm(uint32_t rate, uint16_t ms) {
  return {rate, PcmFormat::S16LE, 1, ms, AudioCodec::PCM};
}

int main(int argc, char **argv) {
  assert(argc == 2);
  const std::string mode = argv[1];
  if (mode.rfind("ordered-receive-", 0) == 0) {
    const bool answer = mode == "ordered-receive-answer";
    for (bool wide : {false, true}) for (bool swapped : {false, true})
        for (bool reverse : {false, true}) for (const std::string flow : {"sendrecv", "sendonly"}) {
      AudioFormatList tx, rx;
      tx.count = 3; rx.count = 4;
      tx.formats[0] = pcm(16000, 20); tx.formats[1] = pcm(16000, 10); tx.formats[2] = pcm(48000, 10);
      rx.formats[0] = pcm(48000, 10); rx.formats[1] = pcm(48000, 20);
      rx.formats[2] = pcm(16000, swapped ? 10 : 20); rx.formats[3] = pcm(16000, swapped ? 20 : 10);
      // Preserve the common 10ms preference from an already transmitted offer.
      RtpAudioPayloads sent_rx;
      assert(sent_rx.add(96, pcm(48000, 10))); assert(sent_rx.add(97, pcm(16000, 10)));
      const LocalAudioOffer local{tx, rx, tx, sent_rx, 1200, 0x7f};
      const std::string narrow_map = "a=rtpmap:117 L16/16000/1\r\n";
      const std::string wide_map = wide ? "a=rtpmap:116 L16/48000/1\r\n" : "";
      const std::string remote = "v=0\r\nc=IN IP4 192.0.2.2\r\nm=audio 40002 RTP/AVP 117" +
          (wide ? std::string(" 116") : "") + "\r\n" +
          (reverse ? wide_map + narrow_map : narrow_map + wide_map) +
          "a=ptime:20\r\na=" + flow + "\r\n";
      SdpAudioMedia retained, selected;
      DtmfConfig dtmf;
      assert(negotiate_audio_contract(remote, 0, answer, false, local, retained, selected, dtmf));
      const auto expected_rx = pcm(wide ? 48000 : 16000, wide || answer || swapped ? 10 : 20);
      assert(selected.rx == expected_rx && selected.receive);
      assert(selected.send == (flow == "sendrecv"));
      if (selected.send) assert(selected.tx == pcm(16000, 20));
      assert(selected.rx_pt == (wide ? (answer ? 96 : 116) : (answer ? 97 : 117)));
      AudioFormat accepted;
      assert(selected.rx_payloads.find(answer ? 97 : 117, &accepted));
      assert(accepted == pcm(16000, expected_rx.frame_ms));
      assert(selected.rx_payloads.count == (wide ? 2 : 1));
    }
    return 0;
  }
  if (mode.rfind("static-l16", 0) == 0) {
    AudioFormatList formats; formats.count = 1;
    formats.formats[0] = pcm(44100, 10);
    const bool stereo = mode == "static-l16-stereo";
    if (stereo) formats.formats[0].channels = 2;
    RtpAudioPayloads sent; assert(sent.add(stereo ? 10 : 11, formats.formats[0]));
    const LocalAudioOffer local{formats, formats, formats, sent, 1200, 0x7f};
    SdpAudioMedia retained, selected; DtmfConfig events;
    const auto base = std::string("v=0\r\nc=IN IP4 192.0.2.2\r\nm=audio 40002 RTP/AVP ") +
                      (stereo ? "10" : "11") + "\r\na=ptime:20\r\n";
    const auto mapping = std::string("a=rtpmap:") + (stereo ? "10" : "11") + " L16/44100/" + (stereo ? "2" : "1") + "\r\n";
    assert(static_rtp_audio_format(10).channels == 2 && static_rtp_audio_format(11).channels == 1);
    assert(static_rtp_audio_payload(formats.formats[0]) == (stereo ? 10 : 11));
    for (const auto &body : {base, base + mapping}) {
      const bool accepted = negotiate_audio_contract(body, 0, false, false, local, retained, selected, events);
      assert(accepted == !stereo);
      if (accepted) assert(selected.tx == formats.formats[0] && selected.tx_pt == 11 && selected.rx_pt == 11);
    }
    assert(formats.formats[0].nominal_rtp_payload_bytes() == (stereo ? 1764 : 882));
    assert(!negotiate_audio_contract(base + "a=rtpmap:11 L16/16000/1\r\n", 0, false, false,
                                     local, retained, selected, events));
    formats.formats[0] = pcm(16000, 10);
    assert(!negotiate_audio_contract(base, 0, false, false, local, retained, selected, events));
    assert(negotiate_audio_contract(
        "v=0\r\nc=IN IP4 192.0.2.2\r\nm=audio 40002 RTP/AVP 11 96\r\n"
        "a=rtpmap:11 L16/16000/1\r\na=rtpmap:96 L16/16000/1\r\n", 0, false, false,
        local, retained, selected, events));
    assert(selected.tx_pt == 96 && selected.rx_pt == 96);
    return 0;
  }
  if (mode == "g722-dtmf") {
    AudioFormatList formats; formats.count = 1;
    formats.formats[0] = {16000, PcmFormat::S16LE, 1, 20, AudioCodec::G722};
    RtpAudioPayloads sent; assert(sent.add(9, formats.formats[0]));
    const LocalAudioOffer local{formats, formats, formats, sent, 1200, 0x7f};
    SdpAudioMedia retained, selected; DtmfConfig events;
    assert(negotiate_audio_contract(
        "v=0\r\nc=IN IP4 192.0.2.2\r\nm=audio 40002 RTP/AVP 9 101 107\r\n"
        "a=rtpmap:101 telephone-event/8000\r\na=rtpmap:107 telephone-event/16000\r\n",
        0, false, false, local, retained, selected, events));
    assert(selected.tx.sample_rate == 16000 && events.tx.clock == 8000 && events.rx.clock == 8000);
    assert(events.tx.payload_type == 101 && events.rx.payload_type == 101);
    return 0;
  }
  AudioFormatList tx, rx;
  tx.count = 3; rx.count = 4;
  tx.formats[0] = pcm(16000, 20);
  tx.formats[1] = pcm(16000, 10);
  tx.formats[2] = pcm(48000, 10);
  rx.formats[0] = pcm(48000, 10);
  rx.formats[1] = pcm(48000, 20);
  rx.formats[2] = pcm(16000, 10);
  rx.formats[3] = pcm(16000, 20);
  RtpAudioPayloads sent_rx;
  assert(sent_rx.add(96, rx.formats[0]));
  assert(sent_rx.add(97, rx.formats[2]));
  // Answer negotiation consumes the exact sent offer, including event payloads.
  // A clock bitmap alone cannot describe directional RTP payload numbers.
  const std::string sent_offer =
      "v=0\r\nc=IN IP4 192.0.2.1\r\nm=audio 40000 RTP/AVP 96 97 120 122 126\r\n"
      "a=rtpmap:96 L16/48000/1\r\na=rtpmap:97 L16/16000/1\r\n"
      "a=rtpmap:120 telephone-event/8000\r\na=fmtp:120 0-15\r\n"
      "a=rtpmap:122 telephone-event/16000\r\na=fmtp:122 0-15\r\n"
      "a=rtpmap:126 telephone-event/48000\r\na=fmtp:126 0-15\r\n"
      "a=ptime:10\r\na=maxptime:20\r\na=sendrecv\r\n";
  AudioFormatList captured_tx;
  RtpAudioPayloads captured_rx;
  DtmfPayloads offered_events;
  uint8_t offered_clocks = 0;
  assert(capture_audio_offer(sent_offer, tx, rx, 1200, &captured_tx, &captured_rx,
                             &offered_clocks, &offered_events));
  assert(captured_rx == sent_rx);
  const LocalAudioOffer local{tx, rx, tx, sent_rx, 1200, offered_clocks,
                             RTP_AUDIO_RECEIVE_PAYLOAD_BYTES, &offered_events};
  const bool answer = mode == "answer" || mode == "directional-dtmf-answer";
  const bool directional_dtmf = mode.rfind("directional-dtmf", 0) == 0;
  std::string remote =
      "v=0\r\nc=IN IP4 192.0.2.2\r\nm=audio 40002 RTP/AVP 116 117 101\r\n"
      "a=rtpmap:116 L16/48000/1\r\na=rtpmap:117 L16/16000/1\r\n"
      "a=ptime:20\r\na=maxptime:20\r\n"
      "a=rtpmap:101 telephone-event/8000\r\na=fmtp:101 0-11\r\n";
  if (directional_dtmf) {
    remote.replace(remote.find("116 117 101"), 11, "116 117 101 107 108");
    remote += "a=rtpmap:107 telephone-event/16000\r\na=fmtp:107 0-9\r\n"
              "a=rtpmap:108 telephone-event/48000\r\na=fmtp:108 10-11\r\n";
  }
  SdpAudioMedia retained, selected;
  DtmfConfig dtmf;
  if (mode == "hard-maximum-no-compatible-duration") {
    // Preserve the original failing witness: a sole 20 ms transmitter cannot
    // satisfy a peer's 10 ms maximum. Adding truthful alternatives is explicit.
    tx.count = 1;
    assert(!negotiate_audio_contract(remote + "a=maxptime:10\r\n", 0, false, false,
                                     local, retained, selected, dtmf));
    return 0;
  }
  if (mode == "hard-maximum") remote += "a=maxptime:10\r\n";
  if (mode == "tx-fallback") {
    remote = "v=0\r\nc=IN IP4 192.0.2.2\r\nm=audio 40002 RTP/AVP 116\r\n"
             "a=rtpmap:116 L16/48000/1\r\na=ptime:20\r\na=maxptime:20\r\n";
  }
  assert(negotiate_audio_contract(remote, 0, answer, false, local, retained, selected, dtmf));
  const auto expected_tx = mode == "tx-fallback" ? tx.formats[2]
                           : mode == "hard-maximum" ? tx.formats[1] : tx.formats[0];
  // RFC 3264 standard alternatives preserve native 16 kHz/20 ms TX and
  // preferred 48 kHz/10 ms RX, with no private attributes in either role.
  assert(selected.tx == expected_tx && selected.rx == rx.formats[0]);
  assert(selected.rx.nominal_rtp_payload_bytes() == 960);
  assert(selected.tx_pt == (mode == "tx-fallback" ? 116 : 117));
  assert(selected.rx_pt == (answer ? 96 : 116));
  assert(selected.audio_ip == 0xc0000202 && selected.audio_port == 40002);
  assert(selected.send && selected.receive);
  const auto accepted = configured_decoded_receive_formats(selected.rx_payloads, rx, RTP_AUDIO_RECEIVE_PAYLOAD_BYTES);
  assert(audio_format_list_contains(accepted, pcm(48000, 20)));
  assert(pcm(48000, 20).nominal_rtp_payload_bytes() == 1920);
  if (directional_dtmf) {
    assert(dtmf.tx.clock == 16000 && dtmf.rx.clock == 48000);
    assert(dtmf.tx.payload_type == 107 && dtmf.rx.payload_type == (answer ? 126 : 108));
    assert(dtmf.tx.events == 0x3ff && dtmf.rx.events == 0xc00);
  } else if (mode != "tx-fallback") {
    assert(dtmf.tx.clock == 8000 && dtmf.rx.clock == 8000 && dtmf.tx.events == 0xfff && dtmf.rx.events == 0xfff);
  }
}
