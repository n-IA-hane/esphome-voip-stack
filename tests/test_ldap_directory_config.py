"""Real ESPHome schema/codegen for standalone and shared-account LDAP."""

from pathlib import Path
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]


def configure(tmp_path, ldap, *, account=False, generate=False, api=False):
    text = (ROOT / "tests/minimal_no_text.yaml").read_text()
    text = text.replace("../esphome/components", str(ROOT / "esphome/components"))
    text = text.replace("components: [voip_stack]", "components: [voip_stack, ldap_directory, phone_client]")
    text = text.replace("voip_stack:\n", "voip_stack:\n  ha_integration: false\n" + ("  registration: {}\n" if account else ""))
    text += "\nldap_directory:\n  id: directory\n  voip_stack_id: phone\n" + ldap
    if api:
        text += "\napi:\n  custom_services: true\n"
    path = tmp_path / "device.yaml"
    path.write_text(text)
    command = [sys.executable, "-m", "esphome", "compile" if generate else "config", str(path)]
    if generate:
        command.append("--only-generate")
    return subprocess.run(command, capture_output=True, text=True, timeout=120)


EXTERNAL = """  host: directory.example.test
  username: uid=reader,dc=voip
  password: fixture-password
  base_dn: ou=phonebook,dc=voip
"""


@pytest.mark.parametrize("account", [False, True])
def test_ldap_codegen_real_external_and_shared_account_without_runtime(tmp_path, account):
    result = configure(tmp_path, "  account_id: ha\n  base_dn: ou=phonebook,dc=voip\n  api_actions: true\n" if account else EXTERNAL,
                       account=account, generate=True, api=account)
    assert result.returncode == 0, result.stdout[-2000:] + result.stderr[-5000:]
    generated = next(tmp_path.rglob("main.cpp")).read_text()
    defines = next(tmp_path.rglob("defines.h")).read_text()
    assert "ldap_directory::LdapDirectory" in generated
    assert "USE_ESPHOME_VOIP_DIRECTORY" in defines
    assert ("USE_ESPHOME_VOIP_REGISTRATION" in defines) is account
    assert ("USE_LDAP_DIRECTORY_API" in defines) is account
    assert "runtime_controller" not in generated
    assert next(tmp_path.rglob("net_stream.cpp")).is_file()
    assert next(tmp_path.rglob("worker.cpp")).is_file()


@pytest.mark.parametrize("suffix,message", [
    ("  account_id: ha\n", "Choose account_id"),
    ("  max_entries: 65\n", "at most 64"),
    ("  filter: '(cn=broken\\xz)'\n", "two hexadecimal"),
    ("  filter: '(cn:dn:=Alice)'\n", "extensible-match"),
    ("  task_core: 2\n", "at most 1"),
    ("  api_actions: true\n", "custom_services"),
])
def test_ldap_editor_rejects_incompatible_or_unbounded_configuration(tmp_path, suffix, message):
    result = configure(tmp_path, EXTERNAL + suffix)
    assert result.returncode != 0
    assert message.lower() in (result.stdout + result.stderr).lower()


def test_shared_account_requires_the_actual_registration_owner(tmp_path):
    result = configure(tmp_path, "  account_id: ha\n")
    assert result.returncode != 0
    assert "LDAP account_id requires" in result.stdout + result.stderr
