"""Native ESPHome migration diagnostics and generic initial destination codegen."""
import pytest

from test_sip_registration_config import run_config


@pytest.mark.parametrize("target", ["", "101", "sip:door@192.0.2.20:5070;transport=tcp"])
def test_generic_initial_contact_codegen_without_directory(tmp_path, target):
    result = run_config(tmp_path, f'  initial_contact: "{target}"\n')
    assert result.returncode == 0, result.stdout + result.stderr
    generated = next(tmp_path.rglob("main.cpp")).read_text()
    assert f'set_initial_contact("{target}")' in generated
    assert "set_use_ha_as_first_contact" not in generated


@pytest.mark.parametrize("option,message", [
    ("  use_ha_as_first_contact: true\n", "Use initial_contact"),
    ("  delete_contact_missing_from: old-provider\n", "Use ldap_directory"),
])
def test_retired_contact_options_have_actionable_migration(tmp_path, option, message):
    result = run_config(tmp_path, option, generate=False)
    assert result.returncode != 0
    assert message in result.stdout + result.stderr


def test_retired_csv_action_has_actionable_migration(tmp_path):
    result = run_config(tmp_path, "", generate=False, extra='''
button:
  - platform: template
    name: Old contact batch
    on_press:
      - voip_stack.set_contacts:
          id: phone
          contacts: Desk
''')
    assert result.returncode != 0
    assert "CSV batches are retired" in result.stdout + result.stderr


def test_retired_esp_media_sensor_points_to_native_ha_entity(tmp_path):
    result = run_config(tmp_path, "", generate=False, extra='''
text_sensor:
  - platform: voip_stack
    voip_stack_id: phone
    type: media_diagnostics
    name: Legacy media diagnostics
''')
    assert result.returncode != 0
    assert "HA SIP account's media processing sensor" in result.stdout + result.stderr


def test_optional_media_diagnostic_uses_native_ha_text_sensor(tmp_path):
    result = run_config(tmp_path, "  ha_integration: false\n", extra='''
api:
substitutions:
  account_media_processing_entity: sensor.selected_account_media_processing
text_sensor:
  - platform: homeassistant
    id: account_media_processing
    entity_id: ${account_media_processing_entity}
    internal: true
''')
    assert result.returncode == 0, result.stdout + result.stderr
    generated = next(tmp_path.rglob("main.cpp")).read_text()
    assert 'set_entity_id("sensor.selected_account_media_processing")' in generated
    assert "set_pbx_media_diagnostics" not in generated
