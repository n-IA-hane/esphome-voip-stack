"""Actual publication coalesces per account and stays on the existing loop."""
import subprocess
from test_refer_dialog_runtime import SOURCE, block


def test_message_waiting_main_loop_is_bounded_and_owner_scoped(tmp_path):
    core = tmp_path / "esphome/core"
    core.mkdir(parents=True)
    (core / "defines.h").write_text("")
    code = r'''
#include "message_waiting.h"
#include <atomic>
#include <cassert>
#include <memory>
#include <vector>
#include <functional>
using namespace esphome::voip_stack;
struct SipRegistration {static constexpr size_t CAPACITY=2;};
bool protocol=false;
struct SipTransport {
 MessageWaitingState rows[2];
 bool copy_message_waiting_slot(size_t i,MessageWaitingState*out){assert(!protocol);*out=rows[i];return !out->account_id.empty();}
};
struct VoipStack {
 std::unique_ptr<SipTransport>transport_=std::make_unique<SipTransport>();
 std::atomic<SipTransport*>message_waiting_owner_{nullptr};
 std::atomic<uint32_t>message_waiting_pending_{0};unsigned wakes=0;
 void enable_loop_soon_any_context(){++wakes;}
 struct Trigger {std::vector<MessageWaitingState>seen;std::function<void()>refill;
 void trigger(const std::string&id,bool available,bool waiting,uint32_t fresh,uint32_t old,uint32_t un,uint32_t uo){
 assert(!protocol);MessageWaitingState s;s.account_id=id;s.available=available;s.waiting=waiting;
 s.new_messages=fresh;s.old_messages=old;s.urgent_new=un;s.urgent_old=uo;seen.push_back(s);if(refill)refill();}
 } message_waiting_trigger_;
 struct Callbacks{unsigned n=0;void call(){assert(!protocol);++n;}}message_waiting_callbacks_;
 static void message_waiting_callback_(void*,SipTransport*,size_t);
 void pump_message_waiting_();void publish_message_waiting_(const MessageWaitingState&);
};
'''
    source = (SOURCE / "voip_message_waiting.cpp").read_text()
    for marker in ["void VoipStack::message_waiting_callback_(", "void VoipStack::pump_message_waiting_(", "void VoipStack::publish_message_waiting_("]:
        code += block(source, marker) + "\n"
    code += r'''
int main(){
 VoipStack app;auto*owner=app.transport_.get();owner->rows[0].account_id="office";owner->rows[1].account_id="ha";
 protocol=true;for(unsigned n=0;n<100;++n){owner->rows[0].new_messages=n;VoipStack::message_waiting_callback_(&app,owner,0);}
 VoipStack::message_waiting_callback_(&app,owner,1);protocol=false;
 assert(app.message_waiting_trigger_.seen.empty());app.pump_message_waiting_();
 assert(app.message_waiting_trigger_.seen.size()==2&&app.message_waiting_trigger_.seen[0].new_messages==99);
 app.message_waiting_trigger_.refill=[&](){VoipStack::message_waiting_callback_(&app,owner,0);};
 VoipStack::message_waiting_callback_(&app,owner,0);app.pump_message_waiting_();
 assert(app.message_waiting_trigger_.seen.size()==3&&app.message_waiting_pending_==1);
 app.message_waiting_trigger_.refill=nullptr;app.pump_message_waiting_();assert(app.message_waiting_trigger_.seen.size()==4);
 // A notification queued by a retired transport must not query its successor.
 VoipStack::message_waiting_callback_(&app,owner,0);auto retired=std::move(app.transport_);
 app.transport_=std::make_unique<SipTransport>();app.pump_message_waiting_();assert(app.message_waiting_trigger_.seen.size()==4);
 assert(app.message_waiting_pending_==0&&app.message_waiting_callbacks_.n==4);
}
'''
    cpp = tmp_path / "main-loop.cpp"
    cpp.write_text(code)
    binary = tmp_path / "main-loop"
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-fno-pie", "-no-pie", "-I", str(tmp_path), "-I", str(SOURCE), str(cpp), "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
