"""Complete SIP owners retain local RX preference and peer encoder constraints."""

import subprocess

import pytest

from test_transport_owner_lifecycle import build_transport_owner


@pytest.fixture(scope="module")
def pcm_refresh_owner(tmp_path_factory):
    return build_transport_owner(tmp_path_factory, "negotiated_media_refresh.cpp", video=True)


@pytest.mark.parametrize("transport", ["udp", "tcp"])
@pytest.mark.parametrize("mode", ["refresh", "hold", "video", "bad-pt", "bad-rate", "l24-storage"])
def test_uac_receive_aliases_survive_real_reoffers(pcm_refresh_owner, transport, mode):
    result = subprocess.run([pcm_refresh_owner, transport, mode], capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.fixture(scope="module")
def opus_answer_owner(tmp_path_factory):
    return build_transport_owner(tmp_path_factory, "negotiated_media_refresh.cpp", opus=True)


@pytest.mark.parametrize("transport", ["udp", "tcp"])
def test_uas_answer_keeps_peer_fmtp_until_real_opus_encoding(opus_answer_owner, transport):
    result = subprocess.run([opus_answer_owner, transport, "opus-uas"], capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result.stdout + result.stderr
