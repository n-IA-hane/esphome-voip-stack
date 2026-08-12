#pragma once

#include "esphome/core/helpers.h"

#include <string>

namespace esphome::voip_stack {

// Persistent SIP messages are control-plane payloads, not realtime media.
// On PSRAM builds keep their retained capacity out of scarce internal RAM.
// The wrapper deliberately exposes only string operations used by the SIP
// transaction store, so ownership cannot leak into parser or media code.
class SipSignalingString {
 public:
#ifdef USE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM
  using Storage = std::basic_string<char, std::char_traits<char>, RAMAllocator<char>>;
#else
  using Storage = std::string;
#endif

  SipSignalingString() = default;
  SipSignalingString(const SipSignalingString &other) {
    this->assign(other.data(), other.size());
  }

  SipSignalingString &operator=(const SipSignalingString &other) {
    if (this != &other) this->assign(other.data(), other.size());
    return *this;
  }

  SipSignalingString &operator=(const std::string &value) {
    this->assign(value.data(), value.size());
    return *this;
  }

  void assign(const char *data, size_t size) { this->value_.assign(data, size); }

  bool empty() const { return this->value_.empty(); }
  size_t size() const { return this->value_.size(); }
  const char *data() const { return this->value_.data(); }

  void clear() {
    Storage empty(this->value_.get_allocator());
    this->value_.swap(empty);
  }

  void append(const char *data, size_t size) { this->value_.append(data, size); }
  size_t find(const char *needle) const { return this->value_.find(needle); }
  size_t rfind(const char *needle, size_t offset) const {
    return this->value_.rfind(needle, offset);
  }
  void erase(size_t offset, size_t size) { this->value_.erase(offset, size); }
  std::string substr(size_t offset, size_t size) const {
    return {this->value_.data() + offset, size};
  }

  std::string str() const { return {this->value_.data(), this->value_.size()}; }
  operator std::string() const { return this->str(); }

  bool operator==(const std::string &other) const {
    return this->size() == other.size() &&
           std::char_traits<char>::compare(this->data(), other.data(), this->size()) == 0;
  }
 private:
#ifdef USE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM
  Storage value_{RAMAllocator<char>(RAMAllocator<char>::ALLOC_EXTERNAL)};
#else
  Storage value_{};
#endif
};

}  // namespace esphome::voip_stack
