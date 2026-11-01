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
    enqueue = method(audio, "void VoipStack::enqueue_rx_frame_(")
    reset = method(audio, "void VoipStack::reset_rx_audio_()")
    start_speaker = method(
        (COMPONENT / "voip_fsm.cpp").read_text(),
        "void VoipStack::start_speaker_for_current_rx_()",
    )
    cpp = tmp_path / "startup.cpp"
    helper = tmp_path / "esphome/core/helpers.h"
    helper.parent.mkdir(parents=True)
    helper.write_text(
        "#include <mutex>\nnamespace esphome {using Mutex=std::mutex;"
        "struct LockGuard {std::lock_guard<std::mutex> lock;explicit LockGuard(Mutex&m):lock(m){}};}\n"
    )
    cpp.write_text(
        r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "esphome/components/voip_stack/rtp_jitter_buffer.h"
using esphome::voip_stack::RtpJitterBuffer;
using TickType_t=uint32_t;
#define pdTRUE 1
uint32_t tick=0;
std::function<void()> on_tick;
void advance(uint32_t duration){for(unsigned n=0;n<duration/10;++n){tick+=10;if(on_tick)on_tick();}}
uint32_t xTaskGetTickCount(){return tick;}
void ulTaskNotifyTake(int,uint32_t duration){advance(duration);}
void xTaskNotifyGive(void*){}
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
enum class AudioCodec{PCM};
struct AudioFormat{
 AudioCodec codec=AudioCodec::PCM;bool is_valid()const{return true;}
 size_t nominal_frame_bytes()const{return 4;}
};
AudioFormat unpack_audio_format(uint32_t){return {};}
struct TransportAudioFrame{
 const uint8_t *pcm;size_t bytes;uint16_t sequence;uint32_t timestamp;
 bool has_rtp_metadata,source_changed;uint32_t format_packed=0;
};
int audio_stream_info_from_format(AudioFormat){return 0;}
struct Speaker {
 unsigned remaining_blocks=0;bool running=false,requested=false;
 bool is_running(){return running;}
 void set_audio_stream_info(int){}
 void start(){requested=true;} // Start retains intent; readiness is asynchronous.
 size_t play(const uint8_t*,size_t bytes,TickType_t budget){
  if(remaining_blocks){--remaining_blocks;advance(budget);return 0;}
  running=true;return bytes;
 }
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
 size_t rx_jitter_frame_alloc_bytes_=4;void *rx_task_handle_=nullptr;
 AudioFormat get_current_rx_audio_format_(){return {};}
 void play_rx_frame_(const uint8_t*,size_t,SilenceReason,TickType_t,uint32_t);
 void reset_rx_audio_();void start_speaker_for_current_rx_();
 void enqueue_rx_frame_(const TransportAudioFrame&);
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
        + enqueue
        + "\n"
        + reset
        + "\n"
        + start_speaker
        + r'''
int main(){
 uint8_t storage[16*4]{};Speaker sink;RtpJitterBuffer jitter(storage,4,16,4);VoipStack app;
 app.speaker_=&sink;app.rx_jitter_buffer_=&jitter;
 app.reset_rx_audio_();unsigned revision=app.rx_audio_revision_.load();
 app.start_speaker_for_current_rx_();assert(sink.requested&&!sink.running);
 uint8_t pcm[4]={1,2,3,4};sink.remaining_blocks=6;
 app.play_rx_frame_(pcm,sizeof(pcm),SilenceReason::NONE,10,revision);
 app.play_rx_frame_(pcm,sizeof(pcm),SilenceReason::NONE,10,revision);
 assert(tick==60&&sink.running);
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
 assert(logs.size()==5); // Request, attempt, zero, real PCM, resumed, each once.
 assert(logs[0].find("event=speaker_start_request")!=std::string::npos);
 assert(logs[0].find("sink_running=0")!=std::string::npos);
 assert(logs[1].find("event=play_attempt")!=std::string::npos);
 assert(logs[1].find("us=0 ")!=std::string::npos);
 assert(logs[2].find("event=first_zero_write")!=std::string::npos);
 assert(logs[3].find("event=pcm_accepted")!=std::string::npos);
 assert(logs[3].find("us=60000 ")!=std::string::npos);
 assert(logs[3].find("sink_running=1")!=std::string::npos);
 assert(logs[3].find("jitter_depth=0 drops=0 discarded=0")!=std::string::npos);
 assert(logs[4].find("event=resumed")!=std::string::npos);
 using Event=VoipStack::RxStartupEvent;
 std::vector<std::thread> threads;
 for(unsigned n=0;n<16;++n)threads.emplace_back([&](){app.debug_rx_startup_event_(Event::RX_ENQUEUE,revision);});
 for(auto &thread:threads)thread.join();
 assert(logs.size()==6);
 app.debug_rx_startup_event_(Event::IN_CALL_GATE,revision);
 assert(logs.size()==7);
 app.reset_rx_audio_();unsigned next=app.rx_audio_revision_.load();
 app.debug_rx_startup_event_(Event::RX_ENQUEUE,revision); // Old producer cannot claim the new first event.
 assert(logs.size()==7);
 app.debug_rx_startup_event_(Event::RX_ENQUEUE,next);
 app.debug_rx_startup_event_(Event::PCM_ACCEPTED,next);
 assert(logs.size()==9);
 app.debug_rx_startup_event_(Event::RX_ENQUEUE,revision); // Nor overwrite the new event's marker.
 app.debug_rx_startup_event_(Event::RX_ENQUEUE,next);
 assert(logs.size()==9);
 // Existing RX revision wrap must still permit the new session's events.
 app.audio_debug_rx_startup_logged_[static_cast<unsigned>(Event::RX_ENQUEUE)].store(UINT32_MAX-1);
 app.rx_audio_revision_.store(UINT32_MAX-1);app.reset_rx_audio_();
 app.debug_rx_startup_event_(Event::RX_ENQUEUE,UINT32_MAX);
 app.reset_rx_audio_();app.debug_rx_startup_event_(Event::RX_ENQUEUE,0);
 app.debug_rx_startup_event_(Event::RX_ENQUEUE,UINT32_MAX);
 assert(logs.size()==11);
 app.audio_debug_=false;app.reset_rx_audio_();
 app.debug_rx_startup_event_(Event::RX_ENQUEUE,app.rx_audio_revision_.load());
 app.start_speaker_for_current_rx_();
 app.play_rx_frame_(pcm,sizeof(pcm),SilenceReason::NONE,10,app.rx_audio_revision_.load());
 assert(logs.size()==11);
 for(bool late_stall:{false,true}){
  logs.clear();tick=0;uint8_t samples[16*4]{},network[4]{},out[4]{};
  Speaker downstream;RtpJitterBuffer queue(samples,4,16,4);VoipStack current;
  current.speaker_=&downstream;current.rx_jitter_buffer_=&queue;current.reset_rx_audio_();
  unsigned rev=current.rx_audio_revision_.load();current.start_speaker_for_current_rx_();
  current.debug_rx_startup_event_(Event::IN_CALL_GATE,rev);
  // Silence, including a zero write, cannot claim network PCM or backpressure.
  downstream.remaining_blocks=1;
  current.play_rx_frame_(network,4,SilenceReason::NETWORK_GAP,0,rev);
  current.play_rx_frame_(network,4,SilenceReason::NETWORK_GAP,0,rev);
  assert(logs.size()==2);
  uint16_t sequence=0;
  on_tick=[&](){current.enqueue_rx_frame_({network,4,sequence++,tick*48,true,false});};
  current.enqueue_rx_frame_({network,4,sequence++,0,true,false});advance(30);
  assert(queue.read(out,4)==RtpJitterBuffer::ReadResult::FRAME);
  current.play_rx_frame_(out,4,SilenceReason::NONE,10,rev); // First received PCM is timely.
  assert(logs.size()==5); // Request, gate, enqueue, attempt, real PCM accepted.
  assert(logs.back().find("event=pcm_accepted us=30000 ")!=std::string::npos);
  assert(queue.read(out,4)==RtpJitterBuffer::ReadResult::FRAME);
  downstream.remaining_blocks=late_stall?60:0;
  current.play_rx_frame_(out,4,SilenceReason::NONE,10,rev);
  on_tick={};
  if(late_stall){
   assert(logs.size()==8&&queue.counters().drops>0);
   assert(logs[5].find("event=first_zero_write us=40000 ")!=std::string::npos);
   assert(logs[6].find("event=first_jitter_drop ")!=std::string::npos);
   assert(logs[6].find("drops=1 discarded=5")!=std::string::npos);
   assert(logs[7].find("event=resumed us=630000 ")!=std::string::npos);
   assert(logs[7].find("drops=10 discarded=50")!=std::string::npos);
  }else{
   assert(logs.size()==5&&!queue.counters().drops); // No fabricated zero/drop/resume.
  }
 }
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
         "-DUSE_ESP32", "-I", str(tmp_path), "-I", str(ROOT),
         str(cpp), "-o", str(executable)],
        check=True, capture_output=True, text=True,
    )
    subprocess.run(
        ["bash", "-c", 'ulimit -c 0; exec "$1"', "bash", str(executable)],
        check=True, capture_output=True, text=True,
    )
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
