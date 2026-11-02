"""Public MWI action and callback codegen stays optional."""
import pytest
from test_sip_registration_config import ACCOUNT, run_config


@pytest.mark.parametrize("enabled", [False, True])
def test_message_waiting_is_compile_gated(tmp_path, enabled):
    registration = ACCOUNT.replace("    accounts:", f"    message_waiting: {'true' if enabled else 'false'}\n    accounts:")
    result = run_config(tmp_path, registration)
    assert result.returncode == 0, result.stdout + result.stderr
    defines = next(tmp_path.rglob("defines.h")).read_text()
    assert ("#define USE_ESPHOME_VOIP_MWI" in defines) is enabled
    assert "#define USE_ESPHOME_VOIP_BLF" not in defines


def test_message_waiting_actions_and_hook_generate(tmp_path):
    config = ACCOUNT.replace("    accounts:", "    message_waiting: true\n    accounts:")
    config += """  on_message_waiting:
    then:
      - logger.log: Mailbox changed
"""
    result = run_config(tmp_path, config, extra="""
button:
  - platform: template
    name: Mailbox
    on_press:
      - if:
          condition:
            voip_stack.has_waiting_messages:
              id: phone
              account_id: office
          then:
            - voip_stack.call_mailbox:
                id: phone
                account_id: office
                target: '*97'
""")
    assert result.returncode == 0, result.stdout + result.stderr
    generated = next(tmp_path.rglob("main.cpp")).read_text()
    assert "get_message_waiting_trigger()" in generated
    assert "CallMailboxAction" in generated and "HasWaitingMessagesCondition" in generated


@pytest.mark.parametrize("feature", ["on_message_waiting", "call_mailbox", "has_waiting_messages"])
def test_message_waiting_disabled_rejects_its_public_hooks(tmp_path, feature):
    config = ACCOUNT
    extra = ""
    if feature == "on_message_waiting":
        config += "  on_message_waiting:\n    then:\n      - logger.log: Mailbox\n"
    elif feature == "call_mailbox":
        extra = "script:\n  - id: mailbox\n    then:\n      - voip_stack.call_mailbox: phone\n"
    else:
        extra = "script:\n  - id: mailbox\n    then:\n      - if:\n          condition:\n            voip_stack.has_waiting_messages: phone\n          then:\n            - logger.log: Waiting\n"
    result = run_config(tmp_path, config, extra=extra)
    assert result.returncode != 0
    assert "message_waiting" in result.stdout + result.stderr
