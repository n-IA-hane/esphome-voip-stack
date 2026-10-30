"""Public keypad action, native API and optional receive-event composition."""
from pathlib import Path
import os
import re
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]


def checked(tmp_path, *, action=None, events=False, native=False, duplicate=False, generate=False):
    source = (ROOT / 'tests/minimal_no_text.yaml').read_text().replace(
        '../esphome/components', str(ROOT / 'esphome/components'))
    if events:
        source = source.replace('voip_stack:\n', 'voip_stack:\n  on_dtmf:\n    then:\n      - lambda: (void) digit;\n')
    if native:
        source += '\napi:\n  custom_services: true\n'
        if duplicate:
            source += '  actions:\n    - action: send_dtmf\n      then:\n        - lambda: return;\n'
    if action:
        source += '\nscript:\n  - id: keypad\n    then:\n      - voip_stack.send_dtmf:\n          id: phone\n' + action
        source += '      - if:\n          condition:\n            voip_stack.can_send_dtmf: phone\n          then:\n            - lambda: return;\n'
    path = tmp_path / 'phone.yaml'
    path.write_text(source)
    command = [os.environ.get('ESPHOME_PYTHON', sys.executable), '-m', 'esphome',
               'compile' if generate else 'config', str(path)]
    if generate:
        command.append('--only-generate')
    run = subprocess.run(command, capture_output=True, text=True, timeout=120)
    (tmp_path / 'validation.log').write_text(run.stdout + run.stderr)
    return run


@pytest.mark.parametrize('events,native', [(False, False), (False, True), (True, True)])
def test_sender_is_standard_but_receive_events_are_optional(tmp_path, events, native):
    run = checked(tmp_path, action="          digit: '1'\n", events=events, native=native, generate=True)
    assert run.returncode == 0, run.stdout + run.stderr
    definitions = next(tmp_path.rglob('src/esphome/core/defines.h'))
    main = (definitions.parents[2] / 'main.cpp').read_text()
    defines = definitions.read_text()
    assert '#define USE_ESPHOME_VOIP_DTMF\n' in defines
    assert ('#define USE_ESPHOME_VOIP_DTMF_EVENTS\n' in defines) is events
    assert ('get_dtmf_trigger()' in main) is events
    assert 'SendDtmfAction' in main and 'VoipCanSendDtmfCondition' in main
    assert re.search(r'set_duration_ms\(\[\]\(\) -> uint16_t \{\s*return 160;\s*\}\)', main)
    assert ('voip_stack::VoipHAIntegration' in main) is native


@pytest.mark.parametrize('digit,duration', [('1', 39), ('1', 8001), ('', 160), ('12', 160), ('E', 160)])
def test_invalid_fixed_digit_or_duration_is_rejected(tmp_path, digit, duration):
    run = checked(tmp_path, action=f"          digit: '{digit}'\n          duration_ms: {duration}\n")
    assert run.returncode != 0
    assert ('digit' if duration == 160 else 'duration_ms') in run.stdout + run.stderr


def test_native_dtmf_action_cannot_be_shadowed(tmp_path):
    run = checked(tmp_path, native=True, duplicate=True)
    assert run.returncode != 0
    assert 'send_dtmf' in run.stdout + run.stderr
    assert 'Remove the old VoIP HA package' in run.stdout + run.stderr


def test_public_action_fixture_and_templates_generate(tmp_path):
    fixture = (ROOT / 'tests/dtmf_actions.yaml').read_text().replace('!include minimal_no_text.yaml',
                                                                               '!include ' + str(ROOT / 'tests/minimal_no_text.yaml'))
    path = tmp_path / 'actions.yaml'
    path.write_text(fixture)
    run = subprocess.run([os.environ.get('ESPHOME_PYTHON', sys.executable), '-m', 'esphome',
                          'compile', str(path), '--only-generate'], capture_output=True, text=True, timeout=120)
    assert run.returncode == 0, run.stdout + run.stderr
    definitions = next(tmp_path.rglob('src/esphome/core/defines.h'))
    main = (definitions.parents[2] / 'main.cpp').read_text()
    assert 'return std::string("#");' in main
    assert 'return uint16_t(200);' in main
    assert 'get_dtmf_trigger()' in main
