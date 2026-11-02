"""Run production call selection and INVITE identity with native API requests."""

from pathlib import Path
import os
import subprocess

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / 'esphome/components/voip_stack'


def consumer_root():
    return Path(os.environ.get('VOIP_CALL_CONSUMER_ROOT', ROOT))


def write_consumer_includes(folder):
    """Only the ESP allocator boundary is adapted; both string types stay real."""
    core = folder / 'esphome/core'
    core.mkdir(parents=True, exist_ok=True)
    (core / 'helpers.h').write_text('''#pragma once
#include <cstdlib>
namespace esphome { template<class T> struct RAMAllocator {
using value_type=T;enum{ALLOC_EXTERNAL=1};explicit RAMAllocator(int flags=0):flags_(flags){}
T*allocate(size_t n){return static_cast<T*>(std::malloc(n*sizeof(T)));}
void deallocate(T*p,size_t){std::free(p);}int flags_;
}; }
''')


def correlated_fixture_source(*, snapshot=False):
    component = consumer_root() / 'esphome/components/voip_stack'
    fsm = (component / 'voip_fsm.cpp').read_text()
    settings = (component / 'voip_settings.cpp').read_text()
    methods = fsm[fsm.index('bool VoipStack::can_start_call_()'):fsm.index('void VoipStack::stop()')]
    methods += settings[settings.index('void VoipStack::call(const std::string &value)'):settings.index('void VoipStack::next_contact()')]
    from test_refer_dialog_runtime import block
    for marker in ('void VoipStack::begin_application_media_start_(', 'void VoipStack::finish_application_media_start_(', 'void VoipStack::pump_application_media_start_('):
        methods += '\n' + block(fsm, marker)
    start = settings.index('#ifdef USE_ESPHOME_VOIP_DIRECTORY\nstd::string VoipStack::get_current_destination()')
    methods += '\n' + settings[start:settings.index('void VoipStack::publish_destination_()', start)]
    if snapshot:
        methods += '\n' + block((component / 'voip_stack.cpp').read_text(),
                                 'std::string VoipStack::build_sip_snapshot_string_() const')
    transport = (component / 'transport.h').read_text()
    transport_snapshot = block(transport, 'struct SipTransportSnapshot {') + ';'
    return r'''
#include <atomic>
#include <arpa/inet.h>
#include <cassert>
#include <cstdint>
#include <map>
#include <functional>
#include <string>
#include "voip_fsm.h"
#include "sip_types.h"
#include "esphome/components/voip_stack/phonebook.h"
#include "esphome/components/voip_stack/sip_signaling_string.h"
#define ESP_LOGW(...)
#define ESP_LOGI(...)
#define ESP_LOGE(...)
uint32_t millis(){return 100;}
uint32_t esp_random(){return 42;}
const char*inet_ntoa_r(in_addr a,char*out,size_t n){return inet_ntop(AF_INET,&a,out,n);}
namespace esphome::voip_stack {
''' + transport_snapshot + r'''
struct Transport {
  bool terminating=false,originate_ok=true;
  mutable int snapshots=0;unsigned originations=0;
  std::string ip;
  SipTransportSnapshot value;
  SipTransportSnapshot snapshot() const {++snapshots;auto out=value;out.terminal_transaction_pending=terminating;return out;}
  void set_peer_directional_audio_v1(bool){}
  void set_sip_signaling_transport(bool){}
  bool prepare_media_path(){return true;}bool commit_media_path(){return true;}void abort_media_path(){}
  bool originate(const std::string &value,uint16_t){++originations;ip=value;return originate_ok;}
};
struct Trigger {unsigned calls=0;template<typename... Args> void trigger(Args...) {++calls;}};
class VoipStack {
public:
  std::atomic<CallState> call_state_{CallState::IDLE};
  Transport transport, *transport_=&transport;
  Phonebook phonebook_;
  VoipStack(){
    assert(phonebook_.add_one("Desk|Desk|192.0.2.20|5060|40000|sip_udp")==AddResult::Added);
    assert(phonebook_.add_one("HA|HA|192.0.2.10|5060|40000|sip_udp")==AddResult::Added);
    assert(phonebook_.select("Desk"));
  }
  std::string ha_peer_name_="HA",device_name_="Phone",device_route_id_="phone",pending_dialplan_target_;
  bool ha_media_fallback_pending_=false,ha_media_fallback_attempted_=false,send_ok=true;
  uint32_t calling_start_time_=0;AudioFormat tx_audio_format_,rx_audio_format_;int invites=0,stops=0;
  unsigned terminations=0;
  Trigger outgoing_call_trigger_,bridge_request_trigger_,call_failed_trigger_;
  std::function<void()> on_publish;
  struct CallSnapshot {std::string call_id,caller_route,caller_name,dest_route,dest_name;};
  CallSnapshot identity,terminated_identity;CallEndReason termination_reason=CallEndReason::LOCAL_HANGUP;
  bool can_start_call_() const;
  void start();void start_with_id_(const std::string &);
  void start_call_attempt_(bool,const std::string & = {});
  enum class ApplicationMediaStart:uint8_t{NONE,ORIGINATE,ANSWER,CONFIRM,OFFER};
  void begin_application_media_start_(ApplicationMediaStart);void finish_application_media_start_(ApplicationMediaStart);void pump_application_media_start_();
  uint32_t prepared_peer_ip_=0x7f000001;uint16_t prepared_peer_port_=5060;bool prepared_peer_tcp_=false;
  ApplicationMediaStart pending_media_start_=ApplicationMediaStart::NONE;std::atomic<uint32_t>rx_audio_revision_{0};
  void reset_rx_audio_(bool=false){++rx_audio_revision_;}bool sink_ready=true;
  bool incoming_setup_pending_=false,incoming_answer_accepted_=false;
  static constexpr unsigned INVITE_NO_RESPONSE_TIMEOUT_MS=5000;
  bool prepare_speaker_for_current_rx_(){return sink_ready;}bool prepare_rx_playout_(){return true;}bool activate_rx_playout_(){return true;}
  void enable_loop_soon_any_context(){}bool send_sip_answer_(const std::string&){return true;}
  void set_in_call_(bool on){if(on)call_state_=CallState::IN_CALL;}void reset_peer_audio_watchdog_(bool){}

  void call(const std::string &);void call_(const std::string &,const std::string &);
  void call_via_ha(const std::string &,const std::string &);void hangup_call_if_id(const std::string &);
  const char *get_call_state_str() const{return call_state_to_str(call_state_.load());}
  CallSnapshot snapshot_call_identity_() const{return identity;}
  std::string get_current_call_id_() const{return identity.call_id;}
#ifdef USE_ESPHOME_VOIP_DIRECTORY
  std::string get_current_contact_ip() const;std::string get_current_destination() const;
  std::string get_current_destination_route() const;
#else
  const std::string &get_current_contact_ip() const;const std::string &get_current_destination() const;
  const std::string &get_current_destination_route() const;
#endif
  uint16_t get_current_contact_port() const;uint16_t get_current_contact_rtp_port() const;
  bool get_current_contact_sip_transport_tcp() const;bool get_current_contact_directional_audio_v1() const;
  std::string build_sip_snapshot_string_() const;
  enum class TransportType{UDP,TCP};TransportType protocol_=TransportType::UDP;
  struct CurrentMediaFormats{AudioFormat tx,rx;};
  CurrentMediaFormats snapshot_current_media_formats_() const{return {tx_audio_format_,rx_audio_format_};}
  std::string last_terminal_call_id_,last_reason_,last_terminal_caller_name_,last_terminal_dest_name_,last_terminal_direction_;
  AudioFormat last_terminal_tx_audio_format_,last_terminal_rx_audio_format_;
  std::atomic<uint32_t>media_tx_queue_depth_{1},media_tx_queue_drops_{2},media_rx_queue_depth_{3},media_rx_queue_drops_{4};
  void clear_terminal_call_snapshot_(){} void clear_terminal_response_(){}
  void set_call_identity_(const std::string &id,const std::string &cr,const std::string &cn,const std::string &dr,const std::string &dn,bool originated=false){assert(originated);identity={id,cr,cn,dr,dn};}
  void set_current_media_formats_(AudioFormat,AudioFormat,AudioFormat,AudioFormat){} void set_call_state_(CallState s){call_state_=s;}
  void set_remote_sip_transport_tcp(bool){} void set_remote_endpoint(const std::string &,uint16_t,uint16_t){}
  void set_audio_devices_active_(bool){} void publish_destination_(){if(on_publish)on_publish();}
  template<typename T> void defer(T f){f();}
  void request_call_termination_(TerminationIntent intent){++terminations;termination_reason=intent.reason;terminated_identity=identity;call_state_=CallState::IDLE;identity={};}
  bool send_sip_invite_(const std::string &id,const std::string &,const std::string &,const std::string &dest,const std::string &){assert(id==identity.call_id);assert(dest==identity.dest_route);++invites;return send_ok;}
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
    write_consumer_includes(tmp_path)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-DUSE_ESP32',
                    '-I', str(tmp_path), '-I', str(consumer_root()),
                    '-I', str(consumer_root() / 'esphome/components/voip_stack'),
                    str(source), '-o', str(binary)], check=True, capture_output=True, text=True)
    subprocess.run([str(binary)], check=True)
