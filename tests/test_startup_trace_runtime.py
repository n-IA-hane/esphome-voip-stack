"""Startup metadata is bounded, allocated once and absent without audio_debug."""

from pathlib import Path
import subprocess

import pytest

from test_refer_dialog_runtime import block

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "esphome/components/voip_stack"


@pytest.mark.parametrize("enabled", [False, True])
def test_real_fixed_startup_trace_debug_boundary_and_quiescent_dump(tmp_path, enabled):
    header = (COMPONENT / "sip_transport.h").read_text()
    source = (COMPONENT / "sip_transport.cpp").read_text()
    start = header.index("  enum class StartupEvent")
    types = header[start:header.index("  std::unique_ptr<StartupTrace", start)]
    methods = "\n".join(block(source, marker) for marker in (
        "void SipTransport::configure_startup_trace(", "void SipTransport::reset_startup_trace_(",
        "void SipTransport::record_startup_(", "uint8_t SipTransport::record_startup_packet_(",
        "void SipTransport::dump_startup_trace(",
    ))
    cpp = tmp_path / "trace.cpp"
    cpp.write_text(r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <new>
unsigned allocations=0,last_flags=99,logs=0;uint32_t clock_us=100;
int64_t esp_timer_get_time(){return clock_us;}
template<class T>struct RAMAllocator {
 enum{ALLOC_INTERNAL=2};unsigned flags;RAMAllocator(unsigned f=0):flags(f){}
 T*allocate(size_t n){++allocations;last_flags=flags;return static_cast<T*>(std::malloc(n*sizeof(T)));}
};
std::function<void()>on_lock;
struct LockGuard{explicit LockGuard(int&){auto callback=on_lock;if(callback)callback();}};
#define ESP_LOGI(...) (++logs)
#define ESP_LOGW(...) (++logs)
struct SipTransport{
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
''' + types + r'''
 struct Free{void operator()(StartupTrace*p)const{if(p){p->~StartupTrace();std::free(p);}}};
 std::unique_ptr<StartupTrace,Free>startup_trace_;
 enum class MediaLifecyclePhase:uint8_t{IDLE,PREPARED,ACTIVE};
 std::atomic<MediaLifecyclePhase>media_lifecycle_phase_{MediaLifecyclePhase::ACTIVE};
 std::atomic<bool>rtp_running_{false},rtp_task_quiesced_{true};int dialog_mutex_=0;
 void configure_startup_trace(bool,bool);void reset_startup_trace_();void record_startup_(StartupEvent);
 uint8_t record_startup_packet_(const uint8_t*,size_t);void dump_startup_trace();
#endif
};
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
''' + methods + r'''
#endif
int main(){
 SipTransport owner;
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
 static_assert(sizeof(SipTransport::StartupTrace::Packet)==20);
 owner.configure_startup_trace(true,true);assert(allocations==1&&last_flags==0);
 owner.configure_startup_trace(true,false);assert(allocations==1);
 owner.reset_startup_trace_();uint8_t packet[12]{0x80,96};
 for(unsigned n=0;n<80;++n){packet[3]=n;packet[7]=n;packet[11]=42;clock_us=1000+n;
  const auto index=owner.record_startup_packet_(packet,12);assert(index==(n<64?n:255));}
 assert(owner.startup_trace_->count==64&&owner.startup_trace_->packets[63].sequence==63);
 assert(owner.startup_trace_->packets[0].ssrc==42&&owner.startup_trace_->packets[0].done_us==0);
 owner.rtp_running_=true;owner.dump_startup_trace();assert(logs==0);
 owner.rtp_running_=false;on_lock=[&](){owner.rtp_running_=true;};
 owner.dump_startup_trace();assert(logs==0); // Recheck under lock, not a TOCTOU gate.
 on_lock={};owner.rtp_running_=false;owner.dump_startup_trace();assert(logs>=64);
 owner.rtp_task_quiesced_=false;owner.reset_startup_trace_();assert(owner.startup_trace_->count==64);
 owner.rtp_task_quiesced_=true;owner.reset_startup_trace_();assert(owner.startup_trace_->count==0&&allocations==1);
#else
 (void)owner;assert(allocations==0&&logs==0);
#endif
}
''')
    binary = tmp_path / "trace"
    flags = ["-DUSE_ESPHOME_VOIP_STACK_AUDIO_DEBUG"] if enabled else []
    built = subprocess.run(["g++", "-std=c++17", *flags, str(cpp), "-o", str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    run = subprocess.run(["bash", "-c", 'ulimit -c 0; exec "$1"', "bash", str(binary)], capture_output=True, text=True)
    assert run.returncode == 0, run.stderr
    if not enabled:
        assert b"RTPStart" not in binary.read_bytes()
        symbols = subprocess.check_output(["nm", "-C", str(binary)], text=True)
        assert "record_startup_" not in symbols
