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
    snapshot = method((COMPONENT / "voip_diagnostics.cpp").read_text(), "bool VoipStack::debug_rx_startup_snapshot_(")
    play = method(audio, "void VoipStack::play_rx_frame_(")
    enqueue = method(audio, "void VoipStack::enqueue_rx_frame_(")
    reset = method(audio, "void VoipStack::reset_rx_audio_(")
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
thread_local bool pause_clock=false;std::atomic<bool>clock_paused{false},clock_release{false};
std::function<void()> on_tick;
void advance(uint32_t duration){for(unsigned n=0;n<duration/10;++n){tick+=10;if(on_tick)on_tick();}}
uint32_t xTaskGetTickCount(){return tick;}
void ulTaskNotifyTake(int,uint32_t duration){advance(duration);}
void xTaskNotifyGive(void*){}
int64_t esp_timer_get_time(){const int64_t now=tick*1000;if(pause_clock){clock_paused=true;while(!clock_release)std::this_thread::yield();}return now;}
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
enum class SilenceReason{NONE,MUTED_SINK,NETWORK_GAP,PREROLL};
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
struct Transport{bool audio_receive_paused()const{return false;}};
struct VoipStack {
 Transport *transport_=nullptr;
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
 enum class RxPlayoutPhase:uint8_t{STOPPED,PREPARING,WAITING_OUTPUT,READY,ACTIVE};
 std::atomic<uint32_t>rx_playout_state_{4},rx_prepare_started_us_{0};uint32_t rx_prepared_format_packed_=0;
 RxPlayoutPhase rx_playout_phase_(uint32_t revision)const{return revision==rx_audio_revision_.load()&&call_state_==CallState::IN_CALL?RxPlayoutPhase::ACTIVE:RxPlayoutPhase::STOPPED;}
 bool play_rx_preroll_(uint32_t,TickType_t){return false;}
 void play_rx_frame_(const uint8_t*,size_t,SilenceReason,TickType_t,uint32_t);
 void reset_rx_audio_(bool=false);void start_speaker_for_current_rx_();
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
        + "\n" + snapshot
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
 assert(logs.empty()); // Capturing never enters the logger.
 using Event=VoipStack::RxStartupEvent;
 VoipStack virgin;virgin.rx_audio_revision_=UINT32_MAX;
 virgin.debug_rx_startup_event_(Event::RX_ENQUEUE,UINT32_MAX);
 assert(virgin.audio_debug_rx_startup_[static_cast<unsigned>(Event::RX_ENQUEUE)].present);
 auto read=[&](VoipStack&owner,Event event,unsigned expected){
  unsigned rev=0;VoipStack::RxStartupData data;
  assert(owner.debug_rx_startup_snapshot_(event,rev,data));
  assert(rev==expected&&(data.flags&2U));return data;
 };
 assert(read(app,Event::SPEAKER_START_REQUEST,revision).us==0);
 assert(read(app,Event::PLAY_ATTEMPT,revision).us==0);
 assert(read(app,Event::PCM_ACCEPTED,revision).us==60000);
 assert(read(app,Event::PCM_ACCEPTED,revision).flags&1U);
 assert(read(app,Event::RESUMED,revision).us==60000);
 std::vector<std::thread> threads;
 for(unsigned n=0;n<16;++n)threads.emplace_back([&](){app.debug_rx_startup_event_(Event::RX_ENQUEUE,revision);});
 for(auto &thread:threads)thread.join();
 assert(read(app,Event::RX_ENQUEUE,revision).us==60000&&logs.empty());
 app.reset_rx_audio_();unsigned next=app.rx_audio_revision_.load();
 app.debug_rx_startup_event_(Event::RX_ENQUEUE,revision);
 app.debug_rx_startup_event_(Event::RX_ENQUEUE,next);
 app.debug_rx_startup_event_(Event::RX_ENQUEUE,revision);
 assert(read(app,Event::RX_ENQUEUE,next).us==60000);
 app.audio_debug_rx_startup_[static_cast<unsigned>(Event::RX_ENQUEUE)].revision.store(UINT32_MAX-1);
 app.rx_audio_revision_.store(UINT32_MAX-1);app.reset_rx_audio_();
 app.debug_rx_startup_event_(Event::RX_ENQUEUE,UINT32_MAX);
 assert(read(app,Event::RX_ENQUEUE,UINT32_MAX).us==60000);
 app.reset_rx_audio_();app.debug_rx_startup_event_(Event::RX_ENQUEUE,0);
 app.debug_rx_startup_event_(Event::RX_ENQUEUE,UINT32_MAX);
 assert(read(app,Event::RX_ENQUEUE,0).us==60000);
 // A writer delayed before claiming its slot cannot replace a newer revision.
 app.reset_rx_audio_();const auto old=app.rx_audio_revision_.load();
 std::thread delayed([&](){pause_clock=true;app.debug_rx_startup_event_(Event::RX_ENQUEUE,old);});
 while(!clock_paused)std::this_thread::yield();
 app.reset_rx_audio_();const auto fresh=app.rx_audio_revision_.load();tick=77;
 app.debug_rx_startup_event_(Event::RX_ENQUEUE,fresh);
 clock_release=true;delayed.join();
 assert(read(app,Event::RX_ENQUEUE,fresh).us==77000);
 // A busy first-event slot reports lost observation instead of waiting.
 auto &busy=app.audio_debug_rx_startup_[static_cast<unsigned>(Event::PCM_ACCEPTED)];
 assert(!busy.busy.test_and_set());
 app.debug_rx_startup_event_(Event::PCM_ACCEPTED,fresh);
 assert(app.audio_debug_rx_startup_quality_.load()&1U);
 unsigned held_rev=0;VoipStack::RxStartupData held;
 assert(!app.debug_rx_startup_snapshot_(Event::PCM_ACCEPTED,held_rev,held));
 assert(app.audio_debug_rx_startup_quality_.load()&2U);
 busy.busy.clear();app.debug_rx_startup_event_(Event::PCM_ACCEPTED,fresh);
 assert(read(app,Event::PCM_ACCEPTED,fresh).us==77000);
 app.audio_debug_=false;app.reset_rx_audio_();
 app.debug_rx_startup_event_(Event::RX_ENQUEUE,app.rx_audio_revision_.load());
 assert(logs.empty());
 for(bool late_stall:{false,true}){
  tick=0;uint8_t samples[16*4]{},network[4]{},out[4]{};
  Speaker downstream;RtpJitterBuffer queue(samples,4,16,4);VoipStack current;
  current.speaker_=&downstream;current.rx_jitter_buffer_=&queue;current.reset_rx_audio_();
  unsigned rev=current.rx_audio_revision_.load();current.start_speaker_for_current_rx_();
  current.debug_rx_startup_event_(Event::IN_CALL_GATE,rev);
  downstream.remaining_blocks=1;
  current.play_rx_frame_(network,4,SilenceReason::NETWORK_GAP,0,rev);
  current.play_rx_frame_(network,4,SilenceReason::NETWORK_GAP,0,rev);
  uint16_t sequence=0;
  on_tick=[&](){current.enqueue_rx_frame_({network,4,sequence++,tick*48,true,false});};
  current.enqueue_rx_frame_({network,4,sequence++,0,true,false});advance(30);
  assert(queue.read(out,4)==RtpJitterBuffer::ReadResult::FRAME);
  current.play_rx_frame_(out,4,SilenceReason::NONE,10,rev);
  assert(read(current,Event::PCM_ACCEPTED,rev).us==30000);
  assert(queue.read(out,4)==RtpJitterBuffer::ReadResult::FRAME);
  downstream.remaining_blocks=late_stall?60:0;
  current.play_rx_frame_(out,4,SilenceReason::NONE,10,rev);on_tick={};
  if(late_stall){
   assert(queue.counters().drops==10&&queue.counters().discarded_frames==50);
   assert(read(current,Event::FIRST_ZERO_WRITE,rev).us==40000);
   auto drop=read(current,Event::FIRST_JITTER_DROP,rev);
   assert((drop.flags&4U)&&drop.drops==1&&drop.discarded==5);
   assert(read(current,Event::RESUMED,rev).us==630000);
  }else assert(!queue.counters().drops);
  assert(logs.empty());
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
            if "RxStartupEvent" in line or "audio_debug_rx_startup_" in line:
                assert guarded, line
