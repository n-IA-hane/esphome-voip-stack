"""Native registrar qualifiers retain loopback and accept owned NAT topology."""

import importlib
from pathlib import Path
import xml.etree.ElementTree as ET

import pytest


@pytest.fixture
def qualifiers(monkeypatch):
    monkeypatch.syspath_prepend(str(Path(__file__).resolve().parents[1] / "tools"))
    return (importlib.import_module("qualify_sip_registration"),
        importlib.import_module("qualify_sip_registration_freeswitch"))


def test_default_driver_and_pbx_control_remain_loopback(qualifiers):
    asterisk, freeswitch = qualifiers
    assert asterisk.probe_command(Path("/probe"), "udp", "127.0.0.1", 5060, "7101", 3) == [
        "/probe", "udp", "127.0.0.1", "5060", "7101", "127.0.0.1", "3"]
    tree = ET.fromstring(freeswitch.configuration(5060, 5062, 8021, "fixture-only"))
    assert {item.attrib["value"] for item in tree.findall(".//profiles/profile/settings/param[@name='sip-ip']")} == {"127.0.0.1"}
    assert tree.find(".//configuration[@name='event_socket.conf']/settings/param[@name='listen-ip']").attrib["value"] == "127.0.0.1"


def test_explicit_lab_address_is_not_a_fabricated_public_contact(qualifiers):
    asterisk, freeswitch = qualifiers
    command = asterisk.probe_command(Path("/probe"), "tcp", "10.203.239.2", 5060, "7101", 52, "sipq-owned-client")
    assert command[:4] == ["ip", "netns", "exec", "sipq-owned-client"]
    assert command[6:10] == ["10.203.239.2", "5060", "7101", "10.203.239.2"]
    assert "10.203.239.1" not in command
    tree = ET.fromstring(freeswitch.configuration(5060, 5062, 8021, "fixture-only", "10.203.239.2"))
    assert {item.attrib["value"] for item in tree.findall(".//profiles/profile/settings/param[@name='sip-ip']")} == {"10.203.239.2"}
    assert tree.find(".//configuration[@name='event_socket.conf']/settings/param[@name='listen-ip']").attrib["value"] == "127.0.0.1"


@pytest.mark.parametrize("address,namespace", [("127.0.0.1", "sipq-owned-client"),
    ("10.203.239.2", "../unowned"), ("10.203.239.2", ""), ("::1", None)])
def test_invalid_topology_fails_before_any_process(qualifiers, address, namespace):
    with pytest.raises(ValueError):
        qualifiers[0].probe_command(Path("/probe"), "udp", address, 5060, "7101", 3, namespace)


def test_maintained_native_registration_driver_builds(qualifiers, tmp_path):
    """Compile the actual CLI driver with the shared production SDP reader."""
    import subprocess

    binary = qualifiers[0].build_probe(tmp_path / "native")
    result = subprocess.run([binary], capture_output=True, timeout=5)
    assert result.returncode == 64  # No topology or credentials supplied.
