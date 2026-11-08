#include "esphome/components/voip_stack/rtp_jitter_buffer.h"

#include <array>
#include <cassert>
#include <cstdlib>
#include <new>

using namespace esphome::voip_stack;
using Result = RtpJitterBuffer::ReadResult;

bool forbid_allocation = false;
void *operator new(size_t bytes) {
  assert(!forbid_allocation);
  if (void *value = std::malloc(bytes)) return value;
  throw std::bad_alloc();
}
void *operator new[](size_t bytes) { return ::operator new(bytes); }
void operator delete(void *value) noexcept { std::free(value); }
void operator delete[](void *value) noexcept { std::free(value); }
void operator delete(void *value, size_t) noexcept { std::free(value); }
void operator delete[](void *value, size_t) noexcept { std::free(value); }

class InspectableJitter : public RtpJitterBuffer {
 public:
  using RtpJitterBuffer::RtpJitterBuffer;

  void assert_blocks(uint8_t *outside, uint8_t *original_outside) const {
    for (uint8_t i = 0; i <= this->slot_count_; ++i) {
      const uint8_t *block = i == this->slot_count_ ? outside : this->slots_[i].pcm;
      bool owned = block == original_outside;
      for (uint8_t j = 0; j < this->slot_count_; ++j)
        owned |= block == this->storage_ + j * this->frame_capacity_;
      assert(owned);
      for (uint8_t j = 0; j < i; ++j) assert(block != this->slots_[j].pcm);
    }
  }
};

void smaller_spare_preserves_copy_contract() {
  constexpr size_t capacity = 64;
  alignas(16) std::array<uint8_t, 4 * capacity> storage{};
  alignas(16) std::array<uint8_t, 32> spare{};
  std::array<uint8_t, 13> packet{};
  InspectableJitter jitter(storage.data(), capacity, 4, 1);
  uint8_t *output = spare.data();
  const AudioFormat format{16000, PcmFormat::S16LE, 1, 20, AudioCodec::OPUS};
  const uint32_t packed = pack_audio_format(format);
  assert(jitter.has_exchange_alignment(output));
  forbid_allocation = true;
  for (unsigned index = 0; index < 12; ++index) {
    for (size_t i = 0; i < packet.size(); ++i) packet[i] = uint8_t(index * 17 + i);
    RtpJitterBuffer::Frame frame;
    frame.pcm = packet.data();
    frame.bytes = packet.size();
    frame.sequence = uint16_t(65530 + index);
    frame.timestamp = 960 * index;
    frame.has_metadata = true;
    frame.source_changed = index == 0;
    frame.format_packed = packed;
    assert(jitter.push(frame));
    uint16_t sequence = 0;
    uint32_t timestamp = 0, actual_format = 0;
    bool metadata = false, changed = false;
    size_t bytes = 0;
    assert(jitter.read_exchange(output, spare.size(), &sequence, &timestamp, &metadata,
                                &bytes, &changed, &actual_format) == Result::FRAME);
    assert(output == spare.data());  // Smaller storage must never enter the slot pool.
    assert(sequence == frame.sequence && timestamp == frame.timestamp && metadata);
    assert(changed == frame.source_changed && actual_format == packed && bytes == packet.size());
    assert(std::memcmp(output, packet.data(), bytes) == 0);
    jitter.assert_blocks(output, spare.data());
    assert(jitter.depth() == 0);
  }
  const auto counters = jitter.counters();
  assert(!counters.drops && !counters.discarded_frames && !counters.missing &&
         !counters.late && !counters.duplicates);
  forbid_allocation = false;
}

