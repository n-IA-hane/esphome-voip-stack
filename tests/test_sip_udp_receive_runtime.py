"""Real UDP boundaries reach the production receive adapter without truncation."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_truncated_datagram_is_not_a_complete_sip_record(tmp_path):
    binary = tmp_path / "sip-udp-receive"
    result = subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
        "-I", str(ROOT / "esphome/components/voip_stack"),
        str(ROOT / "tests/fixtures/sip_udp_receive.cpp"), "-o", str(binary),
    ], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr
