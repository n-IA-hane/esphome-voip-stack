"""Independent wire oracles preserved from the two quarantine RED cases."""
import resource
import subprocess
import pytest
from test_refer_dialog_runtime import SOURCE, ROOT


@pytest.fixture(scope="module")
def adversarial_blf_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("blf-adversarial")
    core = folder / "esphome/core"
    core.mkdir(parents=True)
    (core / "defines.h").write_text("")
    binary = folder / "adversarial"
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g", "-fsanitize=address,undefined",
        "-fno-pie", "-no-pie", "-DUSE_ESPHOME_VOIP_REGISTRATION", "-DUSE_ESPHOME_VOIP_MWI", "-DUSE_ESPHOME_VOIP_BLF",
        "-I", str(folder), "-I", str(SOURCE), str(ROOT / "tests/fixtures/blf/adversarial.cpp"),
        str(SOURCE / "sip_mwi.cpp"), str(SOURCE / "sip_blf.cpp"), str(SOURCE / "sip_registration.cpp"),
        "-lcrypto", "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("scenario", ["version", "atomic", "reply-failure"])
def test_old_full_and_rejected_notify_never_commit_a_false_state(adversarial_blf_binary, scenario):
    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    result = subprocess.run([adversarial_blf_binary, scenario], capture_output=True, text=True, timeout=8, preexec_fn=no_core)
    assert result.returncode == 0, result.stdout + result.stderr
