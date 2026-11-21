"""Video payload lifetime and directional namespaces use compiled production."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "esphome/components/voip_stack"


def test_renumbered_answer_and_retired_payloads_survive_video_removal(tmp_path):
    for name, contents in {
        "esphome/core/defines.h": "",
        "esphome/core/hal.h": "#include <cstdint>\nnamespace esphome { uint32_t millis(); }",
        "esp_system.h": "#include <cstdint>\nuint32_t esp_random();",
        "lwip/sockets.h": "#include <arpa/inet.h>\n#define inet_ntoa_r(a,b,n) inet_ntop(AF_INET,&(a),b,n)",
    }.items():
        header = tmp_path / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text(contents + "\n")
    binary = tmp_path / "video-payload-contract"
    result = subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-ffunction-sections",
        "-fdata-sections", "-Wl,--gc-sections", "-DUSE_ESP32", "-DUSE_ESPHOME_VOIP_SIP_TRANSPORT",
        "-DUSE_ESPHOME_VOIP_STACK_VIDEO", "-DUSE_ESPHOME_VOIP_STACK_VIDEO_H264",
        "-I", str(tmp_path), "-I", str(ROOT), "-I", str(SOURCE),
        str(SOURCE / "sip_message.cpp"), str(SOURCE / "media_negotiator.cpp"),
        str(ROOT / "tests/fixtures/video_payload_contract.cpp"), "-o", str(binary),
    ], capture_output=True, text=True, timeout=45)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr
