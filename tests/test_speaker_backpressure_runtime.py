"""Received PCM survives temporary sink pressure and cannot cross call resets."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def test_received_pcm_is_retained_until_accepted_or_cancelled(tmp_path):
    source = (ROOT / 'esphome/components/voip_stack/voip_audio.cpp').read_text()
    start = source.index('void VoipStack::play_rx_frame_(')
    method = source[start:source.index('\n}\n', start)+2]
    harness = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>
using TickType_t=uint32_t;
#define pdTRUE 1
uint32_t tick=0, waits=0;
std::function<void()> on_wait;
uint32_t xTaskGetTickCount(){return tick;}
void ulTaskNotifyTake(int,uint32_t duration){assert(duration>0);tick+=duration;waits++;if(on_wait)on_wait();}
enum class CallState{IN_CALL,IDLE};
enum class SilenceReason{NONE,MUTED_SINK,NETWORK_GAP};
struct Speaker {
 unsigned attempts=0,blocked=0;bool consumes_wait=false;size_t chunk=3;
 std::vector<uint8_t> accepted;
 size_t play(const uint8_t *data,size_t bytes,TickType_t budget){
  if(++attempts<=blocked){if(consumes_wait)tick+=budget;return 0;}
  size_t n=std::min(chunk,bytes);accepted.insert(accepted.end(),data,data+n);return n;
 }
};
struct VoipStack {
 Speaker *speaker_;uint8_t *rx_silence_chunk_=nullptr;
 std::atomic<float> volume_{1};
 std::atomic<bool> audio_devices_active_{true};
 std::atomic<CallState> call_state_{CallState::IN_CALL};
 std::atomic<uint32_t> rx_audio_revision_{0};
 void play_rx_frame_(const uint8_t*,size_t,SilenceReason,TickType_t,uint32_t);
};
'''
    checks = r'''
int main(){
 std::vector<uint8_t> pcm(64);for(size_t i=0;i<pcm.size();i++)pcm[i]=i*13;
 for(bool blocking:{false,true}) {
  tick=waits=0;on_wait={};Speaker sink;sink.blocked=6;sink.consumes_wait=blocking;
  VoipStack v;v.speaker_=&sink;v.play_rx_frame_(pcm.data(),pcm.size(),SilenceReason::NONE,10,0);
  assert(sink.accepted==pcm);assert(tick==60);assert(waits==(blocking?0u:6u));
 }
 {
  tick=waits=0;Speaker sink;VoipStack v;v.speaker_=&sink;
  v.play_rx_frame_(pcm.data(),pcm.size(),SilenceReason::NONE,10,0);
  assert(sink.accepted==pcm&&waits==0&&tick==0);
 }
 for(bool reset:{false,true}) {
  tick=waits=0;Speaker sink;sink.blocked=100;VoipStack v;v.speaker_=&sink;
  on_wait=[&](){if(waits==2){if(reset)v.rx_audio_revision_++;else v.call_state_=CallState::IDLE;}};
  v.play_rx_frame_(pcm.data(),pcm.size(),SilenceReason::NONE,10,0);
  assert(sink.accepted.empty()&&sink.attempts==2&&waits==2);
  on_wait={};sink.blocked=0;v.call_state_=CallState::IN_CALL;
  if(reset){v.play_rx_frame_(pcm.data(),pcm.size(),SilenceReason::NONE,10,0);assert(sink.accepted.empty());}
  v.play_rx_frame_(pcm.data(),pcm.size(),SilenceReason::NONE,10,v.rx_audio_revision_.load());
  assert(sink.accepted==pcm);
 }
 {
  waits=0;on_wait={};Speaker sink;sink.blocked=100;VoipStack v;v.speaker_=&sink;
  v.play_rx_frame_(pcm.data(),pcm.size(),SilenceReason::NETWORK_GAP,0,0);
  assert(sink.attempts==1&&sink.accepted.empty()&&waits==0);
 }
}
'''
    cpp=tmp_path/'sink.cpp';cpp.write_text(harness+method+checks);exe=tmp_path/'sink'
    subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror',str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
