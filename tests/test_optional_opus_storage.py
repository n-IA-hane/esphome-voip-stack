"""Compile actual media/transport types with and without the existing Opus flag."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "esphome/components/voip_stack"


def test_opus_receiver_storage_disappears_with_the_codec(tmp_path):
    for name in (
        "esphome/core/defines.h", "esphome/core/helpers.h", "esphome/core/log.h", "esphome/core/hal.h",
        "esphome/components/network/util.h", "esp_system.h", "esp_heap_caps.h", "lwip/sockets.h",
        "freertos/FreeRTOS.h", "freertos/task.h", "freertos/portmacro.h", "freertos/semphr.h",
    ):
        header = tmp_path / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "sip_dialog_host_sdk.h"\n')
    cpp = tmp_path / "storage.cpp"
    cpp.write_text(r'''
#include "sip_transport.h"
#include <type_traits>
#include <cstdio>
using namespace esphome::voip_stack;
struct Probe: SipTransport {using SipTransport::NegotiatedMedia;};
template<class T,class=void>struct HasOpus:std::false_type{};
template<class T>struct HasOpus<T,std::void_t<decltype(T::opus_tx)>>:std::true_type{};
#ifdef USE_ESPHOME_VOIP_STACK_OPUS
static_assert(HasOpus<SdpAudioMedia>::value && HasOpus<Probe::NegotiatedMedia>::value);
static_assert(sizeof(decltype(SdpAudioMedia::opus_tx))==8);
#else
static_assert(!HasOpus<SdpAudioMedia>::value && !HasOpus<Probe::NegotiatedMedia>::value);
#endif
int main(){std::printf("%zu %zu %zu\n",sizeof(SdpAudioMedia),sizeof(Probe::NegotiatedMedia),sizeof(SipTransport));}
''')
    sizes = []
    for enabled in (False, True):
        binary = tmp_path / ("enabled" if enabled else "disabled")
        built = subprocess.run([
            "g++", "-std=c++17", "-DUSE_ESP32", "-DUSE_ESPHOME_VOIP_SIP_TRANSPORT",
            *(["-DUSE_ESPHOME_VOIP_STACK_OPUS"] if enabled else []), "-I", str(tmp_path),
            "-I", str(ROOT / "tests/fixtures"), "-I", str(ROOT), "-I", str(SOURCE),
            "-I", "/usr/include/opus", str(cpp), "-o", str(binary),
        ], capture_output=True, text=True, timeout=15)
        assert built.returncode == 0, built.stderr
        sizes.append(tuple(map(int, subprocess.check_output([binary], text=True).split())))
    disabled, enabled = sizes
    assert enabled[0] == disabled[0] + 8
    assert enabled[1] == disabled[1] + 8
    assert enabled[2] > disabled[2]
