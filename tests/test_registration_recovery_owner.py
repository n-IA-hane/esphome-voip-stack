"""Enabled lines retry remote rejection without an HA credential re-push."""
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def registration_recovery(tmp_path_factory):
    folder = tmp_path_factory.mktemp("registration-recovery")
    include = folder / "esphome/core/defines.h"
    include.parent.mkdir(parents=True)
    include.write_text("")
    source = ROOT / "esphome/components/voip_stack"
    binary = folder / "recovery"
    subprocess.run([
        "g++", "-std=c++17", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-fno-pie", "-no-pie", "-DUSE_ESPHOME_VOIP_REGISTRATION", "-I", str(folder), "-I", str(source),
        str(ROOT / "tests/fixtures/registration_recovery_owner.cpp"), str(source / "sip_registration.cpp"),
        "-lcrypto", "-o", str(binary),
    ], check=True)
    return binary


@pytest.mark.parametrize("transport", ["udp", "tcp"])
@pytest.mark.parametrize("status", [401, 403, 404, 407])
@pytest.mark.parametrize("mode", ["remote-enable", "credentials", "disable", "remove"])
def test_rejection_recovery_preserves_local_intent(registration_recovery, transport, status, mode):
    subprocess.run([registration_recovery, str(status), transport, mode], check=True, timeout=5)
