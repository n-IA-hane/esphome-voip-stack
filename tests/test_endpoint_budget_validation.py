"""Validate real ESPHome configs and compare the budget with the C++ publisher."""

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys

import pytest
import yaml

from test_esp_video_renderer_config import BASE
from test_refer_dialog_runtime import block

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "esphome/components/voip_stack"
BOX_NAME = "Waveshare S3 Touch LCD 1.85C V2"
spec = importlib.util.spec_from_file_location(
    "endpoint_budget", SOURCE / "endpoint_budget.py"
)
budget = importlib.util.module_from_spec(spec)
spec.loader.exec_module(budget)


def fmt(rate, ms, pcm="s16le", codec="pcm"):
    return {
        "sample_rate": rate,
        "frame_ms": ms,
        "pcm_format": pcm,
        "channels": 1,
        "codec": codec,
    }


def full_audio():
    return {
        "tx": fmt(16000, 16),
        "tx_formats": [fmt(16000, 10), fmt(16000, 20)],
        "rx": fmt(48000, 10),
        "rx_formats": [
            fmt(32000, 16),
            fmt(16000, 10),
            fmt(16000, 16),
            fmt(16000, 32),
            fmt(16000, 20),
        ],
    }


def endpoint(audio=None):
    return {
        "ha_integration": {},
        "audio": full_audio() if audio is None else audio,
        "sip_port": 5060,
        "rtp_port": 40000,
        "transport": "udp",
        "extension": "101",
        "video": {"codec": "jpeg"},
    }


def generate(tmp_path, name=BOX_NAME, *, standalone=False, duplicate=False, auto=False):
    text = BASE.replace("SOURCE", str(ROOT / "esphome/components"))
    text = text.replace("COMPONENTS", ", voip_stack").replace(
        "RENDERER", "  voip:\n    id: renderer_sink\n"
    )
    config = yaml.safe_load(text)
    config["esphome"].update(
        {
            "name": "endpoint-budget-test",
            "friendly_name": name,
            "build_path": str(tmp_path / "build"),
        }
    )
    config["api"] = {"custom_services": True}
    config["wifi"] = {"ssid": "test-network", "password": "test-password"}
    config["i2s_audio"] = [
        {"id": "rx_bus", "i2s_bclk_pin": "GPIO9", "i2s_lrclk_pin": "GPIO10"},
        {"id": "tx_bus", "i2s_bclk_pin": "GPIO12", "i2s_lrclk_pin": "GPIO13"},
    ]
    config["microphone"] = [
        {
            "platform": "i2s_audio",
            "id": "mic",
            "adc_type": "external",
            "i2s_audio_id": "rx_bus",
            "i2s_din_pin": "GPIO11",
            "sample_rate": 16000,
            "bits_per_sample": "16bit",
        }
    ]
    config["speaker"] = [
        {
            "platform": "i2s_audio",
            "id": "spk",
            "dac_type": "external",
            "i2s_audio_id": "tx_bus",
            "i2s_dout_pin": "GPIO14",
            "sample_rate": 48000,
            "bits_per_sample": "16bit",
            "channel": "mono",
        }
    ]
    phone = endpoint()
    phone.update(
        {
            "id": "phone",
            "microphone_source": {"microphone": "mic"},
            "speaker": "spk",
            "video": {
                "codec": "jpeg",
                "sink": "renderer_sink",
                "width": 320,
                "height": 240,
                "framerate": 2,
            },
        }
    )
    if standalone:
        phone["ha_integration"] = False
    if duplicate:
        phone["audio"]["rx_formats"].extend([fmt(48000, 10), fmt(48000, 10)])
    if auto:
        phone["audio"]["tx"].update(
            {"sample_rate": "auto", "pcm_format": "auto", "channels": "auto"}
        )
    config["voip_stack"] = phone
    path = tmp_path / "device.yaml"
    path.write_text(yaml.safe_dump(config, allow_unicode=True))
    result = subprocess.run(
        [
            os.environ.get("ESPHOME_PYTHON", sys.executable),
            "-m",
            "esphome",
            "compile",
            str(path),
            "--only-generate",
        ],
        capture_output=True,
        text=True,
        timeout=90,
    )
    return result


def test_full_box_formats_and_auto_resolution_remain_valid(tmp_path):
    result = generate(tmp_path, auto=True)
    assert result.returncode == 0, result.stdout + result.stderr
    main = (tmp_path / "build/src/main.cpp").read_text()
    assert "set_video_sink(renderer_sink)" in main
    assert "add_supported_tx_audio_format(16000, 1, 1, 20, 0)" in main
    assert "add_supported_rx_audio_format(16000, 1, 1, 20, 0)" in main
    assert len(budget.endpoint_text(endpoint(), BOX_NAME, "192.168.1.192")) == 250
    assert len(budget.endpoint_text(endpoint(), BOX_NAME)) == 252


