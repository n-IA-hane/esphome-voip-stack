"""Release regressions run complete production SIP units over loopback sockets."""
from pathlib import Path
import os
import subprocess

import pytest

from production_sip_source import transport_units

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def release_regressions(tmp_path_factory):
    folder = tmp_path_factory.mktemp("sip-release-regressions")
    # Allows an independent baseline run with these same behavioral assertions.
    production = Path(os.environ.get("VOIP_TEST_PRODUCTION_ROOT", ROOT))
    component = production / "esphome/components/voip_stack"
    fixtures = ROOT / "tests/fixtures"
    for name in (
        "esphome/core/defines.h", "esphome/core/helpers.h", "esphome/core/log.h",
        "esphome/core/hal.h", "esphome/components/network/util.h", "esp_system.h",
        "esp_heap_caps.h", "lwip/sockets.h", "freertos/FreeRTOS.h",
        "freertos/task.h", "freertos/portmacro.h", "freertos/semphr.h",
    ):
        header = folder / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "sip_dialog_host_sdk.h"\n')
    units = [component / name for name in (
        "sip_transport.cpp", "sip_message.cpp", "sip_request_response.cpp",
        "sip_sdp.cpp", "sip_refer.cpp", "media_negotiator.cpp",
    )]
    units.extend(sorted(component.glob("sip_dialog_*.cpp")))
    units.extend(unit for unit in transport_units(component) if unit not in units)
    binary = folder / "regressions"
    resume = "pump_tcp_tx_" in (component / "sip_transport.h").read_text()
    subprocess.run([
        "g++", "-std=c++17", "-O1", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie", "-DUSE_ESP32",
        "-DUSE_ESPHOME_VOIP_SIP_TRANSPORT", "-DUSE_ESPHOME_VOIP_STACK_PCM", "-DVOIP_HOST_RTP_WORKER", "-pthread",
        "-Wl,--wrap=send", *(["-DVOIP_HAS_TCP_TX_RESUME"] if resume else []),
        "-I", str(folder), "-I", str(fixtures), "-I", str(production), "-I", str(component),
        str(fixtures / "sip_release_regressions.cpp"), *map(str, units), "-o", str(binary),
    ], check=True)
    return binary


@pytest.mark.parametrize("transport", ["udp", "tcp"])
@pytest.mark.parametrize("mode", [
    "timer100", "timer180", "timer-unanswered", "cancel-proceeding", "cancel-timeout", "cancel-late-provisional",
    "bye-before-ack-timeout", "bye-before-ack-delayed", "bye-after-ack",
    "rtp-old-newssrc", "rtp-clean-newssrc", "rtp-old-samessrc",
])
def test_transaction_release_regressions(release_regressions, transport, mode):
    subprocess.run([release_regressions, mode, transport], check=True, timeout=5)


@pytest.mark.parametrize("mode", ["tcp-eagain", "tcp-partial", "tcp-deadline", "tcp-hard-error"])
def test_tcp_release_regressions(release_regressions, mode):
    subprocess.run([release_regressions, mode, "tcp"], check=True, timeout=5)
