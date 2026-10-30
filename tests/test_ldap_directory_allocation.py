"""Independent allocator counts for production LDAP search and contact mapping."""

import json
from pathlib import Path
import os
import subprocess

import pytest

from test_refer_dialog_runtime import block

ROOT = Path(__file__).resolve().parents[1]


def response_wire(count):
    from ldap3.protocol.rfc4511 import LDAPMessage
    from pyasn1.codec.ber.encoder import encode

    messages = []
    for index in range(count):
        message = LDAPMessage()
        message["messageID"] = 1
        entry = message["protocolOp"]["searchResEntry"]
        entry["object"] = f"uid=receiver{index:03},ou=phonebook,dc=voip"
        attributes = {
            "uid": f"esphome:receiver-{index:03}", "cn": f"Receiver display {index:03}",
            "telephoneNumber": str(100 + index),
            "labeledURI": f"sip:receiver{index:03}@192.0.2.1:5060;transport=udp", "ou": "esphome",
        }
        for slot, (name, value) in enumerate(attributes.items()):
            entry["attributes"][slot]["type"] = name
            entry["attributes"][slot]["vals"][0] = value
        messages.append(encode(message))
    done = LDAPMessage()
    done["messageID"] = 1
    done["protocolOp"]["searchResDone"]["resultCode"] = 0
    done["protocolOp"]["searchResDone"]["matchedDN"] = ""
    done["protocolOp"]["searchResDone"]["diagnosticMessage"] = ""
    return b"".join(messages) + encode(done)


@pytest.fixture(scope="module")
def allocation_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("ldap-allocation")
    header = (ROOT / "esphome/components/voip_stack/phonebook.h").read_text()
    fixture = "#pragma once\n#include <string>\n#include <cstdint>\n#define USE_ESPHOME_VOIP_DIRECTORY\nnamespace esphome::voip_stack {\n"
    for name in ("enum class ContactEndpointType", "enum class ContactKind", "enum class ContactOrigin", "struct ContactEntry"):
        fixture += block(header, name) + ";\n"
    (folder / "contact_entry_fixture.h").write_text(fixture + "}\n")
    binary = folder / "allocation"
    result = subprocess.run(["g++", "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-I", str(folder), "-I", str(ROOT),
        str(ROOT / "tests/test_ldap_directory_allocation.cpp"),
        str(ROOT / "esphome/components/ldap_directory/ldap_protocol.cpp"),
        str(ROOT / "esphome/components/ldap_directory/directory_contact.cpp"),
        str(ROOT / "esphome/components/phone_client/net_stream.cpp"), "-o", str(binary)],
        capture_output=True, text=True, timeout=90)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("count", [9, 64])
def test_streamed_directory_bounds_peak_and_reuses_boot_stage(allocation_binary, tmp_path, count):
    packet = tmp_path / "entries.ber"
    packet.write_bytes(response_wire(count))
    modes = {}
    for mode in ("legacy", "stream"):
        result = subprocess.run([str(allocation_binary), str(packet), str(count), mode],
                                capture_output=True, text=True, timeout=10)
        assert result.returncode == 0, result.stdout + result.stderr
        rounds = [dict(zip(("round", "peak_requested_heap_bytes", "allocation_calls", "retained_requested_heap_bytes", "boot_stage_host_bytes"),
                           map(int, line.split()))) for line in result.stdout.splitlines()]
        assert len(rounds) == 5
        assert all(row["retained_requested_heap_bytes"] == 0 for row in rounds)
        assert len({(row["peak_requested_heap_bytes"], row["allocation_calls"]) for row in rounds}) == 1
        modes[mode] = rounds
    worker = Path(os.environ.get("LDAP_WORKER_SOURCE", ROOT / "esphome/components/ldap_directory/ldap_directory.cpp")).read_text()
    current = modes["stream" if "client.search_each(" in worker else "legacy"]
    (tmp_path / "allocation-receipt.json").write_text(json.dumps({
        "scope": "Host ABI requested ordinary-allocator bytes, not physical ESP internal/DMA bytes",
        "count": count, "rounds": modes, "target_boot_stage_bytes_at_64": 8960,
        "receive_buffer_bytes": 8192, "string_allocations_remain_transient": True,
    }, indent=2) + "\n")
    assert current[0]["peak_requested_heap_bytes"] < modes["legacy"][0]["peak_requested_heap_bytes"] * .70
