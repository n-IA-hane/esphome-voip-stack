"""Real POSIX wait expressions must survive ESPHome's select component namespace.

This is a compiler regression for ESPHome 2026.10 precompiled-header lookup,
not a substitute for the separate network and media runtime tests.
"""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_socket_waits_resolve_the_posix_function(tmp_path):
    expressions = []
    for name in ('sip_transport.cpp', 'video_rtp.cpp'):
        source = (ROOT / 'esphome/components/voip_stack' / name).read_text()
        calls = re.findall(r'const int ready\s*=\s*((?:::)?select\([^;]+\));', source)
        assert len(calls) == 2, f'Update the network-wait compilation witness for {name}'
        expressions.extend(calls)
    program = tmp_path / 'waits.cpp'
    program.write_text('''#include <sys/select.h>
namespace esphome::select {}
namespace esphome::voip_stack {
struct Waits {
 int rtp_socket_=-1;
 void check_waits() {
 int maxfd=-1, max_fd=-1, socket=-1;
 fd_set readfds{}, writefds{};
 timeval timeout{};
 timeval *timeout_ptr=&timeout;
''' + '\n'.join(f' (void)({call});' for call in expressions) + '\n}\n};\n}\n')
    result = subprocess.run(['g++', '-std=c++17', '-fsyntax-only', str(program)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
