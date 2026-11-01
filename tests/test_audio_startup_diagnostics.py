"""Optional startup traces observe the existing RX revision and sink contract."""

from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "esphome/components/voip_stack"


def method(source, signature):
    start = source.index(signature)
    return source[start : source.index("\n}\n", start) + 2]


@pytest.mark.parametrize("enabled", [False, True])
def test_startup_trace_is_optional_and_bound_to_receive_revision(tmp_path, enabled):
    audio = (COMPONENT / "voip_audio.cpp").read_text()
    header = (COMPONENT / "voip_stack.h").read_text()
    start = header.index("  enum class RxStartupEvent")
    declarations = header[start : header.index("\n#endif", start)]
    trace = method(audio, "void VoipStack::debug_rx_startup_event_(")
    play = method(audio, "void VoipStack::play_rx_frame_(")
    reset = method(audio, "void VoipStack::reset_rx_audio_()")
    start_speaker = method(
        (COMPONENT / "voip_fsm.cpp").read_text(),
        "void VoipStack::start_speaker_for_current_rx_()",
    )
    cpp = tmp_path / "startup.cpp"
    cpp.write_text(
        r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
using TickType_t=uint32_t;
#define pdTRUE 1
uint32_t tick=0;
uint32_t xTaskGetTickCount(){return tick;}
void ulTaskNotifyTake(int,uint32_t duration){tick+=duration;}
int64_t esp_timer_get_time(){return tick*1000;}
std::mutex logs_mutex;
std::vector<std::string> logs;
void log_line(const char *,const char *format,...){
 char output[512];va_list args;va_start(args,format);
 std::vsnprintf(output,sizeof(output),format,args);va_end(args);
 std::lock_guard<std::mutex> lock(logs_mutex);logs.emplace_back(output);
}
#define ESP_LOGI(...) log_line(__VA_ARGS__)
constexpr const char *TAG="audio";
enum class CallState{IDLE,IN_CALL};
enum class SilenceReason{NONE,MUTED_SINK,NETWORK_GAP};
struct AudioFormat{bool is_valid()const{return true;}};
int audio_stream_info_from_format(AudioFormat){return 0;}
struct Speaker {
 unsigned remaining_blocks=0;bool running=false,requested=false;
 bool is_running(){return running;}
 void set_audio_stream_info(int){}
 void start(){requested=true;} // Start retains intent; readiness is asynchronous.
 size_t play(const uint8_t*,size_t bytes,TickType_t budget){
  if(remaining_blocks){--remaining_blocks;tick+=budget;return 0;}
  running=true;return bytes;
 }
};
struct RtpJitterBuffer {
 struct Counters{unsigned depth=7,drops=9,discarded_frames=45;};
 Counters counters(){return {};}
 void reset(){}
};
struct VoipStack {
 Speaker *speaker_=nullptr;RtpJitterBuffer *rx_jitter_buffer_=nullptr;
 bool audio_debug_=true;
 std::atomic<bool> audio_devices_active_{true};
 std::atomic<CallState> call_state_{CallState::IN_CALL};
 std::atomic<uint32_t> rx_audio_revision_{0},media_rx_queue_depth_{0},media_rx_queue_drops_{0};
 std::atomic<uint32_t> rx_underrun_start_ms_{0};
 std::atomic<float> volume_{1};
 uint8_t *rx_audio_chunk_=nullptr;size_t rx_audio_chunk_alloc_bytes_=0;
 AudioFormat get_current_rx_audio_format_(){return {};}
 void play_rx_frame_(const uint8_t*,size_t,SilenceReason,TickType_t,uint32_t);
 void reset_rx_audio_();void start_speaker_for_current_rx_();
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
 std::atomic<uint32_t> media_tx_queue_drop_bytes_{0};
 std::atomic<uint32_t> audio_debug_rx_late_frames_{0},audio_debug_rx_missing_frames_{0};
 std::atomic<uint32_t> audio_debug_rx_duplicate_frames_{0},audio_debug_rx_silence_frames_{0};
 std::atomic<uint32_t> audio_debug_speaker_short_writes_{0};
'''
        + declarations
        + "\n#endif\n};\n#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG\n"
        + trace
        + "\n#endif\n"
        + play
        + "\n"
        + reset
        + "\n"
        + start_speaker
        + r'''
int main(){
 Speaker sink;RtpJitterBuffer jitter;VoipStack app;
 app.speaker_=&sink;app.rx_jitter_buffer_=&jitter;
 app.reset_rx_audio_();unsigned revision=app.rx_audio_revision_.load();
 app.start_speaker_for_current_rx_();assert(sink.requested&&!sink.running);
 uint8_t pcm[4]={1,2,3,4};sink.remaining_blocks=6;
 app.play_rx_frame_(pcm,sizeof(pcm),SilenceReason::NONE,10,revision);
 app.play_rx_frame_(pcm,sizeof(pcm),SilenceReason::NONE,10,revision);
 assert(tick==60&&sink.running);
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
 assert(logs.size()==3); // Request, first attempt and first accepted PCM only.
 assert(logs[0].find("event=speaker_start_request")!=std::string::npos);
 assert(logs[0].find("sink_running=0")!=std::string::npos);
 assert(logs[1].find("event=play_attempt")!=std::string::npos);
 assert(logs[1].find("us=0 ")!=std::string::npos);
 assert(logs[2].find("event=pcm_accepted")!=std::string::npos);
 assert(logs[2].find("us=60000 ")!=std::string::npos);
 assert(logs[2].find("sink_running=1")!=std::string::npos);
 assert(logs[2].find("jitter_depth=7 drops=9 discarded=45")!=std::string::npos);
 using Event=VoipStack::RxStartupEvent;
 std::vector<std::thread> threads;
 for(unsigned n=0;n<16;++n)threads.emplace_back([&](){app.debug_rx_startup_event_(Event::RX_ENQUEUE,revision);});
 for(auto &thread:threads)thread.join();
 assert(logs.size()==4);
 app.debug_rx_startup_event_(Event::IN_CALL_GATE,revision);
 assert(logs.size()==5);
 app.reset_rx_audio_();unsigned next=app.rx_audio_revision_.load();
 app.debug_rx_startup_event_(Event::RX_ENQUEUE,revision); // Old producer cannot claim the new first event.
 assert(logs.size()==5);
 app.debug_rx_startup_event_(Event::RX_ENQUEUE,next);
 app.debug_rx_startup_event_(Event::PCM_ACCEPTED,next);
 assert(logs.size()==7);
 app.debug_rx_startup_event_(Event::RX_ENQUEUE,revision); // Nor overwrite the new event's marker.
 app.debug_rx_startup_event_(Event::RX_ENQUEUE,next);
 assert(logs.size()==7);
 // Existing RX revision wrap must still permit the new session's events.
 app.audio_debug_rx_startup_logged_[static_cast<unsigned>(Event::RX_ENQUEUE)].store(UINT32_MAX-1);
 app.rx_audio_revision_.store(UINT32_MAX-1);app.reset_rx_audio_();
 app.debug_rx_startup_event_(Event::RX_ENQUEUE,UINT32_MAX);
 app.reset_rx_audio_();app.debug_rx_startup_event_(Event::RX_ENQUEUE,0);
 app.debug_rx_startup_event_(Event::RX_ENQUEUE,UINT32_MAX);
 assert(logs.size()==9);
 app.audio_debug_=false;app.reset_rx_audio_();
 app.debug_rx_startup_event_(Event::RX_ENQUEUE,app.rx_audio_revision_.load());
 app.start_speaker_for_current_rx_();
 app.play_rx_frame_(pcm,sizeof(pcm),SilenceReason::NONE,10,app.rx_audio_revision_.load());
 assert(logs.size()==9);
#else
 assert(logs.empty());
#endif
}
'''
    )
    executable = tmp_path / "startup"
    flags = ["-DUSE_ESPHOME_VOIP_STACK_SPEAKER"]
    if enabled:
        flags.append("-DUSE_ESPHOME_VOIP_STACK_AUDIO_DEBUG")
    subprocess.run(
        ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread", *flags,
         str(cpp), "-o", str(executable)],
        check=True, capture_output=True, text=True,
    )
    subprocess.run([str(executable)], check=True, capture_output=True, text=True)
    if not enabled:
        symbols = subprocess.run(
            ["nm", "-C", str(executable)], check=True, capture_output=True, text=True,
        ).stdout
        assert "debug_rx_startup" not in symbols
        assert b"AudioStartup" not in executable.read_bytes()


def test_startup_trace_observes_gate_and_successful_enqueue():
    fsm = (COMPONENT / "voip_fsm.cpp").read_text()
    audio = (COMPONENT / "voip_audio.cpp").read_text()
    state = method(fsm, "void VoipStack::set_call_state_(")
    assert state.index("call_state_.exchange") < state.index("RxStartupEvent::IN_CALL_GATE")
    assert state.index("RxStartupEvent::IN_CALL_GATE") < state.index("notify_audio_tasks_")
    enqueue = method(audio, "void VoipStack::enqueue_rx_frame_(")
    assert enqueue.index("rx_jitter_buffer_->push") < enqueue.index("RxStartupEvent::RX_ENQUEUE")
    # All optional declarations and hooks must disappear in ordinary builds.
    for source in (audio, fsm, (COMPONENT / "voip_stack.h").read_text()):
        guarded = False
        stack = []
        for line in source.splitlines():
            if line.startswith("#if"):
                stack.append(guarded)
                guarded |= "USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG" in line
            elif line.startswith("#endif"):
                guarded = stack.pop()
            if "RxStartupEvent" in line or "audio_debug_rx_startup_logged_" in line:
                assert guarded, line
