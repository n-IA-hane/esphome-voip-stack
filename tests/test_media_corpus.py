"""The firmware answers real offers from Home Assistant as the devices did on the wire.

The shared media corpus lives in the integration repository. Each case used here holds an offer
Home Assistant sent to an ESP phone and the answer that phone returned. The host build of the
firmware negotiator receives the same offer with the same local formats and must select the
formats the device announced.
"""
import json
import os
from pathlib import Path
import re
import subprocess

import pytest

from test_sdp_receive_payloads import ROOT, sdp_fixture_source

CORPUS = Path(os.environ.get("VOIP_MEDIA_CORPUS", ROOT.parent / "esphome-intercom" / "tests" / "media_corpus"))
# Local formats of the two phones the captures were taken from are set inside the harness.
DEVICES = ("esp-p4", "esp-box-opus")

HARNESS = r'''
#include <cstdio>
#include <fstream>
#include <sstream>
AudioFormat make(AudioCodec codec, unsigned rate, unsigned frame_ms) {
  AudioFormat f; f.codec = codec; f.sample_rate = rate; f.frame_ms = frame_ms; return f;
}
int main(int argc, char **argv) {
  std::ifstream file(argv[1], std::ios::binary); std::stringstream body; body << file.rdbuf();
  SipTransport s;
  auto &tx = s.offer_tx_formats_; auto &rx = s.offer_rx_formats_;
  if (std::string(argv[2]) == "esp-p4") {
    tx.count = 2; tx.formats[0] = make(AudioCodec::PCM, 16000, 20); tx.formats[1] = make(AudioCodec::PCM, 16000, 10);
    rx.count = 2; rx.formats[0] = make(AudioCodec::PCM, 48000, 10); rx.formats[1] = make(AudioCodec::PCM, 16000, 10);
  } else {
    tx.count = 1; tx.formats[0] = make(AudioCodec::OPUS, 16000, 20);
    rx.count = 1; rx.formats[0] = make(AudioCodec::OPUS, 48000, 20);
  }
  if (!s.learn_remote_rtp_from_sdp_(body.str(), 0)) { std::printf("{\"outcome\":\"reject\"}\n"); return 0; }
  std::printf("{\"outcome\":\"accept\",\"tx_rate\":%u,\"tx_ms\":%u,\"tx_pt\":%u,\"rx_rate\":%u,\"rx_ms\":%u,\"rx_pt\":%u}\n",
              (unsigned) s.tx.sample_rate, (unsigned) s.tx.frame_ms, (unsigned) s.tx_pt,
              (unsigned) s.rx.sample_rate, (unsigned) s.rx.frame_ms, (unsigned) s.rx_pt);
  return 0;
}
'''


def _cases():
    if not (CORPUS / "cases").is_dir():
        return []
    found = []
    for path in sorted((CORPUS / "cases").glob("esp-*-answers-*/case.json")):
        device = next((name for name in DEVICES if path.parent.name.startswith(name)), None)
        if device:
            found.append(pytest.param(path.parent, device, id=path.parent.name))
    return found


def _announced(answer: str) -> dict:
    """Read what the real device announced: the payload it receives on and the one it sends with."""
    audio = answer.split("m=video")[0]
    rate = {int(pt): int(clock) for pt, clock in re.findall(r"a=rtpmap:(\d+) (?:L16|opus)/(\d+)", audio, re.I)}
    flow = dict((int(pt), role) for pt, role in re.findall(r"a=x-voip-stack-flow:(\d+) (\w+)", audio))
    receive = next(pt for pt in rate if "recv" in flow.get(pt, "sendrecv"))
    send = next(pt for pt in reversed(list(rate)) if "send" in flow.get(pt, "sendrecv"))
    return {"rx_pt": receive, "rx_rate": rate[receive], "tx_pt": send}


@pytest.fixture(scope="module")
def negotiator(tmp_path_factory):
    work = tmp_path_factory.mktemp("media_corpus")
    source = work / "corpus.cpp"
    source.write_text(sdp_fixture_source() + HARNESS)
    binary = work / "corpus"
    built = subprocess.run(["g++", "-std=c++17", "-I", str(ROOT), str(source), "-o", str(binary)],
                           capture_output=True, text=True)
    assert built.returncode == 0, built.stderr[-3000:]
    return binary


def test_the_shared_corpus_is_available():
    if not _cases():
        # The corpus is owned by the integration repository. A vendored copy with a checksum
        # replaces this skip when the firmware negotiator starts to change.
        pytest.skip(f"no ESP cases under {CORPUS}; set VOIP_MEDIA_CORPUS")


@pytest.mark.parametrize("case,device", _cases())
def test_firmware_selects_what_the_device_announced(negotiator, case, device):
    spec = json.loads((case / "case.json").read_text())
    step = spec["steps"][0]
    arguments = [str(case / step["local_offer"]), device]
    ran = subprocess.run([str(negotiator), *arguments], capture_output=True, text=True, timeout=30)
    assert ran.returncode == 0, ran.stderr[-2000:]
    selected = json.loads(ran.stdout.strip().splitlines()[-1])
    assert selected["outcome"] == "accept", selected
    announced = _announced((case / step["sdp"]).read_text())
    assert {key: selected[key] for key in announced} == announced, (selected, announced)
