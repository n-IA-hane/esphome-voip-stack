"""Master alias resolution uses declared speaker endpoints once at codegen."""
import ast
import os
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

import pytest
import yaml
from esphome.core import ID
import esphome.config_validation as cv

from test_rx_feedback_config import Config, ref

ROOT = Path(__file__).resolve().parents[1]


def provider(config, speaker):
    nodes = ast.parse((ROOT / 'esphome/components/voip_stack/__init__.py').read_text()).body
    function = next(node for node in nodes if isinstance(node, ast.FunctionDef)
                    and node.name == '_speaker_master_volume_provider')
    namespace = {'fv': SimpleNamespace(full_config=SimpleNamespace(get=lambda: config))}
    exec(compile(ast.Module(body=[function], type_ignores=[]), 'master-alias', 'exec'), namespace)
    return namespace[function.name](speaker)


@pytest.mark.parametrize('leaf', ['esp_audio_stack', 'i2s_audio', 'custom_provider'])
@pytest.mark.parametrize('composite', [False, True])
def test_master_resolves_only_audio_stack_through_supported_adapters(leaf, composite):
    speakers = [{'platform': leaf, 'id': ID('output', is_declaration=True), 'esp_audio_stack_id': ref('audio')}]
    paths = {'output': ['speaker', 0]}
    target = ref('output')
    if composite:
        speakers += [
            {'platform': 'mixer', 'id': ID('mixer', is_declaration=True), 'output_speaker': ref('output'),
             'source_speakers': [{'id': ID('source', is_declaration=True)}, {'id': ID('other', is_declaration=True)}]},
            {'platform': 'resampler', 'id': ID('input', is_declaration=True), 'output_speaker': ref('source')}]
        paths.update({'source': ['speaker', 1, 'source_speakers', 0], 'input': ['speaker', 2]})
        target = ref('input')
    config = Config({'speaker': speakers, 'voip_stack': {'speaker': target}}, paths)
    result = provider(config, target)
    assert (result.id if result else None) == ('audio' if leaf == 'esp_audio_stack' else None)
    config['voip_stack']['_master_volume_provider'] = result
    again = provider(config, target)
    assert (again.id if again else None) == (result.id if result else None)


def test_cycle_or_unresolved_speaker_keeps_legacy_volume_owner():
    config = Config({'speaker': [
        {'platform': 'resampler', 'id': ID('one', is_declaration=True), 'output_speaker': ref('two')},
        {'platform': 'resampler', 'id': ID('two', is_declaration=True), 'output_speaker': ref('one')}],
    }, {'one': ['speaker', 0], 'two': ['speaker', 1]})
    assert provider(config, ref('one')) is None
    assert provider(config, ref('missing')) is None


def test_canonical_control_matches_the_resolved_audio_parent_and_is_unique():
    nodes = ast.parse((ROOT / 'esphome/components/voip_stack/__init__.py').read_text()).body
    function = next(node for node in nodes if isinstance(node, ast.FunctionDef)
                    and node.name == '_master_volume_number_for_provider')
    canonical = ID('canonical', is_declaration=True)
    config = Config({'number': [
        {'platform': 'esp_audio_stack', 'esp_audio_stack_id': ref('other'), 'master_volume': {'id': ID('other_number')}},
        {'platform': 'voip_stack', 'master_volume': {'id': ID('native_number')}},
        {'platform': 'esp_audio_stack', 'esp_audio_stack_id': ref('audio'), 'master_volume': {'id': canonical}}]}, {})
    namespace = {'fv': SimpleNamespace(full_config=SimpleNamespace(get=lambda: config)), 'CONF_ID': 'id', 'cv': cv}
    exec(compile(ast.Module(body=[function], type_ignores=[]), 'canonical-master', 'exec'), namespace)
    resolve = namespace[function.name]
    assert resolve(ref('audio')) is canonical
    assert resolve(ref('absent')) is None and resolve(None) is None
    config['number'].append({'platform': 'esp_audio_stack', 'esp_audio_stack_id': ref('audio'),
                             'master_volume': {'id': ID('duplicate')}})
    with pytest.raises(cv.Invalid, match='one master_volume'):
        resolve(ref('audio'))


