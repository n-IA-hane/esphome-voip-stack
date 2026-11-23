"""Replay captured HOME answers through the complete SIP and media owners."""
from pathlib import Path
import subprocess
import pytest

from test_transport_owner_lifecycle import build_transport_owner

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def captured_owner(tmp_path_factory):
    return build_transport_owner(tmp_path_factory, "video_media_activation.cpp", video=True, registration=True, dtmf=True)


@pytest.mark.parametrize("mode", ["captured-home", "captured-home-bad-codec", "captured-home-bad-pt"])
def test_explicit_audio_only_call_can_stop_video_without_ending_audio(captured_owner, mode):
    result = subprocess.run([
        captured_owner, mode, ROOT / "tests/fixtures/video_source_frame.hex",
        ROOT / "tests/fixtures/captured_home_video_off",
    ], capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result.stdout + result.stderr
