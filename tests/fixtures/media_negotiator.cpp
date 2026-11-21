#include "media_negotiator.h"

#include <cassert>
#include <string>

using namespace esphome::voip_stack;

AudioFormat pcm(uint32_t rate, uint16_t ms) {
  return {rate, PcmFormat::S16LE, 1, ms, AudioCodec::PCM};
}

int main(int argc, char **argv) {
  assert(argc == 2);
  const std::string mode = argv[1];
  AudioFormatList tx, rx;
  tx.count = rx.count = 1;
  tx.formats[0] = pcm(16000, 20);
  rx.formats[0] = pcm(48000, 10);
  RtpAudioPayloads sent_rx;
  assert(sent_rx.add(96, rx.formats[0]));
  const LocalAudioOffer local{tx, rx, tx, sent_rx, 1200, true, 0x7f};
  const bool answer = mode == "answer";
  const std::string remote =
      "v=0\r\nc=IN IP4 192.0.2.2\r\nm=audio 40002 RTP/AVP 116 117 101\r\n"
      "a=rtpmap:116 L16/48000/1\r\na=rtpmap:117 L16/16000/1\r\n"
      "a=x-voip-stack-flow:116 send\r\na=x-voip-stack-flow:117 recv\r\n"
      "a=x-voip-stack-independent-ptime:1\r\na=ptime:20\r\n"
      "a=rtpmap:101 telephone-event/8000\r\na=fmtp:101 0-11\r\n";
  SdpAudioMedia retained, selected;
  DtmfConfig dtmf;
  if (mode == "hard-maximum") {
    assert(!negotiate_audio_contract(remote + "a=maxptime:10\r\n", 0, false, false,
                                     local, retained, selected, dtmf));
    return 0;
  }
  assert(negotiate_audio_contract(remote, 0, answer, false, local, retained, selected, dtmf));
  // Contract R1/R3 and profile esp_directional_v1: declared directional
  // capabilities preserve 16 kHz/20 ms TX and 48 kHz/10 ms RX in either role.
  assert(selected.tx == tx.formats[0] && selected.rx == rx.formats[0]);
  assert(selected.tx.nominal_rtp_payload_bytes() == 640 && selected.rx.nominal_rtp_payload_bytes() == 960);
  assert(selected.tx_pt == 117 && selected.rx_pt == (answer ? 96 : 116));
  assert(selected.audio_ip == 0xc0000202 && selected.audio_port == 40002);
  assert(selected.send && selected.receive && selected.directional_audio);
  assert(dtmf.tx.clock == 8000 && dtmf.rx.clock == 8000 && dtmf.tx.events == 0xfff && dtmf.rx.events == 0xfff);
}
