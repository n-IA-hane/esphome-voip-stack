"""Run production call selection and INVITE identity with native API requests."""

from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / 'esphome/components/voip_stack'


def correlated_fixture_source():
    fsm = (COMPONENT / 'voip_fsm.cpp').read_text()
    settings = (COMPONENT / 'voip_settings.cpp').read_text()
    owner_cpp = (COMPONENT / 'voip_stack.cpp').read_text()
    methods = fsm[fsm.index('bool VoipStack::can_start_call_()'):fsm.index('void VoipStack::stop()')]
    begin = fsm.index('void VoipStack::begin_application_media_start_(')
    methods += fsm[begin:fsm.index('\n}\n', begin) + 2]
    for marker in ('void VoipStack::set_remote_endpoint(', 'void VoipStack::set_remote_sip_transport_tcp('):
        begin = owner_cpp.index(marker)
        methods += owner_cpp[begin:owner_cpp.index('\n}\n', begin) + 2]
    begin = fsm.index('void VoipStack::finish_application_media_start_(')
    methods += fsm[begin:fsm.index('\n}\n', begin) + 2]
    begin = fsm.index('void VoipStack::pump_application_media_start_(')
    methods += fsm[begin:fsm.index('\n}\n', begin) + 2]
    methods += settings[settings.index('void VoipStack::call(const std::string &value)'):settings.index('void VoipStack::next_contact()')]
    return r'''
#include <atomic>
#include <arpa/inet.h>
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
const char *inet_ntoa_r(in_addr a,char*out,size_t n){return inet_ntop(AF_INET,&a,out,n);}
namespace esphome::voip_stack {
struct Contact {std::string ip;uint16_t port=5060;uint16_t rtp_port=40000;bool sip_transport_tcp=false;bool directional_audio_v1=true;};
struct Phonebook {
  static constexpr size_t MAX_NAME_BYTES=64;
  static constexpr size_t MAX_ROUTE_BYTES=1024;
  std::map<std::string,Contact> contacts{{"Desk",{"192.0.2.20"}},{"HA",{"192.0.2.10"}}};
  std::string selected="Desk";
  static std::string trim(const std::string &s){auto b=s.find_first_not_of(" \t");return b==std::string::npos?"":s.substr(b,s.find_last_not_of(" \t")-b+1);}
  bool select(const std::string &s){if(!contacts.count(s))return false;selected=s;return true;}
  const Contact *find(const std::string &s) const {auto i=contacts.find(s);return i==contacts.end()?nullptr:&i->second;}
};
struct Transport {
  bool terminating=false,originate_ok=true;
  bool ha_ready=true,office_ready=true;
  std::string account_line,active_line,last_prepared;
  int prepare_calls=0;
  mutable int snapshots=0;
  std::string ip;
  uint32_t selected_ip=0;uint16_t selected_port=0;bool selected_tcp=false;
  struct Snapshot {bool terminal_transaction_pending;uint32_t remote_ip_v4;uint16_t remote_sip_port;bool sip_tcp;};
  Snapshot snapshot() const {++snapshots;return {terminating,selected_ip,selected_port,selected_tcp};}
  void set_peer_directional_audio_v1(bool){}
  void set_remote(const std::string&value,uint16_t port,uint16_t){ip=value;selected_ip=ntohl(inet_addr(value.c_str()));selected_port=port;}
  void set_sip_signaling_transport(bool tcp){selected_tcp=tcp;}
  bool originate(const std::string &value,uint16_t port){set_remote(value,port,0);return originate_ok;}
  bool prepare_media_path(){return true;}bool commit_media_path(){return true;}void abort_media_path(){}
  void clear_account_line(){account_line.clear();}
  bool prepare_account_line(const std::string &line,std::string *host,uint16_t *port,bool *tcp,bool *d1){
    ++prepare_calls;last_prepared=line;
    if((line=="ha"&&!ha_ready)||(line=="office"&&!office_ready))return false;
    assert(line=="ha"||line=="office");account_line=line;
    *host=line=="ha"?"192.0.2.30":"192.0.2.40";*port=5070;*tcp=true;*d1=true;return true;
  }
};
using SipTransport=Transport;
struct TransportHandle {
  Transport *value;
  Transport *get(){return value;} Transport *operator->()const{return value;}
  bool operator!=(std::nullptr_t)const{return value!=nullptr;}
  bool operator==(std::nullptr_t)const{return value==nullptr;}
  explicit operator bool()const{return value!=nullptr;}
};
struct Trigger {unsigned hits=0;template<typename... Args> void trigger(Args...) {++hits;}};
// URI parsing has separate production tests. These controlled labels exercise
// account selection and call identity through the actual application methods.
std::string sip_normalize_dial_target(const std::string &value,size_t){return value;}
class VoipStack {
public:
  std::atomic<CallState> call_state_{CallState::IDLE};
  Transport transport; TransportHandle transport_{&transport};
  Phonebook phonebook_;
  std::string ha_peer_name_="HA",device_name_="Phone",device_route_id_="phone",pending_dialplan_target_;
  bool ha_media_fallback_pending_=false,ha_media_fallback_attempted_=false,send_ok=true;
  bool force_direct_call_=false;
  std::string default_line_="direct",pending_line_;
  uint32_t calling_start_time_=0;int tx_audio_format_=0,rx_audio_format_=0,invites=0,stops=0;
  Trigger outgoing_call_trigger_,bridge_request_trigger_,call_failed_trigger_;
  std::function<void()> on_publish;
  struct CallSnapshot {std::string call_id,caller_route,caller_name,dest_route,dest_name;};
  CallSnapshot identity;
  bool can_start_call_() const;
  void start();void start_with_id_(const std::string &,const std::string & = {});
  void start_call_attempt_(bool,const std::string & = {},const std::string & = {});
  enum class ApplicationMediaStart:uint8_t{NONE,ORIGINATE,ANSWER,CONFIRM,OFFER};
  void begin_application_media_start_(ApplicationMediaStart);
  void finish_application_media_start_(ApplicationMediaStart);
  void pump_application_media_start_();
  uint32_t prepared_peer_ip_=0;uint16_t prepared_peer_port_=0;bool prepared_peer_tcp_=false;
  ApplicationMediaStart pending_media_start_=ApplicationMediaStart::NONE;
  std::atomic<uint32_t> rx_audio_revision_{0};
  void reset_rx_audio_(bool=false){++rx_audio_revision_;}
  bool sink_ready=true;
  bool incoming_setup_pending_=false,incoming_answer_accepted_=false;
  static constexpr unsigned INVITE_NO_RESPONSE_TIMEOUT_MS=5000;
  bool prepare_speaker_for_current_rx_(){return sink_ready;}
  bool prepare_rx_playout_(){return true;}bool activate_rx_playout_(){return true;}
  void enable_loop_soon_any_context(){}
  void call(const std::string &);void call_(const std::string &,const std::string &);
  void call_via_ha(const std::string &,const std::string &);void hangup_call_if_id(const std::string &);
  void start_call_on_line(const std::string &,const std::string &,const std::string & = {});
  void start_call_on_line_(const std::string &,const std::string &,const std::string &,const std::string &);
  bool send_sip_answer_(const std::string&){return true;}
  void set_in_call_(bool on){if(on)call_state_=CallState::IN_CALL;}
  void reset_peer_audio_watchdog_(bool){}
  bool is_registered(const std::string &line){return line=="ha"?transport.ha_ready:line=="office"&&transport.office_ready;}
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
  void set_call_identity_(const std::string &id,const std::string &cr,const std::string &cn,const std::string &dr,const std::string &dn,bool=false){identity={id,cr,cn,dr,dn};}
  void set_current_media_formats_(int,int,int,int){} void set_call_state_(CallState s){call_state_=s;}
  void set_remote_sip_transport_tcp(bool);
  void set_remote_endpoint(const std::string&,uint16_t,uint16_t=0);
  void set_audio_devices_active_(bool){} void publish_destination_(bool=true){if(on_publish)on_publish();}
  template<typename T> void defer(T f){f();}
  void request_call_termination_(TerminationIntent){call_state_=CallState::IDLE;identity={};}
  bool send_sip_invite_(const std::string &id,const std::string &,const std::string &,const std::string &dest,const std::string &){assert(id==identity.call_id);assert(dest==identity.dest_route);transport.active_line=transport.account_line;transport.account_line.clear();++invites;return send_ok;}
  void stop(){++stops;call_state_=CallState::IDLE;identity={};}
};
''' + methods + r'''
}
using namespace esphome::voip_stack;
'''


