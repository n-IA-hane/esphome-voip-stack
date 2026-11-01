// The actual application methods use a deterministic asynchronous speaker.
#include "esphome/components/voip_stack/sip_types.h"
#include "esphome/components/voip_stack/voip_fsm.h"
#include "esphome/components/voip_stack/rtp_jitter_buffer.h"
#include <atomic>
#include <arpa/inet.h>
#include <cassert>
#include <functional>
#include <string>
using namespace esphome::voip_stack;
const char *inet_ntoa_r(in_addr a,char*out,size_t n){return inet_ntop(AF_INET,&a,out,n);}
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define pdTRUE 1
using TickType_t=uint32_t;
uint32_t clock_ms=0;
uint32_t millis(){return clock_ms;}
uint32_t xTaskGetTickCount(){return clock_ms;}
void ulTaskNotifyTake(int,uint32_t duration){clock_ms+=duration;}
enum class SilenceReason{NONE,MUTED_SINK,NETWORK_GAP};
namespace audio {
struct AudioStreamInfo {
 uint8_t bits,channels;uint32_t rate;
 uint8_t get_bits_per_sample()const{return bits;}
 uint8_t get_channels()const{return channels;}
 uint32_t get_sample_rate()const{return rate;}
};
}
audio::AudioStreamInfo audio_stream_info_from_format(const AudioFormat&f){return {16,f.channels,f.sample_rate};}
struct Speaker {
 enum State{STOPPED,STARTING,RUNNING,STOPPING};State state=STOPPED;
 audio::AudioStreamInfo info{16,1,48000};
 bool start_requested=false,stop_requested=false;
 unsigned starts=0,stops=0,accepted=0;uint32_t ready_at=600;
 std::function<void()>on_start;
 bool is_running(){return state==RUNNING;}
 bool is_stopped(){return state==STOPPED;}
 const audio::AudioStreamInfo&get_audio_stream_info(){return info;}
 void set_audio_stream_info(audio::AudioStreamInfo next){info=next;}
 void start(){++starts;start_requested=true;auto callback=on_start;if(callback)callback();}
 void stop(){++stops;stop_requested=true;}
 void progress(){
  if(stop_requested){state=STOPPED;stop_requested=start_requested=false;return;}
  if(start_requested){state=STARTING;start_requested=false;}
  if(state==STARTING&&clock_ms>=ready_at)state=RUNNING;
 }
 size_t play(const uint8_t*,size_t bytes,uint32_t){if(!is_running())return 0;accepted+=bytes;return bytes;}
};
struct Transport{
 unsigned stops=0,answers=0;void stop_audio_path(){++stops;}
 bool originate(const std::string&,uint16_t){return true;}void set_sip_signaling_transport(bool){}
 bool prepare_media_path(){return true;}bool commit_media_path(){return true;}void abort_media_path(){}
};
struct VoipStack {
 Speaker *speaker_=nullptr;Transport transport;Transport *transport_=&transport;
 std::atomic<CallState>call_state_{CallState::CALLING};
 std::atomic<bool>audio_devices_active_{true};
 std::atomic<float>volume_{1};
 std::atomic<uint32_t>rx_audio_revision_{0},media_rx_queue_depth_{0},media_rx_queue_drops_{0},rx_underrun_start_ms_{0};
 RtpJitterBuffer *rx_jitter_buffer_=nullptr;
 uint8_t *rx_audio_chunk_=nullptr;size_t rx_audio_chunk_alloc_bytes_=0;
 enum class ApplicationMediaStart:uint8_t{NONE,ORIGINATE,ANSWER,CONFIRM};
 uint32_t prepared_peer_ip_=0xc0000201;uint16_t prepared_peer_port_=5060;bool prepared_peer_tcp_=false;
 ApplicationMediaStart pending_media_start_=ApplicationMediaStart::NONE;
 AudioFormat format{48000,PcmFormat::S16LE,1,10,AudioCodec::PCM};
 uint32_t calling_start_time_=0;static constexpr unsigned INVITE_NO_RESPONSE_TIMEOUT_MS=5000;
 bool incoming_setup_pending_=false,incoming_answer_accepted_=false;
 unsigned terminated=0,committed=0,invites=0;std::string id="call",last_sent;
 struct CallSnapshot{std::string call_id,caller_route,caller_name,dest_route,dest_name;};
 CallSnapshot snapshot_call_identity_(){return {id,"esp","esp","peer","peer"};}
 bool send_sip_invite_(const std::string&sent,const std::string&,const std::string&,const std::string&,const std::string&){++invites;last_sent=sent;return true;}
 bool send_sip_answer_(const std::string&){++transport.answers;return true;}
 std::string device_name_="esp",pending_dialplan_target_;
 void enable_loop_soon_any_context(){}
 AudioFormat get_current_rx_audio_format_(){return format;}
 std::string get_current_call_id_(){return id;}
 bool recent_terminal_call_(const std::string&,std::string*){return false;}
 void publish_state_(){}
 void reset_peer_audio_watchdog_(bool){}
 void set_audio_devices_active_(bool on){audio_devices_active_=on;}
 void set_call_state_(CallState next){
  if(next==CallState::IN_CALL){
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
   assert(!format.is_valid()||speaker_->is_running());
#endif
   ++committed;
  }
  call_state_=next;
 }
 void request_call_termination_(const TerminationIntent&){
  ++terminated;pending_media_start_=ApplicationMediaStart::NONE;set_in_call_(false);call_state_=CallState::IDLE;id.clear();
 }
 void start_speaker_for_current_rx_();bool prepare_speaker_for_current_rx_();
 void begin_application_media_start_(ApplicationMediaStart);void pump_application_media_start_();
 void finish_application_media_start_(ApplicationMediaStart);
 void set_in_call_(bool);void reset_rx_audio_();
 void play_rx_frame_(const uint8_t*,size_t,SilenceReason,TickType_t,uint32_t);
};
