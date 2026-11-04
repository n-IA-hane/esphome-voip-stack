"""Generate complete bidirectional video owners with real optional adapters."""

import os
from pathlib import Path
import shutil
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("codec", ["jpeg", "h264"])
@pytest.mark.parametrize("owned", [False, True], ids=["legacy-mipi", "owned-mipi"])
def test_p4_bidirectional_sources_and_actual_mipi_capability(tmp_path, codec, owned):
    camera = os.environ.get("ESP_VIDEO_CAMERA_SOURCE")
    intercom = os.environ.get("ESPHOME_MIPI_SOURCE")
    if not camera or not intercom:
        pytest.skip("Set ESP_VIDEO_CAMERA_SOURCE and ESPHOME_MIPI_SOURCE to real checkouts")
    intercom = Path(intercom).resolve()
    mipi = intercom / "esphome/components"
    if not owned:
        # Preserve all old-provider behavior. Only its exact committed Python
        # manifest lacks the new capability; Native's C++ two-surface witness
        # separately executes the old provider interface.
        mipi = tmp_path / "legacy"
        shutil.copytree(intercom / "esphome/components/mipi_dsi", mipi / "mipi_dsi",
                        ignore=shutil.ignore_patterns("__pycache__"))
        shutil.copyfile(intercom / "tests/fixtures/mipi_legacy/display.py", mipi / "mipi_dsi/display.py")
    width, height, rx_width, rx_height, fps = (800, 800, 800, 800, 15) if codec == "jpeg" else (400, 400, 352, 288, 10)
    source = "esp_jpeg_video_source" if codec == "jpeg" else "esp_h264_video_source"
    config = tmp_path / "p4.yaml"
    config.write_text(f'''esphome:
  name: p4-video-contract
  build_path: {tmp_path / 'build'}
esp32:
  board: esp32-p4-evboard
  framework:
    type: esp-idf
network:
psram:
esp_ldo:
  - channel: 3
    voltage: 2.5V
logger:
external_components:
  - source: {ROOT / 'esphome/components'}
    components: [voip_stack, p4_video_renderer, {source}]
  - source: {Path(camera).resolve() / 'components'}
    components: [esp_video_camera]
  - source: {mipi}
    components: [mipi_dsi]
i2c:
  id: camera_i2c
  sda: GPIO7
  scl: GPIO8
i2s_audio:
  id: audio_bus
  i2s_bclk_pin: GPIO9
  i2s_lrclk_pin: GPIO10
microphone:
  - platform: i2s_audio
    id: mic
    adc_type: external
    i2s_audio_id: audio_bus
    i2s_din_pin: GPIO11
    sample_rate: 16000
    bits_per_sample: 16bit
display:
  - platform: mipi_dsi
    id: screen
    model: WAVESHARE-10.1-DSI-TOUCH-A
lvgl:
  displays: [screen]
  rotation: 270
esp_video_camera:
  id: p4_camera
  i2c_id: camera_i2c
  device: {'jpeg' if codec == 'jpeg' else 'csi'}
  resolution: 800x800
  max_framerate: {fps}
  rotation: 270
{source}:
  id: encoded_source
  camera_id: p4_camera
  width: {width}
  height: {height}
  framerate: {fps}
p4_video_renderer:
  id: renderer
  codec: {codec}
  width: {rx_width}
  height: {rx_height}
  framerate: {fps}
  max_decode_width: {rx_width}
  max_decode_height: {rx_height}
  display_id: screen
  display_rotation: 270
voip_stack:
  id: phone
  ha_integration: false
  audio:
    codecs:
      - codec: pcm
        tx: [{{sample_rate: 16000, frame_ms: 20}}]
  microphone_source:
    microphone: mic
  video:
    codec: {codec}
    source: encoded_source
    sink: renderer
    width: {width}
    height: {height}
    framerate: {fps}
''')
    result = subprocess.run([os.environ.get("ESPHOME_PYTHON", sys.executable), "-m", "esphome",
                             "compile", "--only-generate", config], capture_output=True, text=True, timeout=90)
    assert result.returncode == 0, result.stdout + result.stderr
    generated = tmp_path / "build/src"
    main = (generated / "main.cpp").read_text()
    defines = (generated / "esphome/core/defines.h").read_text()
    assert ("#define USE_MIPI_DSI_RGB565_OWNED_TRANSFORM" in defines) is owned
    assert f"lvgl::ROTATION_{'HARDWARE' if owned else 'SOFTWARE'}" in main
    assert f"#define USE_ESPHOME_VOIP_STACK_VIDEO_{codec.upper()}" in defines
    assert f"USE_ESPHOME_VOIP_STACK_VIDEO_{'H264' if codec == 'jpeg' else 'JPEG'}" not in defines
    for call in ["phone->set_video_source(encoded_source)", "phone->set_video_sink(renderer)",
                 f"encoded_source->set_framerate({fps})", f"renderer->set_width({rx_width})",
                 f"renderer->set_height({rx_height})", f"renderer->set_framerate({fps})"]:
        assert call in main
    assert "USE_ESPHOME_VOIP_DIRECTORY" not in defines and "USE_ESPHOME_VOIP_BLF" not in defines
