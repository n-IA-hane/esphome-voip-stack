"""Production RTP worker and independent loopback packet peer, without ESP hardware."""
import subprocess
import pytest
from test_refer_dialog_runtime import SOURCE, block


def worker_source():
    code = r'''
#define USE_ESPHOME_VOIP_DTMF
#define USE_ESPHOME_VOIP_DTMF_EVENTS
#define USE_ESPHOME_VOIP_STACK_PCM
#include "rtp_dtmf.h"
#include "sip_types.h"
#include "rtp_audio_payloads.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cinttypes>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using namespace esphome::voip_stack;
using LockGuard=std::lock_guard<std::mutex>;
const auto started=std::chrono::steady_clock::now();
uint32_t millis(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();}
uint32_t esp_random(){static std::atomic<uint32_t>next{1000};return next.fetch_add(7);}
#define portENTER_CRITICAL(p) (p)->lock()
#define portEXIT_CRITICAL(p) (p)->unlock()
template<typename... T>void ignored_log(T&&...){}
#define ESP_LOGI(...) ignored_log(__VA_ARGS__)
#define ESP_LOGW(...) ignored_log(__VA_ARGS__)
#define ESP_LOGE(...) ignored_log(__VA_ARGS__)
#define VOIP_STARTUP_EVENT(...) ((void)0)
#define TAG "host"
constexpr int pdTRUE=1;constexpr unsigned portMAX_DELAY=~0U;
unsigned ulTaskNotifyTake(int,unsigned){return 1;}
void xTaskNotifyGive(void*){}
void xSemaphoreGive(void*){}
void xSemaphoreTake(void*,int){}
void vTaskDelete(void*){}
const char*socket_errno_name(int){return "host";}const char*socket_errno_text(int){return "host";}
'''
    code += block((SOURCE/'sip_transport.h').read_text(), 'struct DtmfRequest {')+';\n'
    code += r'''
struct Queue {std::mutex lock;DtmfRequest data[8]{};unsigned at=0,count=0;};
unsigned uxQueueSpacesAvailable(Queue*q){LockGuard l(q->lock);return 8-q->count;}
unsigned uxQueueMessagesWaiting(Queue*q){LockGuard l(q->lock);return q->count;}
int xQueueSend(Queue*q,const void*p,int){LockGuard l(q->lock);if(q->count==8)return 0;q->data[(q->at+q->count++)%8]=*static_cast<const DtmfRequest*>(p);return 1;}
int xQueueReceive(Queue*q,void*p,int){LockGuard l(q->lock);if(!q->count)return 0;*static_cast<DtmfRequest*>(p)=q->data[q->at];q->at=(q->at+1)%8;--q->count;return 1;}
void xQueueReset(Queue*q){LockGuard l(q->lock);q->at=q->count=0;}
struct SipTransport {
 mutable std::mutex dialog_mutex_,rtp_socket_mutex_,media_config_lock_,media_lifecycle_mutex_;
 DtmfFormat dtmf_tx_format_{8000,0xFFFF,109},dtmf_rx_format_{8000,0xFFFF,109};
 std::atomic<uint32_t>dtmf_generation_{8},media_proposal_epoch_{8},remote_rtp_ip_v4_{INADDR_LOOPBACK};
 std::atomic<uint16_t>remote_rtp_port_{0},rtp_sequence_{100};std::atomic<uint32_t>rtp_timestamp_{10000};
 uint32_t rtp_ssrc_=1234,rtp_tx_clock_ms_=0,rtp_tx_clock_timestamp_=10000;
 bool refer_context_active_=false;size_t udp_max_payload_=1200;
 AudioFormat audio;AudioFormatList offer_rx_formats_;RtpAudioPayloads negotiated_rx_payloads_;
 int rtp_socket_=-1;uint16_t rtp_port_=0;
 Queue queue;Queue*dtmf_queue_=&queue;
 std::atomic<bool>media_active_{true},rtp_running_{true},rtp_task_terminate_{false},rtp_task_quiesced_{false};
 std::atomic<bool>audio_send_enabled_{true},audio_receive_enabled_{true},audio_receive_reset_pending_{false};
 std::atomic<bool>rtp_source_reacquire_pending_{false};
 std::atomic<uint32_t>rtp_tx_packets_{0},rtp_rx_packets_{0},rtp_tx_bytes_{0},rtp_rx_bytes_{0},rtp_rx_datagrams_{0};
 std::atomic<uint32_t>latched_rtp_ip_v4_{0},latched_rtp_ssrc_{0};std::atomic<uint16_t>latched_rtp_port_{0};std::atomic<bool>rtp_ssrc_latched_{false};
 enum class MediaLifecyclePhase {IDLE,ACTIVE,CLEANING,SHUTTING_DOWN};
 std::atomic<MediaLifecyclePhase>media_lifecycle_phase_{MediaLifecyclePhase::ACTIVE};
 void*rtp_cleanup_done_=nullptr,*rtp_task_done_=nullptr,*rtp_task_handle_=reinterpret_cast<void*>(1);
 std::string call_id_="live";unsigned quiescent=0;std::vector<char>digits;std::vector<uint16_t>audio_seen;
 bool can_send_dtmf()const;bool send_dtmf(char,uint16_t=160);
 uint32_t dtmf_generation()const{return dtmf_generation_.load();}
 void get_dtmf_config_(DtmfFormat*,DtmfFormat*,uint32_t* =nullptr)const;
 void set_dtmf_config_(const DtmfFormat&,const DtmfFormat&);
 bool can_send_dtmf_locked_(const DtmfFormat&,uint32_t)const;
 bool send_dtmf_packet_(const DtmfSender::Report&,const DtmfFormat&,uint32_t,uint32_t,uint16_t*);
 bool handle_dtmf_packet_(const uint8_t*,size_t,uint8_t,uint32_t,uint32_t,uint16_t,uint32_t,DtmfReceiver*);
 void get_media_config_(AudioFormat*t,AudioFormat*r,uint8_t*tp,uint8_t*rp)const{
  LockGuard l(media_config_lock_);if(t)*t=audio;if(r)*r=audio;if(tp)*tp=96;if(rp)*rp=96;}
 bool received_audio_format_(uint8_t,AudioFormat*,const uint8_t* =nullptr,size_t=0)const;
 void emit_dtmf_(char c,uint32_t){digits.push_back(c);}
 void emit_audio_frame_(const uint8_t*p,size_t n,uint16_t seq,uint32_t,bool,uint32_t){
  assert(n==320);for(unsigned i=0;i<160;++i){uint16_t got;std::memcpy(&got,p+i*2,2);assert(got==uint16_t(seq+i));}audio_seen.push_back(seq);}
 void emit_media_quiesced_(){++quiescent;}
 void emit_sip_signal_(SipSignal){assert(false);}
 bool send_bye_unlocked_(const std::string&){return false;}
 void reset_dialog_(){call_id_.clear();}
 void close_media_session_();void wake_rtp_task_();void request_audio_path_stop_locked_();void finish_audio_path_stop_();
 void send_audio_frame(const uint8_t*,size_t);void rtp_task_();
};
'''
    parser=(SOURCE/'sip_message.cpp').read_text()
    for signature in ['size_t pcm_to_rtp_payload(', 'size_t rtp_payload_to_pcm(']:
        code += block(parser, signature)+'\n'
    impl=(SOURCE/'sip_dtmf.cpp').read_text()
    for signature in ['void SipTransport::get_dtmf_config_(', 'void SipTransport::set_dtmf_config_(',
                      'bool SipTransport::can_send_dtmf_locked_(', 'bool SipTransport::can_send_dtmf() const',
                      'bool SipTransport::send_dtmf(', 'bool SipTransport::send_dtmf_packet_(',
                      'bool SipTransport::handle_dtmf_packet_(']:
        code += block(impl,signature)+'\n'
    impl=(SOURCE/'sip_transport.cpp').read_text()
    for signature in ['bool SipTransport::received_audio_format_(', 'void SipTransport::close_media_session_(', 'void SipTransport::wake_rtp_task_(',
                      'void SipTransport::request_audio_path_stop_locked_(', 'void SipTransport::finish_audio_path_stop_(',
                      'void SipTransport::send_audio_frame(', 'void SipTransport::rtp_task_(']:
        code += block(impl,signature)+'\n'
    code += r'''
uint16_t bind_udp(int fd){sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);assert(bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0);socklen_t n=sizeof(a);assert(getsockname(fd,reinterpret_cast<sockaddr*>(&a),&n)==0);return ntohs(a.sin_port);}
uint16_t u16(const uint8_t*p){return uint16_t(p[0])<<8|p[1];}
uint32_t u32(const uint8_t*p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
struct Event {uint32_t timestamp=0;unsigned ends=0,packets=0;};
int main(int argc,char**argv){
 assert(argc==2);const bool same=std::string(argv[1])=="shared";const bool terminate=std::string(argv[1])=="terminate";
 SipTransport dut;dut.audio.frame_ms=10;dut.audio.sample_rate=16000;
 dut.offer_rx_formats_.count=1;dut.offer_rx_formats_.formats[0]=dut.audio;assert(dut.negotiated_rx_payloads_.add(96,dut.audio));
 dut.dtmf_tx_format_.clock=same?16000:8000;
 dut.rtp_socket_=socket(AF_INET,SOCK_DGRAM,0);int peer=socket(AF_INET,SOCK_DGRAM,0);assert(peer>=0&&dut.rtp_socket_>=0);
 dut.rtp_port_=bind_udp(dut.rtp_socket_);dut.remote_rtp_port_=bind_udp(peer);
 sockaddr_in target{};target.sin_family=AF_INET;target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);target.sin_port=htons(dut.rtp_port_);
 const int original=dut.rtp_socket_;std::thread worker([&]{dut.rtp_task_();});
 const std::string keys="0123456789*#";unsigned next_key=0,audio_sent=0,audio_received=0;uint16_t peer_sequence=500;
 std::map<uint32_t,uint16_t>last_sequence;std::map<unsigned,Event>events;std::map<uint32_t,uint32_t>last_audio_ts;
 const auto start=millis();unsigned next_audio=0,next_digit=0;
 while(millis()-start<(terminate?50U:1750U)){
  const auto now=millis()-start;
  if(next_key<keys.size()&&now>=next_digit){assert(dut.send_dtmf(keys[next_key++],terminate?200:40));next_digit+=130;}
  if(now>=next_audio){
   const uint16_t seq=peer_sequence++;
   uint8_t packet[332]{};packet[0]=0x80;packet[1]=96;packet[2]=seq>>8;packet[3]=seq;
   const uint32_t stamp=audio_sent*160;packet[4]=stamp>>24;packet[5]=stamp>>16;packet[6]=stamp>>8;packet[7]=stamp;packet[11]=42;
   for(unsigned i=0;i<160;++i){const uint16_t v=seq+i;packet[12+i*2]=v>>8;packet[13+i*2]=v;}
   assert(sendto(peer,packet,sizeof(packet),0,reinterpret_cast<sockaddr*>(&target),sizeof(target))==sizeof(packet));
   uint8_t pcm[320];for(unsigned i=0;i<160;++i){const uint16_t v=audio_sent+i;std::memcpy(pcm+i*2,&v,2);}dut.send_audio_frame(pcm,sizeof(pcm));
   ++audio_sent;next_audio+=10;
  }
  if(now>=250&&peer_sequence<2000){
   // Independently authored event7, with R=1, initial+duplicate+three ends.
   for(unsigned report=0;report<5;++report){uint8_t p[16]{0x80,109};const auto seq=peer_sequence++;p[2]=seq>>8;p[3]=seq;p[6]=0x10;p[11]=42;p[12]=7;p[13]=0x40|(report>=2?0x80:0);p[14]=1;p[15]=0x40;
    assert(sendto(peer,p,sizeof(p),0,reinterpret_cast<sockaddr*>(&target),sizeof(target))==sizeof(p));}
   peer_sequence=2000;
  }
  fd_set fds;FD_ZERO(&fds);FD_SET(peer,&fds);timeval wait{0,1000};
  if(select(peer+1,&fds,nullptr,nullptr,&wait)>0){uint8_t p[1600];const int n=recv(peer,p,sizeof(p),0);assert(n>=16);
   const uint8_t pt=p[1]&127;const auto ssrc=u32(p+8),timestamp=u32(p+4);const auto seq=u16(p+2);
   auto found=last_sequence.find(ssrc);if(found!=last_sequence.end())assert(seq==uint16_t(found->second+1));last_sequence[ssrc]=seq;
   if(pt==96){assert(n==332&&ssrc==1234);if(last_audio_ts.count(ssrc))assert(timestamp==last_audio_ts[ssrc]+160);last_audio_ts[ssrc]=timestamp;
    for(unsigned i=0;i<160;++i)assert(u16(p+12+i*2)==uint16_t(audio_received+i));++audio_received;
   }else{assert(pt==109&&n==16&&(same?ssrc==1234:ssrc!=1234));const unsigned digit=p[12];assert(digit<12);
    auto &event=events[digit];if(!event.packets){assert(p[1]&0x80);event.timestamp=timestamp;}else assert(!(p[1]&0x80)&&event.timestamp==timestamp);
    assert(!(p[13]&0x40));const unsigned duration=u16(p+14);assert(duration>0&&duration<=(terminate?1600U:same?640U:320U));
    if(p[13]&0x80){++event.ends;assert(duration==(same?640U:320U));}++event.packets;
   }
  }
 }
 if(terminate){assert(next_key==1&&events.size()==1&&events.begin()->second.ends==0);for(char digit:{'1','2','3'})assert(dut.send_dtmf(digit));assert(uxQueueMessagesWaiting(dut.dtmf_queue_)==3);}
 else{assert(next_key==keys.size()&&events.size()==keys.size());for(const auto&row:events)assert(row.second.ends==3);}
 assert(audio_received==audio_sent&&audio_sent>=(terminate?4U:170U));
 // Match stop(): publish media-stop before task termination. Otherwise the
 // worker can observe termination while media still claims to be running.
 {LockGuard lock(dut.media_lifecycle_mutex_);dut.request_audio_path_stop_locked_();dut.rtp_task_terminate_=true;}
 worker.join();assert(dut.rtp_socket_==-1&&dut.rtp_task_quiesced_&&dut.media_lifecycle_phase_==SipTransport::MediaLifecyclePhase::IDLE);
 assert(dut.quiescent==1&&dut.digits==(terminate?std::vector<char>{}:std::vector<char>{'7'})&&dut.audio_seen.size()==audio_sent&&dut.queue.count==0);
 assert(!dut.send_dtmf('1'));uint8_t remaining[1600];while(recv(peer,remaining,sizeof(remaining),MSG_DONTWAIT)>0){}
 fd_set stopped;FD_ZERO(&stopped);FD_SET(peer,&stopped);timeval no_more{0,20000};assert(select(peer+1,&stopped,nullptr,nullptr,&no_more)==0);
 // Rebind proves cleanup owns the original port, not just a published fd field.
 int reuse=socket(AF_INET,SOCK_DGRAM,0);target.sin_port=htons(dut.rtp_port_);assert(bind(reuse,reinterpret_cast<sockaddr*>(&target),sizeof(target))==0);close(reuse);close(peer);
 printf("PASS worker %s audio=%u events=%zu rx-digits=%zu quiescent=%u original-fd=%d\n",same?"shared":"separate",audio_sent,events.size(),dut.digits.size(),dut.quiescent,original);
}
'''
    return code


@pytest.fixture(scope="module")
def worker_binary(tmp_path_factory):
    folder=tmp_path_factory.mktemp("dtmf-worker")
    cpp=folder/'worker.cpp'
    cpp.write_text(worker_source())
    binary=folder/'worker'
    built=subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-g',
        '-fsanitize=address,undefined','-fno-pie','-no-pie','-pthread','-I',str(SOURCE),str(cpp),'-o',str(binary)],capture_output=True,text=True)
    assert built.returncode==0,built.stderr
    return binary


@pytest.mark.parametrize('clock',['shared','separate','terminate'])
def test_worker_owns_dtmf_and_continuous_audio(worker_binary,clock):
    run=subprocess.run([str(worker_binary),clock],capture_output=True,text=True,timeout=8)
    assert run.returncode==0,run.stdout+run.stderr
    print(run.stdout,end='')
