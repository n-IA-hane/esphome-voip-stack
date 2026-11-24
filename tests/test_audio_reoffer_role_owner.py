"""Full transport replays preserve directional PCM contracts through role changes."""
import json
import subprocess

import pytest

from test_transport_owner_lifecycle import ROOT, SOURCE, build_transport_owner

# Anonymized original device offer. Only session identity and network fields
# differ from the preserved physical witness; every audio line is retained.
CAPTURED_OFFER = '''v=0
 o=- 1 0 IN IP4 192.0.2.10
 s=VoIP Stack
 c=IN IP4 192.0.2.10
 t=0 0
 m=audio 40000 RTP/AVP 96 97 9 120 122 126
 b=RS:0
 b=RR:0
 a=rtpmap:96 L16/48000/1
 a=rtpmap:97 L16/16000/1
 a=rtpmap:9 G722/8000/1
 a=rtpmap:120 telephone-event/8000
 a=fmtp:120 0-15
 a=rtpmap:122 telephone-event/16000
 a=fmtp:122 0-15
 a=rtpmap:126 telephone-event/48000
 a=fmtp:126 0-15
 a=ptime:10
 a=maxptime:20
 a=sendrecv
'''.replace('\n ', '\n')


def profile(wide=48000, narrow=16000):
    rows = []
    for flow, formats in (
        ('tx', [('pcm', narrow, 20), ('pcm', narrow, 10), ('pcm', wide, 10),
                ('g722', 16000, 20), ('g722', 16000, 10)]),
        ('rx', [('pcm', wide, 10), ('pcm', wide, 20), ('pcm', narrow, 20), ('pcm', narrow, 10),
                ('g722', 16000, 20), ('g722', 16000, 10)]),
    ):
        rows.extend(f'{flow} {codec} {rate} 1 {ms} s16le' for codec, rate, ms in formats)
    return '\n'.join(rows) + '\n'


def sdp(mappings, *, port=48080, ptime=20, maxptime=None):
    payloads = ' '.join(str(payload) for payload, _ in mappings)
    lines = ['v=0', 'o=independent 123 1 IN IP4 192.0.2.20', 's=Independent protocol witness',
             'c=IN IP4 192.0.2.20', 't=0 0', f'm=audio {port} RTP/AVP {payloads}']
    lines += [f'a=rtpmap:{payload} {meaning}' for payload, meaning in mappings]
    if ptime is not None:
        lines.append(f'a=ptime:{ptime}')
    if maxptime is not None:
        lines.append(f'a=maxptime:{maxptime}')
    return '\r\n'.join([*lines, 'a=sendrecv', ''])


def audio_lines(text):
    result = []
    for line in text.splitlines():
        if line.startswith(('v=', 'o=', 's=', 'c=', 't=')):
            continue
        if line.startswith('m=audio '):
            words = line.split()
            words[1] = '<port>'
            line = ' '.join(words)
        if line:
            result.append(line)
    return result


@pytest.fixture(scope='module')
def role_owner(tmp_path_factory):
    return build_transport_owner(tmp_path_factory, fixture='audio_reoffer_role_owner.cpp',
                                 registration=True, speaker=True, dtmf=True, g722=True)


