"""H.264 receive dependency recovery with a real UDP socket and production reassembly."""
import shutil
import socket
import struct
import subprocess
from pathlib import Path

import pytest

from test_refer_dialog_runtime import SOURCE, block
from test_video_resource_lifecycle import RTOS, compile_harness, write_headers


# A 16x16 grey H.264 Constrained Baseline stream (libx264, GOP 6, no B frames).
# Only the SPS, PPS and coded pictures matter for this transport test.
SPS = bytes.fromhex('6742c00ad91ec044000003000400000300503c489920')
PPS = bytes.fromhex('68cb83cb20')
IDR = bytes.fromhex('6588840ff2628000803e')
P1 = bytes.fromhex('419a381fea')
P2 = bytes.fromhex('419a5407fa80')
P3 = bytes.fromhex('419a603fd4')
P4 = bytes.fromhex('419a803fd4')
P5 = bytes.fromhex('419aa03fd4')
NEXT_P1 = bytes.fromhex('419a381bea')
NEXT_IDR = bytes.fromhex('658882011f2628000809e0')
START = bytes.fromhex('00000001')


@pytest.fixture(scope='module')
def receiver(tmp_path_factory):
    folder = tmp_path_factory.mktemp('h264-receiver')
    for name in ('video_rtp.h', 'video.h', 'rtp_jpeg.h'):
        shutil.copyfile(SOURCE / name, folder / name)
    write_headers(folder, {
        'esphome/core/defines.h': '#pragma once\n',
        'audio_core_task_utils.h': '#pragma once\n',
        'esphome/core/helpers.h': '#pragma once\n#include <mutex>\nnamespace esphome {using Mutex=std::mutex;using LockGuard=std::lock_guard<Mutex>;}\n',
        'freertos/FreeRTOS.h': RTOS,
        'freertos/semphr.h': '#include "FreeRTOS.h"\n',
        'freertos/task.h': '#include "FreeRTOS.h"\n',
    })
    markers = [
        'uint32_t read_be32(', 'void write_be16(', 'void write_be32(',
        'bool rtp_payload_bounds(', 'bool next_annex_b_nal(',
        'VideoRtpSession::VideoRtpSession(', 'bool VideoRtpSession::handle_rtp_packet_(',
        'bool VideoRtpSession::handle_h264_payload_(', 'bool VideoRtpSession::append_annex_b_nal_(',
        'void VideoRtpSession::finish_access_unit_(', 'void VideoRtpSession::reset_reassembly_(',
        'bool VideoRtpSession::acquire_reassembly_slot_(', 'void VideoRtpSession::release_reassembly_slot_(',
        'size_t VideoRtpSession::build_rtcp_report_sdes_(', 'void VideoRtpSession::send_rtcp_pli_(',
    ]
    src = (SOURCE / 'video_rtp.cpp').read_text()
    methods = '\n'.join(block(src, marker) for marker in markers)
    fixture = (Path(__file__).parent / 'fixtures/h264_receive.cpp').read_text()
    return compile_harness(folder, fixture.replace('/* PRODUCTION */', methods), defines=(
        'USE_ESP32', 'USE_ESPHOME_VOIP_STACK_VIDEO', 'USE_ESPHOME_VOIP_STACK_VIDEO_H264',
    ))


@pytest.mark.parametrize('feedback', [False, True])
@pytest.mark.parametrize('fault', ['none', 'packet-loss', 'fragment-loss', 'missing-marker', 'sink-reject', 'no-slot'])
def test_h264_receive_resynchronizes_at_next_complete_idr(receiver, fault, feedback):
    # First IDR carries SPS/PPS; the next IDR reuses those cached parameters.
    stream = [(0, SPS, False), (0, PPS, False), (0, IDR, True),
              (1, P1, True), (2, P2, True), (3, P3, True),
              (4, P4, True), (5, P5, True), (6, NEXT_IDR, True), (7, NEXT_P1, True)]
    if fault == 'fragment-loss':
        # Split the first P slice into three legal FU-A packets; the middle one is lost.
        stream[3:4] = [(1, b'\x5c\x81' + P1[1:2], False), (1, None, False),
                       (1, b'\x5c\x41' + P1[3:], True)]
    elif fault == 'missing-marker':
        # The final packet carrying this picture's marker never reaches the receiver.
        stream[3:4] = [(1, b'\x5c\x81' + P1[1:2], False), (1, None, True)]
    with (socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as inbox,
          socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender):
        inbox.bind(('127.0.0.1', 0))
        for seq, (frame, payload, marker) in enumerate(stream):
            if payload is None or (fault == 'packet-loss' and frame == 1):
                continue
            header = struct.pack('!BBHII', 0x80, 105 | (int(marker) << 7), seq, frame * 9000, 1)
            sender.sendto(header + payload, inbox.getsockname())
        run = subprocess.run([str(receiver), str(inbox.fileno()), fault, str(int(feedback))], pass_fds=(inbox.fileno(),),
                             capture_output=True, text=True, check=True, timeout=10)
    output = [(int(ts) // 9000, bytes.fromhex(data)) for ts, data in (line.split() for line in run.stdout.splitlines())]
    assert output[0] == (0, START + SPS + START + PPS + START + IDR)
    assert output[-2:] == [(6, START + SPS + START + PPS + START + NEXT_IDR), (7, START + NEXT_P1)]
    expected = [0, 6, 7]
    if fault == 'none':
        expected = list(range(8))
    elif fault == 'no-slot':
        expected = [0, 1, 2, 6, 7]
    assert [index for index, _ in output] == expected
    assert int(run.stderr) == int(feedback and fault != 'none')
