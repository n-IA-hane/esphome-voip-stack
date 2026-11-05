"""Authenticate each new subscription exchange with a fresh peer challenge."""

from pathlib import Path
import resource
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "esphome/components/voip_stack"


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


@pytest.fixture(scope="module")
def transaction_auth_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("mwi-transaction-auth")
    defines = folder / "esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    harness = (ROOT / "tests/test_sip_mwi_engine.cpp").read_text()
    code = harness[:harness.index("int main(")]
    code += (ROOT / "tests/fixtures/mwi_transaction_auth.cpp").read_text()
    source = folder / "transaction_auth.cpp"
    source.write_text(code)
    binary = folder / "transaction_auth"
    built = subprocess.run(
        [
            "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-fno-pie", "-no-pie", "-DUSE_ESPHOME_VOIP_REGISTRATION",
            "-DUSE_ESPHOME_VOIP_MWI", "-DUSE_ESPHOME_VOIP_BLF",
            "-I", str(folder), "-I", str(SOURCE), str(source),
            str(SOURCE / "sip_mwi.cpp"), str(SOURCE / "sip_blf.cpp"),
            str(SOURCE / "sip_registration.cpp"), "-lcrypto", "-o", str(binary),
        ],
        capture_output=True, text=True, preexec_fn=no_core,
    )
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize("package", ["mwi", "blf"])
@pytest.mark.parametrize("transport", ["udp", "tcp"])
@pytest.mark.parametrize(
    "scenario",
    ["refresh86", "refresh94", "same-nonce", "unsubscribe-bound", "unsubscribe-unbound", "dual", "bare401"],
)
def test_fresh_subscription_authentication(transaction_auth_binary, scenario, transport, package):
    result = subprocess.run(
        [transaction_auth_binary, scenario, transport, package],
        capture_output=True, text=True, timeout=8, preexec_fn=no_core,
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout == f"PASS {scenario} {transport} {package}\n"
