"""Public configuration gates and native code generation for optional PBX accounts."""
from pathlib import Path
import os
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
ACCOUNT = """  registration:
    accounts:
      - account_id: office
        registrar: pbx.example.org
        username: '100'
        password: test-only-password
"""


def run_config(tmp_path, registration=ACCOUNT, *, generate=True, extra=""):
    config = (ROOT / "tests/minimal_no_text.yaml").read_text().replace("../esphome/components", str(ROOT / "esphome/components"))
    config = config.replace("voip_stack:\n", "voip_stack:\n" + registration)
    config += extra
    path = tmp_path / "registration.yaml"
    path.write_text(config)
    command = [os.environ.get("ESPHOME_PYTHON", sys.executable), "-m", "esphome", "compile" if generate else "config", str(path)]
    if generate:
        command.append("--only-generate")
    return subprocess.run(command, capture_output=True, text=True)


def test_registration_generates_only_when_enabled(tmp_path):
    result = run_config(tmp_path)
    assert result.returncode == 0, result.stdout + result.stderr
    generated = next(tmp_path.rglob("main.cpp")).read_text()
    defines = next(tmp_path.rglob("defines.h")).read_text()
    assert "#define USE_ESPHOME_VOIP_REGISTRATION" in defines
    assert "#define VOIP_REGISTRATION_CAPACITY 2" in defines
    assert "#define USE_ESPHOME_VOIP_MWI" not in defines
    assert 'configure_registration(true, "direct")' in generated
    assert "static const voip_stack::SipAccountConfigView phone_registration_accounts[]" in generated
    assert '.id = "office"' in generated and '.registrar = "pbx.example.org"' in generated
    assert 'set_registration_accounts(phone_registration_accounts, 1)' in generated
    assert "add_registration_account(" not in generated


def test_direct_profile_has_no_registration_code(tmp_path):
    result = run_config(tmp_path, "")
    assert result.returncode == 0, result.stdout + result.stderr
    defines = next(tmp_path.rglob("defines.h")).read_text()
    assert "USE_ESPHOME_VOIP_REGISTRATION" not in defines
    assert "VOIP_REGISTRATION_CAPACITY" not in defines
    assert "configure_registration" not in next(tmp_path.rglob("main.cpp")).read_text()


@pytest.mark.parametrize("count", [0, 1, 2, 4])
def test_flash_baseline_has_only_yaml_rows_and_keeps_runtime_pool(tmp_path, count):
    config = "  registration:\n    max_accounts: 4\n"
    if count:
        config += "    accounts:\n"
        for index in range(count):
            config += (f"      - account_id: line{index}\n"
                       "        registrar: pbx.example.org\n"
                       f"        username: '{100 + index}'\n"
                       "        password: test-only-password\n")
    result = run_config(tmp_path, config)
    assert result.returncode == 0, result.stdout + result.stderr
    generated = next(tmp_path.rglob("main.cpp")).read_text()
    defines = next(tmp_path.rglob("defines.h")).read_text()
    assert "#define VOIP_REGISTRATION_CAPACITY 4" in defines
    if count:
        assert f"set_registration_accounts(phone_registration_accounts, {count})" in generated
        assert generated.count('.registrar = "pbx.example.org"') == count
    else:
        assert "phone_registration_accounts" not in generated
    assert "add_registration_account(" not in generated


def test_pbx_only_single_account_selects_it_automatically(tmp_path):
    config = ACCOUNT.replace("    accounts:", "    accept_direct_calls: false\n    accounts:")
    result = run_config(tmp_path, config)
    assert result.returncode == 0, result.stdout + result.stderr
    assert 'configure_registration(false, "office")' in next(tmp_path.rglob("main.cpp")).read_text()


@pytest.mark.parametrize("replacement", [
    ("account_id: office", "account_id: direct"),
    ("pbx.example.org", "pbx.example.org:5060"),
    ("  registration:", "  registration:\n    max_accounts: 5"),
    ("  registration:", "  registration:\n    default_line: unknown"),
])
def test_invalid_account_configuration_is_rejected(tmp_path, replacement):
    result = run_config(tmp_path, ACCOUNT.replace(*replacement), generate=False)
    assert result.returncode != 0


def test_actions_and_registration_trigger_codegen(tmp_path):
    config = ACCOUNT + """  on_registration_state:
    then:
      - logger.log: PBX registration changed
"""
    extra = """
button:
  - platform: template
    name: Register Office
    on_press:
      - voip_stack.register_account:
          id: phone
          account_id: office
      - if:
          condition:
            voip_stack.is_registered:
              id: phone
              account_id: office
          then:
            - voip_stack.start_call_on_line:
                id: phone
                target: '101'
                line: office
"""
    result = run_config(tmp_path, config, extra=extra)
    assert result.returncode == 0, result.stdout + result.stderr
    generated = next(tmp_path.rglob("main.cpp")).read_text()
    assert "RegistrationAccountAction" in generated and "IsRegisteredCondition" in generated
    assert "get_registration_state_trigger" in generated


def test_message_waiting_is_explicitly_gated_and_generates_its_callback(tmp_path):
    config = ACCOUNT.replace("    accounts:", "    message_waiting: true\n    accounts:")
    config += """  on_message_waiting:
    then:
      - if:
          condition:
            voip_stack.has_waiting_messages:
              id: phone
          then:
            - logger.log: A mailbox has new messages
"""
    result = run_config(tmp_path, config)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "#define USE_ESPHOME_VOIP_MWI" in next(tmp_path.rglob("defines.h")).read_text()
    generated = next(tmp_path.rglob("main.cpp")).read_text()
    assert "get_message_waiting_trigger" in generated and "HasWaitingMessagesCondition" in generated


def test_message_waiting_callback_without_feature_has_actionable_error(tmp_path):
    result = run_config(tmp_path, ACCOUNT + "  on_message_waiting:\n    then:\n      - logger.log: Message\n")
    assert result.returncode != 0
    assert "registration.message_waiting: true" in result.stdout + result.stderr


@pytest.mark.parametrize("enabled", [False, True])
def test_mailbox_call_action_is_independent_of_directory_and_imap(tmp_path, enabled):
    registration = ACCOUNT.replace("    accounts:", "    message_waiting: true\n    accounts:") if enabled else ACCOUNT
    registration += '''  on_idle:
    then:
      - voip_stack.call_mailbox:
          id: phone
          account_id: office
          target: '*97'
'''
    result = run_config(tmp_path, registration)
    if not enabled:
        assert result.returncode != 0
        assert 'voip_stack.call_mailbox requires registration.message_waiting: true' in result.stdout + result.stderr
        return
    assert result.returncode == 0, result.stdout + result.stderr
    generated = next(tmp_path.rglob('main.cpp')).read_text()
    defines = next(tmp_path.rglob('defines.h')).read_text()
    assert 'CallMailboxAction' in generated
    assert 'USE_ESPHOME_VOIP_DIRECTORY' not in defines
    assert 'USE_IMAP_VOICEMAIL' not in defines
    assert 'runtime_controller' not in generated
