#pragma once

#include "net_stream.h"
#include <cstdlib>
#include <memory>
#include <type_traits>
#ifdef USE_ESP32
#include "esphome/core/helpers.h"
#endif

namespace esphome::phone_client {

// STL-compatible adapter around ESPHome's public RAM allocator. Allocation
// preference follows the component, but either allocator can free a block
// from either heap, so moving a completed snapshot never copies its table.
template<typename T> struct BufferAllocator {
  using value_type = T;
  using propagate_on_container_move_assignment = std::true_type;
  using propagate_on_container_swap = std::true_type;
  using is_always_equal = std::true_type;
  bool external{true};
  BufferAllocator() = default;
  explicit BufferAllocator(bool value) : external(value) {}
  template<typename U> BufferAllocator(const BufferAllocator<U> &other) : external(other.external) {}
  T *allocate(size_t count) {
#ifdef USE_ESP32
    if (count > SIZE_MAX / sizeof(T)) std::abort();
    RAMAllocator<T> allocator(this->external ? RAMAllocator<T>::NONE : RAMAllocator<T>::PREFER_INTERNAL);
    auto *result = allocator.allocate(count);
    if (result == nullptr && count != 0) std::abort();
    return result;
#else
    return std::allocator<T>{}.allocate(count);
#endif
  }
  void deallocate(T *pointer, size_t count) {
#ifdef USE_ESP32
    RAMAllocator<T>{}.deallocate(pointer, count);
#else
    std::allocator<T>{}.deallocate(pointer, count);
#endif
  }
  template<typename U> bool operator==(const BufferAllocator<U> &) const { return true; }
  template<typename U> bool operator!=(const BufferAllocator<U> &) const { return false; }
};

}  // namespace esphome::phone_client
