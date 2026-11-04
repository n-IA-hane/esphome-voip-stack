"""Production wrapper contracts, with exact Espressif 2.5 headers and host APIs.

The host API stand-in uses SpanDSP. FFmpeg independently supplies and decodes
wire data. Passing these tests does not execute or qualify the ESP binary.
"""

import array
import math
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "esphome/components/voip_stack"
FIXTURE = ROOT / "tests/fixtures/g722_esp_audio_codec_2_5_0"


@pytest.fixture(scope="module")
def g722_host(tmp_path_factory):
    if shutil.which("ffmpeg") is None or subprocess.run(
        ["pkg-config", "--exists", "spandsp"], check=False
    ).returncode:
        pytest.skip("FFmpeg and SpanDSP development library are required for the independent G.722 oracle")
    build = tmp_path_factory.mktemp("g722-host")
    core = build / "esphome/core"
    core.mkdir(parents=True)
    (core / "defines.h").write_text("#pragma once\n")
    (core / "log.h").write_text(
        "#pragma once\n#include <cstdio>\n"
        "#define ESP_LOGW(tag,...) do{(void)tag;if(false)std::printf(__VA_ARGS__);}while(false)\n"
    )
    executable = build / "g722"
    subprocess.run(
        [
            "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-DUSE_ESPHOME_VOIP_STACK_G722", "-I", str(build),
            "-I", str(FIXTURE), "-I", str(COMPONENT),
            str(COMPONENT / "g722_rtp_codec.cpp"), str(FIXTURE / "runtime.cpp"),
            "-lspandsp", "-o", str(executable),
        ], check=True, capture_output=True, text=True,
    )
    return executable


def test_g722_state_directional_resources_rollback_and_reuse(g722_host):
    subprocess.run([str(g722_host), "contract"], check=True, capture_output=True, text=True)


@pytest.mark.parametrize("frame_ms", [10, 20])
def test_g722_wire_matches_independent_ffmpeg(g722_host, tmp_path, frame_ms):
    samples = array.array("h", (
        int(11000 * math.sin(2 * math.pi * 440 * i / 16000)
            + 5500 * math.sin(2 * math.pi * 2800 * i / 16000))
        for i in range(3200)
    ))
    pcm = tmp_path / "input.s16le"
    pcm.write_bytes(samples.tobytes())
    reference = tmp_path / "ffmpeg.g722"
    wire = tmp_path / "wrapper.g722"
    decoded = tmp_path / "wrapper.s16le"
    reference_decoded = tmp_path / "ffmpeg.s16le"
    subprocess.run([
        "ffmpeg", "-v", "error", "-f", "s16le", "-ar", "16000", "-ac", "1",
        "-i", str(pcm), "-c:a", "adpcm_g722", "-b:a", "64k", "-f", "g722", str(reference),
    ], check=True, capture_output=True)
    subprocess.run([
        str(g722_host), "encode", str(frame_ms), str(pcm), str(wire),
    ], check=True, capture_output=True)
    assert wire.read_bytes() == reference.read_bytes()
    assert wire.stat().st_size == len(samples) // 2
    subprocess.run([
        str(g722_host), "decode", str(frame_ms), str(reference), str(decoded),
    ], check=True, capture_output=True)
    subprocess.run([
        "ffmpeg", "-v", "error", "-f", "g722", "-i", str(wire),
        "-c:a", "pcm_s16le", "-f", "s16le", str(reference_decoded),
    ], check=True, capture_output=True)
    assert decoded.read_bytes() == reference_decoded.read_bytes()
    assert decoded.stat().st_size == pcm.stat().st_size


def test_g722_not_declared_has_no_codec_dependency(tmp_path):
    core = tmp_path / "esphome/core"
    core.mkdir(parents=True)
    (core / "defines.h").write_text("#pragma once\n")
    output = tmp_path / "disabled.o"
    subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(tmp_path),
        "-c", str(COMPONENT / "g722_rtp_codec.cpp"), "-o", str(output),
    ], check=True, capture_output=True, text=True)
    symbols = subprocess.run([
        "nm", "-C", str(output),
    ], check=True, capture_output=True, text=True).stdout
    assert not symbols.strip()
