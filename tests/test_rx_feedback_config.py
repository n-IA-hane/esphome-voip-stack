"""Output feedback is a derived capability of the actual configured source."""

import ast
import os
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

import pytest
from esphome.core import ID

ROOT = Path(__file__).resolve().parents[1]


class Config(dict):
    def __init__(self, data, paths):
        super().__init__(data)
        self.paths = paths

    def get_path_for_id(self, value):
        return self.paths[value.id] + ["id"]

    def get_config_for_path(self, path):
        value = self
        for item in path:
            value = value[item]
        return value


def capability(config, identifier):
    source = (ROOT / "esphome/components/voip_stack/__init__.py").read_text()
    function = next(node for node in ast.parse(source).body
                    if isinstance(node, ast.FunctionDef) and node.name == "_speaker_output_feedback_provider")
    namespace = {"fv": SimpleNamespace(full_config=SimpleNamespace(get=lambda: config)), "CONF_SPEAKER": "speaker"}
    exec(compile(ast.Module(body=[function], type_ignores=[]), "native-feedback", "exec"), namespace)
    return namespace[function.name](identifier)


def ref(name):
    return ID(name, is_declaration=False)


@pytest.mark.parametrize("leaf", ["esp_audio_stack", "i2s_audio", "custom_provider"])
def test_composite_source_feedback_and_leaf_error_guard(leaf):
    top, source, output = ref("top"), ref("source"), ref("output")
    config = Config({"voip_stack": {"speaker": top}, "speaker": [
        {"platform": "resampler", "id": ID("top", is_declaration=True), "output_speaker": source},
        {"platform": "mixer", "id": ID("mixer", is_declaration=True), "output_speaker": output,
         "source_speakers": [{"id": ID("source", is_declaration=True)}]},
        {"platform": leaf, "id": ID("output", is_declaration=True)},
    ]}, {"top": ["speaker", 0], "source": ["speaker", 1, "source_speakers", 0], "output": ["speaker", 2]})
    provider = capability(config, top)
    assert (provider.id if provider is not None else None) == ("output" if leaf != "custom_provider" else None)
    # This hidden derived ID must not turn into a second writer on revalidation.
    config["voip_stack"]["_speaker_output_feedback"] = provider
    again = capability(config, top)
    assert (again.id if again is not None else None) == (provider.id if provider is not None else None)


def test_shared_top_and_unknown_provider_retain_legacy_fallback():
    top = ref("top")
    config = Config({"voip_stack": {"speaker": top}, "other_writer": {"speaker": ref("top")},
                     "speaker": [{"platform": "i2s_audio", "id": ID("top", is_declaration=True)}]},
                    {"top": ["speaker", 0]})
    assert capability(config, top) is None
    del config["other_writer"]
    assert capability(config, top).id == "top"
    config["speaker"][0]["platform"] = "custom_provider"
    assert capability(config, top) is None


def test_cycle_cannot_create_a_runtime_readiness_graph():
    config = Config({"voip_stack": {"speaker": ref("top")}, "speaker": [
        {"platform": "resampler", "id": ID("top", is_declaration=True), "output_speaker": ref("next")},
        {"platform": "resampler", "id": ID("next", is_declaration=True), "output_speaker": ref("top")},
    ]}, {"top": ["speaker", 0], "next": ["speaker", 1]})
    assert capability(config, ref("top")) is None


def test_direct_audio_stack_shared_parent_credit_is_not_own_source_proof():
    config = Config({"voip_stack": {"speaker": ref("one")}, "speaker": [
        {"platform": "esp_audio_stack", "id": ID("one", is_declaration=True), "esp_audio_stack_id": ref("audio")},
        {"platform": "esp_audio_stack", "id": ID("other", is_declaration=True), "esp_audio_stack_id": ref("audio")},
    ]}, {"one": ["speaker", 0]})
    assert capability(config, ref("one")) is None


def test_public_composite_codegen_uses_leaf_status_and_source_callback(tmp_path):
    config = (ROOT / "tests/minimal_speaker_only.yaml").read_text().replace(
        "../esphome/components", str(ROOT / "esphome/components"))
    config = config.replace("voip_stack:\n", """  - platform: mixer
    id: audio_mixer
    output_speaker: spk_main
    source_speakers:
      - id: voip_source
      - id: other_source
  - platform: resampler
    id: voip_input
    output_speaker: voip_source
    sample_rate: 48000
    bits_per_sample: 16
voip_stack:
""")
    config = config.replace("  speaker: spk_main", "  speaker: voip_input")
    path = tmp_path / "feedback.yaml"
    path.write_text(config)
    run = subprocess.run([os.environ.get("ESPHOME_PYTHON", sys.executable), "-m", "esphome", "compile",
                          str(path), "--only-generate"], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr
    generated = next(tmp_path.rglob("main.cpp")).read_text()
    assert "phone->set_speaker(voip_input)" in generated
    assert "phone->set_speaker_output_feedback(spk_main)" in generated
