"""Call-owned identity reaches presentation without changing lifecycle or media."""
from pathlib import Path
import subprocess

from test_refer_dialog_runtime import block

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'esphome/components/voip_stack'


def test_connected_peer_is_call_owned_and_previous_call_dedupe_cannot_hide_it(tmp_path):
    fsm = (SOURCE / 'voip_fsm.cpp').read_text()
    settings = (SOURCE / 'voip_settings.cpp').read_text()
    code = r'''
#include <atomic>
#include <cassert>
#include <string>
#include <vector>
#define ESP_LOGI(...)
struct Mutex{};struct LockGuard{explicit LockGuard(Mutex&) {}};
enum class CallState{IDLE,CALLING,IN_CALL};
enum class SipSignalType{CONNECTED_IDENTITY};
struct Signal{SipSignalType type=SipSignalType::CONNECTED_IDENTITY;bool media_source_changed=false;std::string connected_name,connected_route;};
struct Trigger{std::vector<std::string>events;void trigger(const std::string&value){events.push_back(value);}};
struct VoipStack{
 mutable Mutex call_state_mutex_;std::atomic<CallState>call_state_{CallState::IDLE};
 std::string device_name_="Original device",device_route_id_="esp",selected="Other contact";
 std::string current_call_id_,current_caller_route_id_,current_caller_name_,current_dest_route_id_,current_dest_name_,current_dialed_dest_name_;
 std::string pending_dialplan_target_,last_reason_,last_terminal_direction_,last_terminal_dest_name_,last_published_destination_="Casa";
 bool incoming_setup_pending_=false,incoming_media_answer_pending_=false,incoming_answer_accepted_=false;
 Trigger destination_changed_trigger_,peer_changed_trigger_;unsigned snapshots=0,media_actions=0;
 std::string caller_published;
 struct CallSnapshot{std::string call_id,caller_route,caller_name,dest_route,dest_name,dialed_dest_name;};
 void set_call_identity_(const std::string&,const std::string&,const std::string&,const std::string&,const std::string&,bool=false);
 void clear_call_identity_();CallSnapshot snapshot_call_identity_()const;
 std::string get_active_peer_name()const;std::string get_current_destination()const{return selected;}
 void publish_destination_(bool=true);void publish_sip_snapshot_(){++snapshots;}
 void publish_caller_(const std::string&name){caller_published=name;}
 bool ignore_if_idle_or_stale_(const char*,const std::string&id){return id!=current_call_id_||call_state_==CallState::IDLE;}
 void reset_peer_audio_watchdog_(bool){++media_actions;}
 void receive(const Signal&msg,const std::string&in_call_id){switch(msg.type){CASE}}
};
'''
    code = code.replace('CASE', block(fsm, 'case SipSignalType::CONNECTED_IDENTITY:'))
    for signature in ['void VoipStack::set_call_identity_(', 'void VoipStack::clear_call_identity_(',
                      'VoipStack::CallSnapshot VoipStack::snapshot_call_identity_(',
                      'std::string VoipStack::get_active_peer_name() const']:
        code += block(fsm, signature) + '\n'
    code += block(settings, 'void VoipStack::publish_destination_(')
    code += r'''
int main(){
 VoipStack p;p.pending_dialplan_target_="sip:pickup.Test@192.0.2.1";
 p.set_call_identity_("pickup-call","esp","HA renamed device","sip:pickup.Test@192.0.2.1","Test",true);
 p.publish_destination_();assert(p.destination_changed_trigger_.events==std::vector<std::string>{"Test"});
 assert(p.get_active_peer_name()=="Test");p.pending_dialplan_target_.clear();p.call_state_=CallState::IN_CALL;
 Signal identity;identity.connected_name="Casa";identity.connected_route="Casa";
 p.receive(identity,"pickup-call");
 assert(p.get_active_peer_name()=="Casa"&&p.current_dest_name_=="Casa");
 assert((p.destination_changed_trigger_.events==std::vector<std::string>{"Test","Casa"}));
 assert(p.peer_changed_trigger_.events==std::vector<std::string>{"Casa"});
 assert(p.current_dialed_dest_name_=="Test"&&p.media_actions==0&&p.call_state_==CallState::IN_CALL);
 p.selected="Another contact";p.publish_destination_();assert(p.get_active_peer_name()=="Casa");
 assert(p.destination_changed_trigger_.events.size()==2);
 identity.connected_name="Stale";p.receive(identity,"old-call");assert(p.get_active_peer_name()=="Casa"&&p.peer_changed_trigger_.events.size()==1);
 p.clear_call_identity_();p.call_state_=CallState::IDLE;assert(p.get_active_peer_name().empty());
 p.set_call_identity_("incoming","casa","Casa","esp","HA renamed device");p.call_state_=CallState::IN_CALL;
 assert(p.get_active_peer_name()=="Casa"&&p.current_dialed_dest_name_.empty());
 identity.connected_name="Reception";identity.connected_route="102";p.receive(identity,"incoming");
 assert(p.get_active_peer_name()=="Reception"&&p.caller_published=="Reception");
 assert(p.peer_changed_trigger_.events.back()=="Reception"&&p.media_actions==0);
}
'''
    source = tmp_path / 'peer.cpp'
    source.write_text(code)
    binary = tmp_path / 'peer'
    result = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g',
                             '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                             '-DUSE_ESPHOME_VOIP_PEER_EVENTS', str(source), '-o', str(binary)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