def test_correlated_call_uses_ha_and_scoped_hangup_without_retained_override(tmp_path):
    source = tmp_path / 'correlated_call.cpp'
    source.write_text(correlated_fixture_source() + r'''
int main(){
 VoipStack phone;
 phone.call_via_ha("Desk","ha-ABC123@controller");
 assert(phone.invites==1 && phone.identity.call_id=="ha-ABC123@controller");
 assert(phone.bridge_request_trigger_.hits==1);
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


def test_async_registered_preparation_keeps_reserved_line_and_original_route(tmp_path):
    registration = (COMPONENT / 'voip_registration.cpp').read_text()
    begin = registration.index('void VoipStack::start_call_on_line(')
    end = registration.index('void VoipStack::registration_state_callback_', begin)
    code = correlated_fixture_source() + '\nnamespace esphome::voip_stack {\n' + registration[begin:end] + r'''
}
int main(){
 VoipStack phone;phone.ha_peer_name_.clear();phone.default_line_="office";phone.sink_ready=false;
 phone.call_via_ha("410","reserved-before-ready@controller");
 assert(!phone.invites&&phone.call_state_==CallState::CALLING&&phone.pending_media_start_!=VoipStack::ApplicationMediaStart::NONE);
 assert(phone.transport.account_line=="ha"&&"The wrapper must retain the original account reservation");
 assert(phone.pending_line_.empty()&&phone.pending_dialplan_target_.empty());
 phone.default_line_="direct";phone.phonebook_.selected="Desk";
 phone.set_remote_endpoint("192.0.2.99",5099);phone.set_remote_sip_transport_tcp(false);
 phone.call("Desk");assert(!phone.invites&&phone.identity.dest_route=="410");
 phone.sink_ready=true;phone.pump_application_media_start_();
 assert(phone.invites==1&&phone.transport.active_line=="ha"&&phone.transport.ip=="192.0.2.30");
 assert(phone.transport.selected_port==5070&&phone.transport.selected_tcp);
 assert(phone.identity.call_id=="reserved-before-ready@controller"&&phone.identity.dest_route=="410");
 assert(phone.transport.account_line.empty()&&phone.pending_media_start_==VoipStack::ApplicationMediaStart::NONE);
}
'''
    source = tmp_path / 'reserved.cpp'
    source.write_text(code)
    binary = tmp_path / 'reserved'
    built = subprocess.run(['g++', '-std=c++17', '-DUSE_ESPHOME_VOIP_STACK_SPEAKER',
                            '-DUSE_ESPHOME_VOIP_REGISTRATION', '-DUSE_VOIP_HA_AUTO_REGISTER',
                            '-I', str(COMPONENT), str(source), '-o', str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    run = subprocess.run(['bash', '-c', 'ulimit -c 0; exec "$1"', 'bash', str(binary)], capture_output=True, text=True)
    assert run.returncode == 0, run.stderr


@pytest.mark.parametrize('speaker', [False, True])
def test_publish_reentrant_actual_stop_cancels_the_claimed_attempt(tmp_path, speaker):
    from test_refer_dialog_runtime import block

    fsm = (COMPONENT / 'voip_fsm.cpp').read_text()
    header = (COMPONENT / 'voip_stack.h').read_text()
    start = header.index('  bool is_active() const {')
    active = header[start:header.index('\n  }', start) + 4]
    code = correlated_fixture_source().replace(
        'void stop(){++stops;call_state_=CallState::IDLE;identity={};}',
        'void stop();std::atomic<bool>audio_devices_active_{false};\n' + active,
    )
    code += '\nnamespace esphome::voip_stack {\n' + block(fsm, 'void VoipStack::stop()') + '\n}\n'
    code += r'''
