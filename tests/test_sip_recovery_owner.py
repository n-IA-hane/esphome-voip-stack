"""Whole production SIP owners recover through real UDP/TCP socket records."""
from pathlib import Path
import subprocess

import pytest

from production_sip_source import transport_units

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def recovery_owner(tmp_path_factory):
    folder = tmp_path_factory.mktemp("sip-recovery-owner")
    component = ROOT / "esphome/components/voip_stack"
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
    binary = folder / "recovery"
    subprocess.run([
        "g++", "-std=c++17", "-O1", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie", "-DUSE_ESP32",
        "-DUSE_ESPHOME_VOIP_SIP_TRANSPORT", "-DUSE_ESPHOME_VOIP_STACK_PCM", "-DVOIP_HOST_RTP_WORKER", "-pthread",
        "-I", str(folder), "-I", str(fixtures), "-I", str(ROOT), "-I", str(component),
        str(fixtures / "sip_recovery_owner.cpp"), *map(str, units), "-o", str(binary),
    ], check=True)
    return binary


@pytest.mark.parametrize("transport", ["udp", "tcp"])
@pytest.mark.parametrize("mode", [
    "same-tag", "early-fork", "two-2xx", "fork-cleanup-reuse", "cancel-fork", "negative-fork", "late-2xx",
    "offerless", "offerless-missing", "offerless-invalid", "offerless-timeout", "offerless-glare", "offerless-cancel", "offerless-foreign-ack", "offerless-mime",
    "rtp-port", "rtp-ip", "rtp-rx-ip", "rtp-rx-port", "rtp-invalid", "rtp-payload", "offer-mime", "offer-malformed",
])
def test_transaction_and_dialog_recovery(recovery_owner, transport, mode):
    subprocess.run([recovery_owner, mode, transport], check=True, timeout=5)
