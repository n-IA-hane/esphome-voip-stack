"""Execute the normal VoIP dump without debug, preserving one captured call."""

from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "esphome/components/voip_stack"


@pytest.mark.parametrize("audio_debug,microphone", [(False, False), (True, False), (False, True)])
def test_dump_is_paced_private_and_releases_its_snapshot(tmp_path, audio_debug, microphone):
    source = (COMPONENT / "voip_diagnostics.cpp").read_text()
    source = "\n".join(
        line for line in source.splitlines() if not line.startswith("#include")
    )
    header = (COMPONENT / "voip_stack.h").read_text()
    start = header.index("  struct DiagnosticDump")
    end = header.index("  void cleanup_partial_setup_", start)
    fields = header[start:end]
    start = header.index("  enum class RxStartupEvent")
    startup = header[start:header.index("\n#endif", start)]
    transport = (COMPONENT / "transport.h").read_text()
    start = transport.index("struct SipTransportSnapshot")
    end = transport.index("\n};", start) + 3
    snapshot = transport[start:end]
    cpp = tmp_path / "dump.cpp"
    cpp.write_text(
        r"""
#include <cassert>
#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "sip_types.h"
#include "voip_fsm.h"
#define USE_ESP32
#define USE_ESPHOME_VOIP_STACK_SPEAKER
#define YESNO(x) ((x)?"YES":"NO")
int locked=0;std::vector<std::string> lines;
void log(const char*,const char*fmt,...){assert(!locked);char b[384];va_list a;va_start(a,fmt);vsnprintf(b,sizeof(b),fmt,a);va_end(a);lines.emplace_back(b);}
#define ESP_LOGI(tag,...) log(tag,__VA_ARGS__)
#define ESP_LOGW ESP_LOGI
uint32_t tick=2000;uint32_t millis(){return tick;}
unsigned stack_reads=0;
using UBaseType_t=unsigned;
UBaseType_t uxTaskGetStackHighWaterMark(void *task){assert(task);++stack_reads;return 3072;}
namespace esphome::voip_stack {
struct Mutex{};struct LockGuard{explicit LockGuard(Mutex&){++locked;}~LockGuard(){--locked;}};
"""
        + snapshot
        + r"""
struct Transport{SipTransportSnapshot value;SipTransportSnapshot snapshot(){return value;}};
struct RtpJitterBuffer{struct Counters{uint32_t depth=1,drops=2,late=3,missing=4,duplicates=5,discarded_frames=7;};Counters counters(){return {};}bool has_exchange_alignment(const uint8_t*data)const{return data!=nullptr;}};
struct VoipStack{
 std::atomic<CallState>call_state_{CallState::IN_CALL};
 std::atomic<uint32_t>media_tx_queue_depth_{1},media_tx_queue_drops_{0},media_rx_queue_depth_{2},media_rx_queue_drops_{0};
 std::atomic<bool>audio_devices_active_{true};
 std::atomic<float>volume_{.75f},mic_gain_{2.0f};float get_volume()const{return volume_.load(std::memory_order_relaxed);}
 Mutex call_state_mutex_;std::string current_call_id_="private-caller-token",last_reason_="local_hangup";
 std::unique_ptr<Transport>transport_=std::make_unique<Transport>();
 std::unique_ptr<RtpJitterBuffer>rx_jitter_buffer_=std::make_unique<RtpJitterBuffer>();
 uint8_t rx_storage[960]{};uint8_t*rx_network_chunk_=rx_storage;size_t rx_jitter_frame_alloc_bytes_=sizeof(rx_storage);
 bool buffers_in_psram_=true,task_stacks_in_psram_=true,audio_task_stacks_in_psram_=true;
 unsigned sip_port_=5060,rtp_port_=40000;static constexpr unsigned kRxQueuedFrames=16;
 void *rx_task_handle_=this;unsigned rx_task_stack_bytes_=12288;
#ifdef USE_ESPHOME_VOIP_STACK_MIC
 void *tx_task_handle_=this;unsigned tx_task_stack_bytes_=8192;
#endif
 bool failed=false;bool is_failed(){return failed;}
 struct Formats{AudioFormat tx,rx;};Formats snapshot_current_media_formats_(){return {};}
 const char*configured_sip_transport_name(){return "udp";}const char*audio_capability_(){return "full_duplex";}
 std::function<void()>pending;
 void defer(const char*,std::function<void()>f){assert(!pending);pending=std::move(f);}
 void once(){assert(pending);auto f=std::move(pending);pending={};const auto n=lines.size();f();assert(lines.size()-n<=2);}
 void finish(){unsigned bound=80;while(pending&&bound--)once();assert(!pending);}
 unsigned published_snapshots=0;
 void publish_sip_snapshot_(){++published_snapshots;}
 void dump_diagnostics();uint32_t last_diagnostics_ms_=0;
"""
        + fields
        + "\n#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG\n bool audio_debug_=true;\n" + startup + "\n#endif\n"
        + "};\n}\n"
        + source
        + r"""
int main(){
 using namespace esphome::voip_stack;VoipStack s;
 s.transport_->value.call_active=true;s.transport_->value.running=true;s.transport_->value.rtp_tx_packets=123;

#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
 auto &event=s.audio_debug_rx_startup_[0];event.revision=5;event.present=true;event.data.us=123456;event.data.flags=0;
#endif
 s.dump_diagnostics();assert(s.diagnostic_dump_);assert(lines.size()==1);assert(stack_reads==0);
 s.dump_diagnostics();assert(lines.size()==1 && s.published_snapshots==1);
 // A hangup during output must not rewrite the already captured report.
 s.current_call_id_.clear();s.call_state_=CallState::IDLE;s.transport_->value.rtp_tx_packets=999;s.last_reason_="different";
 s.volume_=0;s.mic_gain_=1;

 s.audio_devices_active_=false;
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
 while(s.diagnostic_dump_->step<11)s.once();
 auto &held=s.audio_debug_rx_startup_[0];assert(!held.busy.test_and_set());
 const auto before=lines.size();s.once();assert(s.pending&&s.diagnostic_dump_->step==11&&lines.size()==before);
 held.busy.clear();
#endif
 s.finish();assert(!s.diagnostic_dump_&&!s.pending);
 bool original_gains=false;for(const auto &line:lines)original_gains|=line.find("rx_volume=0.750 tx_gain=2.000")!=std::string::npos;
 assert(original_gains&&"Diagnostic gains must retain the requested snapshot after hangup");
#ifdef USE_ESPHOME_VOIP_STACK_MIC
 assert(stack_reads==2);
#else
 assert(stack_reads==1);
#endif
 bool stack=false;for(auto &line:lines)stack|=line.find("rx_stack_bytes=12288 rx_min_free_bytes=3072")!=std::string::npos;
 assert(stack);
#ifdef USE_ESPHOME_VOIP_STACK_MIC
 bool tx_stack=false;for(auto &line:lines)tx_stack|=line.find("tx_present=YES tx_stack_bytes=8192 tx_min_free_bytes=3072")!=std::string::npos;
 assert(tx_stack);
#endif
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
 bool startup=false,quality=false;
 for(auto&line:lines){startup|=line.find("AudioStartup rev=5 event=speaker_start_request us=123456")!=std::string::npos;
 quality|=line.find("complete=YES quality=2 deferred_read=YES")!=std::string::npos;}
 assert(startup&&quality);
#endif
 bool captured=false,reason=false,discarded=false;
 for(auto &l:lines){assert(l.find("private-caller-token")==std::string::npos);assert(l.find("memory:")==std::string::npos);discarded|=l.find("discarded_frames=7")!=std::string::npos;captured|=l.find("tx_packets=123")!=std::string::npos;reason|=l.find("last_termination=local_hangup")!=std::string::npos;}
 assert(captured&&reason&&discarded);const auto n=lines.size();s.dump_diagnostics();assert(lines.size()==n);
 tick+=1000;s.failed=true;s.dump_diagnostics();assert(!s.diagnostic_dump_&&lines.size()==n+2);
}
"""
    )
    exe = tmp_path / "dump"
    compiled = subprocess.run(
        [
            "g++",
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            *(["-DUSE_ESPHOME_VOIP_STACK_AUDIO_DEBUG"] if audio_debug else []),
            *(["-DUSE_ESPHOME_VOIP_STACK_MIC"] if microphone else []),
            "-I",
            str(COMPONENT),
            str(cpp),
            "-o",
            str(exe),
        ],
        capture_output=True,
        text=True,
    )
    assert compiled.returncode == 0, compiled.stderr
    subprocess.run([str(exe)], check=True, capture_output=True, text=True)


