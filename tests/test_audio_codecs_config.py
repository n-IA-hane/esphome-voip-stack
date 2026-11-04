"""Explicit codec declarations preserve order and gate generated dependencies."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys

import pytest
import yaml
from esphome import config_validation as cv

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture
def codec_schema(monkeypatch):
    spec = importlib.util.spec_from_file_location(
        "voip_audio_codec_schema", ROOT / "esphome/components/voip_stack/__init__.py"
    )
    module = importlib.util.module_from_spec(spec)
    monkeypatch.setitem(sys.modules, spec.name, module)
    spec.loader.exec_module(module)
    return module.PHONE_CODECS_SCHEMA


def pcm(rate, duration):
    return {"sample_rate": rate, "frame_ms": duration}


def test_codec_lists_preserve_each_direction_and_declared_preferences(codec_schema):
    result = codec_schema({"codecs": [{"codec": "pcm", "tx": [pcm(16000, 20), pcm(16000, 10)],
                                     "rx": [pcm(48000, 10), pcm(16000, 20), pcm(16000, 10)]}]})
    assert result["tx"]["sample_rate"] == 16000 and result["tx"]["frame_ms"] == 20
    assert [fmt["frame_ms"] for fmt in result["tx_formats"]] == [10]
    assert result["rx"]["sample_rate"] == 48000 and result["rx"]["frame_ms"] == 10
    assert [(fmt["sample_rate"], fmt["frame_ms"]) for fmt in result["rx_formats"]] == [(16000, 20), (16000, 10)]
    assert result["tx"]["pcm_format"] == "s16le" and result["tx"]["channels"] == 1


@pytest.mark.parametrize("declaration", [
    {}, {"codecs": []}, {"tx": pcm(16000, 20)},
    {"codecs": [{"codec": "pcm"}]},
    {"codecs": [{"codec": "pcm", "tx": [pcm(16000, 20)]}] * 2},
    {"codecs": [{"codec": "pcm", "tx": [{"sample_rate": 16000}]}]},
    {"codecs": [{"codec": "pcm", "tx": [pcm("auto", 20)]}]},
    {"codecs": [{"codec": "g722", "rx": [pcm(8000, 20)]}]},
    {"codecs": [{"codec": "g722", "rx": [pcm(16000, 16)]}]},
    {"codecs": [{"codec": "g722", "rx": [{**pcm(16000, 20), "channels": 2}]}]},
    {"codecs": [{"codec": "opus", "rx": [pcm(32000, 20)]}]},
    {"codecs": [{"codec": "opus", "rx": [pcm(16000, 32)]}]},
])
def test_invalid_or_implicit_codec_capabilities_are_rejected(codec_schema, declaration):
    with pytest.raises(cv.Invalid):
        codec_schema(declaration)


def test_one_way_and_mixed_codec_order_are_explicit(codec_schema):
    result = codec_schema({"codecs": [
        {"codec": "g722", "rx": [pcm(16000, 20), pcm(16000, 10)]},
        {"codec": "pcm", "rx": [pcm(16000, 20)]},
    ]})
    assert result["tx"] is None and result["tx_formats"] == []
    assert result["rx"]["codec"] == "g722"
    assert [fmt["codec"] for fmt in result["rx_formats"]] == ["g722", "pcm"]


@pytest.mark.parametrize("g722", [False, True])
def test_real_codegen_gates_g722_and_pins_official_backend(tmp_path, g722):
    config = yaml.safe_load((ROOT / "tests/minimal_speaker_only.yaml").read_text())
    config["external_components"][0]["source"] = str(ROOT / "esphome/components")
    config["voip_stack"]["audio"]["codecs"][0].pop("tx")
    if g722:
        config["voip_stack"]["audio"]["codecs"].append({"codec": "g722", "rx": [pcm(16000, 20)]})
    path = tmp_path / "codec.yaml"
    path.write_text(yaml.safe_dump(config, sort_keys=False))
    result = subprocess.run(
        [os.environ.get("ESPHOME_PYTHON", sys.executable), "-m", "esphome", "compile", str(path), "--only-generate"],
        capture_output=True, text=True, timeout=60,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    defines = next(tmp_path.rglob("defines.h")).read_text()
    assert "#define USE_ESPHOME_VOIP_STACK_PCM" in defines
    assert ("#define USE_ESPHOME_VOIP_STACK_G722" in defines) == g722
    assert "#define USE_ESPHOME_VOIP_STACK_OPUS" not in defines
    manifests = [p.read_text() for p in tmp_path.rglob("idf_component.yml")]
    assert any("espressif/esp_audio_codec" in text for text in manifests) == g722
    if g722:
        dependencies = [yaml.safe_load(text).get("dependencies", {}) for text in manifests]
        version = next(deps["espressif/esp_audio_codec"] for deps in dependencies if "espressif/esp_audio_codec" in deps)
        assert (version.get("version") if isinstance(version, dict) else version) == "2.5.0"
