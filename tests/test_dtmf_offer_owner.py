"""Complete SIP/media owners retain the DTMF mappings from their real offers."""
from pathlib import Path
import shutil
import subprocess

import pytest

from test_transport_owner_lifecycle import build_transport_owner

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope='module')
def video_owner(tmp_path_factory):
    return build_transport_owner(tmp_path_factory, 'qa_p4_video_reoffer.cpp',
                                 video=True, registration=True, dtmf=True)


@pytest.mark.parametrize('accept_video', [False, True])
def test_incoming_call_video_offer_preserves_audio_and_dtmf(video_owner, tmp_path, accept_video):
    captured = ROOT / 'tests/fixtures/captured_qa_video_reoffer'
    for path in captured.glob('*.sdp'):
        shutil.copyfile(path, tmp_path / path.name)
    mode = 'captured-home-qa-incoming'
    if accept_video:
        # The independent positive control accepts video while preserving every
        # captured audio/PT/telephone-event fact in the original HA response.
        body = (tmp_path / 'video_on_answer.sdp').read_bytes().decode()
        rejected = 'm=video 0 RTP/AVP 26\r\n'
        accepted = ('m=video 40002 RTP/AVP 26\r\nc=IN IP4 192.168.1.200\r\n'
                    'a=rtcp:40003 IN IP4 192.168.1.200\r\na=rtpmap:26 JPEG/90000\r\n'
                    'a=framerate:10\r\na=recvonly\r\n')
        assert rejected in body
        body = body.replace(rejected, accepted)
        (tmp_path / 'video_on_answer.sdp').write_bytes(body.encode())
        (tmp_path / 'video_off_answer.sdp').write_bytes(body.replace('a=recvonly', 'a=inactive').encode())
        mode += '-accepted'
    result = subprocess.run([video_owner, mode, ROOT / 'tests/fixtures/video_source_frame.hex', tmp_path],
                            capture_output=True, text=True, timeout=20)
    assert result.returncode == 0, result.stdout + result.stderr
    assert 'post_toggle_dtmf_wire_pt=104 digit=5 duration_ticks=2560 end_reports=3' in result.stderr


@pytest.fixture(scope='module')
def delayed_owner(tmp_path_factory):
    return build_transport_owner(tmp_path_factory, 'dtmf_alias_delayed_owner.cpp',
                                 registration=True, dtmf=True)


def test_actual_delayed_offer_retains_both_same_clock_aliases(delayed_owner, tmp_path):
    (tmp_path / 'profile.txt').write_text('tx pcm 16000 1 20 s16le\ntx pcm 16000 1 10 s16le\n'
                                         'rx pcm 16000 1 10 s16le\nrx pcm 16000 1 20 s16le\n')
    answer = ('v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=Independent alias peer\r\n'
              'c=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio 40000 RTP/AVP 96 106\r\n'
              'a=rtpmap:96 L16/16000/1\r\na=ptime:20\r\n'
              'a=rtpmap:106 telephone-event/16000\r\na=fmtp:106 0-11\r\na=sendrecv\r\n')
    (tmp_path / 'answer.sdp').write_bytes(answer.encode())
    result = subprocess.run([delayed_owner, tmp_path / 'profile.txt', tmp_path / 'answer.sdp',
                             tmp_path / 'answer.sdp', tmp_path / 'actual-offer.sdp', 'alias-delayed', '200'],
                            capture_output=True, text=True, timeout=20)
    assert result.returncode == 0, result.stdout + result.stderr
    assert '"rx_pt":122' in result.stdout and '"tx_pt":106' in result.stdout
    assert '"dtmf_end_reports":3' in result.stdout
