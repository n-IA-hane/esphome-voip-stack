"""Getter-only BLF keeps its state without scheduling unused UI callbacks."""

import subprocess

from test_refer_dialog_runtime import SOURCE, block


def test_unobserved_blf_skips_defer_and_inactive_automations_still_receive(tmp_path):
    defines = tmp_path / "esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    header = (SOURCE / "voip_stack.h").read_text()
    source = (SOURCE / "voip_blf.cpp").read_text()
    code = r'''
#include "sip_blf.h"
#include <atomic>
#include <cassert>
#include <functional>
#include <string_view>
#include <vector>
#include <utility>
using namespace esphome::voip_stack;
template<typename...Ts>struct Trigger{
 bool attached=false;unsigned calls=0;
 bool is_action_running()const{return false;}
 void trigger(const Ts&...){if(attached)++calls;}
};
struct Callbacks{
 std::vector<std::function<void(const BlfStateInfo&)>> items;
 template<class F>void add(F&&f){items.emplace_back(std::forward<F>(f));}
 void call(const BlfStateInfo&s){for(auto&f:items)f(s);}
};
'''
    code += block(source, "const char *blf_state_name(") + "\n"
    code += r'''
struct VoipStack{
 std::atomic<bool>blf_notifications_requested_{false};Callbacks blf_callbacks_;
 Trigger<std::string,std::string,std::string,bool>blf_state_trigger_;
 std::vector<std::function<void()>> pending;unsigned defers=0;
 BlfStateInfo cached;
 VoipStack(){cached.contact_id="long-stable-contact-identity-123456";cached.account_id="office";cached.available=true;cached.presence=PresenceState::OPEN;}
 void defer(std::function<void()>f){++defers;pending.push_back(std::move(f));}
 BlfStateInfo get_contact_blf_info_(std::string_view id){assert(id==cached.contact_id);return cached;}
 void drain(){for(auto&f:pending)f();pending.clear();}
 static void blf_callback_(void*,const BlfStateInfo&);
'''
    code += block(header, "template<typename F> void add_on_blf_state_callback(") + "\n"
    code += block(header, "Trigger<std::string, std::string, std::string, bool> *get_blf_state_trigger()") + "\n};\n"
    code += block(source, "void VoipStack::blf_callback_(") + r'''
void notify_twenty(VoipStack&phone){for(unsigned n=0;n<20;++n){++phone.cached.version;VoipStack::blf_callback_(&phone,phone.cached);}}
int main(){
 VoipStack getters;notify_twenty(getters);assert(getters.defers==0&&getters.pending.empty());
 assert(getters.get_contact_blf_info_(getters.cached.contact_id).version==20);
 notify_twenty(getters);assert(getters.defers==0&&getters.cached.version==40);
 VoipStack observed;unsigned delivered=0;
 observed.add_on_blf_state_callback([&](const BlfStateInfo&state){assert(state.available&&state.version==20);++delivered;});
 notify_twenty(observed);assert(observed.defers==20&&delivered==0);observed.drain();assert(delivered==20);
 VoipStack automation;auto*trigger=automation.get_blf_state_trigger();trigger->attached=true;
 assert(!trigger->is_action_running());notify_twenty(automation);assert(automation.defers==20&&trigger->calls==0);
 automation.drain();assert(trigger->calls==20);
}
'''
    cpp = tmp_path / "delivery.cpp"
    cpp.write_text(code)
    binary = tmp_path / "delivery"
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_ESPHOME_VOIP_BLF",
        "-I", str(tmp_path), "-I", str(SOURCE), str(cpp), "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
