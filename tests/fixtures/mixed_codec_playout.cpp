// RTOS scheduling and speaker hardware are adapters. SDP, RTP ingress,
// jitter buffering, playout and both codec implementations are production code.
#include <cmath>
#include <cassert>
#include <cstring>
#include <vector>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include <cerrno>
#include <fcntl.h>
#include "esphome/components/voip_stack/pcm_receive_converter.h"
#include "esphome/components/voip_stack/opus_rtp_codec.h"
#define USE_ESP32
#include "esphome/components/voip_stack/rtp_jitter_buffer.h"
#include "esphome/components/voip_stack/transport.h"
#undef USE_ESP32
using esphome::Mutex;using esphome::LockGuard;
using namespace esphome::voip_stack;
#define portENTER_CRITICAL(...) ((void)0)
#define portEXIT_CRITICAL(...) ((void)0)
#define pdTRUE 1
#define portMAX_DELAY 0xffffffff
#define configTICK_RATE_HZ 1000
using TickType_t=uint32_t;
struct EndRtp{};struct EndPlayout{};
unsigned tick=0;
uint32_t millis(){return tick;}
uint32_t xTaskGetTickCount(){return tick;}
void ulTaskNotifyTake(int,unsigned ticks){if(ticks!=portMAX_DELAY)tick+=ticks;}
void xTaskNotifyGive(void*){}
void xSemaphoreGive(void*){}
int xSemaphoreTake(void*,unsigned){return 0;}
uint32_t esp_random(){return 42;}
constexpr unsigned kRtpSocketRxBufferBytes=65536;
void vTaskDelete(void*){}
const char *socket_errno_name(int){return "error";}
const char *socket_errno_text(int){return "error";}
void deliver(const TransportAudioFrame&);

// TRANSPORT_FIELDS

enum class CallState{IN_CALL,IDLE,CONNECTING};
enum class SilenceReason{NONE,MUTED_SINK,NETWORK_GAP};
struct Speaker {
 std::vector<std::vector<int16_t>> frames;
 size_t play(const uint8_t *data,size_t bytes,unsigned){
  const auto*p=reinterpret_cast<const int16_t*>(data);frames.emplace_back(p,p+bytes/2);
  if(frames.size()==8)throw EndPlayout{};
  assert(frames.size()<8);return bytes;
 }
};
struct VoipStack {
 Speaker *speaker_;RtpJitterBuffer *rx_jitter_buffer_;SipTransport *transport_;
 std::atomic<bool>audio_devices_active_{true},first_audio_received_{false};
 std::atomic<uint32_t>rx_audio_revision_{0},last_peer_audio_ms_{0};
 std::atomic<CallState>call_state_{CallState::IN_CALL};
 std::atomic<uint32_t>rx_underrun_start_ms_{0},media_rx_queue_depth_{0},media_rx_queue_drops_{0};
 std::atomic<float>volume_{1};void *rx_task_handle_=nullptr;
 uint8_t data[2048]{},network[2048]{};
 uint8_t*rx_audio_chunk_=data,*rx_network_chunk_=network;
 size_t rx_audio_chunk_alloc_bytes_=2048,rx_jitter_frame_alloc_bytes_=2048;
 static constexpr unsigned kRxSilenceAfterMs=100;
 AudioFormat get_current_rx_audio_format_(){return transport_->rx;}
 void on_audio_received_(const TransportAudioFrame&);
 void enqueue_rx_frame_(const TransportAudioFrame&);
 void play_rx_frame_(const uint8_t*,size_t,SilenceReason,unsigned,uint32_t);
 void play_silence_frame_(SilenceReason,unsigned,uint32_t);
 void rx_task_();
};
VoipStack *phone=nullptr;
void deliver(const TransportAudioFrame&frame){phone->on_audio_received_(frame);}

// PRODUCTION_METHODS

