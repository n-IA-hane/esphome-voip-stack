#include "ring_buffer.h"

#ifdef USE_ESP32

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <cstring>

namespace esphome::ring_buffer {

static const char *const TAG = "ring_buffer";

RingBuffer::~RingBuffer() {
  if (this->handle_ != nullptr) {
    vRingbufferDelete(this->handle_);
    RAMAllocator<uint8_t> allocator;
    allocator.deallocate(this->storage_, this->size_);
  }
}

std::unique_ptr<RingBuffer> RingBuffer::create(size_t len, MemoryPreference preference) {
  std::unique_ptr<RingBuffer> rb = make_unique<RingBuffer>();

  rb->size_ = len;

  const uint8_t type = (preference == MemoryPreference::INTERNAL_FIRST) ? RAMAllocator<uint8_t>::PREFER_INTERNAL
                                                                        : RAMAllocator<uint8_t>::NONE;

  RAMAllocator<uint8_t> allocator(type);
  rb->storage_ = allocator.allocate(rb->size_);
  if (rb->storage_ == nullptr) {
    return nullptr;
  }

  rb->handle_ = xRingbufferCreateStatic(rb->size_, RINGBUF_TYPE_BYTEBUF, rb->storage_, &rb->structure_);
  ESP_LOGD(TAG, "Created ring buffer with size %u", len);

  return rb;
}

void *RingBuffer::receive_acquire(size_t &length, size_t max_length, TickType_t ticks_to_wait) {
  length = 0;
  void *buffer_data = xRingbufferReceiveUpTo(this->handle_, &length, ticks_to_wait, max_length);
  return buffer_data;
}

void RingBuffer::receive_release(void *item) { vRingbufferReturnItem(this->handle_, item); }

size_t RingBuffer::read(void *data, size_t len, TickType_t ticks_to_wait) {
  size_t bytes_read = 0;

  void *buffer_data = xRingbufferReceiveUpTo(this->handle_, &bytes_read, ticks_to_wait, len);

  if (buffer_data == nullptr) {
    return 0;
  }

  std::memcpy(data, buffer_data, bytes_read);

  vRingbufferReturnItem(this->handle_, buffer_data);

  if (bytes_read < len) {
    size_t follow_up_bytes_read = 0;
    size_t bytes_remaining = len - bytes_read;

    buffer_data = xRingbufferReceiveUpTo(this->handle_, &follow_up_bytes_read, 0, bytes_remaining);

    if (buffer_data == nullptr) {
      return bytes_read;
    }

    std::memcpy((void *) ((uint8_t *) (data) + bytes_read), buffer_data, follow_up_bytes_read);

    vRingbufferReturnItem(this->handle_, buffer_data);
    bytes_read += follow_up_bytes_read;
  }

  return bytes_read;
}

size_t RingBuffer::write(const void *data, size_t len) {
  size_t free = this->free();
  if (free < len) {
    this->discard_bytes_(len - free);
  }
  return this->write_without_replacement(data, len, 0);
}

size_t RingBuffer::write_without_replacement(const void *data, size_t len, TickType_t ticks_to_wait,
                                             bool write_partial) {
  if (!xRingbufferSend(this->handle_, data, len, ticks_to_wait)) {
    if (!write_partial) {
      return 0;
    }
    size_t free = std::min(this->free(), len);
    if (xRingbufferSend(this->handle_, data, free, 0)) {
      return free;
    }
    return 0;
  }
  return len;
}

size_t RingBuffer::available() const {
  UBaseType_t ux_items_waiting = 0;
  vRingbufferGetInfo(this->handle_, nullptr, nullptr, nullptr, nullptr, &ux_items_waiting);
  return ux_items_waiting;
}

size_t RingBuffer::free() const { return xRingbufferGetCurFreeSize(this->handle_); }

BaseType_t RingBuffer::reset() { return this->discard_bytes_(this->available()); }

bool RingBuffer::discard_bytes_(size_t discard_bytes) {
  size_t bytes_read = 0;

  void *buffer_data = xRingbufferReceiveUpTo(this->handle_, &bytes_read, 0, discard_bytes);
  if (buffer_data != nullptr)
    vRingbufferReturnItem(this->handle_, buffer_data);

  if (bytes_read < discard_bytes) {
    size_t wrapped_bytes_read = 0;
    buffer_data = xRingbufferReceiveUpTo(this->handle_, &wrapped_bytes_read, 0, discard_bytes - bytes_read);
    if (buffer_data != nullptr) {
      vRingbufferReturnItem(this->handle_, buffer_data);
      bytes_read += wrapped_bytes_read;
    }
  }

  return (bytes_read == discard_bytes);
}

}  // namespace esphome::ring_buffer

