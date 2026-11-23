"""One username on two independent PBX flows keeps status and pickup scoped."""

import subprocess

import pytest

from test_blf_pickup_wire import blf_pickup_source
from test_refer_dialog_runtime import ROOT, SOURCE


@pytest.fixture(scope="module")
def multi_account_blf_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("blf-multiple-accounts")
    core = folder / "esphome/core"
    core.mkdir(parents=True)
    (core / "defines.h").write_text("")
    source = folder / "multiple.cpp"
    source.write_text(blf_pickup_source().split("int main(int argc,char**argv)")[0] +
                      (ROOT / "tests/fixtures/blf/multiple_accounts.cpp").read_text())
    binary = folder / "multiple"
    compiled = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-Wno-misleading-indentation", "-g", "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
        "-DUSE_ESPHOME_VOIP_REGISTRATION", "-DUSE_ESPHOME_VOIP_MWI", "-DUSE_ESPHOME_VOIP_BLF",
        "-DUSE_ESPHOME_VOIP_CONTACT_METADATA", "-I", str(folder), "-I", str(SOURCE),
        "-I", str(SOURCE.parents[2]), str(source), str(SOURCE / "sip_mwi.cpp"),
        str(SOURCE / "sip_blf.cpp"), str(SOURCE / "sip_registration.cpp"), "-lcrypto", "-o", str(binary)],
        capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stderr
    return binary


@pytest.mark.parametrize("transport", ["udp", "tcp"])
@pytest.mark.parametrize("selected_line", ["office", "other", "direct"])
def test_two_pbx_accounts_share_username_without_status_or_pickup_leak(
    multi_account_blf_binary, transport, selected_line,
):
    result = subprocess.run([multi_account_blf_binary, transport, selected_line],
                            capture_output=True, text=True, timeout=8)
    assert result.returncode == 0, result.stdout + result.stderr
