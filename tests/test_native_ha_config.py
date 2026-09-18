"""Native HA surface is generated without a package and rejects obsolete glue."""
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]


def validate(tmp_path, suffix, *, generate=False):
    config = (ROOT / 'tests/minimal_no_text.yaml').read_text().replace(
        '../esphome/components', str(ROOT / 'esphome/components'))
    config += suffix
    path = tmp_path / 'phone.yaml'
    path.write_text(config)
    python = os.environ.get('ESPHOME_PYTHON', sys.executable)
    args = [python, '-m', 'esphome', 'compile' if generate else 'config', str(path)]
    if generate:
        args.append('--only-generate')
    return subprocess.run(args, text=True, capture_output=True)


def test_native_ha_minimal_codegen(tmp_path):
    result = validate(tmp_path, '\napi:\n  custom_services: true\n', generate=True)
    assert result.returncode == 0, result.stdout + result.stderr
    generated = next(tmp_path.rglob('main.cpp')).read_text()
    assert 'VoipHAIntegration' in generated
    for role in ['endpoint', 'state', 'contacts', 'media_route', 'extension']:
        assert 'voip_' + role in generated


def test_standalone_has_no_ha_adapter(tmp_path):
    result = validate(tmp_path, '', generate=True)
    assert result.returncode == 0, result.stdout + result.stderr
    generated = next(tmp_path.rglob('main.cpp')).read_text()
    assert 'new voip_stack::VoipHAIntegration' not in generated
    assert 'voip_endpoint' not in generated


def test_missing_api_service_support_is_actionable(tmp_path):
    result = validate(tmp_path, '\napi:\n')
    assert result.returncode != 0
    assert 'custom_services: true' in result.stdout + result.stderr


def test_duplicate_native_action_is_rejected(tmp_path):
    result = validate(tmp_path, '\napi:\n  custom_services: true\n  actions:\n    - action: answer_call\n      then:\n        - logger.log: duplicate\n')
    assert result.returncode != 0
    assert 'Remove the old VoIP HA package' in result.stdout + result.stderr


def test_api_can_be_used_without_voip_ha_surface(tmp_path):
    config = (ROOT / 'tests/minimal_no_text.yaml').read_text().replace(
        '../esphome/components', str(ROOT / 'esphome/components'))
    config = config.replace('voip_stack:\n', 'voip_stack:\n  ha_integration: false\n') + '\napi:\n'
    path = tmp_path / 'standalone.yaml'
    path.write_text(config)
    result = subprocess.run([os.environ.get('ESPHOME_PYTHON', sys.executable), '-m',
                             'esphome', 'compile', str(path), '--only-generate'],
                            text=True, capture_output=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert 'new voip_stack::VoipHAIntegration' not in next(tmp_path.rglob('main.cpp')).read_text()


def test_retired_package_marker_is_actionable(tmp_path):
    package = tmp_path / 'old-ha-phone.yaml'
    package.write_text('voip_stack:\n  legacy_ha_package: ha_phone\n')
    result = validate(tmp_path, f'\npackages:\n  old_phone: !include {package}\napi:\n  custom_services: true\n')
    assert result.returncode != 0
    assert 'retired in 2026.10.0' in result.stdout + result.stderr


def test_native_ha_speaker_only_codegen(tmp_path):
    config = (ROOT / 'tests/minimal_no_text.yaml').read_text().replace(
        '../esphome/components', str(ROOT / 'esphome/components'))
    start = config.index('microphone:\n')
    end = config.index('voip_stack:\n', start)
    config = config[:start] + '''speaker:
  - platform: i2s_audio
    id: hw_speaker
    i2s_audio_id: rx_i2s
    dac_type: external
    i2s_dout_pin: GPIO11
    sample_rate: 48000

''' + config[end:]
    start = config.index('  microphone_source:\n')
    config = config[:start] + '  speaker: hw_speaker\n\napi:\n  custom_services: true\n'
    path = tmp_path / 'speaker.yaml'
    path.write_text(config)
    result = subprocess.run([os.environ.get('ESPHOME_PYTHON', sys.executable), '-m',
                             'esphome', 'compile', str(path), '--only-generate'],
                            text=True, capture_output=True)
    assert result.returncode == 0, result.stdout + result.stderr
    generated = next(tmp_path.rglob('main.cpp')).read_text()
    assert 'VoipHAIntegration' in generated
    assert 'voip_endpoint' in generated
