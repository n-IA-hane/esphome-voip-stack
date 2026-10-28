"""Host qualification must use complete peer SIP records rather than lost logs."""
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path

SPEC = spec_from_file_location("qualification", Path(__file__).parents[1] / "tools/qualify_sip_registration_esp.py")
MODULE = module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def test_peer_trace_reads_complete_headers_and_sipfrag_without_partial_body():
    wire = MODULE.BareSipWire.__new__(MODULE.BareSipWire)
    class Peer:
        output = "\x1b[31mrecv UDP\x1b[0m\r\nSIP/2.0 200 OK\r\nVia: SIP/2.0/UDP peer;branch=z9hG4bK1\r\nCall-ID: call-one\r\nCSeq: 7 INVITE\r\nContent-Length: 0\r\n\r\n"
    wire.peer = Peer()
    rows = wire.records()
    assert len(rows) == 1
    assert rows[0]["headers"]["CSeq"] == "7 INVITE"
    assert rows[0]["body"] == ""
    fragment = "SIP/2.0 200 OK\r\n"
    notify = "NOTIFY sip:peer SIP/2.0\r\nCall-ID: call-one\r\nEvent: refer;id=8\r\nContent-Length: " + str(len(fragment)) + "\r\n\r\n"
    wire.peer.output += notify + fragment[:-3]
    assert len(wire.records()) == 1
    wire.peer.output += fragment[-3:]
    rows = wire.records()
    assert rows[1]["line"] == "NOTIFY sip:peer SIP/2.0"
    assert rows[1]["headers"]["Event"] == "refer;id=8"
    assert rows[1]["body"].startswith("SIP/2.0 200")
