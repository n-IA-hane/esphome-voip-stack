"""Real ESPHome codegen does not invent the missing audio endpoint."""
import os
from pathlib import Path
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize('role', ['microphone', 'speaker'])
@pytest.mark.parametrize('legacy_absent_format', [False, True])
def test_absent_audio_direction_is_not_generated(tmp_path, role, legacy_absent_format):
    path = ROOT / 'tests' / ('minimal_no_text.yaml' if role == 'microphone' else 'minimal_speaker_only.yaml')
    config = path.read_text().replace('../esphome/components', str(ROOT / 'esphome/components'))
    if not legacy_absent_format:
        import yaml
        data = yaml.safe_load(config)
        data['voip_stack']['audio'].pop('rx' if role == 'microphone' else 'tx', None)
        config = yaml.safe_dump(data, sort_keys=False)
    fixture = tmp_path / 'phone.yaml'
    fixture.write_text(config)
    proc = subprocess.run([os.environ.get('ESPHOME_PYTHON', sys.executable), '-m', 'esphome', 'compile', str(fixture), '--only-generate'], capture_output=True, text=True, timeout=60)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    code = next(tmp_path.rglob('main.cpp')).read_text()
    present, absent = ('tx', 'rx') if role == 'microphone' else ('rx', 'tx')
    assert f'set_{present}_audio_format(48000' in code
    assert f'set_{absent}_audio_format(' not in code
    assert f'add_supported_{absent}_audio_format(' not in code