void copy_fallback_preserves_invalid_capacity_and_oversize_results() {
  constexpr size_t capacity = 64;
  alignas(16) std::array<uint8_t, 4 * capacity> storage{}, reference_storage{};
  alignas(16) std::array<uint8_t, 8> spare{}, reference_output{};
  spare.fill(0xa5);
  reference_output.fill(0xa5);
  std::array<uint8_t, 13> packet{};
  InspectableJitter jitter(storage.data(), capacity, 4, 1);
  RtpJitterBuffer reference(reference_storage.data(), capacity, 4, 1);
  uint8_t *output = spare.data();
  RtpJitterBuffer::Frame frame;
  frame.pcm = packet.data();
  frame.bytes = packet.size();
  frame.sequence = 77;
  frame.timestamp = 1234;
  frame.has_metadata = true;
  frame.source_changed = true;
  frame.format_packed = 123;
  forbid_allocation = true;
  assert(jitter.push(frame) && reference.push(frame));
  uint16_t sequence = 0xbeef;
  uint32_t timestamp = 0x12345678, format = 0xcafebabe;
  bool metadata = false, changed = false;
  size_t bytes = 999;
  for (size_t invalid : {size_t{0}, capacity + 1}) {
    assert(jitter.read_exchange(output, invalid, &sequence, &timestamp, &metadata,
                                &bytes, &changed, &format) == Result::BUFFERING);
    assert(output == spare.data() && jitter.depth() == 1);
  }
  // read() consumes an oversize frame as MISSING; preserve that old contract,
  // counters and untouched outputs instead of inventing a new buffering rule.
  assert(reference.read(reference_output.data(), reference_output.size()) == Result::MISSING);
  assert(jitter.read_exchange(output, spare.size(), &sequence, &timestamp, &metadata,
                              &bytes, &changed, &format) == Result::MISSING);
  assert(output == spare.data() && spare == reference_output);
  assert(sequence == 0xbeef && timestamp == 0x12345678 && !metadata && !changed &&
         bytes == 999 && format == 0xcafebabe);
  const auto actual = jitter.counters(), expected = reference.counters();
  assert(actual.depth == expected.depth && actual.depth == 0);
  assert(actual.drops == expected.drops && actual.drops == 1);
  assert(actual.discarded_frames == expected.discarded_frames && actual.discarded_frames == 1);
  assert(!actual.missing && !actual.late && !actual.duplicates);
  jitter.assert_blocks(output, spare.data());
  forbid_allocation = false;
}

void rotation_and_reset() {
  constexpr size_t capacity = 64;
  alignas(16) std::array<uint8_t, 4 * capacity> storage{};
  alignas(16) std::array<uint8_t, capacity> original_output{};
  std::array<uint8_t, capacity> packet{};
  InspectableJitter jitter(storage.data(), capacity, 4, 1);
  uint8_t *output = original_output.data();
  assert(jitter.has_exchange_alignment(output));
  assert(!jitter.has_exchange_alignment(nullptr));
  InspectableJitter no_storage(nullptr, capacity, 4, 1);
  assert(!no_storage.has_exchange_alignment(output));
  AudioFormat format{16000, PcmFormat::S16LE, 1, 20, AudioCodec::OPUS};
  const uint32_t packed = pack_audio_format(format);

  forbid_allocation = true;
  for (unsigned round = 0; round < 3; ++round) {
    jitter.reset();
    jitter.assert_blocks(output, original_output.data());
    uint8_t *before = output;
    assert(jitter.read_exchange(output, capacity) == Result::BUFFERING && output == before);
    for (unsigned n = 0; n < 12; ++n) {
      const uint16_t sequence = static_cast<uint16_t>(65530 + n);
      const size_t length = n % 3 == 0 ? 13 : n % 3 == 1 ? 32 : capacity;
      for (size_t i = 0; i < length; ++i) packet[i] = static_cast<uint8_t>(sequence + i);
      RtpJitterBuffer::Frame frame;
      frame.pcm = packet.data();
      frame.bytes = length;
      frame.sequence = sequence;
      frame.timestamp = n * 960;
      frame.has_metadata = true;
      frame.source_changed = n == 0;
      frame.format_packed = packed;
      assert(jitter.push(frame));
      before = output;
      if (n == 0) {
        assert(jitter.read_exchange(output, 0) == Result::BUFFERING);
        assert(jitter.read_exchange(output, capacity + 1) == Result::BUFFERING);
        assert(output == before && jitter.depth() == 1);
      }
      uint16_t actual_sequence = 0;
      uint32_t timestamp = 0, actual_format = 0;
      bool metadata = false, changed = false;
      size_t bytes = 0;
      assert(jitter.read_exchange(output, capacity, &actual_sequence, &timestamp,
                                  &metadata, &bytes, &changed, &actual_format) == Result::FRAME);
      assert(output != before && actual_sequence == sequence && timestamp == n * 960);
      assert(metadata && changed == (n == 0) && actual_format == packed && bytes == length);
      for (size_t i = 0; i < bytes; ++i) assert(output[i] == static_cast<uint8_t>(sequence + i));
      jitter.assert_blocks(output, original_output.data());
      // Decode/conversion may overwrite the entire borrowed block before it
      // returns to a slot, even when its compressed payload was shorter.
      std::memset(output, 0xee, capacity);
    }
  }
  jitter.reset();
  RtpJitterBuffer::Frame frame;
  frame.pcm = packet.data();
  frame.bytes = capacity;
  frame.sequence = 100;
  frame.timestamp = 0;
  frame.has_metadata = true;
  frame.format_packed = packed;
  assert(jitter.push(frame));
  assert(jitter.read_exchange(output, capacity) == Result::FRAME);
  frame.sequence = 102;
  frame.timestamp = 1920;
  assert(jitter.push(frame));
  uint8_t *before = output;
  assert(jitter.read_exchange(output, capacity) == Result::MISSING && output == before);
  assert(jitter.read_exchange(output, capacity) == Result::FRAME);
  jitter.assert_blocks(output, original_output.data());
  forbid_allocation = false;
}

