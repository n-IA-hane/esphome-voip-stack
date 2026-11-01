"""Unchanged-audio proposals must not discard valid packets already on the wire."""
import subprocess

import pytest

from test_audio_hold_runtime import hold_source
from test_refer_dialog_runtime import ROOT, SOURCE, block


def proposal_audio_source():
    transport = (SOURCE / "sip_transport.cpp").read_text()
    parser = (SOURCE / "sip_message.cpp").read_text()
    prefix = r'''
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <future>
#include <atomic>
#include <cstdint>
#include <cassert>
#include <cstring>
#include <cstdio>
#define USE_ESP32
#include "esphome/components/voip_stack/rtp_jitter_buffer.h"
#undef USE_ESP32
#define pdTRUE 1
#define portMAX_DELAY 0xffffffff
#define ESP_LOGE(...) ((void)0)
void ulTaskNotifyTake(int,unsigned){}void xSemaphoreGive(void*){}void xTaskNotifyGive(void*){}void vTaskDelete(void*){}
struct Gates {
 std::mutex mutex;std::condition_variable changed;std::thread::id reader;
 int mode=0;bool staging=false,release_stage=false,even_read=false,release_reader=false,odd_read=false,changed_read=false,last_emitted=false;
 std::atomic<unsigned>emitted{0};
}g;
struct ObservedEpoch:std::atomic<uint32_t> {
 using std::atomic<uint32_t>::atomic;
 uint32_t load(std::memory_order order=std::memory_order_seq_cst)const{
  const uint32_t value=std::atomic<uint32_t>::load(order);
  std::unique_lock lock(g.mutex);
  if(std::this_thread::get_id()!=g.reader)return value;
  if(g.mode==2&&value==0&&!g.even_read){g.even_read=true;g.changed.notify_all();g.changed.wait(lock,[]{return g.release_reader;});}
 if(g.mode==1&&g.staging){g.odd_read=true;g.changed.notify_all();}
 if(g.mode==2&&g.even_read&&g.release_reader){g.changed_read=true;g.changed.notify_all();}
  return value;
 }
};
void stage_network_adapter(const std::atomic<uint32_t>&epoch){
 (void)epoch;
 std::unique_lock lock(g.mutex);if(!g.mode)return;g.staging=true;g.changed.notify_all();g.changed.wait(lock,[]{return g.release_stage;});
}
template<class Predicate>void wait_gate(Predicate predicate){std::unique_lock lock(g.mutex);assert(g.changed.wait_for(lock,std::chrono::seconds(3),predicate));}
'''
    code = hold_source()
    code = code.replace("#define portENTER_CRITICAL(...) ((void)0)", "#define portENTER_CRITICAL(m) (m)->lock()")
    code = code.replace("#define portEXIT_CRITICAL(...) ((void)0)", "#define portEXIT_CRITICAL(m) (m)->unlock()")
    code = code.replace("int media_config_lock_=0;", "mutable std::recursive_mutex media_config_lock_;")
    code = code.replace("std::atomic<uint32_t> media_proposal_epoch_{0};", "ObservedEpoch media_proposal_epoch_{0};")
    old = block(code, "void get_media_config_(")
    actual = block(transport, "void SipTransport::get_media_config_(")
    declaration = actual[:actual.index("{")].replace("SipTransport::", "").strip().replace("RtpAudioPayloads *rx_payloads)", "RtpAudioPayloads *rx_payloads=nullptr)")
    code = code.replace(old, declaration + ";") + "\n" + actual
    old = block(code, "void local_ip_for_peer_(")
    code = code.replace(old, 'void local_ip_for_peer_(uint32_t,std::string*out)const{stage_network_adapter(this->media_proposal_epoch_);*out="127.0.0.1";}')
    declarations = r'''
 std::atomic<bool>rtp_task_terminate_{false},rtp_task_quiesced_{false},rtp_ssrc_latched_{false};
 std::atomic<uint32_t>rtp_rx_datagrams_{0},rtp_rx_packets_{0},rtp_rx_bytes_{0},latched_rtp_ip_v4_{0},latched_rtp_ssrc_{0};
 std::atomic<uint16_t>latched_rtp_port_{0};void*rtp_task_done_=nullptr,*rtp_cleanup_done_=nullptr,*rtp_task_handle_=this;
 RtpJitterBuffer*jitter=nullptr;std::vector<uint16_t>accepted_sequences;
 void rtp_task_();void wake_rtp_task_();void finish_audio_path_stop_(){assert(false);}void emit_media_quiesced_(){}
 void emit_audio_frame_(const uint8_t*p,size_t n,uint16_t seq,uint32_t ts,bool changed,uint32_t format){
  assert(n==320&&p[0]==0xd0&&p[1]==7);assert(jitter->push({p,n,seq,ts,true,changed,format}));accepted_sequences.push_back(seq);g.emitted.fetch_add(1);{std::lock_guard lock(g.mutex);if(seq==1003)g.last_emitted=true;}g.changed.notify_all();
 }
'''
    code = code.replace(" uint16_t sip_port_=5060;", declarations + "\n uint16_t sip_port_=5060;", 1)
    code += "\n" + block(parser, "size_t rtp_payload_to_pcm(")
    code += "\n" + block(transport, "void SipTransport::rtp_task_(")
    code += "\n" + block(transport, "void SipTransport::wake_rtp_task_(")
    fixture = (ROOT / "tests/fixtures/refer_wire_main.cpp").read_text().split("void auth_case(")[0]
    main = r'''
int main(int argc,char**argv){
 const std::string mode=argv[1];SipTransport t;Peer signaling(false),audio(false);setup(t,signaling);t.media_lifecycle_phase_=SipTransport::MediaLifecyclePhase::ACTIVE;
 AudioFormat format;format.frame_ms=10;RtpAudioPayloads mappings;assert(mappings.add(96,format));t.offer_tx_formats_.count=t.offer_rx_formats_.count=1;t.offer_tx_formats_.formats[0]=t.offer_rx_formats_.formats[0]=format;t.set_media_config_(format,format,96,96,&mappings);
 t.remote_rtp_ip_v4_=INADDR_LOOPBACK;t.remote_rtp_port_=ntohs(audio.address.sin_port);
 t.rtp_socket_=socket(AF_INET,SOCK_DGRAM,0);assert(t.rtp_socket_>=0);sockaddr_in receive{};receive.sin_family=AF_INET;receive.sin_addr.s_addr=htonl(INADDR_LOOPBACK);assert(bind(t.rtp_socket_,reinterpret_cast<sockaddr*>(&receive),sizeof(receive))==0);socklen_t length=sizeof(receive);assert(getsockname(t.rtp_socket_,reinterpret_cast<sockaddr*>(&receive),&length)==0);t.rtp_port_=ntohs(receive.sin_port);
 uint8_t storage[16*320]{};RtpJitterBuffer jitter(storage,320,16,0);t.jitter=&jitter;
 std::promise<void>ready;std::thread receiver([&]{{std::lock_guard lock(g.mutex);g.reader=std::this_thread::get_id();}ready.set_value();t.rtp_task_();});ready.get_future().wait();
 auto datagram=[&](const std::vector<uint8_t>&bytes){assert(sendto(audio.fd,bytes.data(),bytes.size(),0,reinterpret_cast<sockaddr*>(&receive),sizeof(receive))==static_cast<ssize_t>(bytes.size()));};
 auto packet=[&](uint16_t seq){std::vector<uint8_t>p(332);p[0]=0x80;p[1]=96;p[2]=seq>>8;p[3]=seq;const uint32_t ts=seq*160u;for(int i=0;i<4;i++)p[4+i]=ts>>(24-8*i);p[8]=0x12;p[9]=0x34;p[10]=0x56;p[11]=0x78;for(int i=12;i<332;i+=2){p[i]=7;p[i+1]=0xd0;}return p;};
 datagram(packet(1000));wait_gate([]{return g.emitted.load()==1;});
 // Deliberate wake, RTCP and unknown RTP are counted before validation but
 // cannot create a missing AUDIO sequence. They disprove deficit==loss alone.
 datagram({1});std::vector<uint8_t>rtcp(28);rtcp[0]=0x80;rtcp[1]=200;rtcp[3]=6;datagram(rtcp);auto keepalive=packet(999);keepalive[1]=127;datagram(keepalive);
 datagram(packet(1001));wait_gate([]{return g.emitted.load()==2;});
 {std::lock_guard lock(g.mutex);g.mode=mode=="odd"?1:mode=="changed"?2:0;}
 if(mode=="changed"){datagram(packet(1002));wait_gate([]{return g.even_read;});}
 const auto old_format=t.selected_rx_format_;const auto old_port=t.remote_rtp_port_.load();const auto old_call=t.call_id_;
 std::string body="v=0\r\no=peer 1 2 IN IP4 127.0.0.1\r\ns=test\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio "+std::to_string(old_port)+" RTP/AVP 96\r\na=rtpmap:96 L16/16000/1\r\na=ptime:10\r\na=sendrecv\r\nm=video 55000 RTP/AVP 26\r\na=rtpmap:26 JPEG/90000\r\na=framerate:10\r\na=sendrecv\r\n";
 auto offer=request(signaling,"INVITE",10,"",t.call_id_,t.local_tag_,t.remote_tag_);auto at=offer.find("Content-Length: 0");offer.replace(at,17,"Content-Type: application/sdp\r\nContent-Length: "+std::to_string(body.size()));offer+=body;
 std::thread proposal([&]{assert(t.handle_reinvite_(offer,signaling.address));});
 if(mode!="control"){
  wait_gate([]{return g.staging;});
  if(mode=="odd"){datagram(packet(1002));wait_gate([]{return g.odd_read;});}
  {std::lock_guard lock(g.mutex);g.release_stage=true;}g.changed.notify_all();
 }
 proposal.join();auto replies=signaling.drain();assert(replies.size()==1&&replies[0].rfind("SIP/2.0 200",0)==0&&replies[0].find("m=video 0")!=std::string::npos);
 assert(t.selected_rx_format_==old_format&&t.remote_rtp_port_==old_port&&t.call_id_==old_call&&t.media_active_);
 if(mode=="changed"){{std::lock_guard lock(g.mutex);g.release_reader=true;}g.changed.notify_all();wait_gate([]{return g.changed_read;});}
 if(mode=="control")datagram(packet(1002));
 datagram(packet(1003));wait_gate([]{return g.last_emitted;});
 t.rtp_task_terminate_=true;t.rtp_running_=false;t.wake_rtp_task_();receiver.join();
 uint8_t out[320];for(unsigned i=0;i<5;i++)jitter.read(out,sizeof(out));auto counters=jitter.counters();
 const unsigned raw=t.rtp_rx_datagrams_.load();
 printf("{\"mode\":\"%s\",\"udp\":%u,\"accepted\":%u,\"known_non_audio\":%u,\"jitter_missing\":%u,\"SIP200_audio_unchanged\":true}\n",mode.c_str(),raw,t.rtp_rx_packets_.load(),3+(raw-7),counters.missing);fflush(stdout);
 close(t.rtp_socket_);close(t.sip_socket_);
 // The final stop wake may remain queued if the loop sees terminate first.
 assert(raw==7||raw==8);assert(t.rtp_rx_packets_==4&&"Valid old audio must survive unchanged-audio proposal staging");assert(counters.missing==0);
}
'''
    return prefix + code + "\n" + fixture + main


def compile_proposal(folder, source):
    helpers = folder / "esphome/core/helpers.h"
    helpers.parent.mkdir(parents=True)
    helpers.write_text("#pragma once\n#include <mutex>\nnamespace esphome {using Mutex=std::mutex; struct LockGuard {std::lock_guard<Mutex> guard; explicit LockGuard(Mutex&m):guard(m){}};}\n")
    (helpers.parent / "defines.h").write_text("#pragma once\n")
    cpp = folder / "proposal.cpp"
    cpp.write_text(source)
    binary = folder / "proposal"
    result = subprocess.run(["g++", "-std=c++17", "-pthread", "-g0", "-fsanitize=address,undefined", "-no-pie", "-I", str(folder), "-I", str(ROOT), str(cpp), "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.fixture(scope="module")
def proposal_audio_binary(tmp_path_factory):
    return compile_proposal(tmp_path_factory.mktemp("proposal-audio"), proposal_audio_source())


@pytest.mark.parametrize("mode", ["control", "odd", "changed"])
def test_reoffer_preserves_audio_already_admitted_by_the_established_contract(proposal_audio_binary, mode):
    result = subprocess.run([proposal_audio_binary, mode], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
