"""Observe Timer-H completion at the whole production SipTransport boundary."""
from pathlib import Path
import os
import subprocess

import pytest

from production_sip_source import transport_units

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def terminal_boundary_owner(tmp_path_factory):
    folder = tmp_path_factory.mktemp("terminal-boundary-owner")
    component = Path(os.environ.get("TERMINAL_OWNER_SOURCE", ROOT / "esphome/components/voip_stack"))
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
    binary = folder / "terminal-boundary"
    result = subprocess.run([
        "g++", "-std=c++17", "-O1", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie", "-DUSE_ESP32",
        "-DUSE_ESPHOME_VOIP_SIP_TRANSPORT", "-DUSE_ESPHOME_VOIP_STACK_PCM", "-DVOIP_HOST_RTP_WORKER", "-pthread",
        "-I", str(folder), "-I", str(fixtures), "-I", str(ROOT), "-I", str(component),
        str(fixtures / "sip_terminal_completion_owner.cpp"), *map(str, units), "-o", str(binary),
    ], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("transport", ["udp", "tcp"])
@pytest.mark.parametrize("mode", ["missing", "ack", "live", "reuse"])
def test_final_response_expiry_notifies_application_after_pending_clears(terminal_boundary_owner, mode, transport):
    result = subprocess.run([terminal_boundary_owner, mode, transport], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr
