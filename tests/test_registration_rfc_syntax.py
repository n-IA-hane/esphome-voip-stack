"""Valid RFC 3261 syntax must retain binding and transaction validation."""

import subprocess

import pytest

from test_sip_registration import registration_binary as registration_binary


@pytest.mark.parametrize("transport", ["udp", "tcp"])
@pytest.mark.parametrize("style", ["hcolon", "fold", "multiple", "reverse"])
@pytest.mark.parametrize("authentication", ["direct", "proxy"])
def test_digest_challenges_use_logical_header_fields(registration_binary, transport, style, authentication):
    result = subprocess.run([registration_binary, "digest-syntax", transport, style, authentication],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("transport,style", [
    (transport, style)
    for transport in ("udp", "tcp")
    for style in ("contact-hcolon", "contact-fold", "cseq-hcolon", "cseq-tab", "cseq-spaces", "cseq-fold",
                  "quoted-angle", "escaped-angle", "quoted-only", "wrong-real-uri", "suffix-junk", "two-uris")
] + [("udp", style) for style in (
    "bare-binding", "bare-expiry-fallback", "wrong-bare-binding", "expired-bare-binding",
)])
@pytest.mark.parametrize("authentication", ["direct", "auth"])
def test_registration_accepts_valid_syntax_without_weakening_binding(registration_binary, transport, style, authentication):
    result = subprocess.run([registration_binary, "rfc-syntax", transport, style, authentication],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr
