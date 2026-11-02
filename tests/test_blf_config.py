"""BLF, presence and metadata stay optional and have public bounded APIs."""
import pytest
from test_sip_registration_config import ACCOUNT, run_config


@pytest.mark.parametrize("presence", [False, True])
def test_blf_and_static_metadata_codegen(tmp_path, presence):
    config = ACCOUNT + f"""  blf:
    account_id: office
    max_subscriptions: 2
    presence: {'true' if presence else 'false'}
    pickup_prefix: '*99'
  static_contacts:
    - name: Desk
      route: '101'
      ip: 127.0.0.1
      contact_id: endpoint-desk
      kind: sip_account
      account_id: office
  on_blf_state:
    then:
      - logger.log: BLF changed
"""
    result = run_config(tmp_path, config, extra="""
script:
  - id: pickup
    then:
      - voip_stack.pickup_contact:
          id: phone
          contact_id: endpoint-desk
""")
    assert result.returncode == 0, result.stdout + result.stderr
    defines = next(tmp_path.rglob("defines.h")).read_text()
    generated = next(tmp_path.rglob("main.cpp")).read_text()
    assert "#define USE_ESPHOME_VOIP_BLF" in defines
    assert "#define USE_ESPHOME_VOIP_CONTACT_METADATA" in defines
    assert ("#define USE_ESPHOME_VOIP_PRESENCE" in defines) is presence
    assert "#define USE_ESPHOME_VOIP_MWI" not in defines
    assert "set_contact_metadata" in generated and "ContactKind::SIP_ACCOUNT" in generated
    assert "get_blf_state_trigger" in generated and "PickupContactAction" in generated


@pytest.mark.parametrize("option", ["max_subscriptions: 0", "max_subscriptions: 65", "buffers_in_psram: true", "pickup_prefix: 'bad;code'"])
def test_invalid_blf_resource_configuration_is_rejected(tmp_path, option):
    result = run_config(tmp_path, ACCOUNT + f"  blf:\n    {option}\n", generate=False)
    assert result.returncode != 0


def test_blf_off_profile_has_no_blf_or_contact_metadata(tmp_path):
    result = run_config(tmp_path, ACCOUNT)
    assert result.returncode == 0, result.stdout + result.stderr
    defines = next(tmp_path.rglob("defines.h")).read_text()
    assert "USE_ESPHOME_VOIP_BLF" not in defines and "USE_ESPHOME_VOIP_CONTACT_METADATA" not in defines
