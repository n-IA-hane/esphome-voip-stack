"""Compile the real registration engine and exchange records over localhost sockets."""
from pathlib import Path
import hashlib
import subprocess
from urllib.request import parse_http_list, parse_keqv_list

import pytest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "esphome/components/voip_stack"


@pytest.fixture(scope="module")
def registration_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("sip-registration")
    defines = folder / "esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    output = folder / "registration"
    command = ["g++", "-std=c++17", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer", "-DUSE_ESPHOME_VOIP_REGISTRATION", "-DVOIP_REGISTRATION_CAPACITY=2",
        "-I", str(folder), "-I", str(SOURCE), str(SOURCE / "sip_registration.cpp"),
        str(ROOT / "tests/fixtures/registration_engine.cpp"), "-lcrypto", "-o", str(output)]
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return output


@pytest.mark.parametrize("tcp", ["udp", "tcp"])
@pytest.mark.parametrize("scenario", ["auth", "identity", "minimum", "retry", "timeout", "multiple", "intent", "unauthorized", "invalid", "async", "deadline", "dual-auth", "refresh-binding", "contacts", "same-user", "send-rejected"])
def test_registration_wire_lifecycle(registration_binary, tcp, scenario):
    result = subprocess.run([registration_binary, scenario, tcp], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr
    assert result.stdout == f"PASS {scenario} {tcp}\n"


@pytest.mark.parametrize("tcp", ["udp", "tcp"])
@pytest.mark.parametrize("style", ["v", "V", "i", "I", "m", "M", "mixed", "mixed-long"])
@pytest.mark.parametrize("authentication", ["direct", "auth"])
def test_registration_accepts_compact_response_headers(registration_binary, tcp, style, authentication):
    result = subprocess.run([registration_binary, "headers", tcp, style, authentication],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr
    assert result.stdout == f"PASS headers {tcp}\n"


@pytest.mark.parametrize("algorithm", ["MD5", "SHA-256", "MD5-sess", "SHA-256-sess"])
def test_digest_against_independent_hashlib(registration_binary, algorithm):
    result = subprocess.run([registration_binary, "digest", algorithm], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr
    fields = parse_keqv_list(parse_http_list(result.stdout.removeprefix("Authorization: Digest ").strip()))
    hash_name = "md5" if algorithm.startswith("MD5") else "sha256"
    def digest(value):
        return hashlib.new(hash_name, value.encode()).hexdigest()
    ha1 = digest("Mufasa:testrealm@host.com:Circle Of Life")
    if algorithm.endswith("-sess"):
        ha1 = digest(f"{ha1}:{fields['nonce']}:0a4f113b")
    expected = digest(f"{ha1}:{fields['nonce']}:00000001:0a4f113b:auth:{digest('GET:/dir/index.html')}")
    assert fields["response"] == expected
    if algorithm == "MD5":
        assert expected == "6629fae49393a05397450978507c4ef1"


@pytest.mark.parametrize("tcp", ["udp", "tcp"])
def test_proxy_authentication(registration_binary, tcp):
    result = subprocess.run([registration_binary, "auth", tcp, "proxy"], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr


def test_registration_engine_emits_no_code_when_disabled(registration_binary, tmp_path):
    include = tmp_path / "esphome/core/defines.h"
    include.parent.mkdir(parents=True)
    include.write_text("")
    output = tmp_path / "disabled.o"
    result = subprocess.run(["g++", "-std=c++17", "-I", str(tmp_path), "-c",
                             str(SOURCE / "sip_registration.cpp"), "-o", str(output)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    symbols = subprocess.run(["nm", "--defined-only", str(output)], capture_output=True, text=True, check=True)
    assert symbols.stdout == ""
