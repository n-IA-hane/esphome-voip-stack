"""Actual TX/microphone debug producers retain samples without console I/O."""
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / 'esphome/components/voip_stack'


def method(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start)+2]


@pytest.mark.parametrize('enabled', [False, True])
@pytest.mark.parametrize('producer', ['tx-first', 'mic-first'])
def test_pcm_and_mic_debug_are_periodic_pending_metadata(tmp_path, enabled, producer):
    header = (COMPONENT/'voip_stack.h').read_text()
    begin = header.index('  struct PcmDebugData')
    declarations = header[begin:header.index('\n#endif', begin)]
    audio = (COMPONENT/'voip_audio.cpp').read_text()
    pcm = method(audio, 'void VoipStack::record_tx_debug_(')
    send = method(audio, 'void VoipStack::send_chunk_(')
    mic = method(audio, 'void VoipStack::on_microphone_data_(')
    emit = method((COMPONENT/'voip_diagnostics.cpp').read_text(), 'void VoipStack::emit_audio_debug_pending_(')
    tag = next(line for line in (COMPONENT/'voip_diagnostics.cpp').read_text().splitlines() if line.startswith('static const char *const AUDIO_TAG'))
    source = tmp_path/'pending.cpp'
    source.write_text(r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include "sip_types.h"
#include "voip_fsm.h"
#include "audio_core_audio_utils.h"
std::atomic<bool>entered{false},release_console{false},sent{false},queued{false};
std::vector<std::string>lines,tags;uint32_t now=1000;std::function<void()>on_log;
void log_line(const char*tag,const char*fmt,...){
 entered=true;while(!release_console)std::this_thread::yield();
 char out[600];va_list a;va_start(a,fmt);vsnprintf(out,sizeof(out),fmt,a);va_end(a);lines.emplace_back(out);tags.emplace_back(tag);auto callback=std::move(on_log);on_log={};if(callback)callback();
}
#define ESP_LOGI(...) log_line(__VA_ARGS__)
uint32_t millis(){return now;}
namespace esphome::voip_stack {
const char*TAG="fixture";
struct Buffer{size_t available(){return 12;}};
struct Transport{void send_audio_frame(const uint8_t*,size_t){sent=true;}};
struct Dc{int16_t process(int16_t x){return x;}};
struct VoipStack{
 bool audio_debug_=true,dc_offset_removal_=false;unsigned dc_channel_phase_=0;
 std::array<Dc,2>dc_blockers_{};Buffer buffer;Buffer*mic_buffer_=&buffer;
 Transport transport;Transport*transport_=&transport;
 std::atomic<int16_t*>mic_converted_{nullptr};std::atomic<float>mic_gain_{1},volume_{1};
 std::atomic<CallState>call_state_{CallState::IN_CALL};AudioFormat format;
 std::atomic<uint32_t>media_tx_queue_depth_{2},media_tx_queue_drops_{3},media_tx_queue_drop_bytes_{4};
 unsigned wakes=0;void enable_loop_soon_any_context(){++wakes;}
 bool is_tx_stream_ready_()const{return true;}AudioFormat get_current_tx_audio_format_(){return format;}
 const char*get_call_state_str(){return call_state_to_str(call_state_.load());}
 size_t tx_audio_chunk_bytes_(){return 4;}size_t mic_processing_samples_(){return 2;}
 bool write_mic_buffer_(const uint8_t*,size_t){queued=true;return true;}
 void send_chunk_(const uint8_t*,size_t);void on_microphone_data_(const uint8_t*,size_t);
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
 uint32_t audio_debug_last_tx_log_ms_=0,audio_debug_tx_frames_=0;
 uint32_t audio_debug_last_mic_callback_ms_=0,audio_debug_mic_callbacks_=0,audio_debug_last_mic_log_ms_=0;
 void record_tx_debug_(const uint8_t*,size_t,const AudioFormat&);
''' + declarations + r'''
#endif
};
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
''' + tag + '\n' + pcm + '\n' + emit + '\n#endif\n' + send + '\n' + mic + r'''
}
int main(){
 using namespace esphome::voip_stack;VoipStack owner;int16_t pcm[]{16384,-16384};
 std::atomic<bool>done{false};
 std::thread worker([&](){WORKER_ORDER;done=true;});
 auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(200);
 while(!done&&!entered&&std::chrono::steady_clock::now()<until)std::this_thread::yield();
 bool progressed=done&&sent&&queued&&!entered;release_console=true;worker.join();
 assert(progressed&&"Debug observer blocked TX or microphone producer on console");
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
 static_assert(sizeof(VoipStack::PcmDebugRecord)==44&&sizeof(VoipStack::MicDebugRecord)==48);
 assert(owner.audio_debug_pcm_.pending&&owner.audio_debug_mic_.pending&&owner.wakes==2);
 // A busy main reader must retain pending and retry on its next ordinary loop.
 owner.audio_debug_pcm_.busy.test_and_set();owner.audio_debug_mic_.busy.test_and_set();
 owner.emit_audio_debug_pending_();assert(lines.empty());
 assert(owner.audio_debug_pcm_.pending&&owner.audio_debug_mic_.pending);
 owner.audio_debug_pcm_.busy.clear();owner.audio_debug_mic_.busy.clear();
 owner.emit_audio_debug_pending_();assert(lines.size()==2);
 assert(tags[0]=="voip_stack.audio"&&tags[1]=="voip_stack.audio");
 assert(lines[0].find("quality_scope=since_boot")!=std::string::npos);
 assert(lines[0].find("AudioDebug[tx_network]: frames=1 bytes=4 samples=2 peak=16384")!=std::string::npos);
 assert(lines[0].find("peak_dbfs=-6.0 rms_dbfs=-6.0")!=std::string::npos);
 assert(lines[0].find("producer_ms=1000")!=std::string::npos);
 for(auto token:{"callbacks=1","len=4","delta_ms=0","tx_frame_bytes=4","accum_available=12","tx_queue_depth=2","tx_drops=3","tx_drop_bytes=4","tx_format=16000:1:1:16","state=in_call","producer_ms=1000"})
  assert(lines[1].find(token)!=std::string::npos);
 const auto initial=lines.size();owner.emit_audio_debug_pending_();assert(lines.size()==initial);
 now=1500;owner.send_chunk_(reinterpret_cast<uint8_t*>(pcm),4);owner.on_microphone_data_(reinterpret_cast<uint8_t*>(pcm),4);
 owner.emit_audio_debug_pending_();assert(lines.size()==initial); // Sampling remains 1Hz.
 now=2000;owner.send_chunk_(reinterpret_cast<uint8_t*>(pcm),4);owner.on_microphone_data_(reinterpret_cast<uint8_t*>(pcm),4);
 now=3000;owner.send_chunk_(reinterpret_cast<uint8_t*>(pcm),4);owner.on_microphone_data_(reinterpret_cast<uint8_t*>(pcm),4);
 assert((owner.audio_debug_stats_quality_.load()&12U)==12U); // Latest pending samples coalesced explicitly.
 owner.emit_audio_debug_pending_();assert(lines.size()==initial+2);
 assert(lines[initial].find("frames=4")!=std::string::npos&&lines[initial].find("producer_ms=3000")!=std::string::npos);
 // Existing empty and non-S16 branches keep their labels/fields and never read a payload later.
 now=4000;owner.record_tx_debug_(nullptr,0,owner.format);
 owner.emit_audio_debug_pending_();assert(lines.back().find("empty state=in_call")!=std::string::npos);
 now=5000;owner.format.pcm_format=PcmFormat::S32LE;
 owner.send_chunk_(reinterpret_cast<uint8_t*>(pcm),4);owner.emit_audio_debug_pending_();
 assert(lines.back().find("levels=skipped_non_s16")!=std::string::npos);
 now=6000;owner.audio_debug_pcm_.busy.test_and_set();owner.send_chunk_(reinterpret_cast<uint8_t*>(pcm),4);
 assert(owner.audio_debug_stats_quality_.load()&1U);owner.audio_debug_pcm_.busy.clear();
 now=7000;owner.send_chunk_(reinterpret_cast<uint8_t*>(pcm),4);
 assert(owner.audio_debug_pcm_.pending);
 owner.audio_debug_=false;const auto n=lines.size();
 owner.send_chunk_(reinterpret_cast<uint8_t*>(pcm),4);owner.on_microphone_data_(reinterpret_cast<uint8_t*>(pcm),4);
 owner.emit_audio_debug_pending_();assert(lines.size()==n);
 // Console formatting runs outside the claim: a producer can publish during it.
 VoipStack another;now=8000;another.send_chunk_(reinterpret_cast<uint8_t*>(pcm),4);
 on_log=[&](){now=9000;another.send_chunk_(reinterpret_cast<uint8_t*>(pcm),4);};
 another.emit_audio_debug_pending_();assert(another.audio_debug_pcm_.pending);
 assert(another.audio_debug_pcm_.data.producer_ms==9000);
 another.emit_audio_debug_pending_();assert(!another.audio_debug_pcm_.pending);
#else
 assert(lines.empty()&&owner.wakes==0);
#endif
}
''')
    order = ['owner.send_chunk_(reinterpret_cast<uint8_t*>(pcm),4)',
             'owner.on_microphone_data_(reinterpret_cast<uint8_t*>(pcm),4)']
    if producer == 'mic-first':
        order.reverse()
    source.write_text(source.read_text().replace('WORKER_ORDER', ';'.join(order)))
    binary = tmp_path/'pending' 
    flags = ['-DUSE_ESPHOME_VOIP_STACK_AUDIO_DEBUG'] if enabled else []
    built = subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-pthread',
        '-DUSE_ESPHOME_VOIP_STACK_MIC',*flags,'-I',str(COMPONENT),str(source),'-o',str(binary)],capture_output=True,text=True)
    assert built.returncode == 0, built.stderr
    run = subprocess.run(['bash','-c','ulimit -c 0; exec "$1"','bash',str(binary)],capture_output=True,text=True,timeout=3)
    assert run.returncode == 0, run.stderr
    if not enabled:
        assert b'AudioDebug[' not in binary.read_bytes()
        assert 'emit_audio_debug_pending_' not in subprocess.check_output(['nm','-C',str(binary)],text=True)


def test_existing_loop_owns_periodic_emission_and_pending_retry():
    source=(COMPONENT/'voip_stack.cpp').read_text()
    loop=method(source,'void VoipStack::loop()')
    assert 'this->emit_audio_debug_pending_();' in loop
    assert 'audio_debug_pcm_.pending.load' in loop and 'audio_debug_mic_.pending.load' in loop
    assert 'keep_loop = keep_loop || (this->audio_debug_ &&' in loop
    for name in ['void VoipStack::record_tx_debug_(', 'void VoipStack::on_microphone_data_(']:
        source=method((COMPONENT/'voip_audio.cpp').read_text(),name)
        assert 'ESP_LOG' not in source