@pytest.mark.parametrize('kind', ['legacy', 'direct', 'composite', 'direct-implicit', 'composite-implicit',
                                'direct-canonical', 'composite-canonical', 'direct-canonical-implicit', 'composite-canonical-implicit'])
def test_real_codegen_binds_the_actual_master_only_when_present(tmp_path, kind):
    data = yaml.safe_load((ROOT / 'tests/minimal_speaker_only.yaml').read_text())
    data['external_components'][0]['source'] = str(ROOT / 'esphome/components')
    data['number'] = [{'platform': 'voip_stack', 'master_volume': {'id': 'native_master', 'name': 'Native Master'}}]
    if kind != 'legacy':
        audio_source = os.environ.get('ESP_AUDIO_STACK_SOURCE')
        if not audio_source:
            pytest.skip('The integrated codegen gate requires ESP_AUDIO_STACK_SOURCE')
        audio_path = Path(audio_source) / 'esphome/components'
        assert (audio_path / 'esp_audio_stack/__init__.py').is_file()
        data['external_components'].append({'source': str(audio_path), 'components': ['esp_audio_stack']})
        data.pop('i2s_audio')
        data['psram'] = {'mode': 'octal', 'speed': '80MHz'}
        data['esp_audio_stack'] = {
            'id': 'audio_master', 'sample_rate': 48000, 'output_sample_rate': 48000,
            'bits_per_sample': 32, 'slot_bit_width': 32, 'i2s_lrclk_pin': 'GPIO7', 'i2s_bclk_pin': 'GPIO6',
            'i2s_din_pin': 'GPIO4', 'i2s_dout_pin': 'GPIO8', 'buffers_in_psram': True}
        data['speaker'] = [{'platform': 'esp_audio_stack', 'id': 'spk_main', 'esp_audio_stack_id': 'audio_master',
                            'sample_rate': 48000, 'bits_per_sample': 16}]
        if 'canonical' in kind:
            data['number'].append({'platform': 'esp_audio_stack', 'esp_audio_stack_id': 'audio_master',
                                   'master_volume': {'id': 'audio_master_control', 'name': 'Audio Master'}})
        if kind.endswith('implicit'):
            data['speaker'][0].pop('esp_audio_stack_id')
            if 'canonical' in kind:
                data['number'][-1].pop('esp_audio_stack_id')
        if kind.startswith('composite'):
            data['speaker'] += [
                {'platform': 'mixer', 'id': 'audio_mixer', 'output_speaker': 'spk_main',
                 'source_speakers': [{'id': 'voip_source'}, {'id': 'other_source'}]},
                {'platform': 'resampler', 'id': 'voip_input', 'output_speaker': 'voip_source',
                 'sample_rate': 48000, 'bits_per_sample': 16}]
            data['voip_stack']['speaker'] = 'voip_input'
    path = tmp_path / 'master.yaml'
    path.write_text(yaml.safe_dump(data, sort_keys=False))
    generated = subprocess.run([os.environ.get('ESPHOME_PYTHON', sys.executable), '-m', 'esphome', 'compile',
                                str(path), '--only-generate'], capture_output=True, text=True, timeout=90)
    assert generated.returncode == 0, generated.stdout + generated.stderr
    main = next(tmp_path.rglob('main.cpp')).read_text()
    defines = next(tmp_path.rglob('defines.h')).read_text()
    assert ('USE_ESPHOME_VOIP_STACK_MASTER_ALIAS' in defines) == (kind != 'legacy')
    assert ('phone->set_master_volume_control(audio_master)' in main) == (kind != 'legacy')
    assert ('USE_ESPHOME_VOIP_STACK_MASTER_NUMBER_ALIAS' in defines) == ('canonical' in kind)
    assert ('phone->set_master_volume_number(audio_master_control)' in main) == ('canonical' in kind)