#else

#include "esphome/core/helpers.h"

#include <algorithm>

namespace esphome::ring_buffer {

RingBuffer::~RingBuffer() = default;

std::unique_ptr<RingBuffer> RingBuffer::create(size_t len, MemoryPreference /*preference*/) {
  std::unique_ptr<RingBuffer> rb = make_unique<RingBuffer>();
  rb->size_ = len;
  rb->storage_.assign(len, 0);
  return rb;
}

size_t RingBuffer::read(void *data, size_t len, TickType_t /*ticks_to_wait*/) {
  const size_t bytes = std::min(len, this->available_);
  auto *out = static_cast<uint8_t *>(data);
  for (size_t i = 0; i < bytes; ++i) {
    out[i] = this->storage_[(this->read_pos_ + i) % this->size_];
  }
  this->read_pos_ = (this->read_pos_ + bytes) % this->size_;
  this->available_ -= bytes;
  return bytes;
}

void *RingBuffer::receive_acquire(size_t &length, size_t max_length, TickType_t /*ticks_to_wait*/) {
  if (this->acquired_active_ || this->available_ == 0 || this->size_ == 0) {
    length = 0;
    return nullptr;
  }
  length = std::min(max_length, this->available_);
  this->acquired_.resize(length);
  for (size_t i = 0; i < length; ++i) {
    this->acquired_[i] = this->storage_[(this->read_pos_ + i) % this->size_];
  }
  this->read_pos_ = (this->read_pos_ + length) % this->size_;
  this->available_ -= length;
  this->acquired_active_ = true;
  return this->acquired_.data();
}

void RingBuffer::receive_release(void *item) {
  if (item == this->acquired_.data()) {
    this->acquired_active_ = false;
  }
}

size_t RingBuffer::write(const void *data, size_t len) {
  const size_t bytes_to_discard = (len > this->free()) ? len - this->free() : 0;
  if (bytes_to_discard > 0) {
    this->discard_bytes_(bytes_to_discard);
  }
  return this->write_without_replacement(data, len, 0);
}

size_t RingBuffer::write_without_replacement(const void *data, size_t len, TickType_t /*ticks_to_wait*/,
                                             bool write_partial) {
  if (this->size_ == 0) {
    return 0;
  }
  const size_t writable = write_partial ? std::min(this->free(), len) : (this->free() >= len ? len : 0);
  const auto *in = static_cast<const uint8_t *>(data);
  const size_t write_pos = (this->read_pos_ + this->available_) % this->size_;
  for (size_t i = 0; i < writable; ++i) {
    this->storage_[(write_pos + i) % this->size_] = in[i];
  }
  this->available_ += writable;
  return writable;
}

size_t RingBuffer::available() const { return this->available_; }

size_t RingBuffer::free() const { return this->size_ - this->available_; }

BaseType_t RingBuffer::reset() {
  this->read_pos_ = 0;
  this->available_ = 0;
  this->acquired_active_ = false;
  return pdPASS;
}

bool RingBuffer::discard_bytes_(size_t discard_bytes) {
  const size_t bytes = std::min(discard_bytes, this->available_);
  if (this->size_ != 0) {
    this->read_pos_ = (this->read_pos_ + bytes) % this->size_;
  }
  this->available_ -= bytes;
  return bytes == discard_bytes;
}

}  // namespace esphome::ring_buffer

#endif
