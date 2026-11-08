"""Optional managed HA accounts coexist with explicit manual SIP lines."""
import pytest

from test_sip_registration_config import ACCOUNT, run_config

API = "\napi:\n  custom_services: true\n"
MANAGED = "  ha_integration:\n    auto_register: true\n"


def test_managed_account_codegen_reserves_real_registration_owner(tmp_path):
    result = run_config(tmp_path, MANAGED, extra=API)
    assert result.returncode == 0, result.stdout + result.stderr
    main = next(tmp_path.rglob("main.cpp")).read_text()
    defines = next(tmp_path.rglob("defines.h")).read_text()
    assert "#define USE_VOIP_HA_AUTO_REGISTER" in defines
    assert "#define USE_ESPHOME_VOIP_REGISTRATION" in defines
    assert "#define VOIP_REGISTRATION_CAPACITY 2" in defines
    assert 'configure_registration(true, "ha")' in main
    assert "phone_registration_accounts" not in main
    assert "USE_ESPHOME_VOIP_MWI" not in defines
    assert "USE_ESPHOME_VOIP_BLF" not in defines


@pytest.mark.parametrize("option", ["", "  ha_integration:\n    auto_register: false\n"])
def test_omitted_or_false_auto_register_preserves_direct_legacy(tmp_path, option):
    result = run_config(tmp_path, option, extra=API)
    assert result.returncode == 0, result.stdout + result.stderr
    main = next(tmp_path.rglob("main.cpp")).read_text()
    defines = next(tmp_path.rglob("defines.h")).read_text()
    assert "USE_VOIP_HA_AUTO_REGISTER" not in defines
    assert "USE_ESPHOME_VOIP_REGISTRATION" not in defines
    assert "configure_registration" not in main
    assert "VoipHAIntegration" in main and "voip_state" in main
    assert "->set_endpoint_sensor(" not in main


@pytest.mark.parametrize("line", ["ha", "direct", "office"])
def test_managed_and_manual_account_have_explicit_independent_lines(tmp_path, line):
    config = MANAGED + ACCOUNT.replace("    accounts:", f"    default_line: {line}\n    accounts:")
    result = run_config(tmp_path, config, extra=API)
    assert result.returncode == 0, result.stdout + result.stderr
    main = next(tmp_path.rglob("main.cpp")).read_text()
    assert f'configure_registration(true, "{line}")' in main
    assert 'set_registration_accounts(phone_registration_accounts, 1)' in main
    assert '.id = "office"' in main and '.id = "ha"' not in main


@pytest.mark.parametrize("registration,reason", [
    (ACCOUNT.replace("account_id: office", "account_id: ha"), "account_id ha is managed"),
    (ACCOUNT.replace("    accounts:", "    max_accounts: 1\n    accounts:"), "leave one slot"),
    (ACCOUNT.replace("    accounts:", "    default_line: unknown\n    accounts:"), "default_line"),
])
def test_invalid_managed_slot_and_line_are_rejected(tmp_path, registration, reason):
    result = run_config(tmp_path, MANAGED + registration, generate=False, extra=API)
    assert result.returncode != 0
    assert reason in result.stdout + result.stderr


def test_managed_display_name_cannot_be_overridden_by_copied_api_glue(tmp_path):
    result = run_config(tmp_path, MANAGED, generate=False, extra=API + """  actions:
    - action: set_ha_display_name
      variables:
        name: string
      then:
        - logger.log: duplicate
""")
    assert result.returncode != 0
    assert "Remove the old VoIP HA package" in result.stdout + result.stderr


def test_quoted_false_uses_the_same_optional_boolean_contract(tmp_path):
    result = run_config(tmp_path, '  ha_integration:\n    auto_register: "false"\n', extra=API)
    assert result.returncode == 0, result.stdout + result.stderr
    defines = next(tmp_path.rglob("defines.h")).read_text()
    assert "USE_VOIP_HA_AUTO_REGISTER" not in defines
    assert "USE_ESPHOME_VOIP_REGISTRATION" not in defines
