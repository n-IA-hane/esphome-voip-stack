"""Exercise packet arrival between reads while the downstream sink is buffered."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_steady_packets_do_not_repeat_startup_prefetch(tmp_path):
    helper = tmp_path / 'esphome/core/helpers.h'
    helper.parent.mkdir(parents=True)
    helper.write_text('namespace esphome { struct Mutex {}; struct LockGuard { explicit LockGuard(Mutex &) {} }; }\n')
    source = tmp_path / 'jitter.cpp'
    source.write_text(r'''
#include <cassert>
#include <cstdint>
#include "esphome/components/voip_stack/rtp_jitter_buffer.h"
using esphome::voip_stack::RtpJitterBuffer;
using Result = RtpJitterBuffer::ReadResult;

void run(bool compressed, uint16_t first) {
  uint8_t storage[16 * 4]{};
  uint8_t packet[4]{1, 2, 3, 4}, output[4]{};
  RtpJitterBuffer buffer(storage, 4, 16, 4);
  auto push = [&](uint16_t sequence) {
    RtpJitterBuffer::Frame frame;
    frame.pcm = packet;
    frame.bytes = compressed ? 2 + sequence % 2 : 4;
    frame.sequence = sequence;
    frame.timestamp = sequence * 960u;
    frame.has_metadata = true;
    assert(buffer.push(frame));
  };
  auto read = [&]() {
    size_t bytes = 0;
    return buffer.read(output, sizeof(output), nullptr, nullptr, nullptr,
                       compressed ? &bytes : nullptr);
  };
  for (uint16_t n = 0; n < 3; n++) {
    push(first + n);
    assert(read() == Result::BUFFERING);
  }
  push(first + 3);
  for (int n = 0; n < 4; n++) assert(read() == Result::FRAME);
  // The speaker already owns buffered audio. Empty network storage between
  // consecutive packets must not impose another four-packet startup delay.
  for (uint16_t n = 4; n < 80; n++) {
    assert(read() == Result::BUFFERING);
    push(first + n);
    assert(read() == Result::FRAME);
  }
  auto counters = buffer.counters();
  assert(counters.drops == 0 && counters.missing == 0 && counters.late == 0);
  // A real missing packet is still detected when its successor arrives.
  push(first + 81);
  assert(read() == Result::MISSING);
  assert(read() == Result::FRAME);
  // A new stream/call must still perform its initial prefetch.
  buffer.reset();
  push(100);
  assert(read() == Result::BUFFERING);
}
int main() {
  run(false, 100);
  run(true, 100);
  run(false, 65530);
  run(true, 65530);
}
''')
    executable = tmp_path / 'jitter'
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-DUSE_ESP32', '-I', str(tmp_path), '-I', str(ROOT),
                    str(source), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