int main(){
 VoipStack phone;phone.on_publish=[&](){phone.stop();};phone.call("Desk");
 assert(!phone.invites&&phone.identity.call_id.empty());
}
'''
    cpp = tmp_path / 'publish_stop.cpp'
    cpp.write_text(code)
    binary = tmp_path / 'publish_stop'
    flags = ['-DUSE_ESPHOME_VOIP_STACK_SPEAKER'] if speaker else []
    built = subprocess.run(['g++', '-std=c++17', *flags, '-I', str(COMPONENT), str(cpp), '-o', str(binary)],
                           capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    run = subprocess.run(['bash', '-c', 'ulimit -c 0; exec "$1"', 'bash', str(binary)], capture_output=True, text=True)
    assert run.returncode == 0, run.stderr


@pytest.fixture(scope='module')
def managed_correlated_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp('managed-correlated-call')
    registration = (COMPONENT / 'voip_registration.cpp').read_text()
    begin = registration.index('void VoipStack::start_call_on_line(')
    end = registration.index('void VoipStack::registration_state_callback_', begin)
    methods = registration[begin:end]
    source = folder / 'managed_correlated.cpp'
    source.write_text(correlated_fixture_source() + '\nnamespace esphome::voip_stack {\n' + methods + r'''
}
int main(int argc,char **argv){
 assert(argc==2);const std::string scenario=argv[1];VoipStack phone;
 phone.ha_peer_name_.clear();phone.default_line_="office";
 if(scenario=="not-ready"||scenario=="legacy-not-ready"){
  phone.transport.ha_ready=false;
  if(scenario=="legacy-not-ready")phone.ha_peer_name_="HA";
  phone.call_via_ha("410","managed-unready@controller");
  assert(phone.invites==0 && phone.call_state_==CallState::IDLE);
  assert(phone.transport.ip.empty() && phone.pending_dialplan_target_.empty());
  assert(phone.pending_line_.empty() && phone.identity.call_id.empty());
  // The failed managed attempt must not contaminate a later ordinary call.
  phone.call("Desk");assert(phone.invites==1);
  assert(phone.transport.active_line=="office" && phone.transport.ip=="192.0.2.40");
  assert(phone.identity.call_id!="managed-unready@controller");return 0;
 }
 if(scenario=="reentry"){
  phone.on_publish=[&phone](){phone.call_state_=CallState::RINGING;phone.identity.call_id="incoming-dialog";};
  phone.call_via_ha("410","outgoing-action@controller");
  assert(phone.invites==0 && phone.identity.call_id=="incoming-dialog");
  assert(phone.pending_dialplan_target_.empty() && phone.pending_line_.empty());
  assert(phone.transport.account_line.empty() && "aborted originate must release the prepared account");return 0;
 }
 assert(scenario=="ready");
 phone.call_via_ha("410","ha-ExactCASE123@controller");
 assert(phone.invites==1 && "managed account must originate without a legacy HA contact");
 assert(phone.identity.call_id=="ha-ExactCASE123@controller" && phone.identity.dest_route=="410");
 assert(phone.transport.active_line=="ha" && phone.transport.ip=="192.0.2.30");
 assert(phone.transport.last_prepared=="ha" && phone.transport.prepare_calls==1);
 assert(phone.bridge_request_trigger_.hits==1 && "managed HA route must retain the bridge event");
 assert(phone.default_line_=="office" && phone.pending_line_.empty() && phone.pending_dialplan_target_.empty());
 phone.hangup_call_if_id("ha-exactcase123@controller");assert(phone.stops==0);
 phone.call_via_ha("411","other-call@controller");assert(phone.invites==1);
 phone.hangup_call_if_id("ha-ExactCASE123@controller");assert(phone.stops==1);
 phone.call("Desk");assert(phone.invites==2);
 assert(phone.transport.active_line=="office" && phone.transport.ip=="192.0.2.40");
 assert(phone.identity.call_id!="ha-ExactCASE123@controller" && phone.identity.dest_route=="Desk");
 assert(phone.bridge_request_trigger_.hits==1);
 phone.hangup_call_if_id("ha-ExactCASE123@controller");assert(phone.stops==1);
}
''')
    binary = folder / 'managed_correlated'
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-DUSE_ESPHOME_VOIP_REGISTRATION', '-DUSE_VOIP_HA_AUTO_REGISTER',
                    '-I', str(COMPONENT), str(source), '-o', str(binary)],
                   check=True, capture_output=True, text=True)
    return binary


@pytest.mark.parametrize('scenario', ['ready', 'not-ready', 'legacy-not-ready', 'reentry'])
def test_managed_correlated_call_keeps_account_and_supplied_identity(managed_correlated_binary, scenario):
    result = subprocess.run([managed_correlated_binary, scenario], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr


def test_manual_account_named_ha_keeps_line_semantics_with_or_without_identity(tmp_path):
    registration = (COMPONENT / 'voip_registration.cpp').read_text()
    begin = registration.index('void VoipStack::start_call_on_line(')
    methods = registration[begin:registration.index('void VoipStack::registration_state_callback_', begin)]
    source = tmp_path / 'manual_account.cpp'
    source.write_text(correlated_fixture_source() + '\nnamespace esphome::voip_stack {\n' + methods + r'''
}
int main(){
 VoipStack phone;phone.default_line_="ha";phone.phonebook_.selected="HA";
 phone.start_call_on_line("410","ha");
 assert(phone.invites==1&&phone.transport.active_line=="ha");
 assert(phone.bridge_request_trigger_.hits==0&&"a manual account is not the legacy selected peer");
 phone.stop();phone.start_call_on_line_("411","ha","manual-correlation@external",{});
 assert(phone.invites==2&&phone.identity.call_id=="manual-correlation@external");
 assert(phone.transport.active_line=="ha"&&phone.bridge_request_trigger_.hits==0);
 phone.stop();phone.call_via_ha("412","legacy-correlation@controller");
 assert(phone.invites==3&&phone.transport.active_line.empty());
 assert(phone.identity.call_id=="legacy-correlation@controller"&&phone.transport.ip=="192.0.2.10");
 assert(phone.bridge_request_trigger_.hits==1&&phone.outgoing_call_trigger_.hits==3);
}
''')
    binary = tmp_path / 'manual_account'
    built = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-DUSE_ESPHOME_VOIP_REGISTRATION', '-I', str(COMPONENT),
                            str(source), '-o', str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
