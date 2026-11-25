"""Public account correction after real 401/403/407 exchanges, not forced FAILED."""
import subprocess

import pytest

from test_transport_owner_lifecycle import build_transport_owner


@pytest.fixture(scope="module")
def public_recovery(tmp_path_factory):
    return build_transport_owner(tmp_path_factory, fixture="registration_public_recovery.cpp", registration=True)


@pytest.mark.parametrize("transport", ["udp", "tcp"])
@pytest.mark.parametrize("status", [401, 403, 407])
def test_public_credentials_replacement_in_retry(public_recovery, transport, status):
    result = subprocess.run([public_recovery, transport, str(status)], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
