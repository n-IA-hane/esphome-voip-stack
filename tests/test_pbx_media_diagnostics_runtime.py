"""Execute the call-scoped diagnostic methods and their real enum contract."""
from pathlib import Path
import subprocess

from test_refer_dialog_runtime import block

SOURCE = Path(__file__).resolve().parents[1] / "esphome/components/voip_stack"


def test_optional_diagnostics_are_call_scoped_and_do_not_change_signaling(tmp_path):
    header = (SOURCE / "voip_stack.h").read_text()
    fsm = (SOURCE / "voip_fsm.cpp").read_text()
    fields = header[header.index("  enum class PbxMediaOperation"):header.index("  std::string current_caller_route_id_")]
    code = r'''
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "voip_fsm.h"
#define USE_TEXT_SENSOR
struct LockGuard{std::lock_guard<std::mutex>lock;explicit LockGuard(std::mutex&m):lock(m){}};
using esphome::voip_stack::CallState;
struct Sensor{std::string state;std::vector<std::string>events;void publish_state(const std::string&s){state=s;events.push_back(s);}};
struct VoipStack{
 mutable std::mutex call_state_mutex_;
 std::atomic<CallState>call_state_{CallState::IDLE};
 std::string current_call_id_;
 Sensor sensor;Sensor*media_diagnostics_sensor_=&sensor;
 std::map<std::string,std::function<void()>>pending;
 template<class F>void defer(const char*key,F f){pending[key]=std::move(f);}
 void drain(){auto jobs=std::move(pending);pending.clear();for(auto&job:jobs)job.second();}
 bool set_pbx_media_diagnostics(const std::string&,const std::string&,const std::string&);
 std::string get_media_diagnostics_str()const;
''' + fields + "};\n"
    for marker in (
        "bool VoipStack::parse_pbx_media_diagnostic_pair_(",
        "std::string VoipStack::format_pbx_media_diagnostic_pair_(",
        "void VoipStack::set_call_sip_mode_(",
        "bool VoipStack::set_pbx_media_diagnostics(",
        "std::string VoipStack::get_media_diagnostics_str() const",
        "void VoipStack::publish_media_diagnostics_()",
    ):
        code += block(fsm, marker) + "\n"
    code += r'''
int main(){
 VoipStack p;p.current_call_id_="first-leg";p.set_call_sip_mode_(false);
 assert(p.get_media_diagnostics_str().empty());
 assert(!p.set_pbx_media_diagnostics("first-leg","forwarded,forwarded","inactive,inactive"));
 for(const auto state:{CallState::CALLING,CallState::REMOTE_RINGING,CallState::RINGING,CallState::TERMINATING,
                      CallState::BUSY,CallState::DECLINED,CallState::CANCELLED,CallState::MEDIA_INCOMPATIBLE,
                      CallState::TRANSPORT_UNREACHABLE,CallState::AUTH_REQUIRED_UNSUPPORTED}){
  p.call_state_=state;
  assert(!p.set_pbx_media_diagnostics("first-leg","forwarded,forwarded","inactive,inactive"));
 }
 p.call_state_=CallState::CONNECTING;
 assert(!p.set_pbx_media_diagnostics("wrong-leg","forwarded,forwarded","inactive,inactive"));
 assert(p.set_pbx_media_diagnostics("first-leg","forwarded,forwarded","inactive,inactive"));
 p.drain();assert(p.sensor.events.empty()&&p.get_media_diagnostics_str().empty());
 p.call_state_=CallState::IN_CALL;p.publish_media_diagnostics_();
 assert(p.sensor.events.size()==1&&p.sensor.state=="HA PBX | A: forwarded");
 p.set_call_sip_mode_(false);
 p.call_state_=CallState::IN_CALL;assert(p.get_media_diagnostics_str()=="Direct");
 p.set_call_sip_mode_(true);assert(p.get_media_diagnostics_str()=="Via PBX | unknown");
 assert(!p.set_pbx_media_diagnostics("session-id","forwarded,forwarded","inactive,inactive"));
 assert(!p.set_pbx_media_diagnostics("","forwarded,forwarded","inactive,inactive"));
 for(const std::string invalid:{"forwarded","forwarded,forwarded,forwarded","converted,forwarded","forwarded, arbitrary"})
  assert(!p.set_pbx_media_diagnostics("first-leg",invalid,"inactive,inactive"));
 assert(!p.set_pbx_media_diagnostics("first-leg","forwarded,forwarded","resampled,forwarded"));
 assert(p.pending.empty()&&p.get_media_diagnostics_str()=="Via PBX | unknown");
 assert(p.set_pbx_media_diagnostics("first-leg","resampled,codec_converted","inactive,converted"));
 assert(p.call_state_==CallState::IN_CALL&&p.current_call_id_=="first-leg");
 assert(p.pending.size()==1);p.drain();
 assert(p.sensor.state=="HA PBX | A: Tx resampled/Rx codec converted | V: Rx converted");
 const auto events=p.sensor.events.size();
 assert(p.set_pbx_media_diagnostics("first-leg","resampled,codec_converted","inactive,converted"));
 assert(p.pending.empty()&&p.sensor.events.size()==events);
 assert(p.set_pbx_media_diagnostics("first-leg","resampled_codec_converted,resampled_codec_converted","forwarded,forwarded"));
 p.drain();assert(p.sensor.state=="HA PBX | A: resampled+codec | V: forwarded");
 p.call_state_=CallState::TERMINATING;p.publish_media_diagnostics_();assert(p.sensor.state.empty());
 assert(!p.set_pbx_media_diagnostics("first-leg","forwarded,forwarded","forwarded,forwarded"));
 p.current_call_id_="second-leg";p.set_call_sip_mode_(false);p.call_state_=CallState::IN_CALL;
 assert(p.get_media_diagnostics_str()=="Direct");
 assert(!p.set_pbx_media_diagnostics("first-leg","forwarded,forwarded","forwarded,forwarded"));
 p.media_diagnostics_sensor_=nullptr;
 assert(p.set_pbx_media_diagnostics("second-leg","inactive,inactive","inactive,inactive"));
 p.drain();assert(p.get_media_diagnostics_str()=="HA PBX");
 // Publication reads the current call, so a deferred old notification cannot leak.
 assert(p.set_pbx_media_diagnostics("second-leg","forwarded,forwarded","forwarded,forwarded"));
 p.current_call_id_="third-leg";p.set_call_sip_mode_(true);p.drain();
 assert(p.get_media_diagnostics_str()=="Via PBX | unknown");
 p.current_call_id_.clear();p.publish_media_diagnostics_();assert(p.get_media_diagnostics_str().empty());
}
'''
    source = tmp_path / "diagnostics.cpp"
    source.write_text(code)
    binary = tmp_path / "diagnostics"
    built = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                            "-I", str(SOURCE),
                            str(source), "-o", str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    run = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert run.returncode == 0, run.stdout + run.stderr
