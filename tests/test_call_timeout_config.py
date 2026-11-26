"""Public YAML omission and explicit zero reach the actual C++ timeout policy."""
from pathlib import Path
import re
import subprocess

import pytest

from test_refer_dialog_runtime import block
from test_sip_registration_config import ROOT, run_config


@pytest.mark.parametrize("options,outgoing,incoming", [
    ("", 0, 0),
    ("  ringing_timeout: 30s\n", 0, 30000),
    ("  ringing_timeout: 0s\n", 0, 0),
    ("  calling_timeout: 240s\n", 240000, 0),
    ("  calling_timeout: 0s\n  ringing_timeout: 30s\n", 0, 30000),
    ("  calling_timeout: 60s\n  ringing_timeout: 0s\n", 60000, 0),
])
def test_public_timeout_configuration_preserves_omission_and_explicit_zero(tmp_path, options, outgoing, incoming):
    result = run_config(tmp_path, options)
    assert result.returncode == 0, result.stdout + result.stderr
    generated = next(tmp_path.rglob("main.cpp")).read_text()
    calls = "\n".join(re.findall(r"phone->set_(?:calling|ringing)_timeout\(\d+\);", generated))
    header = (ROOT / "esphome/components/voip_stack/voip_stack.h").read_text()
    fields = header[header.rfind("  //", 0, header.index("  uint32_t calling_timeout_ms_")):
                    header.index("  // === SIP call state ===")]
    setters = "\n".join(block(header, f"void {name}(") for name in
                        ("set_calling_timeout", "set_ringing_timeout"))
    source = tmp_path / "policy.cpp"
    source.write_text("#include <cstdint>\n#include <cassert>\nstruct Policy {\n" + fields + setters +
                      "\n};\nint main(){Policy value;auto*phone=&value;\n" + calls +
                      f"\nassert(value.calling_timeout_ms_=={outgoing});\n"
                      f"assert(value.ringing_timeout_ms_=={incoming});\n}}\n")
    binary = Path(tmp_path) / "policy"
    compiled = subprocess.run(["g++", "-std=c++17", str(source), "-o", str(binary)],
                              capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stderr
    subprocess.run([binary], check=True)


@pytest.mark.parametrize("value,valid", [("2147483647ms", True), ("2147483648ms", False)])
def test_calling_timeout_fits_wrap_safe_millisecond_deadlines(tmp_path, value, valid):
    result = run_config(tmp_path, f"  calling_timeout: {value}\n", generate=False)
    assert (result.returncode == 0) is valid, result.stdout + result.stderr
    if not valid:
        assert "wrap-safe SIP deadlines" in result.stdout + result.stderr


def test_legacy_incoming_fallback_has_specific_migration_guidance(tmp_path):
    result = run_config(tmp_path, "  ringing_timeout: 30s\n", generate=False)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "now applies only to incoming calls" in result.stderr


def test_intentional_zero_does_not_receive_a_migration_warning(tmp_path):
    result = run_config(tmp_path, "  calling_timeout: 0s\n  ringing_timeout: 30s\n", generate=False)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "now applies only to incoming calls" not in result.stderr
