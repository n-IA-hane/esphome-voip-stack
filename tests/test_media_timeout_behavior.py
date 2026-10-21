"""Exercise the real call timeout handler with receive and transmit-only roles."""

from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("speaker_support", [False, True])
def test_media_timeout_requires_a_receive_device(tmp_path, speaker_support):
    source = (ROOT / "esphome/components/voip_stack/voip_stack.cpp").read_text()
    method = source[
        source.index("void VoipStack::handle_call_timeouts_(") : source.index(
            "\nvoid VoipStack::loop()"
        )
    ]
    header = (ROOT / "esphome/components/voip_stack/voip_stack.h").read_text()
    capability = header[
        header.index("  bool has_speaker_() const {") : header.index(
            "  const char *audio_capability_() const {"
        )
    ]
    harness = (
        r"""
#include "esphome/components/voip_stack/voip_fsm.h"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <string>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
using namespace esphome::voip_stack;
struct Snapshot { uint32_t rtp_rx_packets{0}; unsigned last_sip_status_code{0}; };
struct Transport {
  Snapshot value;
  mutable unsigned snapshot_reads{0};
  Snapshot snapshot() const { ++snapshot_reads; return value; }
  uint32_t received_audio_packets() const { return value.rtp_rx_packets; }
  unsigned last_sip_status_code() const { return value.last_sip_status_code; }
};
class VoipStack {
 public:
  std::atomic<CallState> call_state_{CallState::IN_CALL};
  Transport backing;
  Transport *transport_{&backing};
  void *speaker_{nullptr};
  uint32_t ringing_timeout_ms_{0}, ringing_start_time_{0}, calling_start_time_{0};
  static constexpr uint32_t INVITE_NO_RESPONSE_TIMEOUT_MS=5000, MEDIA_TIMEOUT_MS=15000;
  std::atomic<uint32_t> media_timeout_rtp_rx_packets_{0}, last_peer_audio_ms_{100};
  std::atomic<bool> first_audio_received_{false};
  unsigned terminations{0}, dial_timeouts{0};
  std::string get_current_call_id_() const { return "issue-114"; }
  void fire_timeout_decline_() { ++dial_timeouts; }
  void fire_unanswered_invite_timeout_() { ++dial_timeouts; }
  void request_call_termination_(const TerminationIntent &intent) {
    assert(intent.reason == CallEndReason::MEDIA_TIMEOUT);
    assert(intent.sip_action == SipTerminationAction::BYE);
    ++terminations;
    call_state_=CallState::TERMINATING;
  }
  void handle_call_timeouts_(uint32_t, uint32_t);
"""
        + capability
        + r"""
};
"""
    )
    checks = r"""
int main() {
  VoipStack mic;
  for (uint32_t now=100; now<=60100; now+=100)
    mic.handle_call_timeouts_(now, 0);
  assert(mic.terminations == 0);
  assert(mic.call_state_ == CallState::IN_CALL);
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
  // Speaker-only and duplex share the same receive watchdog.
  VoipStack receiver;
  receiver.speaker_=&receiver;
  receiver.handle_call_timeouts_(15099, 0);
  assert(receiver.terminations == 0);
  receiver.handle_call_timeouts_(15100, 0);
  assert(receiver.terminations == 1);

  VoipStack flowing;
  flowing.speaker_=&flowing;
  for (uint32_t now=100; now<=60100; now+=10) {
    ++flowing.backing.value.rtp_rx_packets;
    flowing.handle_call_timeouts_(now, 0);
  }
  assert(flowing.terminations == 0);
  flowing.handle_call_timeouts_(75099, 0);
  assert(flowing.terminations == 0);
  flowing.handle_call_timeouts_(75100, 0);
  assert(flowing.terminations == 1);
  // The receive watchdog must not acquire the diagnostic snapshot's dialog
  // mutex on every loop iteration. Its authoritative counters suffice.
  assert(flowing.backing.snapshot_reads == 0);

  VoipStack wrapped;
  wrapped.speaker_=&wrapped;
  wrapped.last_peer_audio_ms_=UINT32_MAX-999;
  wrapped.handle_call_timeouts_(13999, 0);
  assert(wrapped.terminations == 0);
  wrapped.handle_call_timeouts_(14000, 0);
  assert(wrapped.terminations == 1);
#endif
  // No speaker must not disable outgoing SIP setup timeouts.
  VoipStack unanswered;
  unanswered.call_state_=CallState::CALLING;
  unanswered.handle_call_timeouts_(5000, 0);
  assert(unanswered.dial_timeouts == 1);
  assert(unanswered.backing.snapshot_reads == 0);

  VoipStack provisional;
  provisional.call_state_=CallState::CALLING;
  provisional.backing.value.last_sip_status_code=180;
  provisional.handle_call_timeouts_(5000, 0);
  assert(provisional.dial_timeouts == 0);
  provisional.handle_call_timeouts_(10000, 10000);
  assert(provisional.dial_timeouts == 1);
  assert(provisional.backing.snapshot_reads == 0);
}
"""
    cpp = tmp_path / "media_timeout.cpp"
    cpp.write_text(harness + method + checks)
    binary = tmp_path / "media_timeout"
    args = ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT)]
    if speaker_support:
        args.append("-DUSE_ESPHOME_VOIP_STACK_SPEAKER")
    subprocess.run([*args, str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_sip_health_reads_use_live_atomics_and_preserve_snapshot_only_backends(tmp_path):
    """Compile actual accessors, including compatibility with older transports."""
    component = ROOT / "esphome/components/voip_stack"
    transport = (component / "transport.h").read_text()
    sip = (component / "sip_transport.h").read_text()
    base_methods = []
    methods = []
    for kind, name in (("uint32_t", "received_audio_packets"), ("uint16_t", "last_sip_status_code")):
        start = transport.index(f"  virtual {kind} {name}() const")
        base_methods.append(transport[start:transport.index("\n", start)])
        start = sip.index(f"  {kind} {name}() const override")
        methods.append(sip[start:sip.index("\n  }", start) + len("\n  }")])
    cpp = tmp_path / "health_accessors.cpp"
    cpp.write_text(r'''
#include <atomic>
#include <cassert>
#include <cstdint>
struct SipTransportSnapshot { uint32_t rtp_rx_packets=0; uint16_t last_sip_status_code=0; };
struct Transport {
  SipTransportSnapshot value;
  mutable unsigned snapshots=0;
  virtual ~Transport()=default;
  virtual SipTransportSnapshot snapshot() const { ++snapshots; return value; }
''' + "\n".join(base_methods) + r'''
};
struct SipTransport : Transport {
  std::atomic<uint32_t> rtp_rx_packets_{0};
  std::atomic<uint16_t> last_sip_status_code_{0};
''' + "\n".join(methods) + r'''
};
int main() {
  Transport legacy;
  legacy.value.rtp_rx_packets=17; legacy.value.last_sip_status_code=183;
  assert(legacy.received_audio_packets()==17 && legacy.last_sip_status_code()==183);
  assert(legacy.snapshots==2);
  SipTransport sip;
  Transport &phone=sip;
  sip.rtp_rx_packets_=UINT32_MAX; sip.last_sip_status_code_=200;
  assert(phone.received_audio_packets()==UINT32_MAX && phone.last_sip_status_code()==200);
  sip.rtp_rx_packets_=0; sip.last_sip_status_code_=0;
  assert(phone.received_audio_packets()==0 && phone.last_sip_status_code()==0);
  assert(sip.snapshots==0);
}
''')
    exe = tmp_path / "health_accessors"
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)],
                   check=True, capture_output=True, text=True)
    subprocess.run([str(exe)], check=True, capture_output=True, text=True)
