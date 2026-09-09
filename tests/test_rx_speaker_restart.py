"""The actual RX task must deliver late media to a speaker that went idle."""

from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_rx_task_recovers_idle_speaker(tmp_path):
    source = (ROOT / "esphome/components/voip_stack/voip_audio.cpp").read_text()
    methods = ""
    for name in ["play_rx_frame_", "play_silence_frame_", "rx_task_"]:
        start = source.index("void VoipStack::" + name + "(")
        methods += source[start : source.index("\n}\n", start) + 2] + "\n"
    harness = r"""
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstddef>
#define USE_ESPHOME_VOIP_STACK_SPEAKER
#define ESP_LOGD(...) ((void)0)
#define pdTRUE 1
#define portMAX_DELAY 0xffffffff
#define configTICK_RATE_HZ 1000
using TickType_t=uint32_t;
struct Done{};
unsigned tick=0, waits=0;
uint32_t millis(){return tick;}
uint32_t xTaskGetTickCount(){return tick;}
void ulTaskNotifyTake(int,uint32_t duration){tick+=duration;if(++waits>100)throw Done{};}
enum class CallState{IN_CALL,IDLE};
enum class AudioCodec{PCM,OPUS};
enum class SilenceReason{NONE,MUTED_SINK,NETWORK_GAP};
struct AudioFormat{unsigned frame_ms=10;AudioCodec codec=AudioCodec::PCM;size_t nominal_frame_bytes() const{return 320;}};
struct Speaker {
 bool running=false;unsigned played=0,restarts=0;
 bool is_running(){return running;}
 size_t play(const uint8_t*,size_t size,TickType_t){if(!running){running=true;++restarts;}++played;return size;}
};
struct RtpJitterBuffer {
 enum class ReadResult{FRAME,BUFFERING,MISSING};
 unsigned reads=0;
 ReadResult read(uint8_t*,size_t,void*,void*,void*,size_t*,bool*){if(++reads>3)throw Done{};return ReadResult::FRAME;}
 unsigned depth(){return 2;}
};
struct VoipStack {
 Speaker *speaker_;RtpJitterBuffer *rx_jitter_buffer_;
 std::atomic<bool> audio_devices_active_{true},first_audio_received_{true};
 std::atomic<CallState> call_state_{CallState::IN_CALL};
 std::atomic<uint32_t> rx_underrun_start_ms_{0},media_rx_queue_depth_{0};
 std::atomic<float> volume_{1};
 uint8_t data[320]{},silence[320]{};uint8_t *rx_audio_chunk_=data,*rx_silence_chunk_=silence;
 size_t rx_audio_chunk_alloc_bytes_=320;
 static constexpr unsigned kRxSilenceAfterMs=100;
 AudioFormat get_current_rx_audio_format_(){return {};}
 void play_rx_frame_(const uint8_t*,size_t,SilenceReason,TickType_t);
 void play_silence_frame_(SilenceReason,TickType_t);
 void rx_task_();
};
"""
    check = r"""
int main(){
 for(bool already_running:{false,true}){
  waits=0;Speaker sink;sink.running=already_running;RtpJitterBuffer jitter;VoipStack voip;
  voip.speaker_=&sink;voip.rx_jitter_buffer_=&jitter;
  try{voip.rx_task_();}catch(Done&){}
  assert(sink.played==3);assert(sink.restarts==(already_running?0u:1u));
 }
}
"""
    cpp = tmp_path / "rx.cpp"
    cpp.write_text(harness + methods + check)
    binary = tmp_path / "rx"
    subprocess.run(
        ["g++", "-std=c++17", str(cpp), "-o", str(binary)],
        check=True,
        capture_output=True,
        text=True,
    )
    # Exit on assertion without producing a large core dump.
    subprocess.run(
        ["bash", "-c", 'ulimit -c 0; exec "$1"', "bash", str(binary)], check=True
    )
