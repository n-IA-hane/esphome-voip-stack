"""SIP/UDP can receive fragmented IPv4 before application dispatch."""

import pytest

from test_sip_registration_config import ACCOUNT, run_config


@pytest.mark.parametrize("registration", ["", ACCOUNT], ids=["direct", "registered"])
def test_ipv4_reassembly_is_enabled_in_generated_sdkconfig(tmp_path, registration):
    result = run_config(tmp_path, registration)
    assert result.returncode == 0, result.stdout + result.stderr

    sdkconfig = next(tmp_path.rglob("sdkconfig.voip-minimal-no-text"))
    options = dict(
        line.split("=", 1)
        for line in sdkconfig.read_text().splitlines()
        if line.startswith("CONFIG_") and "=" in line
    )
    assert options.get("CONFIG_LWIP_IP4_REASSEMBLY") == "y"
    # Keep Espressif's bounded default pool rather than adding a custom budget.
    assert "CONFIG_LWIP_IP_REASS_MAX_PBUFS" not in options

    defines = next(tmp_path.rglob("defines.h")).read_text()
    assert ("#define USE_ESPHOME_VOIP_REGISTRATION" in defines) == bool(registration)
