"""Negotiated named events: bounded timing, masks, clocks and real UDP packets."""
import subprocess

import pytest

from test_refer_dialog_runtime import SOURCE, block


@pytest.fixture(scope="module")
def dtmf_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("dtmf-protocol")
    helper = folder / "esphome/core/helpers.h"
    helper.parent.mkdir(parents=True)
    helper.write_text('#pragma once\nnamespace esphome { using Mutex=std::mutex; using LockGuard=::LockGuard; }\n')
    cpp = folder / "dtmf.cpp"
    code = r'''
#include "rtp_dtmf.h"
#include "sip_types.h"
#include <array>
#include <atomic>
#include <cassert>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
using namespace esphome::voip_stack;
struct LockGuard{std::mutex&m;explicit LockGuard(std::mutex&v):m(v){m.lock();}~LockGuard(){m.unlock();}};
#include "rtp_jitter_buffer.h"
struct SipTransport {
 std::mutex rtp_socket_mutex_;int rtp_socket_=-1;
 std::atomic<bool>rtp_running_{true},media_active_{true};
 std::atomic<uint32_t>media_proposal_epoch_{8},remote_rtp_ip_v4_{INADDR_LOOPBACK};
 std::atomic<uint16_t>remote_rtp_port_{0},rtp_sequence_{100};
 std::atomic<uint32_t>rtp_timestamp_{0xFFFFFF00};size_t udp_max_payload_=1200;
 uint32_t rtp_ssrc_=1234;std::atomic<uint32_t>rtp_tx_packets_{0},rtp_tx_bytes_{0};
 uint8_t direction=3;uint8_t get_audio_direction_(){return direction;}
 bool send_dtmf_packet_(const DtmfSender::Report&,const DtmfFormat&,uint32_t,uint32_t,uint16_t*);
 void get_media_config_(AudioFormat*f,void*,uint8_t*pt,void*){if(f)*f=AudioFormat{};if(pt)*pt=96;}
 void send_audio_frame(const uint8_t*,size_t);
};
'''
    media = (SOURCE / "sip_message.cpp").read_text()
    code += block(media, "size_t pcm_to_rtp_payload(") + "\n"
    code += block(media, "size_t rtp_payload_to_pcm(") + "\n"
    code += block((SOURCE / "sip_dtmf.cpp").read_text(), "bool SipTransport::send_dtmf_packet_(")
    code += block((SOURCE / "sip_transport.cpp").read_text(), "void SipTransport::send_audio_frame(")
    code += r'''
int main(int argc,char**argv){
 assert(argc==2);const std::string name=argv[1];
 if(name=="mapping"){
  uint8_t ranks[128];std::memset(ranks,255,sizeof(ranks));ranks[98]=0;ranks[101]=1;
  auto fmt=negotiate_dtmf("m=audio 100 RTP/AVP 98 101\r\na=fmtp:98 0-11\r\na=rtpmap:101 telephone-event/8000\r\na=rtpmap:98 TELEPHONE-EVENT/16000/1\r\n",ranks,16000,false,0);
  assert(fmt.payload_type==98&&fmt.clock==16000&&fmt.events==0x0FFF);
  fmt=negotiate_dtmf("a=rtpmap:101 telephone-event/8000\r\n",ranks,48000,false,0);assert(fmt.valid()&&fmt.events==0xFFFF);
  assert(negotiate_dtmf("a=rtpmap:101 telephone-event/8000\r\n",ranks,48000,true,1).valid());
  assert(!negotiate_dtmf("a=rtpmap:101 telephone-event/8000\r\n",ranks,48000,true,0).valid());
  ranks[120]=0;assert(negotiate_dtmf("a=rtpmap:120 telephone-event/8000\r\n",ranks,48000,true,1).valid());return 0;
 }
 if(name=="invalid"){
  DtmfFormat fmt;for(auto line:{"a=rtpmap:0 telephone-event/8000","a=rtpmap:128 telephone-event/8000","a=rtpmap:101 telephone-event/8000/2","a=rtpmap:101 telephone-event/99999"})assert(!dtmf_rtpmap(line,&fmt));
  uint16_t mask;for(auto text:{"","0-15,","16-0","0-999","0;15","a","1--2"})assert(!dtmf_event_mask(text,&mask));
  assert(dtmf_event_mask("0-9,11,12-15",&mask)&&mask==0xFBFF);assert(dtmf_event('x')==-1&&dtmf_event('#')==11&&dtmf_event('d')==15);return 0;
 }
 if(name=="timing"||name=="wrap"||name=="stall"){
  const uint32_t base=name=="wrap"?0xFFFFFF00U:0;
  for(auto clock:DTMF_CLOCKS){DtmfSender tx;assert(tx.start(5,160,{clock,0xFFFF,101},123,8,base));DtmfSender::Report r{};
   assert(!tx.poll(base+19,&r));assert(tx.poll(base+20,&r)&&r.marker&&!r.end&&r.timestamp==123&&r.duration==clock/50);
   const uint32_t end=name=="stall"?base+1000:base+160;
   assert(tx.poll(end,&r)&&r.end&&!r.marker&&r.duration==clock*160/1000);
   assert(tx.poll(end+20,&r)&&r.end);assert(tx.poll(end+40,&r)&&r.end&&!tx.active());
   assert(!tx.ready(end+69)&&tx.ready(end+70));assert(!tx.poll(end+70,&r));
   tx.cancel();assert(tx.ready(0));assert(!tx.start(5,8000,{48000,0xFFFF,101},0,8,0));
  }return 0;
 }
 if(name=="receiver"){
  DtmfReceiver rx;assert(rx.accept(100,0xFFFFFF00,1,8));assert(!rx.accept(100,0xFFFFFF00,1,8));
  assert(rx.accept(200,10,2,8));assert(!rx.accept(100,0xFFFFFF00,1,8));
  assert(rx.accept(100,0x100,1,8));assert(!rx.accept(100,0xFFFFFF00,1,8));
  assert(rx.accept(100,0x100,2,8));assert(!rx.accept(100,0x100,2,8));
  assert(rx.accept(100,0,1,10));return 0;
 }
 if(name=="interleaved-same"||name=="interleaved-separate"){
  SipTransport a,b;a.rtp_socket_=socket(AF_INET,SOCK_DGRAM,0);b.rtp_socket_=socket(AF_INET,SOCK_DGRAM,0);b.rtp_sequence_=200;b.rtp_ssrc_=5678;
  auto bind_socket=[](int fd){sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);assert(bind(fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0);socklen_t n=sizeof(addr);assert(getsockname(fd,reinterpret_cast<sockaddr*>(&addr),&n)==0);return ntohs(addr.sin_port);};
  const auto ap=bind_socket(a.rtp_socket_),bp=bind_socket(b.rtp_socket_);a.remote_rtp_port_=bp;b.remote_rtp_port_=ap;
  uint8_t storage_a[512*16]{},storage_b[512*16]{},pcm[512]{},decoded[512]{},played[512]{};
  RtpJitterBuffer qa(storage_a,512,16,1),qb(storage_b,512,16,1);AudioFormat audio;
  std::vector<uint8_t> expected,received_a,received_b;unsigned audio_packets=0,events_a=0,events_b=0;
  uint16_t seq_a=100,seq_b=200;const bool separate=name=="interleaved-separate";
  auto read_audio=[&](int fd,RtpJitterBuffer&q,std::vector<uint8_t>&result,uint16_t&sequence,uint32_t ssrc){
   uint8_t bytes[1600]{};assert(recv(fd,bytes,sizeof(bytes),0)==524);uint16_t seq;std::memcpy(&seq,bytes+2,2);assert(ntohs(seq)==sequence++);
   uint32_t ts;std::memcpy(&ts,bytes+4,4);uint32_t got_ssrc;std::memcpy(&got_ssrc,bytes+8,4);assert(ntohl(got_ssrc)==ssrc&&(bytes[1]&127)==96);
   assert(rtp_payload_to_pcm(bytes+12,512,audio,decoded,sizeof(decoded))==512);RtpJitterBuffer::Frame frame;
   frame.pcm=decoded;frame.bytes=512;frame.sequence=ntohs(seq);frame.timestamp=ntohl(ts);frame.has_metadata=true;frame.format_packed=pack_audio_format(audio);assert(q.push(frame));
   assert(q.read(played,512)==RtpJitterBuffer::ReadResult::FRAME);result.insert(result.end(),played,played+512);
  };
  DtmfSender da,db;DtmfFormat event{separate?8000U:16000U,0xFFFF,109};uint16_t ea=500,eb=700;
  assert(da.start(5,160,event,separate?0xABC00000:a.rtp_timestamp_.load(),8,0));
  for(unsigned now=0;now<=1000;now+=4){
   if(now==64)assert(db.start(11,160,event,separate?0xDEF00000:b.rtp_timestamp_.load(),8,now));
   if(now%16==0){for(unsigned i=0;i<512;++i)pcm[i]=static_cast<uint8_t>(i*7+audio_packets);expected.insert(expected.end(),pcm,pcm+512);
    a.send_audio_frame(pcm,512);read_audio(b.rtp_socket_,qb,received_b,seq_a,1234);
    b.send_audio_frame(pcm,512);read_audio(a.rtp_socket_,qa,received_a,seq_b,5678);++audio_packets;
   }
   for(unsigned side=0;side<2;++side){auto&tx=side==0?a:b;auto&sender=side==0?da:db;int rx=side==0?b.rtp_socket_:a.rtp_socket_;auto&sequence=side==0?seq_a:seq_b;auto&counter=side==0?ea:eb;auto&ends=side==0?events_a:events_b;
    DtmfSender::Report report{};if(!sender.poll(now,&report))continue;
    assert(tx.send_dtmf_packet_(report,event,8,side==0?9876:8765,separate?&counter:nullptr));uint8_t bytes[32]{};assert(recv(rx,bytes,sizeof(bytes),0)==16);
    uint16_t wire_seq;std::memcpy(&wire_seq,bytes+2,2);if(!separate)assert(ntohs(wire_seq)==sequence++);
    if(bytes[13]&128)++ends;
   }
  }
  assert(audio_packets==63&&events_a==3&&events_b==3);
  assert(received_a==expected&&received_b==expected&&qa.counters().missing==0&&qb.counters().missing==0);
  close(a.rtp_socket_);close(b.rtp_socket_);return 0;
 }
 if(name=="wire"){
  SipTransport tx;tx.rtp_socket_=socket(AF_INET,SOCK_DGRAM,0);int peer=socket(AF_INET,SOCK_DGRAM,0);assert(peer>=0&&tx.rtp_socket_>=0);
  sockaddr_in local{};local.sin_family=AF_INET;local.sin_addr.s_addr=htonl(INADDR_LOOPBACK);assert(bind(peer,reinterpret_cast<sockaddr*>(&local),sizeof(local))==0);
  socklen_t len=sizeof(local);assert(getsockname(peer,reinterpret_cast<sockaddr*>(&local),&len)==0);tx.remote_rtp_port_=ntohs(local.sin_port);
  DtmfSender sender;assert(sender.start(11,160,{8000,0xFFFF,109},0xFFFFFF00,8,0));DtmfSender::Report report{};
  uint16_t separate=500;unsigned ends=0,packets=0;
  for(uint32_t now=0;now<=220;now+=20)if(sender.poll(now,&report)){
   assert(tx.send_dtmf_packet_(report,sender.format,8,9876,&separate));uint8_t bytes[32]{};assert(recv(peer,bytes,sizeof(bytes),0)==16);
   assert((bytes[1]&127)==109&&((bytes[1]&128)!=0)==(packets==0));assert(bytes[12]==11);
   uint32_t ts;std::memcpy(&ts,bytes+4,4);assert(ntohl(ts)==0xFFFFFF00);uint32_t ssrc;std::memcpy(&ssrc,bytes+8,4);assert(ntohl(ssrc)==9876);
   uint16_t seq;std::memcpy(&seq,bytes+2,2);assert(ntohs(seq)==500+packets);assert(tx.rtp_sequence_==100);
   if(bytes[13]&128){++ends;assert((unsigned(bytes[14])<<8|bytes[15])==1280);}++packets;
  }
  assert(ends==3&&packets==10);
  report={42,320,1,true,false};assert(tx.send_dtmf_packet_(report,{16000,0xFFFF,110},8,9999,nullptr));
  uint8_t bytes[32]{};assert(recv(peer,bytes,sizeof(bytes),0)==16);uint32_t ssrc;std::memcpy(&ssrc,bytes+8,4);assert(ntohl(ssrc)==1234&&tx.rtp_sequence_==101);
  tx.media_proposal_epoch_=10;assert(!tx.send_dtmf_packet_(report,{16000,0xFFFF,110},8,9999,nullptr));
  tx.media_proposal_epoch_=8;tx.direction=2;assert(!tx.send_dtmf_packet_(report,{16000,0xFFFF,110},8,9999,nullptr));
  close(peer);close(tx.rtp_socket_);return 0;
 }
 return 2;
}
'''
    cpp.write_text(code)
    binary = folder / "dtmf"
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation", "-g",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_ESP32", "-I", str(folder), "-I", str(SOURCE), str(cpp), "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("scenario", ["mapping", "invalid", "timing", "wrap", "stall", "wire", "receiver", "interleaved-same", "interleaved-separate"])
def test_named_events_keep_negotiated_clocks_and_bounded_lifetime(dtmf_binary, scenario):
    result = subprocess.run([dtmf_binary, scenario], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr
