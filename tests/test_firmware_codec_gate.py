"""The CI link checker must reject missing or leaked codec implementations."""

import importlib.util
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
from types import SimpleNamespace

import pytest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("firmware_codec_gate", ROOT / "tools/check_firmware_codecs.py")
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)


def artifact(tmp_path, monkeypatch, *, enabled=True, machine=94, missing=None):
    build = tmp_path / "build"
    build.mkdir()
    core = tmp_path / "src/esphome/core"
    core.mkdir(parents=True)
    header = bytearray(20)
    header[:6] = b"\x7fELF\x01\x01"
    struct.pack_into("<HH", header, 16, 2, machine)
    (build / "firmware.elf").write_bytes(header)
    (core / "defines.h").write_text("#define USE_ESPHOME_VOIP_STACK_PCM\n" +
        ("#define USE_ESPHOME_VOIP_STACK_G722\n#define USE_ESPHOME_VOIP_STACK_MIC\n#define USE_ESPHOME_VOIP_STACK_SPEAKER\n" if enabled else ""))
    nm = tmp_path / "esp-nm"
    nm.touch()
    (build / "CMakeCache.txt").write_text(f"CMAKE_NM:FILEPATH={nm}\n")
    functions = GATE.G722_FUNCTIONS - ({missing} if missing else set()) if enabled else set()
    monkeypatch.setattr(GATE.subprocess, "run", lambda args, **kwargs: SimpleNamespace(
        stdout="".join(f"{name} T 40000000 10\n" for name in functions)))


@pytest.mark.parametrize("target,machine", [("esp32s3", 94), ("esp32p4", 243)])
@pytest.mark.parametrize("enabled", [False, True])
def test_checker_accepts_exact_architecture_and_codec_gate(tmp_path, monkeypatch, target, machine, enabled):
    artifact(tmp_path, monkeypatch, enabled=enabled, machine=machine)
    proof = GATE.inspect_build(tmp_path, target=target, expect_g722=enabled)
    assert proof["g722"] is enabled
    assert set(proof["linked_g722_functions"]) == (GATE.G722_FUNCTIONS if enabled else set())


@pytest.mark.parametrize("missing", sorted(GATE.G722_FUNCTIONS))
def test_checker_rejects_either_missing_codec_direction(tmp_path, monkeypatch, missing):
    artifact(tmp_path, monkeypatch, missing=missing)
    with pytest.raises(ValueError, match="Missing linked G722"):
        GATE.inspect_build(tmp_path, target="esp32s3", expect_g722=True)


def test_checker_rejects_wrong_architecture_and_default_codec_leak(tmp_path, monkeypatch):
    artifact(tmp_path, monkeypatch)
    with pytest.raises(ValueError, match="architecture"):
        GATE.inspect_build(tmp_path, target="esp32p4", expect_g722=True)
    with pytest.raises(ValueError, match="define"):
        GATE.inspect_build(tmp_path, target="esp32s3", expect_g722=False)
    (tmp_path / "src/esphome/core/defines.h").write_text("#define USE_ESPHOME_VOIP_STACK_PCM\n")
    with pytest.raises(ValueError, match="unexpectedly contains"):
        GATE.inspect_build(tmp_path, target="esp32s3", expect_g722=False)


def test_checker_requires_both_real_audio_owners(tmp_path, monkeypatch):
    artifact(tmp_path, monkeypatch)
    path = tmp_path / "src/esphome/core/defines.h"
    path.write_text(path.read_text().replace("#define USE_ESPHOME_VOIP_STACK_SPEAKER\n", ""))
    with pytest.raises(ValueError, match="both microphone and speaker"):
        GATE.inspect_build(tmp_path, target="esp32s3", expect_g722=True)


@pytest.mark.parametrize("profile", ["minimal_g722_s3", "minimal_g722_p4"])
def test_sdk_gate_profiles_generate_both_codec_directions_without_sdk_compile(tmp_path, profile):
    for name in ("g722_duplex_base", profile):
        text = (ROOT / f"tests/{name}.yaml").read_text()
        text = text.replace("../esphome/components", str(ROOT / "esphome/components"))
        if name == profile:
            text = text.replace("esphome:\n", f"esphome:\n  build_path: {tmp_path / 'build'}\n", 1)
        (tmp_path / f"{name}.yaml").write_text(text)
    result = subprocess.run([os.environ.get("ESPHOME_PYTHON", sys.executable), "-m", "esphome", "compile",
                             str(tmp_path / f"{profile}.yaml"), "--only-generate"],
                            capture_output=True, text=True, timeout=90)
    assert result.returncode == 0, result.stdout + result.stderr
    defines = next(tmp_path.rglob("defines.h")).read_text()
    for flag in ("PCM", "G722", "MIC", "SPEAKER"):
        assert f"#define USE_ESPHOME_VOIP_STACK_{flag}" in defines
    assert "#define USE_ESPHOME_VOIP_STACK_OPUS" not in defines
    code = next(tmp_path.rglob("main.cpp")).read_text()
    declared = {"tx": set(), "rx": set()}
    for direction, parameters in re.findall(r"phone->add_supported_(tx|rx)_audio_format\(([^)]+)\)", code):
        args = [value.strip() for value in parameters.split(",")]
        if args[-1] == "2":
            assert args[:3] == ["16000", "1", "1"]
            declared[direction].add(int(args[3]))
    assert declared == {"tx": {10, 20}, "rx": {10, 20}}
