"""Run the complete SipTransport with its real SIP/RTP task entries and sockets."""
from pathlib import Path
import subprocess
import os
import shlex

import pytest

from production_sip_source import transport_units

ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(os.environ.get("TRANSPORT_LIFECYCLE_SOURCE", ROOT / "esphome/components/voip_stack"))


def build_transport_owner(tmp_path_factory, fixture="transport_owner_lifecycle.cpp", video=False, registration=False, opus=False):
    tmp_path = tmp_path_factory.mktemp("transport-lifecycle")
    sdk = (ROOT / "tests/fixtures/video_host_sdk.h").read_text()
    sdk = sdk.replace("struct HostTask { std::thread thread; };", """
struct HostTask { std::thread thread;std::mutex mutex;std::condition_variable cv;unsigned notifications=0; };
inline thread_local HostTask *current_task=nullptr;
inline std::vector<HostTask*> host_tasks;
inline void reap_host_tasks(){for(auto *task:host_tasks){task->thread.join();delete task;--video_probe::tasks;}host_tasks.clear();}
""")
    sdk = sdk.replace("inline BaseType_t xTaskCreatePinnedToCore", "inline unsigned scheduler_creations=0,scheduler_fail_at=0;\ninline BaseType_t xTaskCreatePinnedToCore")
    sdk = sdk.replace("  *out=new HostTask;", "  if(++scheduler_creations==scheduler_fail_at){*out=nullptr;return 0;}\n  *out=new HostTask;")
    sdk = sdk.replace("#include <thread>", "#include <thread>\n#include <vector>\n#include <array>")
    sdk = sdk.replace("(*out)->thread=std::thread([fn,context]{try{fn(context);}catch(const HostTaskFinished&){};});", """
  auto *task=*out;host_tasks.push_back(task);
  task->thread=std::thread([fn,context,task]{current_task=task;try{fn(context);}catch(const HostTaskFinished&){};});
""")
    sdk = sdk.replace(
        "assert(task);task->thread.join();delete task;--video_probe::tasks;",
        "if(!task)throw HostTaskFinished{};task->thread.join();host_tasks.erase(std::find(host_tasks.begin(),host_tasks.end(),task));delete task;--video_probe::tasks;"
        if video else "if(!task)throw HostTaskFinished{};std::abort();",
    )
    sdk = sdk.replace("inline void xTaskNotifyGive(TaskHandle_t) { std::abort(); }", """
inline void xTaskNotifyGive(TaskHandle_t task) {
 {std::lock_guard<std::mutex> lock(task->mutex);++task->notifications;}task->cv.notify_one();
}
""")
    sdk = sdk.replace("inline unsigned ulTaskNotifyTake(BaseType_t,unsigned) { std::abort(); }", """
inline unsigned ulTaskNotifyTake(BaseType_t clear,unsigned ticks) {
 auto *task=current_task;assert(task);std::unique_lock<std::mutex> lock(task->mutex);
 if(ticks==portMAX_DELAY)task->cv.wait(lock,[&]{return task->notifications>0;});
 else if(!task->cv.wait_for(lock,std::chrono::milliseconds(ticks),[&]{return task->notifications>0;}))return 0;
 auto result=task->notifications;if(clear)task->notifications=0;else --task->notifications;return result;
}
""")
    sdk += """
namespace esphome::network {
constexpr unsigned IP_ADDRESS_BUFFER_SIZE=48;
struct Address{bool is_ip4()const{return false;}void str_to(char*)const{}};
inline std::array<Address,0> get_ip_addresses(){return {};}
}
using portMUX_TYPE=std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(p) (p)->lock()
#define portEXIT_CRITICAL(p) (p)->unlock()
"""
    if registration:
        sdk += (ROOT / "tests/fixtures/transport_registration_host_sdk.h").read_text()
    (tmp_path / "transport_host_sdk.h").write_text(sdk)
    for name in (
        "esphome/core/defines.h", "esphome/core/helpers.h", "esphome/core/log.h", "esphome/core/hal.h",
        "esphome/components/network/util.h", "esp_system.h", "esp_heap_caps.h", "esp_timer.h", "lwip/sockets.h",
        "freertos/FreeRTOS.h", "freertos/task.h", "freertos/portmacro.h", "freertos/semphr.h",
    ):
        header = tmp_path / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "transport_host_sdk.h"\n')
    if registration:
        for name in ("lwip/dns.h", "lwip/tcpip.h", "mbedtls/md.h"):
            header = tmp_path / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "transport_host_sdk.h"\n')
    units = transport_units(SOURCE)
    units.extend(SOURCE / name for name in ("sip_message.cpp", "sip_request_response.cpp", "sip_sdp.cpp", "sip_refer.cpp", "media_negotiator.cpp"))
    units.extend(unit for unit in sorted(SOURCE.glob("sip_dialog_*.cpp")) if unit not in units)
    if video:
        units.extend(SOURCE / name for name in ("video_rtp.cpp", "rtp_jpeg.cpp"))
    if registration:
        units.extend(SOURCE / name for name in ("sip_registration.cpp", "sip_registration_transport.cpp"))
    opus_flags = []
    if opus:
        units.append(SOURCE / "opus_rtp_codec.cpp")
        opus_flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "opus"], text=True))
    binary = tmp_path / "transport-owner"
    built = subprocess.run([
        "g++", "-std=c++17", "-O0" if video else "-O1", "-g1" if video else "-g", "-pthread", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie", "-DUSE_ESP32",
        "-DUSE_ESPHOME_VOIP_SIP_TRANSPORT", "-DUSE_ESPHOME_VOIP_STACK_PCM",
        *(["-DUSE_ESPHOME_VOIP_STACK_OPUS"] if opus else []),
        *(["-DUSE_ESPHOME_VOIP_REGISTRATION"] if registration else []),
        *(["-DUSE_ESPHOME_VOIP_STACK_VIDEO", "-DUSE_ESPHOME_VOIP_STACK_VIDEO_JPEG"] if video else []),
        "-I", str(tmp_path), "-I", str(ROOT), "-I", str(SOURCE),
        str(ROOT / "tests/fixtures" / fixture), *map(str, units), *(["-lcrypto"] if registration else []), *opus_flags, "-o", str(binary),
    ], capture_output=True, text=True, timeout=150)
    assert built.returncode == 0, built.stderr
    return binary


@pytest.fixture(scope="module")
def transport_lifecycle_binary(tmp_path_factory):
    return build_transport_owner(tmp_path_factory)


@pytest.mark.parametrize("mode", ["normal", "early-sip-exit", "startup-failure"])
def test_complete_transport_joins_workers_and_reuses_one_socket_reader(transport_lifecycle_binary, mode):
    ran = subprocess.run([transport_lifecycle_binary, mode], capture_output=True, text=True, timeout=15)
    assert ran.returncode == 0, ran.stdout + ran.stderr


@pytest.fixture(scope="module")
def audio_alias_history_binary(tmp_path_factory):
    return build_transport_owner(tmp_path_factory, fixture="audio_alias_history_owner.cpp")


@pytest.mark.parametrize("mode", ["shrink-restore", "retired-remap", "unprepared-alias", "unprepared-owner", "primary-change"])
def test_direction_only_alias_changes_reuse_committed_media(audio_alias_history_binary, mode):
    ran = subprocess.run([str(audio_alias_history_binary), mode], capture_output=True, text=True, timeout=15)
    assert ran.returncode == 0, ran.stdout + ran.stderr