@pytest.mark.parametrize('case', [
    'captured', 'captured-offerless', 'incoming-offerless', 'alternate', 'alternate-incoming-offerless',
    'omitted-peer-ptime', 'new-local-offered-pt', 'new-alias', 'old-pt-dropped', 'old-pt-remapped',
    'peer-ptime-change', 'peer-maxptime', 'l16-to-g722-new-pt', 'l16-to-g722-remap',
    'g722-to-l16', 'g722-retarget',
])
def test_complete_owner_reoffer_retains_only_admitted_receive_contract(role_owner, tmp_path, case):
    alternate = case.startswith('alternate')
    wide, narrow = (32000, 8000) if alternate else (48000, 16000)
    old_g722 = case in {'g722-to-l16', 'g722-retarget'}
    initial = [(9, 'G722/8000/1')] if old_g722 else [(97, f'L16/{narrow}/1')]
    proposal = initial
    packet_time, maximum, expected = 20, None, 200
    if case == 'new-local-offered-pt':
        proposal = [*initial, (96, f'L16/{wide}/1')]
    elif case == 'new-alias':
        proposal = [*initial, (98, f'L16/{narrow}/1')]
    elif case == 'old-pt-dropped':
        proposal = [(96, f'L16/{wide}/1')]
    elif case == 'old-pt-remapped':
        proposal = [(97, f'L16/{wide}/1')]
    elif case == 'peer-ptime-change':
        packet_time = 10
    elif case == 'peer-maxptime':
        maximum = 10
    elif case == 'l16-to-g722-new-pt':
        proposal = [*initial, (9, 'G722/8000/1')]
    elif case == 'l16-to-g722-remap':
        proposal = [(97, 'G722/8000/1')]
    elif case == 'g722-to-l16':
        proposal = [(97, f'L16/{narrow}/1')]
    elif case == 'omitted-peer-ptime':
        packet_time = None
    if case in {'new-local-offered-pt', 'new-alias', 'old-pt-dropped', 'old-pt-remapped',
                'peer-ptime-change', 'peer-maxptime', 'l16-to-g722-new-pt',
                'l16-to-g722-remap', 'g722-to-l16'}:
        expected = 488
    mode = 'incoming' if 'incoming' in case else 'outgoing'
    if 'offerless' in case:
        mode += '-offerless'
    files = [tmp_path / name for name in ('profile.txt', 'initial.sdp', 'reoffer.sdp', 'local.sdp')]
    files[0].write_text(profile(wide, narrow))
    files[1].write_text(sdp(initial))
    files[2].write_text(sdp(proposal, port=48082, ptime=packet_time, maxptime=maximum))
    result = subprocess.run([str(role_owner), *map(str, files), mode, str(expected)],
                            capture_output=True, text=True, timeout=20)
    assert result.returncode == 0, result.stdout + result.stderr
    facts = json.loads(result.stdout.splitlines()[-1])
    assert facts['wire_status'] == expected and facts['same_media_resources'] and facts['media_usable']
    assert facts['cleanup_complete'] and facts['offerless_ok']
    assert facts['initial_tx']['codec'] == (2 if old_g722 else 0)
    assert facts['initial_rx']['ms'] == (20 if mode.startswith('incoming') else 10)
    if expected == 200:
        assert facts['proposed_rx'] == facts['initial_rx']
    if case.startswith('captured'):
        assert audio_lines(files[3].read_text()) == audio_lines(CAPTURED_OFFER)


@pytest.fixture(scope='module')
def snapshot_contract(tmp_path_factory):
    folder = tmp_path_factory.mktemp('audio-offer-snapshot')
    for name in ('esphome/core/defines.h', 'esphome/core/hal.h', 'esp_system.h', 'lwip/sockets.h'):
        header = folder / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "sip_dialog_host_sdk.h"\n')
    binary = folder / 'contract'
    result = subprocess.run([
        'g++', '-std=c++17', '-DUSE_ESP32', '-DUSE_ESPHOME_VOIP_SIP_TRANSPORT',
        '-DUSE_ESPHOME_VOIP_STACK_SPEAKER', '-I', str(folder), '-I', str(ROOT / 'tests/fixtures'),
        '-I', str(ROOT), '-I', str(SOURCE),
        str(ROOT / 'tests/fixtures/audio_offer_snapshot_contract.cpp'), str(SOURCE / 'sip_message.cpp'),
        str(SOURCE / 'media_negotiator.cpp'), '-o', str(binary),
    ], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize('case', ['bounded-fallback', 'absent-maxptime', 'atomic-failure',
                                  'explicit-local-offer', 'remote-maxptime', 'retired-local-rx',
                                  'retained-complete-contract'])
def test_actual_offer_snapshot_owns_receive_policy(snapshot_contract, case):
    result = subprocess.run([str(snapshot_contract), case], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr
