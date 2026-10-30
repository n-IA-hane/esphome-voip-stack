"""Native IMAP protocol against real pymap, independently seeded with imaplib."""

from contextlib import closing
from email.message import EmailMessage
from email.policy import SMTP
from io import BytesIO
from pathlib import Path
import imaplib
import os
import socket
import subprocess
import sys
import time
import wave

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def imap_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("imap-native")
    binary = folder / "client"
    command = ["g++", "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie", "-I", str(ROOT),
               str(ROOT / "tests/test_imap_voicemail_protocol.cpp"),
               str(ROOT / "esphome/components/imap_voicemail/imap_protocol.cpp"),
               str(ROOT / "esphome/components/phone_client/net_stream.cpp"), "-o", str(binary)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=90)
    assert result.returncode == 0, result.stderr
    return binary


def fixture_message(subject="New voicemail", duration=1):
    wave_file = BytesIO()
    with wave.open(wave_file, "wb") as writer:
        writer.setnchannels(1)
        writer.setsampwidth(2)
        writer.setframerate(16000)
        writer.writeframes(bytes(range(256)) * 8)
    message = EmailMessage()
    message["From"] = "Reception <reception@example.test>"
    message["To"] = "reader@example.test"
    message["Subject"] = subject
    message["Date"] = "Thu, 29 Oct 2026 21:00:00 +0000"
    message.set_content("A voicemail audio attachment.")
    message.add_attachment(wave_file.getvalue(), maintype="audio", subtype="wav", filename="voicemail.wav")
    if duration is not None:
        # set_content and MIME conversion manage Content-* fields, so set the
        # final message header only after the multipart structure exists.
        message["Content-Duration"] = str(duration)
    return message.as_bytes(policy=SMTP), wave_file.getvalue()


@pytest.fixture
def imap_server(tmp_path):
    pytest.importorskip("pymap")
    with closing(socket.socket()) as reserve:
        reserve.bind(("127.0.0.1", 0))
        port = reserve.getsockname()[1]
    command = [str(Path(sys.executable).with_name("pymap")), "--host", "127.0.0.1", "--port", str(port), "--no-tls",
               "dict", "--demo-user", "reader", "--demo-password", "fixture-password"]
    log = (tmp_path / "pymap.log").open("w+")
    process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, text=True)
    try:
        deadline = time.monotonic() + 8
        while True:
            try:
                client = imaplib.IMAP4("127.0.0.1", port, timeout=1)
                client.login("reader", "fixture-password")
                break
            except (OSError, imaplib.IMAP4.error):
                if process.poll() is not None or time.monotonic() >= deadline:
                    log.seek(0)
                    pytest.fail(log.read())
                time.sleep(0.02)
        raw, wave_bytes = fixture_message()
        assert client.append("INBOX", None, None, raw)[0] == "OK"
        client.select("INBOX")
        yield port, client, wave_bytes
        client.logout()
    finally:
        process.terminate()
        process.wait(timeout=10)
        log.close()


def execute(binary, *arguments):
    result = subprocess.run([str(binary), *map(str, arguments)], capture_output=True, text=True, timeout=20,
                            env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1:abort_on_error=1"})
    assert result.returncode == 0, result.stdout + result.stderr
    return result.stdout


def test_imap_bounded_parser_mime_splits_and_uid_sanitizers(imap_binary):
    assert execute(imap_binary, "unit").startswith("PASS")


def test_native_login_catalogue_and_bodystructure_against_pymap(imap_binary, imap_server):
    port, _client, _audio = imap_server
    result = execute(imap_binary, "list", port)
    assert "MAILBOX " in result and " 1 1\n" in result
    assert "|New voicemail|Reception|2|audio/wav|1|1\n" in result


def test_duration_omitted_by_external_server_remains_unknown(imap_binary, imap_server):
    port, client, _audio = imap_server
    raw, _ = fixture_message("External message", duration=None)
    assert client.append("INBOX", None, None, raw)[0] == "OK"
    result = execute(imap_binary, "list", port)
    assert "|External message|Reception|2|audio/wav|0|0\n" in result


def test_partial_fetch_base64_exact_wave_bytes_without_marking_seen(imap_binary, imap_server, tmp_path):
    port, client, expected = imap_server
    path = tmp_path / "received.wav"
    assert execute(imap_binary, "fetch", port, path).startswith("FETCHED ")
    assert path.read_bytes() == expected
    status, data = client.uid("FETCH", "1:*", "(FLAGS)")
    assert status == "OK" and b"\\Seen" not in b" ".join(data)


def test_explicit_seen_and_unseen_update_actual_server_flags(imap_binary, imap_server):
    port, client, _audio = imap_server
    execute(imap_binary, "seen", port)
    assert b"\\Seen" in b" ".join(client.uid("FETCH", "1:*", "(FLAGS)")[1])
    execute(imap_binary, "unseen", port)
    assert b"\\Seen" not in b" ".join(client.uid("FETCH", "1:*", "(FLAGS)")[1])


def test_uid_expunge_deletes_only_chosen_uid(imap_binary, imap_server):
    port, client, _audio = imap_server
    raw, _ = fixture_message("Second voicemail")
    assert client.append("INBOX", None, None, raw)[0] == "OK"
    ids = client.uid("SEARCH", None, "ALL")[1][0].split()
    client.uid("STORE", ids[0], "+FLAGS.SILENT", "(\\Deleted)")
    result = execute(imap_binary, "delete", port)
    assert result == f"CHANGED {int(ids[-1])}\n"
    # Synchronize the other selected session before inspecting its mailbox.
    # Servers may defer that session's unilateral EXPUNGE until a command
    # whose response permits it (RFC3501 section7.4.1).
    assert client.noop()[0] == "OK"
    remaining = client.uid("SEARCH", None, "ALL")[1][0].split()
    assert ids[0] in remaining and ids[-1] not in remaining


def test_real_idle_delivers_append_change_without_polling(imap_binary, imap_server):
    port, client, _audio = imap_server
    child = subprocess.Popen([str(imap_binary), "idle", str(port)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        assert child.stdout.readline().strip() == "IDLE_READY"
        raw, _ = fixture_message("Arrived during IDLE")
        assert client.append("INBOX", None, None, raw)[0] == "OK"
        stdout, stderr = child.communicate(timeout=10)
        assert child.returncode == 0, stdout + stderr
        assert stdout.strip() == "UPDATED 2"
    finally:
        if child.poll() is None:
            child.kill()
            child.wait(timeout=5)


def test_bad_login_has_no_catalogue(imap_binary, imap_server):
    assert execute(imap_binary, "bad-login", imap_server[0]) == "ERROR authentication\n"
