"""The existing media owner pairs native Wi-Fi roaming requests exactly once."""
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def active_function():
    source = (ROOT / 'esphome/components/voip_stack/voip_fsm.cpp').read_text()
    start = source.index('void VoipStack::set_audio_devices_active_(')
    end = source.index('\nvoid VoipStack::start_speaker_for_current_rx_', start)
    return source[start:end]


@pytest.mark.parametrize('wifi_enabled', [False, True])
def test_native_roaming_guard_follows_owned_media_lifetime(tmp_path, wifi_enabled):
    source = tmp_path / 'guard.cpp'
    source.write_text(r'''
#include <atomic>
#include <cassert>
namespace esphome {
namespace wifi {
struct Radio {
  unsigned owners=3;
  void request_roaming_suppression() { ++owners; }
  void release_roaming_suppression() { assert(owners>0); --owners; }
};
Radio radio;
Radio *global_wifi_component=&radio;
}
namespace voip_stack {
struct VoipStack {
  std::atomic<bool> audio_devices_active_{false};
  bool wifi_roaming_suppressed_{false};
  unsigned notifications=0;
  void notify_audio_tasks_() { ++notifications; }
  void reset_peer_audio_watchdog_(bool) {}
  void set_audio_devices_active_(bool);
};
'''+active_function()+r'''
}}
int main() {
  using namespace esphome;
  voip_stack::VoipStack a,b;
  a.set_audio_devices_active_(false);
  assert(wifi::radio.owners==3 && a.notifications==0);
  a.set_audio_devices_active_(true);a.set_audio_devices_active_(true);
#ifdef USE_WIFI
  assert(wifi::radio.owners==4);
#else
  assert(wifi::radio.owners==3);
#endif
  assert(a.notifications==1);
  b.set_audio_devices_active_(true);
#ifdef USE_WIFI
  assert(wifi::radio.owners==5);
#endif
  a.set_audio_devices_active_(false);a.set_audio_devices_active_(false);
  b.set_audio_devices_active_(false);
  assert(wifi::radio.owners==3);
  // No radio at acquisition must not release someone else's later request.
  wifi::global_wifi_component=nullptr;
  a.set_audio_devices_active_(true);
  wifi::global_wifi_component=&wifi::radio;
  a.set_audio_devices_active_(false);
  assert(wifi::radio.owners==3);
  for(unsigned i=0;i<20;++i) {
    a.set_audio_devices_active_(true);a.set_audio_devices_active_(false);
  }
  assert(wifi::radio.owners==3);
}
''')
    binary = tmp_path / 'guard'
    defines = ['-DUSE_WIFI', '-DUSE_WIFI_RUNTIME_ROAMING_SUPPRESSION'] if wifi_enabled else []
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', *defines,
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
