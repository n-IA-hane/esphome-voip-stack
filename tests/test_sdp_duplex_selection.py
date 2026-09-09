"""Actual SDP selection must agree with the advertised duplex contract."""

from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_plain_pcm_uses_common_format_and_opus_keeps_local_rates(tmp_path):
    source = (ROOT / "esphome/components/voip_stack/sip_sdp.cpp").read_text()
    start = source.index("        AudioFormat local_rx;")
    end = source.index("        } else if (!selected_tx && !selected_rx)", start)
    selection = source[start:end] + "        }\n"
    harness = r'''
#include "esphome/components/voip_stack/sip_types.h"
#include <cassert>
#include <vector>
#define ESP_LOGI(...) ((void)0)
using namespace esphome::voip_stack;
struct Selector {
 AudioFormatList offer_tx_formats_,offer_rx_formats_;
 bool remote_directional_audio_v1_=false;
 size_t udp_max_payload_=1200;
 AudioFormat selected_rx_format,selected_tx_format;
 uint8_t selected_rx_payload_type=0,selected_tx_payload_type=0;
 bool run(std::vector<AudioFormat> offer) {
  bool selected_rx=false,selected_tx=false;
  uint8_t payload_flow[128]{},media_flow=3,pt=95;
  for(const auto &fmt:offer) { ++pt;
'''
    tail = r'''
  }
  return selected_rx && selected_tx;
 }
};
AudioFormat pcm(unsigned rate) {AudioFormat f;f.sample_rate=rate;f.frame_ms=10;return f;}
int main(){
 for(unsigned rate:{16000,24000,32000}) {
  Selector s;s.offer_tx_formats_.count=1;s.offer_tx_formats_.formats[0]=pcm(rate);
  s.offer_rx_formats_.count=2;s.offer_rx_formats_.formats[0]=pcm(48000);s.offer_rx_formats_.formats[1]=pcm(rate);
  assert(s.run({pcm(48000),pcm(rate)}));
  assert(s.selected_tx_format.sample_rate==rate && s.selected_rx_format.sample_rate==rate);
  assert(s.selected_rx_payload_type==s.selected_tx_payload_type);
  s.remote_directional_audio_v1_=true;assert(s.run({pcm(48000),pcm(rate)}));
  assert(s.selected_tx_format.sample_rate==rate && s.selected_rx_format.sample_rate==48000);
 }
 Selector none;none.offer_tx_formats_.count=none.offer_rx_formats_.count=1;
 none.offer_tx_formats_.formats[0]=pcm(16000);none.offer_rx_formats_.formats[0]=pcm(48000);
 assert(!none.run({pcm(48000),pcm(16000)}));
 Selector opus;auto tx=pcm(16000),rx=pcm(48000),wire=pcm(48000);
 tx.codec=rx.codec=wire.codec=AudioCodec::OPUS;tx.frame_ms=rx.frame_ms=wire.frame_ms=20;wire.channels=2;
 opus.offer_tx_formats_.count=opus.offer_rx_formats_.count=1;
 opus.offer_tx_formats_.formats[0]=tx;opus.offer_rx_formats_.formats[0]=rx;
 assert(opus.run({wire}));assert(opus.selected_tx_format.sample_rate==16000 && opus.selected_rx_format.sample_rate==48000);
}
'''
    cpp = tmp_path / "sdp.cpp"
    cpp.write_text(harness + selection + tail)
    exe = tmp_path / "sdp"
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT),
                    str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
