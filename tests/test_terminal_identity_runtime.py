"""Terminal history and callback peer follow the owned dialog, not display labels."""
import subprocess
from test_refer_dialog_runtime import SOURCE, block


def test_terminal_history_role_survives_managed_labels_and_peer_updates(tmp_path):
    fsm = (SOURCE / "voip_fsm.cpp").read_text()
    code = r'''
#include "voip_fsm.h"
#include <atomic>
#include <cassert>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
using namespace esphome::voip_stack;
template<class...T>void log_values(T&&...v){((void)v,...);}
const char*TAG="terminal";
#define ESP_LOGI(...) log_values(__VA_ARGS__)
struct LockGuard{std::lock_guard<std::mutex>lock;explicit LockGuard(std::mutex&m):lock(m){}};
struct Trigger{std::vector<std::pair<std::string,std::string>>events;void trigger(const std::string&p,const std::string&r){events.emplace_back(p,r);}};
struct VoipStack{
 mutable std::mutex call_state_mutex_;std::atomic<CallState>call_state_{CallState::IN_CALL};
 std::string device_name_="Device",selected="Unrelated selected contact",pending_dialplan_target_;
 std::string current_call_id_,current_caller_route_id_,current_caller_name_,current_dest_route_id_,current_dest_name_,current_dialed_dest_name_;
 std::string last_terminal_call_id_,last_terminal_caller_name_,last_terminal_dest_name_,last_terminal_direction_,last_reason_;
 int last_terminal_tx_audio_format_=0,last_terminal_rx_audio_format_=0;
 bool incoming_setup_pending_=false,incoming_media_answer_pending_=false,incoming_answer_accepted_=false,ha_media_fallback_pending_=false;
 struct CurrentMediaFormats{int tx=48000,rx=16000;};
 struct CallSnapshot{std::string call_id,caller_route,caller_name,dest_route,dest_name,dialed_dest_name;};
 Trigger hangup_trigger_,call_failed_trigger_;unsigned finishes=0,transitions=0;
 std::vector<std::function<void()>>pending;
 template<class F>void defer(F f){pending.push_back(std::move(f));}
 void drain(){auto jobs=std::move(pending);for(auto&job:jobs)job();}
 void set_call_state_(CallState v){call_state_=v;++transitions;}
 void finish_call_termination_(){++finishes;}
 void publish_last_reason_(const std::string&v){last_reason_=v;}
 CurrentMediaFormats snapshot_current_media_formats_()const{return {};}
 std::string get_current_destination()const{return selected;}
 void set_call_identity_(const std::string&,const std::string&,const std::string&,const std::string&,const std::string&,bool=false);
 void clear_call_identity_();CallSnapshot snapshot_call_identity_()const;
 void end_call_(CallEndReason,const std::string&);
};
'''
    for marker in ["void VoipStack::set_call_identity_(", "void VoipStack::clear_call_identity_(",
                   "VoipStack::CallSnapshot VoipStack::snapshot_call_identity_(", "void VoipStack::end_call_("]:
        code += block(fsm, marker) + "\n"
    code += r'''
int main(){
 for(const std::string local_label:{"Device","Managed line name","Remote caller"}){
  VoipStack p;p.set_call_identity_("incoming","caller","Remote caller","ha",local_label);
  p.end_call_(CallEndReason::REMOTE_HANGUP,"");
  assert(p.last_terminal_direction_=="incoming");
  assert(p.last_terminal_call_id_=="incoming"&&p.last_terminal_caller_name_=="Remote caller"&&p.last_terminal_dest_name_==local_label);
  assert(p.current_call_id_.empty()&&p.current_dialed_dest_name_.empty());
  assert(p.transitions==1&&p.call_state_==CallState::TERMINATING&&p.hangup_trigger_.events.empty());
  p.drain();assert(p.finishes==1&&p.hangup_trigger_.events.size()==1&&p.hangup_trigger_.events[0].first=="Remote caller");
  assert(p.last_terminal_tx_audio_format_==48000&&p.last_terminal_rx_audio_format_==16000);
 }
 VoipStack out;out.set_call_identity_("outgoing","ha","Managed display","*99101","Dialed target",true);
 out.set_call_identity_("outgoing","ha","Managed display","102","Connected peer");
 out.end_call_(CallEndReason::LOCAL_HANGUP,"local_hangup");out.drain();
 assert(out.last_terminal_direction_=="outgoing"&&out.last_terminal_dest_name_=="Dialed target");
 assert(out.hangup_trigger_.events.size()==1&&out.hangup_trigger_.events[0].first=="Connected peer");
 VoipStack empty;empty.call_state_=CallState::IDLE;empty.last_terminal_direction_="prior";
 empty.end_call_(CallEndReason::LOCAL_HANGUP,"");
 assert(empty.last_terminal_direction_=="prior"&&empty.transitions==0&&empty.pending.empty());
 VoipStack unowned;unowned.end_call_(CallEndReason::TRANSPORT_UNREACHABLE,"");
 assert(unowned.last_terminal_direction_.empty());
}
'''
    source = tmp_path / "terminal.cpp"
    source.write_text(code)
    binary = tmp_path / "terminal"
    built = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-I", str(SOURCE), str(source), "-o", str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    executed = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert executed.returncode == 0, executed.stdout + executed.stderr
