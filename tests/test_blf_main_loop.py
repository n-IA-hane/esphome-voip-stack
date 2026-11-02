"""Latest-state callbacks are optional and bounded even with observer refill."""
import subprocess
from test_refer_dialog_runtime import SOURCE, block


def test_blf_loop_coalesces_and_bounds_work_without_a_duplicate_state_cache(tmp_path):
    core = tmp_path / "esphome/core"
    core.mkdir(parents=True)
    (core / "defines.h").write_text("")
    source = (SOURCE / "voip_blf.cpp").read_text()
    code = r'''
#include "sip_blf.h"
#include <atomic>
#include <cassert>
#include <functional>
#include <memory>
using namespace esphome::voip_stack;
struct SipTransport {BlfStateInfo rows[64];bool copy_blf_slot(size_t i,BlfStateInfo*out){*out=rows[i];return !out->contact_id.empty();}};
struct VoipStack {
 std::unique_ptr<SipTransport>transport_=std::make_unique<SipTransport>();uint8_t blf_capacity_=64;
 std::atomic<bool>blf_notifications_requested_{false};std::atomic<SipTransport*>blf_event_owner_{nullptr};
 std::array<std::atomic<uint32_t>,2>blf_pending_{};unsigned wakes=0;
 struct Callbacks{unsigned count=0;std::function<void()>refill;void call(const BlfStateInfo&){++count;if(refill)refill();}}blf_callbacks_;
 struct Trigger{unsigned count=0;void trigger(const std::string&,const std::string&,const char*,bool){++count;}}blf_state_trigger_;
 void enable_loop_soon_any_context(){++wakes;}
 static void blf_callback_(void*,SipTransport*,size_t);void pump_blf_events_();
};
'''
    for marker in ["const char *blf_state_name(", "void VoipStack::blf_callback_(", "void VoipStack::pump_blf_events_("]:
        code += block(source, marker) + "\n"
    code += r'''
int main(){
 VoipStack app;auto*owner=app.transport_.get();for(unsigned i=0;i<64;++i)owner->rows[i].contact_id=std::to_string(i);
 for(unsigned i=0;i<100;++i)VoipStack::blf_callback_(&app,owner,i%64);
 assert(app.wakes==0&&app.blf_pending_[0]==0&&app.blf_pending_[1]==0);
 app.blf_notifications_requested_=true;
 for(unsigned i=0;i<1000;++i)VoipStack::blf_callback_(&app,owner,i%64);
 assert(app.blf_callbacks_.count==0);app.pump_blf_events_();assert(app.blf_callbacks_.count==8);
 for(unsigned i=0;i<7;++i)app.pump_blf_events_();assert(app.blf_callbacks_.count==64&&app.blf_pending_[0]==0&&app.blf_pending_[1]==0);
 app.blf_callbacks_.refill=[&]{VoipStack::blf_callback_(&app,owner,0);};
 VoipStack::blf_callback_(&app,owner,0);app.pump_blf_events_();assert(app.blf_callbacks_.count==65&&app.blf_pending_[0]==1);
 app.blf_callbacks_.refill=nullptr;app.pump_blf_events_();assert(app.blf_callbacks_.count==66);
 VoipStack::blf_callback_(&app,owner,0);auto retired=std::move(app.transport_);app.transport_=std::make_unique<SipTransport>();
 app.pump_blf_events_();assert(app.blf_callbacks_.count==66&&app.blf_state_trigger_.count==66);
}
'''
    cpp = tmp_path / "loop.cpp"
    cpp.write_text(code)
    binary = tmp_path / "loop"
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation", "-g", "-fsanitize=address,undefined",
        "-fno-pie", "-no-pie", "-DUSE_ESPHOME_VOIP_BLF", "-I", str(tmp_path), "-I", str(SOURCE), str(cpp), "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
