"""Exercise the real MWI subscription engine over UDP/TCP socket records."""

from pathlib import Path
import hashlib
import resource
import subprocess
from urllib.request import parse_http_list, parse_keqv_list

import pytest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "esphome/components/voip_stack"


@pytest.fixture(scope="module")
def mwi_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("sip-mwi")
    header = folder / "esphome/core/defines.h"
    header.parent.mkdir(parents=True)
    header.write_text("")
    binary = folder / "mwi"
    command = [
        "g++",
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-g",
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
        "-fno-pie",
        "-no-pie",
        "-DUSE_ESPHOME_VOIP_REGISTRATION",
        "-DUSE_ESPHOME_VOIP_MWI",
        "-I",
        str(folder),
        "-I",
        str(SOURCE),
        str(SOURCE / "sip_mwi.cpp"),
        str(SOURCE / "sip_registration.cpp"),
        str(ROOT / "tests/test_sip_mwi_engine.cpp"),
        "-lcrypto",
        "-o",
        str(binary),
    ]
    built = subprocess.run(command, capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize("transport", ["udp", "tcp"])
@pytest.mark.parametrize(
    "scenario",
    [
        "auth",
        "early",
        "identity",
        "duplicates",
        "minimum",
        "unsupported",
        "deadline",
        "retransmit",
        "unsubscribe",
        "runtime-enable",
        "routes",
        "terminated",
        "multiple",
        "fork-response",
        "request-line",
        "refresh-error",
        "refresh-response-tag",
        "refresh-terminal",
        "notify-new-connection",
    ],
)
def test_mwi_wire_lifecycle(mwi_binary, transport, scenario):
    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    result = subprocess.run(
        [mwi_binary, scenario, transport],
        capture_output=True,
        text=True,
        timeout=8,
        preexec_fn=no_core,
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout == f"PASS {scenario} {transport}\n"


@pytest.mark.parametrize("algorithm", ["MD5", "SHA-256", "MD5-sess", "SHA-256-sess"])
def test_subscribe_digest_against_hashlib(mwi_binary, algorithm):
    result = subprocess.run(
        [mwi_binary, "digest", algorithm], capture_output=True, text=True, timeout=8
    )
    assert result.returncode == 0, result.stderr
    fields = parse_keqv_list(
        parse_http_list(result.stdout.removeprefix("Digest ").strip())
    )

    def digest(value):
        return hashlib.new(
            "md5" if algorithm.startswith("MD5") else "sha256", value.encode()
        ).hexdigest()

    ha1 = digest("alice:example.test:test-password")
    if algorithm.endswith("-sess"):
        ha1 = digest(f"{ha1}:{fields['nonce']}:{fields['cnonce']}")
    expected = digest(
        f"{ha1}:{fields['nonce']}:{fields['nc']}:{fields['cnonce']}:auth:{digest('SUBSCRIBE:' + fields['uri'])}"
    )
    assert fields["response"] == expected


@pytest.mark.parametrize(
    ("body", "expected"),
    [
        ("Messages-Waiting: yes\r\nVoice-Message: 2/4 (1/2)\r\n", "1 1 2 4 1 2"),
        ("messages-waiting: NO\r\n", "1 0 0 0 0 0"),
        (
            "Messages-Waiting: yes\r\nVoice-Message: 99999999999999999999999/4294967296 (7/8)\r\n",
            "1 1 4294967295 4294967295 7 8",
        ),
        ("Messages-Waiting: perhaps\r\n", "0 0 0 0 0 0"),
        ("Voice-Message: 1/2\r\n", "0 0 0 0 0 0"),
        ("Messages-Waiting: yes\r\nVoice-Message: -1/2\r\n", "0 0 0 0 0 0"),
    ],
)
def test_summary_parser(mwi_binary, body, expected):
    result = subprocess.run(
        [mwi_binary, "parse"], input=body, capture_output=True, text=True, timeout=8
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == expected


@pytest.mark.parametrize("registration_only", [False, True])
def test_mwi_emits_no_code_when_disabled(tmp_path, registration_only):
    header = tmp_path / "esphome/core/defines.h"
    header.parent.mkdir(parents=True)
    header.write_text("")
    obj = tmp_path / "disabled.o"
    flags = ["-DUSE_ESPHOME_VOIP_REGISTRATION"] if registration_only else []
    result = subprocess.run(
        [
            "g++",
            "-std=c++17",
            *flags,
            "-I",
            str(tmp_path),
            "-c",
            str(SOURCE / "sip_mwi.cpp"),
            "-o",
            str(obj),
        ],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr
    symbols = subprocess.run(
        ["nm", "--defined-only", str(obj)], capture_output=True, text=True, check=True
    )
    assert symbols.stdout == ""


@pytest.mark.parametrize("transport", ["udp", "tcp"])
def test_authenticated_subscribe_framing_with_independent_header_parser(
    mwi_binary, transport
):
    from email.parser import BytesParser
    from email.policy import HTTP

    result = subprocess.run(
        [mwi_binary, "wire", transport], capture_output=True, timeout=8
    )
    assert result.returncode == 0, result.stderr
    request_line, remainder = result.stdout.split(b"\r\n", 1)
    headers, body = remainder.split(b"\r\n\r\n", 1)
    parsed = BytesParser(policy=HTTP).parsebytes(headers + b"\r\n\r\n")
    assert request_line.startswith(b"SUBSCRIBE sip:")
    assert parsed["Content-Length"] == "0"
    assert len(body) == int(parsed["Content-Length"])
    assert parsed["Authorization"].startswith("Digest ")
    assert parsed["Proxy-Authorization"].startswith("Digest ")
    assert parsed["Event"] == "message-summary"