@pytest.mark.parametrize("audio_debug", [False, True])
def test_call_snapshot_publishes_transitions_without_periodic_media_churn(tmp_path, audio_debug):
    from test_refer_dialog_runtime import block

    stack = (COMPONENT / "voip_stack.cpp").read_text()
    fsm = (COMPONENT / "voip_fsm.cpp").read_text()
    source = tmp_path / "activity.cpp"
    source.write_text(r"""
#include <atomic>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#define ESP_LOGD(...) ((void)0)
enum class CallState { IDLE, RINGING, IN_CALL };
static uint32_t clock_ms;
static uint32_t millis() { return clock_ms; }
struct Sensor {
  std::vector<std::string> events;
  void publish_state(const std::string &state) { events.push_back(state); }
};
struct VoipStack {
  Sensor snapshot, state, reason;
  Sensor *sip_snapshot_sensor_{&snapshot}, *state_sensor_{&state}, *last_reason_sensor_{&reason};
  std::atomic<CallState> call_state_{CallState::IDLE};
  std::string last_sip_snapshot_, last_reason_;
  uint32_t last_sip_snapshot_refresh_ms_{0}, cycle_started_at_{0}, packets{0};
  static constexpr uint32_t calling_timeout_ms_=3000, ringing_timeout_ms_=3000;
  bool audio_debug_{false}, cycle_active_{false};
  void pump_application_media_start_() {}
  void handle_call_timeouts_(uint32_t, uint32_t) {}
  void disable_loop() {}
  template<class F> void defer(F) {}
  std::string get_state_str() const {
    switch (call_state_.load()) {
      case CallState::RINGING: return "ringing";
      case CallState::IN_CALL: return "in_call";
      default: return "idle";
    }
  }
  std::string build_sip_snapshot_string_() const {
    return get_state_str() + ":" + std::to_string(packets) + ":" + last_reason_;
  }
  void loop();
  void publish_state_();
  void publish_last_reason_(const std::string &);
  void publish_sip_snapshot_();
};
""" + "\n".join([
        block(stack, "void VoipStack::loop()"),
        block(stack, "void VoipStack::publish_sip_snapshot_()"),
        block(fsm, "void VoipStack::publish_state_()"),
        block(fsm, "void VoipStack::publish_last_reason_("),
    ]) + r"""
int main() {
  VoipStack phone;
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
  phone.audio_debug_ = true;
#endif
  phone.call_state_ = CallState::RINGING;
  phone.publish_state_();
  phone.call_state_ = CallState::IN_CALL;
  phone.publish_state_();
  for (clock_ms=1; clock_ms<=60000; ++clock_ms) {
    ++phone.packets;
    phone.loop();
  }
  assert(phone.snapshot.events.size() == (phone.audio_debug_ ? 122 : 2));
  phone.publish_last_reason_("remote_hangup");
  phone.call_state_ = CallState::IDLE;
  phone.publish_state_();
  assert(phone.state.events == std::vector<std::string>({"ringing", "in_call", "idle"}));
  assert(phone.snapshot.events.back() == "idle:60000:remote_hangup");
  const auto count = phone.snapshot.events.size();
  clock_ms += 2000;
  ++phone.packets;
  phone.loop();
  assert(phone.snapshot.events.size() == count);
}
""")
    exe = tmp_path / "activity"
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-DUSE_TEXT_SENSOR",
                    *(["-DUSE_ESPHOME_VOIP_STACK_AUDIO_DEBUG"] if audio_debug else []),
                    str(source), "-o", str(exe)], check=True)
    subprocess.run([exe], check=True)
