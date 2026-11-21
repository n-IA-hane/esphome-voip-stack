"""Public roster destinations and labels use the production identity resolver."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "esphome/components/voip_stack"


def test_public_route_precedence_and_literal_utf8_names(tmp_path):
    binary = tmp_path / "roster-contact"
    result = subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_ESP32",
        "-DUSE_ESPHOME_VOIP_CONTACT_METADATA", "-I", str(SOURCE),
        str(ROOT / "tests/fixtures/roster_contact.cpp"), str(SOURCE / "roster_contact.cpp"),
        "-o", str(binary),
    ], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr
