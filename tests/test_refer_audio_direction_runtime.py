"""Production REFER and RTP owners retain each negotiated audio direction."""
import subprocess

import pytest

from test_refer_dialog_runtime import ROOT, SOURCE, block, refer_fixture_source
from audio_tx_fixture import audio_tx_fixture


def source():
    code = refer_fixture_source()
    owner = (SOURCE / 'sip_transport.cpp').read_text()
    parser = (SOURCE / 'sip_message.cpp').read_text()
    code = '#define portENTER_CRITICAL(p) (p)->lock()\n#define portEXIT_CRITICAL(p) (p)->unlock()\n#define USE_ESPHOME_VOIP_STACK_PCM\n#include <mutex>\n#include <functional>\n#include <thread>\n#include <chrono>\n#include <cstring>\n#include <cinttypes>\n#include <cerrno>\n#include <unistd.h>\n' + code
    code = code.replace('struct Mutex{};struct LockGuard{explicit LockGuard(Mutex&) {}};',
                        'using Mutex=std::mutex;using LockGuard=std::lock_guard<Mutex>;')
    extra = r'''
 mutable Mutex media_config_lock_;Mutex rtp_socket_mutex_,media_lifecycle_mutex_,dialog_mutex_;
 std::atomic<bool>audio_receive_reset_pending_{false},rtp_source_reacquire_pending_{false};
 std::atomic<bool>rtp_running_{true},rtp_task_terminate_{false},rtp_task_quiesced_{false},rtp_ssrc_latched_{false};
 std::atomic<uint32_t>rtp_timestamp_{10000},rtp_tx_packets_{0},rtp_tx_bytes_{0},rtp_rx_packets_{0},rtp_rx_bytes_{0},rtp_rx_datagrams_{0};
 std::atomic<uint32_t>latched_rtp_ip_v4_{0},latched_rtp_ssrc_{0};std::atomic<uint16_t>latched_rtp_port_{0},rtp_sequence_{100};
 uint32_t rtp_ssrc_=1234;int rtp_socket_=-1;size_t udp_max_payload_=1200;AudioFormatList offer_rx_formats_;
 enum class MediaLifecyclePhase {IDLE,ACTIVE,CLEANING,SHUTTING_DOWN};
 std::atomic<MediaLifecyclePhase>media_lifecycle_phase_{MediaLifecyclePhase::ACTIVE};
 void*rtp_cleanup_done_=nullptr,*rtp_task_done_=nullptr;
 std::atomic<unsigned>admitted{0},changed{0};
 std::function<void(const AudioFormat&,const AudioFormat&,bool,bool)>on_config;
 void emit_audio_frame_(const uint8_t*p,size_t n,uint16_t,uint32_t,bool change,uint32_t){
  assert(n==320);for(unsigned i=0;i<n;++i)assert(p[i]==uint8_t(i*7));if(change)++changed;++admitted;}
 void emit_media_quiesced_(){}void finish_audio_path_stop_(){}
 bool received_audio_format_(uint8_t,AudioFormat*,const uint8_t* =nullptr,size_t=0)const;
 void send_audio_frame(const uint8_t*,size_t);void rtp_task_();
'''
    if 'std::atomic<bool> audio_send_enabled_' not in code:
        extra += 'std::atomic<bool> audio_send_enabled_{true},audio_receive_enabled_{true};\n'
    code = code.replace(' std::atomic<uint32_t> media_proposal_epoch_{0};', extra + '\n std::atomic<uint32_t> media_proposal_epoch_{0};')
    for fixture, production in [
        ('void set_media_config_(', 'void SipTransport::set_media_config_('),
        ('void get_media_config_(', 'void SipTransport::get_media_config_('),
        ('void reset_rtp_latch_()', 'void SipTransport::reset_rtp_latch_('),
    ]:
        body = block(owner, production)
        declaration = body[:body.index('{')].replace('SipTransport::', '').strip()
        if 'get_media_config' in production:
            declaration = declaration.replace('RtpAudioPayloads *rx_payloads)', 'RtpAudioPayloads *rx_payloads = nullptr)')
        code = code.replace(block(code, fixture), declaration + ';', 1)
        code += '\n' + body
    helpers = r'''
#define ESP_LOGE(...) ((void)0)
constexpr int pdTRUE=1;constexpr unsigned portMAX_DELAY=~0U;
unsigned ulTaskNotifyTake(int,unsigned){return 1;}void xSemaphoreGive(void*){}void vTaskDelete(void*){}
const char*socket_errno_name(int){return "host";}const char*socket_errno_text(int){return "host";}
'''
    code = code.replace('struct SipPhoneTransport {', helpers + '\nstruct SipPhoneTransport {')
    commit = block(code, 'void SipTransport::commit_audio_config_(')
    code = code.replace(commit, commit.replace('portEXIT_CRITICAL(&this->media_config_lock_);',
                                             'portEXIT_CRITICAL(&this->media_config_lock_);if(on_config)on_config(tx,rx,send,receive);'))
    for marker in ['size_t pcm_to_rtp_payload(', 'size_t rtp_payload_to_pcm(']:
        body = block(parser, marker)
        code = code.replace('struct SipPhoneTransport {', body + '\nstruct SipPhoneTransport {')
    for marker in ['bool SipTransport::received_audio_format_(', 'void SipTransport::send_audio_frame(', 'void SipTransport::rtp_task_(']:
        code += '\n' + block(owner, marker)
    return audio_tx_fixture(code) + r'''
uint16_t bind_udp(int fd){sockaddr_in p{};p.sin_family=AF_INET;p.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 assert(bind(fd,reinterpret_cast<sockaddr*>(&p),sizeof(p))==0);socklen_t n=sizeof(p);assert(getsockname(fd,reinterpret_cast<sockaddr*>(&p),&n)==0);return ntohs(p.sin_port);}
int main(int argc,char**argv){
 assert(argc==3);const std::string direction=argv[1],outcome=argv[2];SipTransport s;
 s.call_id_="original";s.local_tag_="local";s.remote_tag_="remote";s.local_uri_="<sip:room@127.0.0.1>";
 s.remote_uri_="<sip:pbx@127.0.0.1>";s.remote_ip_v4_=INADDR_LOOPBACK;s.remote_sip_port_=5060;
 AudioFormat pcm;pcm.sample_rate=16000;pcm.frame_ms=10;RtpAudioPayloads payloads;assert(payloads.add(96,pcm));
 s.set_media_config_(pcm,pcm,96,96,&payloads);s.offer_rx_formats_.formats[0]=pcm;
 const bool send=direction=="sendonly",receive=direction=="recvonly";
 s.audio_send_enabled_=send;s.audio_receive_enabled_=receive;
 s.rtp_socket_=socket(AF_INET,SOCK_DGRAM,0);const int peer=socket(AF_INET,SOCK_DGRAM,0);assert(s.rtp_socket_>=0&&peer>=0);
 const auto local_port=bind_udp(s.rtp_socket_),remote_port=bind_udp(peer);
 s.remote_rtp_ip_v4_=INADDR_LOOPBACK;s.remote_rtp_port_=remote_port;
 sockaddr_in target{};target.sin_family=AF_INET;target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);target.sin_port=htons(local_port);
 sockaddr_in sip{};sip.sin_addr.s_addr=htonl(INADDR_LOOPBACK);sip.sin_port=htons(5060);
 const std::string refer="REFER sip:room@127.0.0.1 SIP/2.0\r\nCall-ID: original\r\nFrom: <sip:pbx@127.0.0.1>;tag=remote\r\n"
  "To: <sip:room@127.0.0.1>;tag=local\r\nCSeq: 2 REFER\r\nRefer-To: <sip:other@127.0.0.2>\r\nContent-Length: 0\r\n\r\n";
 std::thread worker([&](){s.rtp_task_();});
 uint8_t samples[320];for(unsigned i=0;i<320;++i)samples[i]=uint8_t(i*7);
 uint16_t sequence=500;
 const auto wait=[&](auto predicate){const auto start=std::chrono::steady_clock::now();
  while(!predicate()&&std::chrono::steady_clock::now()-start<std::chrono::seconds(2))std::this_thread::sleep_for(std::chrono::milliseconds(1));assert(predicate());};
 const auto media=[&](bool tx,bool rx){
  s.send_audio_frame(samples,sizeof(samples));uint8_t packet[400];const auto got=recv(peer,packet,sizeof(packet),MSG_DONTWAIT);
  if(tx){assert(got==332&&(packet[1]&127)==96);for(unsigned i=0;i<320;i+=2)assert(packet[12+i]==samples[i+1]&&packet[13+i]==samples[i]);}
  else assert(got<0&&(errno==EAGAIN||errno==EWOULDBLOCK));
  uint8_t incoming[332]{};incoming[0]=0x80;incoming[1]=96;incoming[2]=sequence>>8;incoming[3]=sequence++;incoming[11]=42;
  const uint32_t stamp=sequence*160;incoming[4]=stamp>>24;incoming[5]=stamp>>16;incoming[6]=stamp>>8;incoming[7]=stamp;
  for(unsigned i=0;i<320;i+=2){incoming[12+i]=samples[i+1];incoming[13+i]=samples[i];}
  const auto before=s.admitted.load(),datagrams=s.rtp_rx_datagrams_.load();
  assert(sendto(peer,incoming,sizeof(incoming),0,reinterpret_cast<sockaddr*>(&target),sizeof(target))==sizeof(incoming));
  wait([&](){return s.rtp_rx_datagrams_>datagrams;});
  if(rx)wait([&](){return s.admitted>before;});else std::this_thread::sleep_for(std::chrono::milliseconds(5));
  assert(s.admitted==before+unsigned(rx));
 };
 media(send,receive);
 s.audio_receive_reset_pending_=true;s.rtp_source_reacquire_pending_=true;
 unsigned observations=0;
 s.on_config=[&](const auto&tx,const auto&rx,bool sending,bool receiving){
  assert((s.media_proposal_epoch_&1U)!=0&&"REFER must fence every configuration publication");
  assert(s.audio_send_enabled_==(sending&&tx.is_valid()));
  assert(s.audio_receive_enabled_==(receiving&&rx.is_valid()));
  media(false,false);++observations;
 };
 assert(s.handle_refer_(refer,sip));
 media(send,receive);
 assert(s.audio_send_enabled_==send&&s.audio_receive_enabled_==receive);
 if(!receive)assert(s.audio_receive_reset_pending_&&s.rtp_source_reacquire_pending_);
 if(receive)assert(!s.audio_receive_reset_pending_&&!s.rtp_source_reacquire_pending_&&s.changed==1);
 for(unsigned n=0;n<3;++n){s.pump_refer_();media(send,receive);}
 if(outcome!="pending"){
  {SipTransport::ReferScope scope(s);s.outgoing_invite_pending_=false;
   if(outcome=="completed"){
    s.set_media_config_(pcm,pcm,96,96,&payloads);s.remote_rtp_ip_v4_=INADDR_LOOPBACK;s.remote_rtp_port_=remote_port;
   }
   SipSignal result;result.type=outcome=="completed"?SipSignalType::STATUS_200_OK:SipSignalType::FINAL_RESPONSE;
   result.status_code=outcome=="completed"?200:486;s.emit_sip_signal_(result);}
  s.pump_refer_();
  const bool complete=outcome=="completed";
  assert(s.audio_send_enabled_==(complete||send)&&s.audio_receive_enabled_==(complete||receive));
  media(complete||send,complete||receive);
 }
 if(receive)assert(s.changed==1&&!s.audio_receive_reset_pending_&&!s.rtp_source_reacquire_pending_);
 assert(observations>=8);s.on_config={};
 s.rtp_task_terminate_=true;s.rtp_running_=false;uint8_t wake=0;assert(sendto(peer,&wake,1,0,reinterpret_cast<sockaddr*>(&target),sizeof(target))==1);worker.join();
 assert(s.rtp_task_quiesced_);close(s.rtp_socket_);close(peer);
}
'''


@pytest.fixture(scope='module')
def refer_audio_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp('refer-audio')
    cpp = folder / 'refer-audio.cpp'
    cpp.write_text(source())
    binary = folder / 'refer-audio'
    built = subprocess.run(['g++', '-std=c++17', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                            '-pthread', '-I', str(ROOT), str(cpp), '-o', str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize('direction', ['inactive', 'sendonly', 'recvonly'])
@pytest.mark.parametrize('outcome', ['pending', 'failed', 'completed'])
def test_refer_keeps_audio_direction_and_consumed_reset(refer_audio_binary, direction, outcome):
    run = subprocess.run([str(refer_audio_binary), direction, outcome], capture_output=True, text=True, timeout=8)
    assert run.returncode == 0, run.stdout + run.stderr