@pytest.mark.parametrize(
    ("extra", "accepted"),
    [("abc", True), ("abcd", False), ("😀😀😀", True), ("😀😀😀😀", False)],
)
def test_real_configuration_255_character_boundary(tmp_path, extra, accepted):
    result = generate(tmp_path, BOX_NAME + extra)
    assert (result.returncode == 0) is accepted, result.stdout + result.stderr
    if not accepted:
        assert "255-character limit" in result.stdout + result.stderr


def test_deduplicated_formats_fit_the_budget(tmp_path):
    result = generate(tmp_path, duplicate=True)
    assert result.returncode == 0, result.stdout + result.stderr


def test_oversize_standalone_preserves_all_formats(tmp_path):
    result = generate(
        tmp_path, BOX_NAME + " extra-long standalone SIP identity", standalone=True
    )
    assert result.returncode == 0, result.stdout + result.stderr
    main = (tmp_path / "build/src/main.cpp").read_text()
    assert "add_supported_rx_audio_format(16000, 1, 1, 20, 0)" in main
    assert (
        "USE_VOIP_HA_INTEGRATION"
        not in (tmp_path / "build/src/esphome/core/defines.h").read_text()
    )


@pytest.mark.parametrize("shape", ["pcm", "opus", "mic_only", "speaker_only"])
def test_budget_matches_actual_cpp_builder(tmp_path, shape):
    config = endpoint()
    if shape == "opus":
        config["audio"] = {
            "tx": fmt(16000, 20, codec="opus"),
            "tx_formats": [fmt(16000, 20)],
            "rx": fmt(16000, 20, codec="opus"),
            "rx_formats": [fmt(16000, 20)],
        }
    elif shape == "mic_only":
        config["audio"]["rx"] = None
    elif shape == "speaker_only":
        config["audio"]["tx"] = None
    source = (SOURCE / "voip_stack.cpp").read_text()
    code = r"""
#include <cstdio>
#include <cstring>
#include <string>
#include <iostream>
#include "esphome/components/voip_stack/sip_types.h"
using namespace esphome::voip_stack;
#define ESP_LOGW(...) ((void)0)
struct FriendlyName{std::string str()const{return "unused";}};
struct Application{FriendlyName get_friendly_name()const{return {};}}App;
enum class TransportType{TCP,UDP};
struct VoipStack {
 std::string device_name_, extension_; uint16_t sip_port_=5060,rtp_port_=40000;
 TransportType protocol_=TransportType::UDP;
 AudioFormatList tx_audio_formats_{},rx_audio_formats_{};
 std::string local_ip_string_()const{return "255.255.255.255";}
 const char*audio_capability_()const{
   return tx_audio_formats_.count&&rx_audio_formats_.count?"full_duplex":tx_audio_formats_.count?"mic_only":"speaker_only";
 }
 std::string build_endpoint_string_()const;
 static std::string audio_format_token_(const AudioFormat&);
 static std::string audio_rtp_format_token_(const AudioFormat&);
 static void append_audio_format_(AudioFormatList*, const AudioFormat&);
};
"""
    for marker in [
        "void VoipStack::append_audio_format_(",
        "std::string VoipStack::audio_format_token_(",
        "std::string VoipStack::audio_rtp_format_token_(",
        "std::string VoipStack::build_endpoint_string_(",
    ]:
        code += "\n" + block(source, marker)
    code += (
        "\nint main(){VoipStack s;s.tx_audio_formats_.count=0;s.rx_audio_formats_.count=0;s.device_name_="
        + json.dumps(BOX_NAME + "😀", ensure_ascii=False)
        + ';s.extension_="101";\n'
    )
    pcms = {
        "s16le": "S16LE",
        "s24le": "S24LE",
        "s24le_in_s32": "S24LE_IN_S32",
        "s32le": "S32LE",
    }
    for direction in ("tx", "rx"):
        primary = config["audio"][direction]
        if primary is None:
            continue
        # Use the production append primitive, including a duplicate primary.
        for item in (
            primary,
            primary,
            *config["audio"].get(direction + "_formats", []),
        ):
            code += f"VoipStack::append_audio_format_(&s.{direction}_audio_formats_, AudioFormat{{{item['sample_rate']},PcmFormat::{pcms[item['pcm_format']]},{item['channels']},{item['frame_ms']},AudioCodec::{item['codec'].upper()}}});\n"
    code += "std::cout<<s.build_endpoint_string_();}\n"
    cpp = tmp_path / "endpoint.cpp"
    cpp.write_text(code)
    binary = tmp_path / "endpoint"
    built = subprocess.run(
        [
            "g++",
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-DUSE_ESPHOME_VOIP_STACK_VIDEO_JPEG",
            "-I",
            str(ROOT),
            str(cpp),
            "-o",
            str(binary),
        ],
        capture_output=True,
        text=True,
    )
    assert built.returncode == 0, built.stderr
    actual = subprocess.check_output([binary], text=True)
    assert budget.endpoint_text(config, BOX_NAME + "😀") == actual
