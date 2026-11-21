"""Compile the complete production negotiation owner and parser as real units."""
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "esphome/components/voip_stack"


@pytest.fixture(scope="module")
def media_negotiator_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("media-negotiator")
    for name, contents in {
        "esphome/core/defines.h": "",
        "esphome/core/hal.h": "#include <cstdint>\nnamespace esphome { uint32_t millis(); }",
        "esp_system.h": "#include <cstdint>\nuint32_t esp_random();",
        "lwip/sockets.h": "#include <arpa/inet.h>\n#define inet_ntoa_r(a,b,n) inet_ntop(AF_INET,&(a),b,n)",
    }.items():
        header = folder / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text(contents + "\n")
    binary = folder / "media-negotiator"
    result = subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-ffunction-sections",
        "-fdata-sections", "-Wl,--gc-sections", "-DUSE_ESP32", "-DUSE_ESPHOME_VOIP_SIP_TRANSPORT",
        "-DUSE_ESPHOME_VOIP_STACK_SPEAKER", "-DUSE_ESPHOME_VOIP_DTMF",
        "-I", str(folder), "-I", str(ROOT), "-I", str(SOURCE),
        str(SOURCE / "sip_message.cpp"), str(SOURCE / "media_negotiator.cpp"),
        str(ROOT / "tests/fixtures/media_negotiator.cpp"), "-o", str(binary),
    ], capture_output=True, text=True, timeout=45)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("mode", ["offer", "answer", "hard-maximum", "directional-dtmf", "directional-dtmf-answer", "g722-dtmf"])
def test_declared_directional_capabilities_and_independent_packet_times(media_negotiator_binary, mode):
    result = subprocess.run([media_negotiator_binary, mode], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr
