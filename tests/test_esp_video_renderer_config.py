"""Validate independent and VoIP-connected software rendering with ESPHome."""

import os
import re
import textwrap
from pathlib import Path
import subprocess
import sys

import pytest
import yaml

ROOT = Path(__file__).resolve().parents[1]

BASE = """
esphome:
  name: jpeg-renderer-test
  on_boot:
    priority: -100
    then:
      - esp_video_renderer.start: renderer
esp32:
  board: esp32-s3-devkitc-1
  variant: esp32s3
  framework:
    type: esp-idf
psram:
  mode: octal
  speed: 80MHz
logger:
external_components:
  - source: SOURCE
    components: [esp_video_rendererCOMPONENTS]
spi:
  clk_pin: GPIO4
  mosi_pin: GPIO2
display:
  - platform: mipi_spi
    id: lcd
    model: ST7789V
    cs_pin: GPIO5
    dc_pin: GPIO6
    reset_pin: GPIO7
    dimensions:
      width: 240
      height: 320
    auto_clear_enabled: false
lvgl:
  rotation: 90
  on_boot:
    - lambda: id(renderer).attach_video_container(id(video_surface));
  pages:
    - id: video_page
      widgets:
        - obj:
            id: video_surface
            width: 320
            height: 240
esp_video_renderer:
  id: renderer
RENDERER
"""


def generate(tmp_path, extra="", renderer="", components=""):
    text = BASE.replace("SOURCE", str(ROOT / "esphome/components"))
    text = text.replace("COMPONENTS", components).replace("RENDERER", renderer) + extra
    config = tmp_path / "renderer.yaml"
    config.write_text(text)
    result = subprocess.run([
        os.environ.get("ESPHOME_PYTHON", sys.executable), "-m", "esphome",
        "compile", str(config), "--only-generate",
    ], capture_output=True, text=True, timeout=90)
    return result


def test_renderer_standalone_has_no_sip_dependency(tmp_path):
    result = generate(tmp_path)
    assert result.returncode == 0, result.stdout + result.stderr
    code = next(tmp_path.rglob("main.cpp")).read_text()
    defines = next(tmp_path.rglob("defines.h")).read_text()
    assert "esp_video_renderer::EspVideoRenderer();" in code
    assert "voip_stack::VoipStack" not in code
    assert "USE_ESP_VIDEO_RENDERER_VOIP" not in defines
    assert not list(tmp_path.rglob("sip_transport.cpp"))
    manifest = yaml.safe_load(next(tmp_path.rglob("idf_component.yml")).read_text())
    assert manifest["dependencies"]["espressif/esp_jpeg"]["version"] == "1.3.2"
    assert "espressif/esp_new_jpeg" not in manifest["dependencies"]


def test_renderer_voip_sink_is_explicit(tmp_path):
    result = generate(tmp_path, """
wifi:
  ssid: test-network
  password: test-password
i2s_audio:
  i2s_bclk_pin: GPIO9
  i2s_lrclk_pin: GPIO10
microphone:
  - platform: i2s_audio
    id: mic
    adc_type: external
    i2s_din_pin: GPIO11
    sample_rate: 16000
    bits_per_sample: 16bit
voip_stack:
  id: phone
  microphone_source:
    microphone: mic
  video:
    codec: jpeg
    sink: renderer_sink
    width: 320
    height: 240
    framerate: 2
""", "  voip:\n    id: renderer_sink\n", ", voip_stack")
    assert result.returncode == 0, result.stdout + result.stderr
    code = next(tmp_path.rglob("main.cpp")).read_text()
    assert "esp_video_renderer::EspVideoRendererSink(renderer);" in code
    assert "phone->set_video_sink(renderer_sink)" in code


@pytest.mark.parametrize("setting", ["  width: 319\n", "  width: 640\n  height: 640\n"])
def test_invalid_geometry_is_rejected_before_build(tmp_path, setting):
    result = generate(tmp_path, renderer=setting)
    assert result.returncode != 0
    assert "multiple of 8" in result.stdout or "at most 640x480" in result.stdout


def test_renderer_codegen_applies_explicit_task_and_memory_choices(tmp_path):
    result = generate(tmp_path, renderer="""  task_core: 1
  task_priority: 7
  task_stack_size: 12kB
  task_stack_in_psram: false
  buffer_in_psram: false
""")
    assert result.returncode == 0, result.stdout + result.stderr
    code = next(tmp_path.rglob("main.cpp")).read_text()
    for call in ("set_task_core(1)", "set_task_priority(7)", "set_task_stack_size(12000)",
                 "set_task_stack_in_psram(false)", "set_buffer_in_psram(false)"):
        assert call in code


@pytest.mark.parametrize("setting", ["  task_core: 2\n", "  task_priority: 25\n", "  task_stack_size: 8193\n"])
def test_renderer_rejects_invalid_task_geometry(tmp_path, setting):
    result = generate(tmp_path, renderer=setting)
    assert result.returncode != 0


def test_renderer_requires_declared_psram_for_external_placement(tmp_path, monkeypatch):
    monkeypatch.setitem(globals(), "BASE", BASE.replace("psram:\n  mode: octal\n  speed: 80MHz\n", ""))
    result = generate(tmp_path)
    assert result.returncode != 0
    assert "PSRAM placement requires" in result.stdout + result.stderr
    result = generate(tmp_path, renderer="  task_stack_in_psram: false\n  buffer_in_psram: false\n")
    assert result.returncode == 0, result.stdout + result.stderr


def test_renderer_expanded_component_roundtrip_preserves_any_core(tmp_path):
    result = generate(tmp_path)
    assert result.returncode == 0, result.stdout + result.stderr
    config = tmp_path / "renderer.yaml"
    expanded = subprocess.run([
        os.environ.get("ESPHOME_PYTHON", sys.executable), "-m", "esphome", "config", str(config),
    ], capture_output=True, text=True, timeout=60)
    assert expanded.returncode == 0, expanded.stdout + expanded.stderr
    assert "task_core: any" in expanded.stdout
    block = re.search(r"(?m)^esp_video_renderer:\n(?:[ \t].*\n|\n)*", expanded.stdout)
    component = yaml.safe_load(block.group())["esp_video_renderer"]
    component.pop("id")
    regenerated = generate(tmp_path, renderer=textwrap.indent(yaml.safe_dump(component), "  "))
    assert regenerated.returncode == 0, regenerated.stdout + regenerated.stderr
