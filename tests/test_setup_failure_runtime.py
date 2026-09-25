"""Exercise production allocation/task cleanup and retry with counted resources."""

from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize(
    "microphone,speaker,opus",
    [
        (True, True, False),
        (True, True, True),
        (True, False, False),
        (False, True, False),
    ],
)
def test_partial_setup_ownership_and_retry(tmp_path, microphone, speaker, opus):
    source = (ROOT / "esphome/components/voip_stack/voip_stack.cpp").read_text()
    methods = []
    for sig in [
        "void VoipStack::cleanup_partial_setup_",
        "bool VoipStack::allocate_setup_buffers_",
        "bool VoipStack::start_runtime_tasks_",
    ]:
        start = source.index(sig)
        methods.append(source[start : source.index("\n}", start) + 2])
    code = (ROOT / "tests/fixtures/setup_failure.cpp").read_text()
    code = code.replace("// PRODUCTION_METHODS", "\n".join(methods))
    cpp = tmp_path / "setup.cpp"
    cpp.write_text(code)
    defines = []
    for enabled, name in [(microphone, "MIC"), (speaker, "SPEAKER"), (opus, "OPUS")]:
        if enabled:
            defines.append("-DUSE_ESPHOME_VOIP_STACK_" + name)
    exe = tmp_path / "setup"
    subprocess.run(
        [
            "g++",
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-parameter",
            "-fsanitize=address,undefined",
            *defines,
            str(cpp),
            "-o",
            str(exe),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    subprocess.run([str(exe)], check=True, capture_output=True, text=True)
