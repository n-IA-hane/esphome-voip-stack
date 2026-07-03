#pragma once

#ifdef USE_ESP32

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "esphome/core/helpers.h"

namespace esphome {
namespace voip_stack {

class RtpJitterBuffer {
 public:
  enum class ReadResult : uint8_t {
    FRAME,
    BUFFERING,
    MISSING,
  };

  struct Frame {
    const uint8_t *pcm{nullptr};
    size_t bytes{0};
    uint16_t sequence{0};
    uint32_t timestamp{0};
    bool has_metadata{false};
  };

  struct Counters {
    uint32_t depth{0};
    uint32_t drops{0};
    uint32_t late{0};
    uint32_t missing{0};
    uint32_t duplicates{0};
  };

  RtpJitterBuffer(uint8_t *storage, size_t frame_capacity, uint8_t slots, uint8_t prebuffer);

  void reset();
  bool push(const Frame &frame);
  ReadResult read(uint8_t *out, size_t expected_bytes, uint16_t *sequence = nullptr,
                  uint32_t *timestamp = nullptr, bool *has_metadata = nullptr);
  Counters counters() const;
  uint32_t depth() const;

 protected:
  struct Slot {
    bool valid{false};
    uint16_t sequence{0};
    uint32_t timestamp{0};
    size_t bytes{0};
    uint8_t *pcm{nullptr};
  };

  static int16_t sequence_delta_(uint16_t sequence, uint16_t reference) {
    return static_cast<int16_t>(sequence - reference);
  }
  void clear_slots_();
  void keep_window_from_(uint16_t next_sequence);

