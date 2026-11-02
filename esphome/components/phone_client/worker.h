#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32
#include "esphome/core/component.h"
#include "net_stream.h"
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

namespace esphome::phone_client {

// One permanent worker, created at boot, asleep on a notification when idle.
// Cold configuration/results use one static mutex. No task is created by a
// refresh, reconnect, play, stop, or an API action.
class WorkerComponent : public Component {
 public:
  void set_task_core(int core) { this->task_core_ = core < 0 ? tskNO_AFFINITY : core; }
  void set_task_priority(uint8_t priority) { this->task_priority_ = priority; }
  void set_task_stack_size(size_t bytes) { this->stack_bytes_ = bytes; }
  void set_task_stack_in_psram(bool value) { this->stack_psram_ = value; }
  void set_buffer_in_psram(bool value) { this->buffers_psram_ = value; }
  void on_shutdown() override;
 protected:
  bool setup_worker_(const char *name);
  void wake_worker_();
  void interrupt_worker_();
  virtual void worker_run_() = 0;
  void notify_main_() { this->enable_loop_soon_any_context(); }
  void lock_() { if (this->mutex_ != nullptr) xSemaphoreTake(this->mutex_, portMAX_DELAY); }
  void unlock_() { if (this->mutex_ != nullptr) xSemaphoreGive(this->mutex_); }
  static void *allocate_(size_t bytes, bool psram);
  std::atomic<bool> shutdown_{false};
  std::atomic<uint32_t> generation_{1};
  TcpStream stream_;
  TaskHandle_t task_{nullptr};
  SemaphoreHandle_t mutex_{nullptr};
  StaticSemaphore_t mutex_storage_{};
  StaticTask_t task_storage_{};
  StackType_t *stack_{nullptr};
  size_t stack_bytes_{8192};
  int task_core_{tskNO_AFFINITY};
  uint8_t task_priority_{2};
  bool stack_psram_{false}, buffers_psram_{true};
};

}  // namespace esphome::phone_client
#endif
