#pragma once

// POSIX sockets and mutexes service one real SipTransport on its calling
// thread. Every scheduler operation is forbidden in this signaling fixture.
#include <arpa/inet.h>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <sys/socket.h>
#include <unistd.h>

#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define inet_ntoa_r(a, b, n) inet_ntop(AF_INET, &(a), b, n)

inline uint32_t millis() { return 100; }
inline void delay(uint32_t) {}
inline uint32_t esp_random() {
  static uint32_t value = 10000;
  return ++value;
}

namespace esphome {
using Mutex = std::mutex;
using LockGuard = std::lock_guard<Mutex>;

template <class T>
struct RAMAllocator : std::allocator<T> {
  enum { ALLOC_EXTERNAL, ALLOC_INTERNAL };
  explicit RAMAllocator(int = 0) {}
};

namespace network {
constexpr unsigned IP_ADDRESS_BUFFER_SIZE = 48;
struct Address {
  bool is_ip4() const { return false; }
  void str_to(char *) const {}
};
inline std::array<Address, 0> get_ip_addresses() { return {}; }
}  // namespace network
}  // namespace esphome

constexpr int MALLOC_CAP_8BIT = 1, MALLOC_CAP_INTERNAL = 2, MALLOC_CAP_SPIRAM = 4;
inline void *heap_caps_malloc(size_t bytes, int) { return std::malloc(bytes); }
inline void heap_caps_free(void *p) { std::free(p); }

using BaseType_t = int;
using UBaseType_t = unsigned;
using StackType_t = uint32_t;
using TaskFunction_t = void (*)(void *);
using TaskHandle_t = void *;
using SemaphoreHandle_t = void *;
struct StaticTask_t {};
struct StaticSemaphore_t {};
using portMUX_TYPE = int;
constexpr unsigned portMAX_DELAY = ~0u;
constexpr int portMUX_INITIALIZER_UNLOCKED = 0, pdPASS = 1, pdTRUE = 1, eDeleted = 1;
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))

inline TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t, const char *, uint32_t, void *, UBaseType_t,
                                                  StackType_t *, StaticTask_t *, BaseType_t) {
  std::abort();
}
inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t, const char *, uint32_t, void *, UBaseType_t, TaskHandle_t *,
                                          BaseType_t) {
  std::abort();
}
inline void taskYIELD() { std::abort(); }
inline int eTaskGetState(TaskHandle_t) { std::abort(); }
inline void vTaskDelete(TaskHandle_t) { std::abort(); }
inline void vTaskSuspend(TaskHandle_t) { std::abort(); }
inline void xTaskNotifyGive(TaskHandle_t) { std::abort(); }
inline uint32_t ulTaskNotifyTake(BaseType_t, uint32_t) { std::abort(); }
inline SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *) { std::abort(); }
inline int xSemaphoreTake(SemaphoreHandle_t, uint32_t) { std::abort(); }
inline int xSemaphoreGive(SemaphoreHandle_t) { std::abort(); }
