"""Retain decoder leases across real video owner stop, redial and destruction."""
from pathlib import Path
import os
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(os.environ.get("VIDEO_LIFETIME_SOURCE", ROOT / "esphome/components/voip_stack"))


@pytest.fixture(scope="module")
def video_owner_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("video-owner")
    for name in (
        "esphome/core/defines.h", "esphome/core/helpers.h", "esphome/core/log.h", "esphome/core/hal.h",
        "esp_system.h", "esp_heap_caps.h", "esp_timer.h", "lwip/sockets.h",
        "freertos/FreeRTOS.h", "freertos/task.h", "freertos/semphr.h",
    ):
        header = folder / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "video_host_sdk.h"\n')
    binary = folder / "video-owner"
    compiled = subprocess.run([
        "g++", "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable", "-Wno-unused-parameter",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie", "-pthread",
        "-DUSE_ESP32", "-DUSE_ESPHOME_VOIP_STACK_VIDEO", "-DUSE_ESPHOME_VOIP_STACK_VIDEO_H264",
        "-I", str(folder), "-I", str(ROOT / "tests/fixtures"), "-I", str(SOURCE),
        str(SOURCE / "video_rtp.cpp"), str(ROOT / "tests/fixtures/video_owner_lifetime.cpp"), "-o", str(binary),
    ], capture_output=True, text=True, timeout=45)
    assert compiled.returncode == 0, compiled.stderr
    return binary


@pytest.mark.parametrize("mode", ["stop-redial-resize", "destroy-held", "destroy-release-tail"])
def test_complete_producer_owner_retains_decoder_storage(video_owner_binary, mode):
    ran = subprocess.run([video_owner_binary, mode], capture_output=True, text=True, timeout=10)
    assert ran.returncode == 0, ran.stdout + ran.stderr
