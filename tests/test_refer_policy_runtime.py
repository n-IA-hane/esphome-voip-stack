"""Configured REFER budgets and renewable event leases on real UDP/TCP sockets."""
from pathlib import Path
import subprocess

import pytest

from test_refer_dialog_runtime import ROOT
from test_refer_wire_runtime import wire_source


@pytest.fixture(scope="module")
def refer_policy_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("refer-configured-policy")
    fixtures = Path(__file__).parent / "fixtures"
    peers = (fixtures / "refer_wire_main.cpp").read_text()
    peers = peers[:peers.index("int main(")]
    source = folder / "refer_configured_policy.cpp"
    source.write_text(wire_source() + "\n" + peers + "\n" +
                      (fixtures / "refer_configured_policy.cpp").read_text())
    binary = folder / "refer_configured_policy"
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-O1", "-g",
                             "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-I", str(ROOT),
                             str(source), "-o", str(binary)], capture_output=True, text=True, timeout=90)
    (folder / "build.log").write_text(result.stdout + result.stderr)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("original_tcp,target_tcp", [(0, 0), (0, 1), (1, 0), (1, 1)])
@pytest.mark.parametrize("scenario,value", [
    ("slow-answer", 45000), ("slow-answer", 90000), ("wrapped-answer", 90000),
    ("custom-timeout", 45000), ("custom-timeout", 90000),
    ("refresh", 0), ("refresh-ended", 0), ("invalid-refresh", 0), ("expire", 0),
    ("unsubscribe", 0), ("silent-cancel", 0),
])
def test_shared_outgoing_policy_and_independent_refer_subscription(
    refer_policy_binary, original_tcp, target_tcp, scenario, value
):
    result = subprocess.run([refer_policy_binary, scenario, str(original_tcp), str(target_tcp), str(value)],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
