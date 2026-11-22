"""Run late video activation through the complete dialog and RTP owners."""
from pathlib import Path
import subprocess

import pytest
from test_transport_owner_lifecycle import build_transport_owner

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def video_activation_binary(tmp_path_factory):
    return build_transport_owner(tmp_path_factory, "video_media_activation.cpp", video=True, registration=True)


@pytest.mark.parametrize("mode", ["rejected", "local-failure", "absent", "initial-on", "route", "incoming", "reject-488", "reject-video", "timeout", "glare"])
def test_video_source_can_join_established_audio(video_activation_binary, mode):
    ran = subprocess.run(
        [video_activation_binary, mode, ROOT / "tests/fixtures/video_source_frame.hex"],
        capture_output=True, text=True, timeout=15,
    )
    assert ran.returncode == 0, ran.stdout + ran.stderr
