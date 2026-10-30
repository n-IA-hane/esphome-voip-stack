"""Public native-player schema/codegen, with and without a SIP account."""

from pathlib import Path
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]


def configure(tmp_path, imap, *, account=False, generate=False, api=False):
    if account:
        text = (ROOT / "tests/minimal_no_text.yaml").read_text().replace("../esphome/components", str(ROOT / "esphome/components"))
        text = text.replace("components: [voip_stack]", "components: [voip_stack, imap_voicemail, phone_client]")
        text = text.replace("voip_stack:\n", "voip_stack:\n  ha_integration: false\n  registration: {}\n")
    else:
        text = f"""esphome:
  name: imap-standalone
esp32:
  board: freenove_esp32_s3_wroom
  framework:
    type: esp-idf
external_components:
  - source: {ROOT / 'esphome/components'}
    components: [imap_voicemail, phone_client]
logger:
wifi:
  ssid: fixture-network
  password: fixture-password
"""
    text += "\nimap_voicemail:\n  id: voicemail\n" + imap
    if api:
        text += "\napi:\n  custom_services: true\n"
    path = tmp_path / "device.yaml"
    path.write_text(text)
    command = [sys.executable, "-m", "esphome", "compile" if generate else "config", str(path)]
    if generate:
        command.append("--only-generate")
    return subprocess.run(command, capture_output=True, text=True, timeout=120)


EXTERNAL = """  host: mail.example.test
  username: reader
  password: fixture-password
"""


@pytest.mark.parametrize("account", [False, True])
def test_imap_native_source_codegen_external_and_shared_without_http_forks(tmp_path, account):
    imap = "  voip_stack_id: phone\n  account_id: ha\n  api_actions: true\n" if account else EXTERNAL
    result = configure(tmp_path, imap, account=account, generate=True, api=account)
    assert result.returncode == 0, result.stdout[-2000:] + result.stderr[-5000:]
    generated = next(tmp_path.rglob("main.cpp")).read_text()
    defines = next(tmp_path.rglob("defines.h")).read_text()
    assert "imap_voicemail::ImapVoicemail" in generated
    assert "USE_MEDIA_SOURCE" in defines and "USE_AUDIO_WAV_SUPPORT" in defines
    assert ("USE_IMAP_VOICEMAIL_ACCOUNT" in defines) is account
    assert ("USE_IMAP_VOICEMAIL_API" in defines) is account
    assert ("voip_stack::VoipStack" in generated) is account
    assert "runtime_controller" not in generated
    assert "USE_HTTP_REQUEST" not in defines
    assert not (next(tmp_path.rglob("src")) / "esphome/components/speaker_source").exists()


@pytest.mark.parametrize("suffix,message", [
    ("  account_id: ha\n", "Choose account_id"),
    ("  max_messages: 65\n", "at most 64"),
    ("  fetch_chunk_size: 0\n", "at least 1024"),
    ("  task_core: 2\n", "at most 1"),
    ("  api_actions: true\n", "custom_services"),
    ("  folder: ''\n", "at least 1"),
    ("  managed_by_ha: true\n", "account_id: ha"),
])
def test_imap_editor_rejects_incompatible_or_unbounded_configuration(tmp_path, suffix, message):
    result = configure(tmp_path, EXTERNAL + suffix)
    assert result.returncode != 0
    assert message in result.stdout + result.stderr


def test_imap_account_requires_explicit_parent_not_a_second_password(tmp_path):
    result = configure(tmp_path, "  account_id: ha\n")
    assert result.returncode != 0
    assert "requires voip_stack_id" in result.stdout + result.stderr