template<size_t Capacity> void alignment_case(size_t storage_offset, size_t output_offset, bool exchange) {
  alignas(16) std::array<uint8_t, 4 * Capacity + 16> storage{};
  alignas(16) std::array<uint8_t, Capacity + 16> original_output{};
  std::array<uint8_t, Capacity> packet{};
  InspectableJitter jitter(storage.data() + storage_offset, Capacity, 4, 1);
  uint8_t *output = original_output.data() + output_offset;
  uint8_t *const original = output;
  assert(jitter.has_exchange_alignment(output) == exchange);
  forbid_allocation = true;
  for (uint16_t sequence = 0; sequence < 12; ++sequence) {
    const size_t length = sequence % 2 == 0 ? 13 : Capacity;
    for (size_t i = 0; i < length; ++i) packet[i] = static_cast<uint8_t>(sequence + i);
    RtpJitterBuffer::Frame frame;
    frame.pcm = packet.data();
    frame.bytes = length;
    frame.sequence = sequence;
    frame.has_metadata = true;
    assert(jitter.push(frame));
    size_t bytes = 0;
    uint8_t *before = output;
    assert(jitter.read_exchange(output, Capacity, nullptr, nullptr, nullptr, &bytes) == Result::FRAME);
    assert((output != before) == exchange && bytes == length);
    assert(jitter.has_exchange_alignment(output) == exchange);
    for (size_t i = 0; i < bytes; ++i) assert(output[i] == static_cast<uint8_t>(sequence + i));
    jitter.assert_blocks(output, original);
    if (sequence == 5) jitter.reset();
  }
  forbid_allocation = false;
}

int main() {
  smaller_spare_preserves_copy_contract();
  copy_fallback_preserves_invalid_capacity_and_oversize_results();
  rotation_and_reset();
  alignment_case<1275>(0, 0, false);
  alignment_case<1764>(0, 0, true);
  alignment_case<64>(4, 12, true);
  alignment_case<68>(4, 8, true);
  alignment_case<64>(1, 0, false);
  alignment_case<64>(0, 1, false);
  alignment_case<64>(2, 0, false);
  alignment_case<64>(0, 2, false);
}
