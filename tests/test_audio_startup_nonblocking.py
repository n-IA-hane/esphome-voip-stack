"""The actual startup producer must not enter a blocked console writer."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / 'esphome/components/voip_stack'


def method(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 2]


def test_first_enqueue_notifies_without_entering_console(tmp_path):
    header = (COMPONENT / 'voip_stack.h').read_text()
    start = header.index('  enum class RxStartupEvent')
    declarations = header[start:header.index('\n#endif', start)]
    audio = (COMPONENT / 'voip_audio.cpp').read_text()
    methods = method(audio, 'void VoipStack::debug_rx_startup_event_(') + '\n' + method(audio, 'void VoipStack::enqueue_rx_frame_(')
    helper = tmp_path / 'esphome/core/helpers.h'
    helper.parent.mkdir(parents=True)
    helper.write_text('#include <mutex>\nnamespace esphome {using Mutex=std::mutex;struct LockGuard{std::lock_guard<std::mutex>l;LockGuard(Mutex&m):l(m){}};}\n')
    source = tmp_path / 'worker.cpp'
    source.write_text(r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <thread>
#include "esphome/components/voip_stack/rtp_jitter_buffer.h"
using esphome::voip_stack::RtpJitterBuffer;
std::atomic<bool>console_entered{false},release_console{false},notified{false};
void blocked_console(){console_entered=true;while(!release_console.load())std::this_thread::yield();}
#define ESP_LOGI(...) blocked_console()
int64_t esp_timer_get_time(){return 123456;}
void xTaskNotifyGive(void*){notified=true;}
enum class CallState{IDLE,IN_CALL};
enum class AudioCodec{PCM};
struct AudioFormat{AudioCodec codec=AudioCodec::PCM;size_t nominal_frame_bytes()const{return 4;}};
AudioFormat unpack_audio_format(uint32_t){return {};}
struct Speaker{bool is_running(){return true;}};
struct TransportAudioFrame{const uint8_t*pcm;size_t bytes;uint16_t sequence;uint32_t timestamp;bool has_rtp_metadata,source_changed;uint32_t format_packed=0;};
struct VoipStack{
 bool audio_debug_=true;std::atomic<uint32_t>rx_audio_revision_{1};Speaker*speaker_=nullptr;
 std::atomic<CallState>call_state_{CallState::IN_CALL};RtpJitterBuffer*rx_jitter_buffer_=nullptr;
 size_t rx_jitter_frame_alloc_bytes_=4;void*rx_task_handle_=reinterpret_cast<void*>(1);
 std::atomic<uint32_t>media_rx_queue_depth_{0},media_rx_queue_drops_{0};
 std::atomic<uint32_t>audio_debug_rx_late_frames_{0},audio_debug_rx_missing_frames_{0},audio_debug_rx_duplicate_frames_{0};
 AudioFormat get_current_rx_audio_format_(){return {};}
 void enqueue_rx_frame_(const TransportAudioFrame&);
''' + declarations + '\n};\n' + methods + r'''
int main(){
 uint8_t store[64]{},pcm[4]{};RtpJitterBuffer jitter(store,4,16,4);VoipStack owner;Speaker sink;
 owner.rx_jitter_buffer_=&jitter;owner.speaker_=&sink;
 std::thread worker([&](){owner.enqueue_rx_frame_({pcm,4,1,0,true,false});});
 const auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(200);
 while(!notified&&!console_entered&&std::chrono::steady_clock::now()<until)std::this_thread::yield();
 bool passed=notified&&!console_entered;release_console=true;worker.join();
 assert(passed&&"The RTP producer blocked on AudioStartup console before RX notification");
}
''')
    binary = tmp_path / 'worker'
    compiled = subprocess.run(['g++','-std=c++17','-pthread','-DUSE_ESP32','-DUSE_ESPHOME_VOIP_STACK_AUDIO_DEBUG',
        '-I',str(tmp_path),'-I',str(ROOT),str(source),'-o',str(binary)],capture_output=True,text=True)
    assert compiled.returncode == 0, compiled.stderr
    result = subprocess.run(['bash','-c','ulimit -c 0; exec "$1"','bash',str(binary)],capture_output=True,text=True,timeout=3)
    assert result.returncode == 0, result.stderr
