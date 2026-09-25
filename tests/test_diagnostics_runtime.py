"""Execute the normal VoIP dump without debug, preserving one captured call."""

from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "esphome/components/voip_stack"


def test_dump_is_paced_private_and_releases_its_snapshot(tmp_path):
    source = (COMPONENT / "voip_diagnostics.cpp").read_text()
    source = "\n".join(
        line for line in source.splitlines() if not line.startswith("#include")
    )
    header = (COMPONENT / "voip_stack.h").read_text()
    start = header.index("  struct DiagnosticDump")
    end = header.index("  void cleanup_partial_setup_", start)
    fields = header[start:end]
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
namespace esphome::voip_stack {
struct Mutex{};struct LockGuard{explicit LockGuard(Mutex&){++locked;}~LockGuard(){--locked;}};
"""
        + snapshot
        + r"""
struct Transport{SipTransportSnapshot value;SipTransportSnapshot snapshot(){return value;}};
struct RtpJitterBuffer{struct Counters{uint32_t depth=1,drops=2,late=3,missing=4,duplicates=5;};Counters counters(){return {};}};
struct VoipStack{
 std::atomic<CallState>call_state_{CallState::IN_CALL};
 std::atomic<uint32_t>media_tx_queue_depth_{1},media_tx_queue_drops_{0},media_rx_queue_depth_{2},media_rx_queue_drops_{0};
 std::atomic<bool>audio_devices_active_{true};
 Mutex call_state_mutex_;std::string current_call_id_="private-caller-token",last_reason_="local_hangup";
 std::unique_ptr<Transport>transport_=std::make_unique<Transport>();
 std::unique_ptr<RtpJitterBuffer>rx_jitter_buffer_=std::make_unique<RtpJitterBuffer>();
 bool buffers_in_psram_=true,task_stacks_in_psram_=true,audio_task_stacks_in_psram_=true;
 unsigned sip_port_=5060,rtp_port_=40000;static constexpr unsigned kRxQueuedFrames=16;
 bool failed=false;bool is_failed(){return failed;}
 struct Formats{AudioFormat tx,rx;};Formats snapshot_current_media_formats_(){return {};}
 const char*configured_sip_transport_name(){return "udp";}const char*audio_capability_(){return "full_duplex";}const char*get_media_route_str(){return "direct";}
 std::function<void()>pending;
 void defer(const char*,std::function<void()>f){assert(!pending);pending=std::move(f);}
 void finish(){while(pending){auto f=std::move(pending);pending={};const auto n=lines.size();f();assert(lines.size()-n<=2);}}
 void dump_diagnostics();uint32_t last_diagnostics_ms_=0;
"""
        + fields
        + "};\n}\n"
        + source
        + r"""
int main(){
 using namespace esphome::voip_stack;VoipStack s;
 s.transport_->value.call_active=true;s.transport_->value.running=true;s.transport_->value.rtp_tx_packets=123;
 s.dump_diagnostics();assert(s.diagnostic_dump_);assert(lines.size()==1);
 s.dump_diagnostics();assert(lines.size()==1);
 // A hangup during output must not rewrite the already captured report.
 s.current_call_id_.clear();s.call_state_=CallState::IDLE;s.transport_->value.rtp_tx_packets=999;s.last_reason_="different";
 s.finish();assert(!s.diagnostic_dump_&&!s.pending);
 bool captured=false,reason=false;
 for(auto &l:lines){assert(l.find("private-caller-token")==std::string::npos);assert(l.find("memory:")==std::string::npos);captured|=l.find("tx_packets=123")!=std::string::npos;reason|=l.find("last_termination=local_hangup")!=std::string::npos;}
 assert(captured&&reason);const auto n=lines.size();s.dump_diagnostics();assert(lines.size()==n);
 tick+=1000;s.failed=true;s.dump_diagnostics();assert(!s.diagnostic_dump_&&lines.size()==n+2);
}
"""
    )
    exe = tmp_path / "dump"
    subprocess.run(
        [
            "g++",
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-I",
            str(COMPONENT),
            str(cpp),
            "-o",
            str(exe),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    subprocess.run([str(exe)], check=True, capture_output=True, text=True)