  uint8_t *storage_{nullptr};
  size_t frame_capacity_{0};
  uint8_t slot_count_{0};
  uint8_t prebuffer_{0};
  static constexpr uint8_t MAX_SLOTS = 16;
  Slot slots_[MAX_SLOTS]{};
  mutable Mutex mutex_;
  uint32_t valid_count_{0};
  bool buffering_{true};
  bool next_sequence_valid_{false};
  uint16_t next_sequence_{0};
  Counters counters_{};
};

inline RtpJitterBuffer::RtpJitterBuffer(uint8_t *storage, size_t frame_capacity, uint8_t slots, uint8_t prebuffer)
    : storage_(storage), frame_capacity_(frame_capacity),
      slot_count_(slots > MAX_SLOTS ? MAX_SLOTS : slots), prebuffer_(prebuffer) {
  if (this->storage_ == nullptr || this->frame_capacity_ == 0 || this->slot_count_ == 0) {
    return;
  }
  for (uint8_t i = 0; i < this->slot_count_; i++) {
    this->slots_[i].pcm = this->storage_ + (static_cast<size_t>(i) * this->frame_capacity_);
  }
}

inline void RtpJitterBuffer::clear_slots_() {
  for (uint8_t i = 0; i < this->slot_count_; i++) {
    this->slots_[i].valid = false;
    this->slots_[i].bytes = 0;
  }
}

inline void RtpJitterBuffer::keep_window_from_(uint16_t next_sequence) {
  uint32_t count = 0;
  for (uint8_t i = 0; i < this->slot_count_; i++) {
    Slot &slot = this->slots_[i];
    if (!slot.valid)
      continue;
    const int16_t delta = sequence_delta_(slot.sequence, next_sequence);
    if (delta < 0 || delta >= static_cast<int16_t>(this->slot_count_)) {
      slot.valid = false;
      slot.bytes = 0;
      continue;
    }
    count++;
  }
  this->valid_count_ = count;
}

inline void RtpJitterBuffer::reset() {
  LockGuard lock(this->mutex_);
  this->clear_slots_();
  this->valid_count_ = 0;
  this->buffering_ = true;
  this->next_sequence_valid_ = false;
  this->next_sequence_ = 0;
  this->counters_ = {};
}

inline bool RtpJitterBuffer::push(const Frame &frame) {
  if (this->slot_count_ == 0 || frame.pcm == nullptr || frame.bytes == 0 || frame.bytes > this->frame_capacity_) {
    return false;
  }

  LockGuard lock(this->mutex_);
  uint16_t sequence = frame.sequence;
  if (!frame.has_metadata) {
    sequence = this->next_sequence_valid_ ? static_cast<uint16_t>(this->next_sequence_ + this->valid_count_) : 0;
  }

  if (!this->next_sequence_valid_) {
    this->next_sequence_ = sequence;
    this->next_sequence_valid_ = true;
  }

  if (!this->buffering_) {
    const int16_t delta = sequence_delta_(sequence, this->next_sequence_);
    if (delta < 0) {
      this->counters_.late++;
      return false;
    }
    if (delta >= static_cast<int16_t>(this->slot_count_)) {
      const uint8_t keep_frames = this->slot_count_ > this->prebuffer_ ? this->slot_count_ - this->prebuffer_ : 1;
      this->next_sequence_ = static_cast<uint16_t>(sequence - keep_frames + 1);
      this->keep_window_from_(this->next_sequence_);
      this->buffering_ = this->valid_count_ < this->prebuffer_;
      this->counters_.drops++;
    }
  }

  Slot &slot = this->slots_[sequence % this->slot_count_];
  if (slot.valid) {
    if (slot.sequence == sequence) {
      this->counters_.duplicates++;
      return false;
    }
    slot.valid = false;
    slot.bytes = 0;
    if (this->valid_count_ > 0) this->valid_count_--;
    this->counters_.drops++;
  }

  memcpy(slot.pcm, frame.pcm, frame.bytes);
  slot.valid = true;
  slot.sequence = sequence;
  slot.timestamp = frame.timestamp;
  slot.bytes = frame.bytes;
  this->valid_count_++;

  if (this->buffering_ && this->valid_count_ >= this->prebuffer_) {
    this->buffering_ = false;
  }
  return true;
}

inline RtpJitterBuffer::ReadResult RtpJitterBuffer::read(uint8_t *out, size_t expected_bytes, uint16_t *sequence,
                                                         uint32_t *timestamp, bool *has_metadata) {
  if (this->slot_count_ == 0 || out == nullptr || expected_bytes == 0 || expected_bytes > this->frame_capacity_) {
    return ReadResult::BUFFERING;
  }

  LockGuard lock(this->mutex_);
  if (!this->next_sequence_valid_ || this->buffering_) {
    if (this->next_sequence_valid_ && this->valid_count_ >= this->prebuffer_) {
      this->buffering_ = false;
    } else {
      return ReadResult::BUFFERING;
    }
  }

  Slot &slot = this->slots_[this->next_sequence_ % this->slot_count_];
  if (slot.valid && slot.sequence == this->next_sequence_) {
    if (sequence != nullptr) *sequence = slot.sequence;
    if (timestamp != nullptr) *timestamp = slot.timestamp;
    if (has_metadata != nullptr) *has_metadata = true;
    memcpy(out, slot.pcm, slot.bytes);
    slot.valid = false;
    slot.bytes = 0;
    if (this->valid_count_ > 0) this->valid_count_--;
    this->next_sequence_ = static_cast<uint16_t>(this->next_sequence_ + 1);
    return ReadResult::FRAME;
  }

  if (this->valid_count_ == 0) {
    this->buffering_ = true;
    this->next_sequence_valid_ = false;
    return ReadResult::BUFFERING;
  }

  this->counters_.missing++;
  this->next_sequence_ = static_cast<uint16_t>(this->next_sequence_ + 1);
  return ReadResult::MISSING;
}

inline RtpJitterBuffer::Counters RtpJitterBuffer::counters() const {
  LockGuard lock(this->mutex_);
  Counters out = this->counters_;
  out.depth = this->valid_count_;
  return out;
}

inline uint32_t RtpJitterBuffer::depth() const {
  LockGuard lock(this->mutex_);
  return this->valid_count_;
}

}  // namespace voip_stack
}  // namespace esphome

#endif  // USE_ESP32
