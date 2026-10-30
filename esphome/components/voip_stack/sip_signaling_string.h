#pragma once

#include "esphome/core/helpers.h"

#include <string>
#include <string_view>

namespace esphome::voip_stack {

// Persistent SIP messages and subscription metadata are control-plane payloads.
// On PSRAM builds keep their retained capacity out of scarce internal RAM.
// The wrapper deliberately exposes only string operations used by the SIP
// transaction stores, so ownership cannot leak into parser or media code.
class SipSignalingString {
 public:
#ifdef USE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM
  using Storage = std::basic_string<char, std::char_traits<char>, RAMAllocator<char>>;
#else
  using Storage = std::string;
#endif

  SipSignalingString() = default;
  // Every instance uses the same fixed allocation policy. Swap the backing
  // storage so dialog handoff does not copy retained signaling messages.
  SipSignalingString(SipSignalingString &&other) noexcept { this->value_.swap(other.value_); }
  SipSignalingString &operator=(SipSignalingString &&other) noexcept {
    if (this != &other) this->value_.swap(other.value_);
    return *this;
  }
  SipSignalingString(const SipSignalingString &other) {
    this->assign(other.data(), other.size());
  }

  SipSignalingString &operator=(const SipSignalingString &other) {
    if (this != &other) this->assign(other.data(), other.size());
    return *this;
  }

  SipSignalingString &operator=(std::string_view value) {
    this->assign(value.data(), value.size());
    return *this;
  }

  void assign(const char *data, size_t size) { this->value_.assign(data, size); }

  bool empty() const { return this->value_.empty(); }
  size_t size() const { return this->value_.size(); }
  size_t capacity() const { return this->value_.capacity(); }
  const char *data() const { return this->value_.data(); }
  const char *c_str() const { return this->value_.c_str(); }

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
  operator std::string_view() const { return {this->data(), this->size()}; }

  bool operator==(std::string_view other) const {
    return this->size() == other.size() &&
           std::char_traits<char>::compare(this->data(), other.data(), this->size()) == 0;
  }
  bool operator!=(std::string_view other) const { return !(*this == other); }
  bool operator==(const SipSignalingString &other) const {
    return this->size() == other.size() &&
           std::char_traits<char>::compare(this->data(), other.data(), this->size()) == 0;
  }
  bool operator!=(const SipSignalingString &other) const { return !(*this == other); }
  friend bool operator==(std::string_view left, const SipSignalingString &right) { return right == left; }
  friend bool operator!=(std::string_view left, const SipSignalingString &right) { return !(right == left); }

  // Builders materialize their temporary wire message at this explicit boundary.
  // The retained fields continue to use their configured allocation policy.
  friend std::string operator+(std::string left, const SipSignalingString &right) {
    left.append(right.data(), right.size());
    return left;
  }
  friend std::string operator+(const char *left, const SipSignalingString &right) {
    return std::string(left) + right;
  }
 private:
#ifdef USE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM
  Storage value_{RAMAllocator<char>(RAMAllocator<char>::ALLOC_EXTERNAL)};
#else
  Storage value_{};
#endif
};

}  // namespace esphome::voip_stack
