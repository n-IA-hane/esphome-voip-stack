"""Release video firmware retains actual send failures without debug logging."""

import subprocess

from test_transport_owner_lifecycle import build_transport_owner


def test_release_video_reports_real_failed_and_successful_udp_sends(tmp_path_factory):
    binary = build_transport_owner(tmp_path_factory, fixture="video_send_evidence.cpp", video=True)
    result = subprocess.run([binary], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
