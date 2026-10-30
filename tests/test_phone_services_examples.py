"""Published phone-service examples use the official ESPHome generator."""

from pathlib import Path
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("name,managed", [("managed-ha-phone", True), ("external-phone-services", False)])
def test_phone_service_example_uses_one_account_and_native_playback(tmp_path, name, managed):
    for source in (ROOT / "examples").glob("*.yaml"):
        text = source.read_text().replace("github://n-IA-hane/esphome-voip-stack@dev", str(ROOT / "esphome/components"))
        (tmp_path / source.name).write_text(text)
    (tmp_path / "secrets.yaml").write_text(
        "wifi_ssid: fixture\nwifi_password: fixture-password\noffice_sip_password: sip-fixture\n"
        "directory_password: directory-fixture\nmailbox_password: mailbox-fixture\n")
    result = subprocess.run([sys.executable, "-m", "esphome", "compile", "--only-generate", str(tmp_path / f"{name}.yaml")], capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stdout + result.stderr
    generated = next(tmp_path.rglob("main.cpp")).read_text()
    defines = next(tmp_path.rglob("defines.h")).read_text()
    assert "ldap_directory::LdapDirectory" in generated
    assert "imap_voicemail::ImapVoicemail" in generated
    assert "USE_ESPHOME_VOIP_BLF" in defines and "USE_ESPHOME_VOIP_MWI" in defines
    assert ("USE_VOIP_HA_AUTO_REGISTER" in defines) is managed
    assert ("voip_stack::VoipHAIntegration" in generated) is managed
    assert "speaker_source::" in generated and "mixer_speaker::MixerSpeaker" in generated
    assert "voicemail_speaker->set_output_speaker(voicemail_mixer_input)" in generated
    assert "runtime_controller" not in generated
