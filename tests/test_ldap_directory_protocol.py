"""Production LDAP encoder/client against independent ASN.1 and ldaptor."""

from pathlib import Path
import os
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def ldap_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("ldap-native")
    binary = folder / "client"
    command = ["g++", "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie", "-I", str(ROOT),
               str(ROOT / "tests/test_ldap_directory_protocol.cpp"),
               str(ROOT / "esphome/components/ldap_directory/ldap_protocol.cpp"),
               str(ROOT / "esphome/components/ldap_directory/directory_contact.cpp"),
               str(ROOT / "esphome/components/phone_client/net_stream.cpp"), "-o", str(binary)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=90)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.fixture(scope="module")
def ldap_server():
    pytest.importorskip("ldaptor")
    process = subprocess.Popen([sys.executable, str(ROOT / "tests/ldap_reference_server.py")],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        port = process.stdout.readline().strip()
        assert port.isdigit(), process.stderr.read()
        yield port
    finally:
        process.terminate()
        process.communicate(timeout=10)


def execute(binary, *args):
    result = subprocess.run([str(binary), *args], capture_output=True, text=True, timeout=10,
                            env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1:abort_on_error=1"})
    assert result.returncode == 0, result.stdout + result.stderr
    return result.stdout


def test_bounded_parser_framing_filters_mapping_and_sanitizers(ldap_binary):
    assert execute(ldap_binary, "unit").startswith("PASS")


@pytest.mark.parametrize("value,kind", [
    ("(cn=Alice)", "equalityMatch"), ("(cn=Alice*)", "substringFilter"),
    ("(cn=*)", "present"), ("(!(cn=Bob))", "notFilter"),
    ("(|(cn=Alice)(telephoneNumber=102))", "or"),
    ("(&(cn=*)(telephoneNumber>=100))", "and"),
])
def test_search_filter_wire_matches_independent_ldap3_encoder(ldap_binary, value, kind):
    from ldap3 import SUBTREE, DEREF_NEVER
    from ldap3.operation.search import search_operation
    from ldap3.protocol.rfc4511 import LDAPMessage
    from pyasn1.codec.ber.encoder import encode

    encoded = bytes.fromhex(execute(ldap_binary, "filter", value).strip())
    search = search_operation("ou=phonebook,dc=voip", value, SUBTREE, DEREF_NEVER,
                              ["uid", "entryUUID", "cn", "displayName", "telephoneNumber", "labeledURI", "ou"],
                              64, 3, False, False, True)
    message = LDAPMessage()
    message["messageID"] = 9
    message["protocolOp"]["searchRequest"] = search
    assert encoded == encode(message)
    assert search["filter"].getName() == kind
    assert int(search["sizeLimit"]) == 64


def test_real_ldaptor_bind_substring_search_and_contact_mapping(ldap_binary, ldap_server):
    result = execute(ldap_binary, "wire", ldap_server, "(cn=A*)", "64", "fixture-password")
    assert result == "SUCCESS 1\ndevice-a|Alice|101|127.0.0.1|5060|1|esphome\n"


def test_real_ldaptor_boolean_filter_and_multiple_result_frames(ldap_binary, ldap_server):
    result = execute(ldap_binary, "wire", ldap_server, "(|(cn=Alice)(telephoneNumber=102))", "64", "fixture-password")
    assert result.startswith("SUCCESS 2\n")
    assert "device-a|Alice|101" in result and "device-b|Bob|102" in result


def test_real_ldaptor_rejects_wrong_bind_without_search(ldap_binary, ldap_server):
    assert execute(ldap_binary, "wire", ldap_server, "(cn=*)", "64", "incorrect") == "BIND_ERROR 49\n"


def test_real_ldaptor_empty_results_commit_empty_snapshot(ldap_binary, ldap_server):
    assert execute(ldap_binary, "wire", ldap_server, "(cn=Missing)", "64", "fixture-password") == "SUCCESS 0\n"
