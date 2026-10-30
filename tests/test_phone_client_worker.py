"""Execute boot worker creation against the actual ESP-IDF core-ID contract."""

from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_worker_accepts_unpinned_or_explicit_core(tmp_path):
    include = tmp_path / "include"
    headers = {
        "esphome/core/defines.h": "#pragma once\n",
        "esphome/core/component.h": """#pragma once
namespace esphome { class Component { public:
 virtual ~Component() = default; virtual void on_shutdown() {}
 void disable_loop() {} void enable_loop_soon_any_context() {} void mark_failed() {}
}; }
""",
        "freertos/FreeRTOS.h": """#pragma once
#include <cassert>
#include <cstddef>
#include <cstdint>
using BaseType_t = int32_t; using StackType_t = uint8_t;
using TaskHandle_t = void *; using SemaphoreHandle_t = void *;
struct StaticTask_t { char storage[336]; };
struct StaticSemaphore_t { char storage[80]; };
// IDF 5.5.6 non-SMP FreeRTOS task.h:206. A POSIX-style -1 is invalid.
#define tskNO_AFFINITY ((BaseType_t)0x7fffffff)
#define pdTRUE 1
#define portMAX_DELAY 0xffffffff
""",
        "freertos/task.h": """#pragma once
#include "FreeRTOS.h"
inline int last_core = -99;
inline TaskHandle_t xTaskCreateStaticPinnedToCore(void (*)(void *), const char *, size_t,
 void *, unsigned, StackType_t *stack, StaticTask_t *tcb, BaseType_t core) {
 assert(stack && tcb);
 // Independent kernel admissibility oracle, freertos_tasks_c_additions.h:301.
 assert((core >= 0 && core < 2) || core == tskNO_AFFINITY);
 last_core = core; return tcb;
}
inline unsigned ulTaskNotifyTake(int, unsigned) { return 0; }
inline void vTaskSuspend(void *) {}
inline void xTaskNotifyGive(void *) {}
""",
        "freertos/semphr.h": """#pragma once
#include "FreeRTOS.h"
inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s) { return s; }
inline void xSemaphoreTake(void *, unsigned) {} inline void xSemaphoreGive(void *) {}
""",
        "esp_heap_caps.h": """#pragma once
#include <cstdlib>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
inline void *heap_caps_calloc(size_t n, size_t size, unsigned) { return std::calloc(n,size); }
inline void heap_caps_free(void *p) { std::free(p); }
""",
    }
    for name, text in headers.items():
        path = include / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    code = """#include <esp_heap_caps.h>
#include "esphome/components/phone_client/worker.h"
class Worker : public esphome::phone_client::WorkerComponent {
 public: bool start() { return setup_worker_("qualification"); }
 ~Worker() { heap_caps_free(stack_); } // No host scheduler owns this test allocation.
 void worker_run_() override {}
};
int main() {
 for (int core : {-1, 0, 1}) {
  const int expected = core < 0 ? 0x7fffffff : core;
  Worker worker; worker.set_task_core(core); assert(worker.start());
  assert(last_core == expected);
 }
 Worker defaults; assert(defaults.start()); assert(last_core == 0x7fffffff);
}
"""
    source = tmp_path / "worker.cpp"
    source.write_text(code)
    binary = tmp_path / "worker"
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_ESP32",
        "-I", str(include), "-I", str(ROOT), str(source),
        str(ROOT / "esphome/components/phone_client/worker.cpp"),
        str(ROOT / "esphome/components/phone_client/net_stream.cpp"), "-o", str(binary)],
        capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
