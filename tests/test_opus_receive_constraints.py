"""Link complete signaling/codec owners to libopus and POSIX sockets."""
from production_sip_source import transport_units
from pathlib import Path
import shlex
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "esphome/components/voip_stack"


@pytest.fixture(scope="module")
def opus_owner_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("opus-owner")
    for name in (
        "esphome/core/defines.h", "esphome/core/helpers.h", "esphome/core/log.h",
        "esphome/core/hal.h", "esphome/components/network/util.h", "esp_system.h",
        "esp_heap_caps.h", "lwip/sockets.h", "freertos/FreeRTOS.h",
        "freertos/task.h", "freertos/portmacro.h", "freertos/semphr.h",
    ):
        header = folder / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "sip_dialog_host_sdk.h"\n')
    opus_flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "opus"], text=True))
    units = [SOURCE / name for name in (
        "sip_transport.cpp", "sip_message.cpp", "sip_request_response.cpp", "sip_sdp.cpp",
        "sip_refer.cpp", "media_negotiator.cpp", "opus_rtp_codec.cpp",
    )]
    units.extend(sorted(SOURCE.glob("sip_dialog_*.cpp")))
    units.extend(unit for unit in transport_units(SOURCE) if unit not in units)
    binary = folder / "opus-owner"
    built = subprocess.run([
        "g++", "-std=c++17", "-O1", "-g", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie", "-DUSE_ESP32",
        "-DUSE_ESPHOME_VOIP_SIP_TRANSPORT", "-DUSE_ESPHOME_VOIP_STACK_OPUS",
        "-I", str(folder), "-I", str(ROOT / "tests/fixtures"), "-I", str(ROOT), "-I", str(SOURCE),
        str(ROOT / "tests/fixtures/opus_receive_constraints.cpp"), *map(str, units), *opus_flags, "-o", str(binary),
    ], capture_output=True, text=True, timeout=150)
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize("mode", ["codec", "negotiation", "transport"])
def test_peer_receive_constraints_reach_only_committed_encoder(opus_owner_binary, mode):
    result = subprocess.run([opus_owner_binary, mode], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
