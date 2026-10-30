#include "esphome/components/voip_stack/rtp_jitter_buffer.h"
#include "esphome/components/voip_stack/sip_types.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <random>
#include <string>

using namespace esphome::voip_stack;
using Result = RtpJitterBuffer::ReadResult;
struct Queue {
  std::array<uint8_t, 16 * 4096> storage{};
  std::array<uint8_t, 4096> input{}, output{};
  RtpJitterBuffer queue;
  uint16_t sequence{};
  uint32_t timestamp{};
  bool metadata{}, changed{};
  size_t bytes{};
  explicit Queue(uint8_t prebuffer = 1)
      : queue(storage.data(), 4096, 16, prebuffer) {}
  void push(uint16_t seq, uint32_t ts, AudioFormat format = {},
            bool has_metadata = true, bool source_changed = false,
            size_t length = SIZE_MAX, bool unknown_format = false) {
    input.fill(uint8_t(seq));
    if (length == SIZE_MAX)
      length =
          format.codec == AudioCodec::OPUS ? 13 : format.nominal_frame_bytes();
    RtpJitterBuffer::Frame frame;
    frame.pcm = input.data();
    frame.bytes = length;
    frame.sequence = seq;
    frame.timestamp = ts;
    frame.has_metadata = has_metadata;
    frame.source_changed = source_changed;
    frame.format_packed = unknown_format ? 0 : pack_audio_format(format);
    assert(queue.push(frame));
  }
  Result read() {
    return queue.read(output.data(), output.size(), &sequence, &timestamp,
                      &metadata, &bytes, &changed);
  }
  void frame(uint16_t seq) {
    assert(read() == Result::FRAME);
    assert(sequence == seq && output[0] == uint8_t(seq));
  }
};
int main(int argc, char **argv) {
  assert(argc == 2);
  const std::string scenario = argv[1];
  if (scenario == "interleave" || scenario == "lost-event") {
    for (unsigned gap : {1U, 3U, 7U}) {
      Queue q;
      q.push(100, 0);
      q.frame(100);
      assert(q.read() == Result::BUFFERING);
      q.push(uint16_t(101 + gap), 256);
      q.frame(uint16_t(101 + gap));
      assert(q.queue.counters().missing == 0);
    }
  } else if (scenario == "real-loss") {
    Queue q;
    q.push(100, 0);
    q.frame(100);
    q.push(102, 512);
    assert(q.read() == Result::MISSING);
    q.frame(102);
    assert(q.queue.counters().missing == 1);
  } else if (scenario == "mixed-loss") {
    Queue q;
    q.push(100, 0);
    q.frame(100);
    q.push(105, 768);
    assert(q.read() == Result::MISSING);
    assert(q.read() == Result::MISSING);
    q.frame(105);
    assert(q.queue.counters().missing == 2);
  } else if (scenario == "reorder") {
    Queue q(3);
    q.push(100, 256);
    q.push(102, 512);
    q.push(99, 0);
    q.frame(99);
    q.frame(100);
    q.frame(102);
    assert(q.queue.counters().missing == 0 && q.queue.counters().late == 0);
  } else if (scenario == "sequence-wrap") {
    Queue q;
    q.push(65534, 0);
    q.frame(65534);
    q.push(1, 256);
    q.frame(1);
    assert(q.queue.counters().missing == 0);
  } else if (scenario == "timestamp-wrap") {
    Queue q;
    q.push(100, UINT32_MAX - 255);
    q.frame(100);
    q.push(102, 0);
    q.frame(102);
    assert(q.queue.counters().missing == 0);
  } else if (scenario == "format-change") {
    Queue q;
    AudioFormat next;
    next.frame_ms = 10;
    q.push(100, 0);
    q.frame(100);
    q.push(102, 256, next);
    q.frame(102);
    q.push(104, 416, next);
    q.frame(104);
    assert(q.queue.counters().missing == 0);
  } else if (scenario == "opus-clock") {
    Queue q;
    AudioFormat opus;
    opus.codec = AudioCodec::OPUS;
    opus.frame_ms = 20;
    opus.sample_rate = 16000;
    q.push(100, 0, opus);
    q.frame(100);
    q.push(103, 960, opus);
    q.frame(103);
    assert(q.queue.counters().missing == 0);
  } else if (scenario == "reset") {
    Queue q;
    q.push(100, 0);
    q.frame(100);
    q.queue.reset();
    q.push(300, 9000);
    q.frame(300);
    q.push(302, 9512);
    assert(q.read() == Result::MISSING);
    q.frame(302);
    assert(q.queue.counters().missing == 1);
  } else if (scenario == "realign") {
    Queue q;
    q.push(100, 0);
    q.frame(100);
    q.push(200, 9000);
    q.frame(200);
    q.push(202, 9512);
    assert(q.read() == Result::MISSING);
    q.frame(202);
    assert(q.queue.counters().drops == 1 && q.queue.counters().missing == 1);
  } else if (scenario == "unknown-format") {
    Queue q;
    q.push(100, 0, {}, true, false, SIZE_MAX, true);
    q.frame(100);
    q.push(102, 256, {}, true, false, SIZE_MAX, true);
    assert(q.read() == Result::MISSING);
    q.frame(102);
    assert(q.queue.counters().missing == 1);
  } else if (scenario == "no-metadata") {
    Queue q;
    q.push(100, 0, {}, false);
    assert(q.read() == Result::FRAME && q.sequence == 0 && q.output[0] == 100);
    q.push(102, 256, {}, false);
    assert(q.read() == Result::FRAME && q.sequence == 1);
    assert(q.queue.counters().missing == 0);
  } else if (scenario == "nearest-future") {
    Queue q;
    q.push(100, 0);
    q.frame(100);
    q.push(102, 512);
    q.push(103, 256);
    assert(q.read() == Result::MISSING);
    q.frame(102);
    q.frame(103);
    assert(q.queue.counters().missing == 1);
  } else if (scenario == "invalid-size") {
    Queue q;
    q.push(100, 0);
    q.frame(100);
    q.push(101, 256, {}, true, false, 256);
    q.push(103, 512);
    assert(q.queue.read(q.output.data(), 512) == Result::MISSING);
    q.frame(103);
    assert(q.queue.counters().drops == 1 &&
           q.queue.counters().discarded_frames == 1);
  } else if (scenario == "source-change") {
    Queue q;
    q.push(100, 0);
    q.frame(100);
    q.push(102, 256, {}, true, true);
    assert(q.read() == Result::MISSING);
    q.frame(102);
    assert(q.changed);
  } else if (scenario == "random-mixed") {
    std::mt19937 random(731);
    Queue q;
    uint16_t seq = 65530;
    uint32_t ts = UINT32_MAX - 255;
    uint32_t missing = 0;
    q.push(seq, ts);
    q.frame(seq);
    for (unsigned round = 0; round < 1024; ++round) {
      unsigned steps = 1 + random() % 3;
      for (unsigned n = 0; n < steps; ++n) {
        seq = uint16_t(seq + 1 + random() % 3);
        ts += 256;
      }
      q.push(seq, ts);
      for (unsigned lost = 1; lost < steps; ++lost) {
        assert(q.read() == Result::MISSING);
        ++missing;
      }
      q.frame(seq);
      assert(q.queue.counters().missing == missing);
      assert(q.read() == Result::BUFFERING);
    }
  } else
    assert(false);
}
