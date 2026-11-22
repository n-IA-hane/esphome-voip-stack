#pragma once

// Host adapters for the complete video_rtp.cpp owner. Production task entries,
// POSIX UDP I/O, stop/join and all lease callbacks execute without substitution.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <unistd.h>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <arpa/inet.h>

#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define inet_ntoa_r(a,b,n) inet_ntop(AF_INET,&(a),b,n)

inline int64_t esp_timer_get_time() {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline uint32_t millis() { return esp_timer_get_time() / 1000; }
inline void delay(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
inline uint32_t esp_random() { static std::atomic<uint32_t> n{10}; return ++n; }
namespace esphome {
inline std::function<void(void*)> before_mutex;
struct Mutex {
  std::mutex value;
  void lock() {if(before_mutex)before_mutex(this);value.lock();}
  void unlock() {value.unlock();}
};
using LockGuard=std::lock_guard<Mutex>;
template<class T> struct RAMAllocator : std::allocator<T> {
  enum { ALLOC_EXTERNAL, ALLOC_INTERNAL };
  explicit RAMAllocator(int=0) {}
};
}
namespace video_probe {
inline std::mutex mutex;
inline std::unordered_map<void*,size_t> allocations;
inline std::atomic<unsigned> allocated{0},freed{0},tasks{0};
}
constexpr unsigned MALLOC_CAP_SPIRAM=1,MALLOC_CAP_INTERNAL=2,MALLOC_CAP_8BIT=4;
inline void *heap_caps_malloc(size_t bytes,unsigned) {
  auto *p=std::malloc(bytes); assert(p);
  std::lock_guard<std::mutex> lock(video_probe::mutex);
  assert(video_probe::allocations.emplace(p,bytes).second); ++video_probe::allocated; return p;
}
inline void heap_caps_free(void *p) {
  std::lock_guard<std::mutex> lock(video_probe::mutex);
  assert(video_probe::allocations.erase(p)==1); ++video_probe::freed; std::free(p);
}
using BaseType_t=int;
using UBaseType_t=unsigned;
using TickType_t=unsigned;
using StackType_t=uint32_t;
using TaskFunction_t=void(*)(void*);
struct StaticTask_t {};
struct HostTask { std::thread thread; };
using TaskHandle_t=HostTask*;
struct StaticSemaphore_t { std::mutex mutex; std::condition_variable cv; bool given=false; };
using SemaphoreHandle_t=StaticSemaphore_t*;
constexpr unsigned portMAX_DELAY=~0u,configTICK_RATE_HZ=1000;
constexpr int pdPASS=1,pdTRUE=1,eDeleted=1;
#define pdMS_TO_TICKS(ms) (ms)
inline SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *value) { return value; }
inline int xSemaphoreTake(SemaphoreHandle_t value,unsigned ticks) {
  std::unique_lock<std::mutex> lock(value->mutex);
  if(ticks==portMAX_DELAY) value->cv.wait(lock,[&]{return value->given;});
  else if(!value->cv.wait_for(lock,std::chrono::milliseconds(ticks),[&]{return value->given;})) return 0;
  value->given=false; return pdTRUE;
}
inline int xSemaphoreGive(SemaphoreHandle_t value) {
  {std::lock_guard<std::mutex> lock(value->mutex);value->given=true;}
  value->cv.notify_one(); return pdTRUE;
}
struct HostTaskFinished {};
inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn,const char*,uint32_t,void *context,
                                         UBaseType_t,TaskHandle_t *out,BaseType_t) {
  *out=new HostTask;
  ++video_probe::tasks;
  (*out)->thread=std::thread([fn,context]{try{fn(context);}catch(const HostTaskFinished&){};});
  return pdPASS;
}
inline TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t fn,const char *name,uint32_t bytes,void *context,
                                                  UBaseType_t priority,StackType_t*,StaticTask_t*,BaseType_t core) {
  TaskHandle_t result; xTaskCreatePinnedToCore(fn,name,bytes,context,priority,&result,core); return result;
}
inline int eTaskGetState(TaskHandle_t) { return 0; }
inline void vTaskDelete(TaskHandle_t task) { assert(task);task->thread.join();delete task;--video_probe::tasks; }
inline void vTaskSuspend(TaskHandle_t task) { assert(!task);throw HostTaskFinished{}; }
inline void taskYIELD() { std::this_thread::yield(); }
inline void xTaskNotifyGive(TaskHandle_t) { std::abort(); }
inline unsigned ulTaskNotifyTake(BaseType_t,unsigned) { std::abort(); }