AudioFormat fmt(unsigned rate,unsigned frame,AudioCodec codec){AudioFormat f;f.sample_rate=rate;f.frame_ms=frame;f.codec=codec;return f;}
int main(int argc,char**argv){
 const bool opus_first=std::string(argv[1])=="opus";const unsigned rate=std::stoul(argv[2]),frame=std::stoul(argv[3]);
 SipTransport s;const auto pcm=fmt(rate,frame,AudioCodec::PCM),opus=fmt(rate,frame,AudioCodec::OPUS);
 s.offer_tx_formats_.count=s.offer_rx_formats_.count=2;
 s.offer_tx_formats_.formats[0]=s.offer_rx_formats_.formats[0]=opus_first?opus:pcm;
 s.offer_tx_formats_.formats[1]=s.offer_rx_formats_.formats[1]=opus_first?pcm:opus;
 s.set_audio_formats(s.offer_tx_formats_,s.offer_rx_formats_);assert(s.opus_codec_ready_);
 const auto offer=s.build_sdp_offer_();assert(!offer.empty());
 const unsigned pcm_pt=opus_first?97:96,opus_pt=opus_first?96:97;
 const std::string answer="v=0\r\nc=IN IP4 127.0.0.1\r\nm=audio 40002 RTP/AVP 96 97\r\na=ptime:"+std::to_string(frame)+
   "\r\na=rtpmap:"+std::to_string(pcm_pt)+" L16/"+std::to_string(rate)+"/1\r\na=rtpmap:"+std::to_string(opus_pt)+" opus/48000/2\r\na=sendrecv\r\n";
 assert(s.learn_remote_rtp_from_sdp_(answer,0,true)&&s.accepted.count==2);
 // Even PCM-preferred calls must validate the advertised fallback decoder.
 auto wrong=opus;wrong.sample_rate=rate==48000?16000:48000;
 assert(s.opus_codec_.configure(nullptr,&wrong,1200));
 assert(!s.prepare_media_path_locked_()&&s.rtp_socket_==-1);
 s.set_audio_formats(s.offer_tx_formats_,s.offer_rx_formats_);
 s.rtp_port_=0;assert(s.prepare_media_path_locked_());
 assert(s.media_lifecycle_phase_==SipTransport::MediaLifecyclePhase::PREPARED);
 assert(s.commit_media_path_locked_());
 sockaddr_in destination{};
 socklen_t length=sizeof(destination);assert(getsockname(s.rtp_socket_,reinterpret_cast<sockaddr*>(&destination),&length)==0);
 const int sender=socket(AF_INET,SOCK_DGRAM,0);assert(sender>=0);
 uint8_t storage[16*2048]{};RtpJitterBuffer jitter(storage,2048,16,1);Speaker speaker;VoipStack app;
 app.speaker_=&speaker;app.rx_jitter_buffer_=&jitter;app.transport_=&s;phone=&app;
 std::vector<int16_t> tone(pcm.nominal_frame_samples());
 for(size_t i=0;i<tone.size();++i)tone[i]=int16_t(9000*std::sin(2*3.141592653589793*400*i/rate));
 int error=0;auto *encoder=opus_encoder_create(rate,1,OPUS_APPLICATION_VOIP,&error);assert(encoder&&error==OPUS_OK);
 auto send_packet=[&](unsigned seq,unsigned pt,bool encoded,bool bad_size=false){
  std::vector<uint8_t> packet(12+1275);packet[0]=0x80;packet[1]=pt;
  packet[2]=seq>>8;packet[3]=seq;
  const unsigned timestamp=seq*(encoded?48*frame:pcm.nominal_frame_samples());
  packet[4]=timestamp>>24;packet[5]=timestamp>>16;packet[6]=timestamp>>8;packet[7]=timestamp;packet[11]=42;
  size_t bytes;
  if(encoded){int n=opus_encode(encoder,tone.data(),tone.size(),packet.data()+12,1275);assert(n>0);bytes=n;}
  else{bytes=tone.size()*2;for(size_t i=0;i<tone.size();++i){packet[12+2*i]=uint16_t(tone[i])>>8;packet[13+2*i]=tone[i];}}
  if(bad_size)--bytes;
  assert(sendto(sender,packet.data(),12+bytes,0,reinterpret_cast<sockaddr*>(&destination),sizeof(destination))==ssize_t(12+bytes));
 };
 send_packet(20,99,false);send_packet(21,pcm_pt,false,true);
 for(unsigned seq:{1U,2U,4U,5U,7U,8U})send_packet(seq,(seq==4||seq==5)?opus_pt:pcm_pt,seq==4||seq==5);
 try{s.rtp_task_();assert(false);}catch(const EndRtp&){}
 assert(s.rtp_rx_packets_==6&&jitter.depth()==6);
 try{app.rx_task_();assert(false);}catch(const EndPlayout&){}
 assert(speaker.frames.size()==8&&s.decodes==3&&s.plc==1&&"Opus decode/PLC follows each packet's codec, not the preferred codec");
 for(size_t i:{0U,1U,6U,7U})assert(speaker.frames[i]==tone&&"PCM samples must pass unchanged through an Opus-preferred session");
 for(size_t i:{2U})for(auto v:speaker.frames[i])assert(v==0);
 for(size_t i:{3U,4U,5U}){
  assert(speaker.frames[i].size()==tone.size());double power=0;for(auto v:speaker.frames[i])power+=double(v)*v;
  assert(power>1000000&&"Real decoded Opus and PLC must reach the speaker as PCM");
 }
 assert(s.resets==3);opus_encoder_destroy(encoder);close(sender);close(s.rtp_socket_);
}
