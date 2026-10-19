"""Distinguish overflow decisions from actual buffered frames discarded."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_discard_accounting_counts_frames_not_window_moves(tmp_path):
    helpers = tmp_path / 'esphome/core/helpers.h'
    helpers.parent.mkdir(parents=True)
    helpers.write_text('namespace esphome { struct Mutex {}; struct LockGuard { explicit LockGuard(Mutex &) {} }; }\n')
    source = tmp_path / 'discard.cpp'
    source.write_text(r'''
#include <cassert>
#include "esphome/components/voip_stack/rtp_jitter_buffer.h"
using esphome::voip_stack::RtpJitterBuffer;
int main() {
  uint8_t storage[64]{}, pcm[4]{}, out[4]{};
  RtpJitterBuffer queue(storage, 4, 16, 4);
  auto push = [&](uint16_t sequence) {
    RtpJitterBuffer::Frame frame;
    frame.pcm = pcm; frame.bytes = 4; frame.has_metadata = true;
    frame.sequence = sequence;
    assert(queue.push(frame));
  };
  for (uint16_t seq = 0; seq < 16; ++seq) push(seq);
  push(16);
  assert(queue.counters().drops == 1);
  assert(queue.counters().discarded_frames == 5);
  assert(queue.depth() == 12);
  for (uint16_t expected = 5; expected <= 16; ++expected) {
    uint16_t sequence;
    assert(queue.read(out, 4, &sequence) == RtpJitterBuffer::ReadResult::FRAME);
    assert(sequence == expected);
  }
  queue.reset();
  assert(queue.counters().drops == 0 && queue.counters().discarded_frames == 0);
  push(100); push(102); push(200);
  assert(queue.counters().drops == 1 && queue.counters().discarded_frames == 2);
  push(201); push(202); push(203);
  assert(queue.read(out, 2) == RtpJitterBuffer::ReadResult::MISSING);
  assert(queue.counters().drops == 2 && queue.counters().discarded_frames == 3);
  queue.reset();
  for (uint16_t seq = 65530; seq != 10; ++seq) push(seq);
  push(10);
  assert(queue.counters().drops == 1 && queue.counters().discarded_frames == 5);
}
''')
    executable = tmp_path / 'discard'
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-DUSE_ESP32',
                    '-I', str(tmp_path), '-I', str(ROOT), str(source), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
