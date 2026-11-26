"""REFER refresh uses production dialog target routing and immutable Route sets."""
from pathlib import Path
import subprocess

import pytest

from test_refer_dialog_runtime import ROOT, SOURCE
from test_sip_registration_transport import registration_transport_source


@pytest.fixture(scope="module")
def refer_target_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("refer-subscribe-target")
    defines = folder / "esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    fixtures = Path(__file__).parent / "fixtures"
    peers = (fixtures / "refer_wire_main.cpp").read_text().split("std::string begin(")[0]
    accounts = (fixtures / "registration_transport.cpp").read_text().split("int main(")[0]
    source = folder / "target.cpp"
    source.write_text(registration_transport_source(full_invite=True) + "\n" + peers +
                      "\n" + accounts + "\n" + (fixtures / "refer_subscription_target.cpp").read_text())
    binary = folder / "target"
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-O1", "-g",
                             "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                             "-DUSE_ESPHOME_VOIP_REGISTRATION", "-I", str(folder), "-I", str(ROOT),
                             "-I", str(SOURCE), str(source), str(SOURCE / "sip_registration.cpp"),
                             "-lcrypto", "-o", str(binary)], capture_output=True, text=True, timeout=90)
    (folder / "build.log").write_text(result.stdout + result.stderr)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("original_tcp,target_tcp", [(0, 0), (0, 1), (1, 0), (1, 1)])
@pytest.mark.parametrize("route", ["none", "loose", "strict", "invalid"])
def test_subscribe_contact_refresh_preserves_route_and_candidate(
    refer_target_binary, original_tcp, target_tcp, route
):
    result = subprocess.run([refer_target_binary, str(original_tcp), str(target_tcp), route],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
