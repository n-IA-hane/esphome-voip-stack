"""The real component schema rejects an unofferrable sendrecv audio catalog."""

import os
import importlib.util
from pathlib import Path
import subprocess
import sys

import pytest
import yaml
from esphome import config_validation as cv

ROOT = Path(__file__).resolve().parents[1]


def declared(rate, duration):
    return {"sample_rate": rate, "frame_ms": duration}


@pytest.mark.parametrize("codec,transmit,receive,compatible", [
    ("pcm", declared(16000, 20), declared(16000, 10), True),
    ("pcm", {**declared(16000, 20), "pcm_format": "s24le"},
     {**declared(16000, 10), "pcm_format": "s24le_in_s32"}, True),
    ("pcm", declared(16000, 20), {**declared(16000, 10), "pcm_format": "s24le"}, False),
    ("opus", declared(16000, 20), declared(48000, 10), True),
])
def test_common_mapping_uses_wire_identity_not_packet_or_storage_geometry(monkeypatch, codec, transmit, receive, compatible):
    spec = importlib.util.spec_from_file_location("duplex_codec_schema", ROOT / "esphome/components/voip_stack/__init__.py")
    module = importlib.util.module_from_spec(spec)
    monkeypatch.setitem(sys.modules, spec.name, module)
    spec.loader.exec_module(module)
    audio = module.PHONE_CODECS_SCHEMA({"codecs": [{"codec": codec, "tx": [transmit], "rx": [receive]}]})
    if compatible:
        module._validate_rtp_duplex_mapping(audio)
    else:
        with pytest.raises(cv.Invalid, match="Bidirectional SIP audio requires"):
            module._validate_rtp_duplex_mapping(audio)


@pytest.mark.parametrize("direction", ["bidirectional", "microphone-only", "speaker-only"])
@pytest.mark.parametrize("common", [False, True], ids=["disjoint", "truthful-alternatives"])
def test_directional_rtp_catalog_is_validated_after_real_endpoint_selection(tmp_path, direction, common):
    config = yaml.safe_load((ROOT / "tests/minimal_speaker_only.yaml").read_text())
    mic = yaml.safe_load((ROOT / "tests/minimal_no_text.yaml").read_text())
    config["external_components"][0]["source"] = str(ROOT / "esphome/components")
    if direction != "speaker-only":
        mic["i2s_audio"][0]["i2s_bclk_pin"] = "GPIO4"
        mic["i2s_audio"][0]["i2s_lrclk_pin"] = "GPIO5"
        mic["microphone"][0]["i2s_din_pin"] = "GPIO6"
        mic["microphone"][0]["sample_rate"] = 16000
        mic["microphone"][0]["bits_per_sample"] = "16bit"
        config["i2s_audio"] += mic["i2s_audio"]
        config["microphone"] = mic["microphone"]
        config["voip_stack"]["microphone_source"] = mic["voip_stack"]["microphone_source"]
    if direction == "microphone-only":
        config.pop("speaker")
        config["voip_stack"].pop("speaker")
    tx = [declared(16000, 20), declared(16000, 10)]
    rx = [declared(48000, 10)]
    if common:
        tx += [declared(48000, 10)]
        rx += [declared(48000, 20), declared(16000, 20), declared(16000, 10)]
    config["voip_stack"]["audio"] = {"codecs": [{"codec": "pcm", "tx": tx, "rx": rx}]}
    path = tmp_path / "direction.yaml"
    path.write_text(yaml.safe_dump(config, sort_keys=False))
    result = subprocess.run([os.environ.get("ESPHOME_PYTHON", sys.executable), "-m", "esphome", "config", str(path)],
                            capture_output=True, text=True, timeout=60)
    if direction == "bidirectional" and not common:
        assert result.returncode != 0
        assert "Bidirectional SIP audio requires at least one RTP mapping" in result.stdout + result.stderr
    else:
        assert result.returncode == 0, result.stdout + result.stderr
