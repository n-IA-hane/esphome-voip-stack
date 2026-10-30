"""Run production call selection and INVITE identity with native API requests."""

from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / 'esphome/components/voip_stack'


def test_correlated_call_uses_ha_and_scoped_hangup_without_retained_override(tmp_path):
    fsm = (COMPONENT / 'voip_fsm.cpp').read_text()
    settings = (COMPONENT / 'voip_settings.cpp').read_text()
    methods = fsm[fsm.index('bool VoipStack::can_start_call_()'):fsm.index('void VoipStack::stop()')]
    methods += settings[settings.index('void VoipStack::call(const std::string &value)'):settings.index('void VoipStack::next_contact()')]
    source = tmp_path / 'correlated_call.cpp'
    source.write_text(r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <map>
#include <functional>
#include <string>
#include "voip_fsm.h"
#define ESP_LOGW(...)
#define ESP_LOGI(...)
#define ESP_LOGE(...)
uint32_t millis(){return 100;}
uint32_t esp_random(){return 42;}
namespace esphome::voip_stack {
struct Contact {std::string ip;uint16_t port=5060;uint16_t rtp_port=40000;bool sip_transport_tcp=false;bool directional_audio_v1=true;};
struct Phonebook {
  static constexpr size_t MAX_NAME_BYTES=64;
  std::map<std::string,Contact> contacts{{"Desk",{"192.0.2.20"}},{"HA",{"192.0.2.10"}}};
  std::string selected="Desk";
  static std::string trim(const std::string &s){auto b=s.find_first_not_of(" \t");return b==std::string::npos?"":s.substr(b,s.find_last_not_of(" \t")-b+1);}
  bool select(const std::string &s){if(!contacts.count(s))return false;selected=s;return true;}
  const Contact *find(const std::string &s) const {auto i=contacts.find(s);return i==contacts.end()?nullptr:&i->second;}
};
struct Transport {
  bool terminating=false,originate_ok=true;
  mutable int snapshots=0;
  std::string ip;
  struct Snapshot {bool terminal_transaction_pending;};
  Snapshot snapshot() const {++snapshots;return {terminating};}
  void set_peer_directional_audio_v1(bool){}
  bool originate(const std::string &value,uint16_t){ip=value;return originate_ok;}
};
struct Trigger {template<typename... Args> void trigger(Args...) {}};
class VoipStack {
public:
  std::atomic<CallState> call_state_{CallState::IDLE};
  Transport transport, *transport_=&transport;
  Phonebook phonebook_;
  std::string ha_peer_name_="HA",device_name_="Phone",device_route_id_="phone",pending_dialplan_target_;
  bool ha_media_fallback_pending_=false,ha_media_fallback_attempted_=false,send_ok=true;
  uint32_t calling_start_time_=0;int tx_audio_format_=0,rx_audio_format_=0,invites=0,stops=0;
  Trigger outgoing_call_trigger_,bridge_request_trigger_,call_failed_trigger_;
  std::function<void()> on_publish;
  struct CallSnapshot {std::string call_id,caller_route,caller_name,dest_route,dest_name;};
  CallSnapshot identity;
  bool can_start_call_() const;
  void start();void start_with_id_(const std::string &);
  void start_call_attempt_(bool,const std::string & = {});
  void call(const std::string &);void call_(const std::string &,const std::string &);
  void call_via_ha(const std::string &,const std::string &);void hangup_call_if_id(const std::string &);
  const char *get_call_state_str() const{return "state";}
  const std::string &get_caller_display_name() const{return device_name_;}
  CallSnapshot snapshot_call_identity_() const{return identity;}
  std::string get_current_call_id_() const{return identity.call_id;}
  std::string get_current_contact_ip() const {auto c=phonebook_.find(phonebook_.selected);return c?c->ip:"";}
  uint16_t get_current_contact_port() const{return 5060;}
  uint16_t get_current_contact_rtp_port() const{return 40000;}
  bool get_current_contact_sip_transport_tcp() const{return false;}
  bool get_current_contact_directional_audio_v1() const{return true;}
  std::string get_current_destination() const{return phonebook_.selected;}
  std::string get_current_destination_route() const{return phonebook_.selected;}
  void clear_terminal_call_snapshot_(){} void clear_terminal_response_(){}
  void set_call_identity_(const std::string &id,const std::string &cr,const std::string &cn,const std::string &dr,const std::string &dn){identity={id,cr,cn,dr,dn};}
  void set_current_media_formats_(int,int,int,int){} void set_call_state_(CallState s){call_state_=s;}
  void set_remote_sip_transport_tcp(bool){} void set_remote_endpoint(const std::string &,uint16_t,uint16_t){}
  void set_audio_devices_active_(bool){} void publish_destination_(){if(on_publish)on_publish();}
  template<typename T> void defer(T f){f();}
  void request_call_termination_(TerminationIntent){call_state_=CallState::IDLE;identity={};}
  bool send_sip_invite_(const std::string &id,const std::string &,const std::string &,const std::string &dest,const std::string &){assert(id==identity.call_id);assert(dest==identity.dest_route);++invites;return send_ok;}
  void stop(){++stops;call_state_=CallState::IDLE;identity={};}
};
''' + methods + r'''
}
using namespace esphome::voip_stack;
int main(){
 VoipStack phone;
 phone.call_via_ha("Desk","ha-ABC123@controller");
 assert(phone.invites==1 && phone.identity.call_id=="ha-ABC123@controller");
 assert(phone.transport.ip=="192.0.2.10" && phone.identity.dest_route=="Desk");
 assert(phone.pending_dialplan_target_.empty());
 phone.hangup_call_if_id("ha-abc123@controller");assert(phone.stops==0);
 phone.call_via_ha("Other","different-id");assert(phone.invites==1 && phone.identity.dest_name=="Desk");
 phone.hangup_call_if_id("ha-ABC123@controller");assert(phone.stops==1);
 phone.call("Desk");assert(phone.invites==2 && phone.transport.ip=="192.0.2.20");
 assert(phone.identity.call_id=="phone-100-42@phone");
 phone.hangup_call_if_id("ha-ABC123@controller");assert(phone.stops==1);
 phone.stop();
 for(const std::string bad:{"", "bad\r\nHeader", "bad value", "@ha", "value@", "a@@b"})phone.call_via_ha("Desk",bad);
 phone.call_via_ha("Desk",std::string(129,'x'));assert(phone.invites==2);
 phone.transport.terminating=true;phone.call_via_ha("999","unique");assert(phone.invites==2 && phone.pending_dialplan_target_.empty());
 phone.transport.terminating=false;phone.transport.originate_ok=false;phone.call_via_ha("999","fail-id");
 assert(phone.call_state_==CallState::IDLE && phone.pending_dialplan_target_.empty());
 phone.transport.originate_ok=true;phone.call("Desk");assert(phone.invites==3 && phone.identity.call_id!="fail-id");
 phone.stop();
 phone.on_publish=[&phone](){phone.call_state_=CallState::RINGING;phone.identity.call_id="incoming-dialog";};
 phone.call_via_ha("Desk","outgoing-action");
 assert(phone.invites==3 && phone.identity.call_id=="incoming-dialog");
 assert(phone.pending_dialplan_target_.empty());
}
''')
    binary = tmp_path / 'correlated_call'
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(COMPONENT), str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
