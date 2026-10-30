"""Audio sample time distinguishes nonaudio RTP gaps from real missing audio."""

from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def jitter_timestamp_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("jitter-timestamps")
    helper = folder / "esphome/core/helpers.h"
    helper.parent.mkdir(parents=True)
    helper.write_text("namespace esphome {struct Mutex {};struct LockGuard {explicit LockGuard(Mutex&) {}};}\n")
    binary = folder / "jitter"
    result = subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_ESP32",
        "-I", str(folder), "-I", str(ROOT),
        str(ROOT / "tests/test_jitter_audio_timestamps.cpp"), "-o", str(binary),
    ], capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("scenario", [
    "interleave", "lost-event", "real-loss", "mixed-loss", "reorder",
    "sequence-wrap", "timestamp-wrap", "format-change", "opus-clock", "reset",
    "realign", "unknown-format", "no-metadata", "nearest-future", "invalid-size",
    "source-change", "random-mixed",
])
def test_audio_timestamps_preserve_real_loss(jitter_timestamp_binary, scenario):
    result = subprocess.run([str(jitter_timestamp_binary), scenario], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
