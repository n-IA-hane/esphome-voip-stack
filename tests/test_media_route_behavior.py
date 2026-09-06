"""Execute the production route projection methods with stale call updates."""
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def test_route_projection_is_bound_to_the_established_call(tmp_path):
    source = (ROOT / 'esphome/components/voip_stack/voip_fsm.cpp').read_text()
    begin = source.index('const char *VoipStack::get_media_route_str() const')
    end = source.index('void VoipStack::publish_state_()', begin)
    methods = source[begin:end]
    probe = tmp_path / 'media_route.cpp'
    probe.write_text(r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <string>
#define USE_TEXT_SENSOR
enum class CallState { IDLE, CALLING, IN_CALL, TERMINATING };
struct Sensor {
  std::string state;
  int publications=0;
  bool has_state() const { return publications != 0; }
  void publish_state(const char *value) {state=value; ++publications;}
};
class VoipStack {
public:
  enum class MediaRoute : uint8_t { UNKNOWN, DIRECT, HA_TRANSCODING };
  std::atomic<CallState> call_state_{CallState::IDLE};
  std::atomic<MediaRoute> media_route_{MediaRoute::UNKNOWN};
  Sensor *media_route_sensor_=nullptr;
  std::string call_id;
  std::string get_current_call_id_() const {return call_id;}
  const char *get_media_route_str() const;
  bool set_media_route(const std::string &, const std::string &);
  void publish_media_route_();
};
''' + methods + r'''
int main() {
  VoipStack phone;
  Sensor sensor;
  phone.media_route_sensor_=&sensor;
  phone.publish_media_route_();
  assert(sensor.state.empty());
  phone.call_id="call-a";
  assert(!phone.set_media_route("call-a", "ha_transcoding"));
  phone.call_state_=CallState::CALLING;
  assert(!phone.set_media_route("call-a", "ha_transcoding"));
  phone.call_state_=CallState::IN_CALL;
  assert(!phone.set_media_route("call-old", "ha_transcoding"));
  assert(!phone.set_media_route("", "direct"));
  assert(!phone.set_media_route("call-a", "transcoding_pending"));
  assert(sensor.state.empty());
  assert(phone.set_media_route("call-a", "direct"));
  assert(sensor.state=="direct");
  int published=sensor.publications;
  assert(phone.set_media_route("call-a", "direct"));
  assert(sensor.publications==published);
  assert(phone.set_media_route("call-a", "ha_transcoding"));
  assert(sensor.state=="ha_transcoding");
  phone.call_state_=CallState::TERMINATING;
  phone.publish_media_route_();
  assert(sensor.state.empty());
  assert(!phone.set_media_route("call-a", "direct"));
  phone.call_id="call-b";
  phone.media_route_=VoipStack::MediaRoute::UNKNOWN;
  phone.call_state_=CallState::IN_CALL;
  assert(!phone.set_media_route("call-a", "ha_transcoding"));
  assert(std::string(phone.get_media_route_str()).empty());
  phone.media_route_sensor_=nullptr;
  assert(phone.set_media_route("call-b", "direct"));
}
''')
    binary = tmp_path / 'media_route'
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    str(probe), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
