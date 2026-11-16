"""Generate complete bidirectional video owners with real optional adapters."""

import os
from pathlib import Path
import subprocess
import sys

import pytest
import yaml

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("codec", ["jpeg", "h264"])
@pytest.mark.parametrize("receive", [False, True], ids=["source-only", "source-and-renderer"])
def test_p4_sources_and_renderer_use_official_display_api(tmp_path, codec, receive):
    camera = os.environ.get("ESP_VIDEO_CAMERA_SOURCE")
    if not camera:
        pytest.skip("Set ESP_VIDEO_CAMERA_SOURCE to the real camera checkout")
    renderer = Path(os.environ.get("VIDEO_RENDERER_SOURCE", ROOT.parent / "esphome-video-renderer")).resolve()
    width, height, rx_width, rx_height, fps = (800, 800, 800, 800, 15) if codec == "jpeg" else (400, 400, 352, 288, 10)
    source = "esp_jpeg_video_source" if codec == "jpeg" else "esp_h264_video_source"
    renderer_config = f"""p4_video_renderer:
  id: renderer
  codec: {codec}
  width: {rx_width}
  height: {rx_height}
  framerate: {fps}
  max_decode_width: {rx_width}
  max_decode_height: {rx_height}
  display_id: screen
  display_rotation: 270
""" if receive else ""
    mipi = Path(os.environ.get("MIPI_COMPONENTS_SOURCE", ROOT.parent / "esphome-intercom/esphome/components"))
    driver_source = f"  - source: {mipi}\n    components: [mipi_dsi]\n" if receive else ""
    config = tmp_path / "p4.yaml"
    sink_config = "    sink: renderer" if receive else ""
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
    components: [voip_stack, {source}]
  - source: {renderer / "esphome/components"}
    components: [video_renderer{", p4_video_renderer" if receive else ""}]
  - source: {Path(camera).resolve() / 'components'}
    components: [esp_video_camera]
{driver_source}i2c:
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
{renderer_config}
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
{sink_config}
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
    assert ("USE_MIPI_DSI_RGB565_OWNED_TRANSFORM" in defines) is receive
    assert "USE_ESPHOME_VOIP_STACK_VIDEO_RENDERER" in defines if receive else "USE_ESPHOME_VOIP_STACK_VIDEO_RENDERER" not in defines
    assert f"#define USE_ESPHOME_VOIP_STACK_VIDEO_{codec.upper()}" in defines
    assert f"USE_ESPHOME_VOIP_STACK_VIDEO_{'H264' if codec == 'jpeg' else 'JPEG'}" not in defines
    assert "phone->set_video_source(encoded_source)" in main
    assert f"encoded_source->set_framerate({fps})" in main
    if receive:
        assert "voip_stack::P4RendererVideoSink(renderer)" in main
        assert "phone->set_video_sink(" in main
        for call in [f"renderer->set_width({rx_width})", f"renderer->set_height({rx_height})", f"renderer->set_framerate({fps})"]:
            assert call in main
    else:
        assert "P4VideoRenderer" not in main
    assert "set_display_rotation" not in main
    if codec == "h264":
        dependency = yaml.safe_load((generated / "idf_component.yml").read_text())[
            "dependencies"
        ]["espressif/esp_h264"]
        selected = Path(dependency.get("path", dependency.get("override_path")))
        assert subprocess.check_output(
            ["git", "-C", str(selected), "rev-parse", "HEAD"], text=True
        ).strip() == "176cda67716c24deef0e76e4451048981e154bd4"
        assert not subprocess.check_output(
            ["git", "-C", str(selected), "status", "--porcelain"], text=True
        )
        sdkconfig = (tmp_path / "build/sdkconfig.p4-video-contract").read_text()
        assert "CONFIG_ESP_H264_HW_ENCODER_PREFER_DB_SPIRAM=y" in sdkconfig
    assert "USE_ESPHOME_VOIP_DIRECTORY" not in defines and "USE_ESPHOME_VOIP_BLF" not in defines
