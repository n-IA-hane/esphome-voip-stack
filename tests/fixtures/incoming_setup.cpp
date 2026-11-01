// Hardware and publication adapters only. Behavioral methods are read from production.

#include "esphome/components/voip_stack/sip_types.h"
#include "esphome/components/voip_stack/voip_fsm.h"
#include <atomic>
#include <arpa/inet.h>
#include <cassert>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>
using namespace esphome::voip_stack;
#define ESP_LOGI(...)
#define ESP_LOGW(...)
#define ESP_LOGD(...)
#define ESP_LOGE(...)
uint32_t millis(){return 100;}
struct Trigger {template<class... T>void trigger(T...) {}};
struct Transport {
 unsigned prepares=0,disconnects=0,aborts=0;
 unsigned invites=0; bool prepare_ok=true,commit_ok=true,ringing_ok=true,final_ok=true;
 struct Snapshot {bool terminal_transaction_pending=false;unsigned media_lifecycle_phase=0;};
 Snapshot snapshot()const{return {};}
 bool prepare_media_path(){++prepares;return prepare_ok;}
 bool commit_media_path(){return commit_ok;}
 bool start_audio_path(){return true;}
 bool originate(const std::string&,uint16_t){return true;}void set_sip_signaling_transport(bool){}
 void abort_media_path(){++aborts;}
 void set_connected_identity(const std::string&,const std::string&){}
 void disconnect(){++disconnects;}
 bool is_connected()const{return true;}
 bool send_final_response(const std::string&,uint16_t status,const std::string&);
};
struct VoipStack {
 static const char*inet_ntoa_r(in_addr a,char*out,size_t n){return inet_ntop(AF_INET,&a,out,n);}
 std::atomic<CallState>call_state_{CallState::IDLE};
 Transport t;Transport*transport_=&t;
 bool do_not_disturb_=false,auto_answer_=true,incoming_setup_pending_=false;
 bool incoming_media_answer_pending_=false;
 bool incoming_answer_accepted_=false;
 int call_state_mutex_=0;struct LockGuard {explicit LockGuard(int&) {}};
 std::vector<std::function<void()>> deferred;
 Trigger call_failed_trigger_,hangup_trigger_;
 std::string last_terminal_call_id_,last_terminal_caller_name_,last_terminal_dest_name_,last_terminal_direction_;
 std::string pending_dialplan_target_;
 AudioFormat last_terminal_tx_audio_format_,last_terminal_rx_audio_format_;
 struct CurrentMediaFormats {AudioFormat tx,rx;};
 CurrentMediaFormats snapshot_current_media_formats_()const{return {};}
 void publish_last_reason_(const std::string&){}
 void start_call_attempt_(bool){assert(false);}
 bool ha_media_fallback_pending_=false,ha_media_fallback_attempted_=false;
 std::atomic<bool>first_audio_received_{false};
 std::atomic<uint32_t>rx_audio_revision_{0};
 uint32_t ringing_start_time_=0,calling_start_time_=0;
 static constexpr uint32_t INVITE_NO_RESPONSE_TIMEOUT_MS=5000;
 enum class ApplicationMediaStart:uint8_t{NONE,ORIGINATE,ANSWER,CONFIRM};
 uint32_t prepared_peer_ip_=0x7f000001;uint16_t prepared_peer_port_=5060;bool prepared_peer_tcp_=false;
 ApplicationMediaStart pending_media_start_=ApplicationMediaStart::NONE;
 void enable_loop_soon_any_context(){}
 bool prepare_speaker_for_current_rx_(){return true;}
 void begin_application_media_start_(ApplicationMediaStart);
 void finish_application_media_start_(ApplicationMediaStart);
 void pump_application_media_start_();
 static constexpr uint32_t TERMINAL_RESPONSE_REPLAY_MS=32000;
 std::string device_name_="esp",device_route_id_="esp",ha_peer_name_;
 AudioFormatList tx_audio_formats_,rx_audio_formats_;
 Trigger incoming_call_trigger_;
 struct Phonebook {void*find(const std::string&){return nullptr;}}phonebook_;
 struct CallSnapshot {std::string call_id,caller_route,caller_name,dest_route,dest_name,dialed_dest_name;}identity;
 std::string &current_call_id_=identity.call_id,&current_caller_route_id_=identity.caller_route,&current_caller_name_=identity.caller_name,&current_dest_route_id_=identity.dest_route,&current_dest_name_=identity.dest_name,&current_dialed_dest_name_=identity.dialed_dest_name;
 unsigned final_responses=0,end_calls=0,media_stops=0,watchdog_resets=0,rx_resets=0;
 std::string cached_id,cached_reason;
 uint16_t final_status=0;
 VoipStack(){audio_format_list_default(&tx_audio_formats_);audio_format_list_default(&rx_audio_formats_);}
 void on_sip_signal_received_(const SipSignal&);
 void complete_incoming_answer_(bool);
 void request_call_termination_(const TerminationIntent&);
 const char *get_call_state_str()const{return call_state_to_str(call_state_.load());}
 void snapshot_terminal_response_(std::string*a,std::string*b,uint32_t*age){*a="";*b="";*age=UINT32_MAX;}
 void set_terminal_response_(const std::string &id,const std::string &reason){cached_id=id;cached_reason=reason;}
 bool send_sip_final_response_(const std::string&,const std::string&);
 bool send_sip_bye_(const std::string&){return true;}
 bool send_sip_cancel_(const std::string&){return true;}
 void set_current_media_formats_(const AudioFormat&,const AudioFormat&,const AudioFormat&,const AudioFormat&){}
 void clear_terminal_call_snapshot_(){}
 void set_call_identity_(const std::string&a,const std::string&b,const std::string&c,const std::string&d,const std::string&e){identity={a,b,c,d,e,""};}
 void clear_terminal_response_(){}
 void publish_caller_(const std::string&){}
 template<class F>void defer(F fn){deferred.emplace_back(fn);}
 void drain(){while(!deferred.empty()){auto fn=deferred.front();deferred.erase(deferred.begin());fn();}}
 void set_audio_devices_active_(bool on){if(!on)++media_stops;}
 void set_call_state_(CallState s){call_state_=s;}
 bool send_sip_answer_(const std::string&){return true;}
 bool send_sip_invite_(const std::string&,const std::string&,const std::string&,const std::string&,const std::string&){++t.invites;return true;}
 bool send_sip_ringing_(const std::string&){return t.ringing_ok;}
 void set_in_call_(bool on){if(on)call_state_=CallState::IN_CALL;}
 bool ignore_if_idle_or_stale_(const char*,const std::string&)const{return false;}
 void finish_call_termination_();
 void clear_call_identity_();
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
 void reset_rx_audio_(){++rx_resets;++rx_audio_revision_;}
#endif
 void reset_peer_audio_watchdog_(bool){++watchdog_resets;}
 CallSnapshot snapshot_call_identity_()const{return identity;}
 std::string get_current_call_id_()const{return identity.call_id;}
 void publish_destination_(){}
 bool recent_terminal_call_(const std::string&,std::string*)const{return false;}
 void publish_state_(){}
 void answer_call(){}
 std::string get_current_destination()const{return "destination";}
 void end_call_(CallEndReason,const std::string&);
};

static uint16_t last_final_status=0;
bool Transport::send_final_response(const std::string&,uint16_t status,const std::string&){last_final_status=status;return final_ok;}
