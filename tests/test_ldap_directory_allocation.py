"""Independent allocator counts for production LDAP search and contact mapping."""

import json
from pathlib import Path
import os
import subprocess

import pytest

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


@pytest.fixture(scope="module", params=[False, True], ids=["internal", "signaling-psram"])
def allocation_binary(tmp_path_factory, request):
    folder = tmp_path_factory.mktemp("ldap-allocation")
    external = request.param
    (folder / "contact_entry_fixture.h").write_text(
        '#pragma once\n#include "esphome/components/voip_stack/phonebook.h"\n')
    helper = folder / "esphome/core/helpers.h"
    helper.parent.mkdir(parents=True)
    helper.write_text("""#pragma once
#include <cstdlib>
namespace esphome { template<class T>struct RAMAllocator {
 using value_type=T; enum { ALLOC_EXTERNAL=1 }; unsigned flags;
 explicit RAMAllocator(unsigned value=0):flags(value){}
 T*allocate(size_t n){return static_cast<T*>(std::malloc(n*sizeof(T)));}
 void deallocate(T*p,size_t){std::free(p);}
}; }
""")
    binary = folder / "allocation"
    result = subprocess.run(["g++", "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-I", str(folder), "-I", str(ROOT),
        "-DUSE_ESP32", "-DUSE_ESPHOME_VOIP_DIRECTORY",
        *(["-DUSE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM"] if external else []),
        str(ROOT / "tests/test_ldap_directory_allocation.cpp"),
        str(ROOT / "esphome/components/ldap_directory/ldap_protocol.cpp"),
        str(ROOT / "esphome/components/ldap_directory/directory_contact.cpp"),
        str(ROOT / "esphome/components/phone_client/net_stream.cpp"), "-o", str(binary)],
        capture_output=True, text=True, timeout=90)
    assert result.returncode == 0, result.stderr
    return binary, external


@pytest.mark.parametrize("count", [9, 64])
def test_streamed_directory_bounds_peak_and_reuses_boot_stage(allocation_binary, tmp_path, count):
    binary, external = allocation_binary
    packet = tmp_path / "entries.ber"
    packet.write_bytes(response_wire(count))
    modes = {}
    for mode in ("legacy", "stream"):
        result = subprocess.run([str(binary), str(packet), str(count), mode],
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
        "count": count, "rounds": modes, "signaling_in_psram": external,
        "target_boot_stage_bytes_at_64": 10240 if external else 8960,
        "receive_buffer_bytes": 8192, "string_allocations_remain_transient": True,
    }, indent=2) + "\n")
    assert current[0]["peak_requested_heap_bytes"] < modes["legacy"][0]["peak_requested_heap_bytes"] * .70
