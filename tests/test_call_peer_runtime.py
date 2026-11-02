"""Actual identity, state transitions and worker-to-main-loop peer delivery."""
import subprocess
from test_refer_dialog_runtime import SOURCE, block


def test_peer_updates_preserve_owned_call_and_lifecycle_on_the_main_loop(tmp_path):
    fsm = (SOURCE / "voip_fsm.cpp").read_text()
    code = r'''
#include "voip_fsm.h"
#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
using namespace esphome::voip_stack;
template<class...T>void host_log(T&&...values){((void)values,...);}
const char*TAG="peer-test";
#define ESP_LOGI(...) host_log(__VA_ARGS__)
#define ESP_LOGD(...) host_log(__VA_ARGS__)
struct LockGuard {std::lock_guard<std::mutex> lock;explicit LockGuard(std::mutex&m):lock(m){}};
enum class SipSignalType {CONNECTED_IDENTITY};
struct SipSignal{SipSignalType type=SipSignalType::CONNECTED_IDENTITY;bool media_source_changed=false;std::string connected_name,connected_route,call_id;};
enum class MediaRoute{UNKNOWN};
struct Trigger{
 std::vector<std::string>events;std::thread::id expected=std::this_thread::get_id();unsigned calls=0;
 void trigger(const std::string&v){assert(std::this_thread::get_id()==expected);++calls;events.push_back(v);}
 void trigger(){assert(std::this_thread::get_id()==expected);++calls;}
 void call(CallState){assert(std::this_thread::get_id()==expected);++calls;}
};
struct VoipStack{
 mutable std::mutex call_state_mutex_;std::atomic<CallState>call_state_{CallState::IDLE};
 std::atomic<MediaRoute>media_route_{MediaRoute::UNKNOWN};std::string ha_peer_name_;
 struct Contact{std::string ip;};struct Book{const Contact*find(const std::string&)const{return nullptr;}}phonebook_;
 struct Transport{struct Snapshot{uint32_t remote_ip_v4=0;};Snapshot snapshot()const{return {};}}transport;Transport*transport_=&transport;
 std::string device_name_="Device",device_route_id_="esp",selected="Selected other";
 std::string current_call_id_,current_caller_route_id_,current_caller_name_,current_dest_route_id_,current_dest_name_,current_dialed_dest_name_;
 std::string pending_dialplan_target_,last_reason_,last_terminal_direction_,last_terminal_dest_name_,last_published_destination_="Casa";
 bool incoming_setup_pending_=false,incoming_media_answer_pending_=false,incoming_answer_accepted_=false;
 Trigger destination_changed_trigger_,peer_changed_trigger_,calling_trigger_,dest_ringing_trigger_,ringing_trigger_,in_call_trigger_,idle_trigger_,state_callback_;
 unsigned snapshots=0,media_actions=0,published=0,audio_notifications=0;std::atomic<unsigned>wakes{0};std::string caller_published;
 std::mutex deferred_mutex;std::vector<std::function<void()>>deferred;
 template<class F>void defer(F f){std::lock_guard<std::mutex>l(deferred_mutex);deferred.push_back(std::move(f));}
 void pump(){for(unsigned n=0;n<8;++n){std::vector<std::function<void()>>q;{std::lock_guard<std::mutex>l(deferred_mutex);q.swap(deferred);}if(q.empty())return;for(auto&f:q)f();}assert(false);}
 void enable_loop_soon_any_context(){++wakes;}
 struct CallSnapshot{std::string call_id,caller_route,caller_name,dest_route,dest_name,dialed_dest_name;};
 void set_call_identity_(const std::string&,const std::string&,const std::string&,const std::string&,const std::string&,bool=false);
 void clear_call_identity_();CallSnapshot snapshot_call_identity_()const;std::string get_current_call_id_()const;
 std::string get_active_peer_name()const;std::string get_current_destination()const{return selected;}
 void publish_destination_();void publish_sip_snapshot_(){++snapshots;}void publish_state_(){++published;}
 void publish_last_reason_(const std::string&){}void notify_audio_tasks_(){++audio_notifications;}
 void set_media_route(const std::string&,const char*){assert(false);}
 void publish_caller_(const std::string&name){caller_published=name;}
 bool ignore_if_idle_or_stale_(const char*,const std::string&)const;
 void reset_peer_audio_watchdog_(bool){++media_actions;}
 void set_call_state_(CallState);static void transport_sip_signal_callback_(void*,SipSignal);
 void on_sip_signal_received_(const SipSignal&msg){const auto&in_call_id=msg.call_id;switch(msg.type){CASE}}
};
'''.replace("CASE", block(fsm, "case SipSignalType::CONNECTED_IDENTITY:"))
    for marker in ["void VoipStack::set_call_identity_(", "void VoipStack::clear_call_identity_(", "VoipStack::CallSnapshot VoipStack::snapshot_call_identity_(",
                   "std::string VoipStack::get_current_call_id_() const", "std::string VoipStack::get_active_peer_name() const",
                   "bool VoipStack::ignore_if_idle_or_stale_(", "void VoipStack::set_call_state_("]:
        code += block(fsm, marker) + "\n"
    code += block((SOURCE / "voip_settings.cpp").read_text(), "void VoipStack::publish_destination_(") + "\n"
    code += block((SOURCE / "voip_stack.cpp").read_text(), "void VoipStack::transport_sip_signal_callback_(") + r'''
void deliver(VoipStack&p,SipSignal signal){std::thread worker([&]{VoipStack::transport_sip_signal_callback_(&p,signal);});worker.join();}
int main(){
 VoipStack p;assert(p.get_active_peer_name().empty());p.pending_dialplan_target_="*99101";
 p.set_call_identity_("pickup","esp","HA renamed device","*99101","Desk",true);
 assert(p.current_dialed_dest_name_=="Desk"&&p.get_active_peer_name()=="Desk");
 p.publish_destination_();assert(p.destination_changed_trigger_.events.back()=="Desk");p.pending_dialplan_target_.clear();
 p.set_call_state_(CallState::CALLING);p.pump();assert(p.calling_trigger_.events.back()=="Desk");
 p.set_call_state_(CallState::IN_CALL);p.pump();assert(p.in_call_trigger_.calls==1&&p.in_call_trigger_.events.back()=="Desk");
 SipSignal signal;signal.call_id="pickup";signal.connected_name="Casa";signal.connected_route="102";
 deliver(p,signal);assert(p.get_active_peer_name()=="Desk"&&p.peer_changed_trigger_.calls==0);p.pump();
 assert(p.get_active_peer_name()=="Casa"&&p.current_dest_route_id_=="102"&&p.peer_changed_trigger_.events==std::vector<std::string>{"Casa"});
 assert(p.in_call_trigger_.calls==1&&p.idle_trigger_.calls==0&&p.media_actions==0&&p.call_state_==CallState::IN_CALL&&p.current_dialed_dest_name_=="Desk");
 p.selected="Unrelated";p.publish_destination_();assert(p.get_active_peer_name()=="Casa"&&p.destination_changed_trigger_.events.back()=="Casa");
 signal.call_id="previous";signal.connected_name="Stale";deliver(p,signal);p.pump();assert(p.get_active_peer_name()=="Casa"&&p.peer_changed_trigger_.calls==1);
 signal.call_id="pickup";signal.connected_name.clear();signal.connected_route="103";deliver(p,signal);p.pump();assert(p.get_active_peer_name()=="103");
 signal.connected_route.clear();deliver(p,signal);p.pump();assert(p.peer_changed_trigger_.calls==2);
 signal.connected_name="Moved";signal.media_source_changed=true;deliver(p,signal);p.pump();assert(p.media_actions==1&&p.in_call_trigger_.calls==1);
 p.clear_call_identity_();p.set_call_state_(CallState::IDLE);p.pump();assert(p.get_active_peer_name().empty()&&p.idle_trigger_.calls==1);
 deliver(p,signal);p.pump();assert(p.peer_changed_trigger_.calls==3&&p.get_active_peer_name().empty());
 // A caller with the same display name as the device is still incoming.
 p.set_call_identity_("incoming","caller","Device","esp","Managed label");p.set_call_state_(CallState::RINGING);p.pump();
 assert(p.current_dialed_dest_name_.empty()&&p.ringing_trigger_.events.back()=="Device");
 p.set_call_state_(CallState::IN_CALL);p.pump();assert(p.in_call_trigger_.events.back()=="Device");
 signal.call_id="incoming";signal.connected_route="104";signal.connected_name="Reception";signal.media_source_changed=false;
 deliver(p,signal);p.pump();assert(p.get_active_peer_name()=="Reception"&&p.caller_published=="Reception"&&p.current_dest_name_=="Managed label");
 assert(p.in_call_trigger_.calls==2&&p.idle_trigger_.calls==1&&p.media_actions==1);
}
'''
    source = tmp_path / "peer.cpp"
    source.write_text(code)
    binary = tmp_path / "peer"
    built = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g", "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
        "-DUSE_ESPHOME_VOIP_PEER_EVENTS", "-I", str(SOURCE), str(source), "-pthread", "-o", str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    executed = subprocess.run([binary], capture_output=True, text=True, timeout=8)
    assert executed.returncode == 0, executed.stdout + executed.stderr
