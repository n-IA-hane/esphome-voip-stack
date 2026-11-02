#include "worker.h"
#ifdef USE_ESP32
#include <esp_heap_caps.h>

namespace esphome::phone_client {

void *WorkerComponent::allocate_(size_t bytes, bool psram) {
  void *result = psram ? heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : nullptr;
  return result != nullptr ? result : heap_caps_calloc(1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

bool WorkerComponent::setup_worker_(const char *name) {
  this->disable_loop();
  this->mutex_ = xSemaphoreCreateMutexStatic(&this->mutex_storage_);
  this->stack_ = static_cast<StackType_t *>(allocate_(this->stack_bytes_, this->stack_psram_));
  if (this->mutex_ == nullptr || this->stack_ == nullptr) { this->mark_failed(); return false; }
  this->task_ = xTaskCreateStaticPinnedToCore([](void *argument) {
    auto *owner = static_cast<WorkerComponent *>(argument);
    while (!owner->shutdown_.load()) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      if (!owner->shutdown_.load()) owner->worker_run_();
    }
    // TCB and stack remain boot-owned through shutdown; never free storage
    // while the scheduler may still reference it.
    vTaskSuspend(nullptr);
  }, name, this->stack_bytes_, this, this->task_priority_, this->stack_, &this->task_storage_, this->task_core_);
  if (this->task_ == nullptr) {
    heap_caps_free(this->stack_); this->stack_ = nullptr;
    this->mark_failed(); return false;
  }
  return true;
}

void WorkerComponent::wake_worker_() { if (this->task_ != nullptr) xTaskNotifyGive(this->task_); }
void WorkerComponent::interrupt_worker_() { ++this->generation_; this->stream_.interrupt(); this->wake_worker_(); }
void WorkerComponent::on_shutdown() {
  this->shutdown_.store(true);
  this->stream_.interrupt();
  this->wake_worker_();
}

}  // namespace esphome::phone_client
#endif
