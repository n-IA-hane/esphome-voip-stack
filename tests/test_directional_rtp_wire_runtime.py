"""Final SDP selection drives actual PCM RTP bytes, payload IDs and timestamps."""
import subprocess

import pytest

from test_refer_dialog_runtime import ROOT, SOURCE, block
from test_sdp_receive_payloads import sdp_fixture_source
from audio_tx_fixture import audio_tx_fixture


@pytest.fixture(scope="module")
def media_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("directional-wire")
    code = sdp_fixture_source()
    additions = r'''
#include <array>
#include <vector>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <iostream>
struct Mutex{};struct LockGuard{explicit LockGuard(Mutex&){}};
'''
    code = additions + code
    code = code.replace("struct SipTransport {", r'''struct SipTransport {
 std::atomic<bool>rtp_running_{true};std::atomic<uint32_t>media_proposal_epoch_{0};
 std::atomic<uint16_t>rtp_sequence_{100};std::atomic<uint32_t>rtp_timestamp_{1000};uint32_t rtp_ssrc_=0x12345678;
 std::atomic<uint32_t>rtp_tx_packets_{0},rtp_tx_bytes_{0};int rtp_socket_=-1;Mutex rtp_socket_mutex_;
 void send_audio_frame(const uint8_t*,size_t);
''', 1)
    parser = (SOURCE / "sip_message.cpp").read_text()
    for marker in ["size_t pcm_to_rtp_payload(", "size_t rtp_payload_to_pcm("]:
        body = block(parser, marker)
        code = code.replace("struct SipTransport {", body + "\nstruct SipTransport {", 1)
    code += "\n" + block((SOURCE / "sip_transport.cpp").read_text(), "void SipTransport::send_audio_frame(")
    code = audio_tx_fixture(code)
    code += r'''
AudioFormat fmt(unsigned rate,unsigned frame){AudioFormat f;f.sample_rate=rate;f.frame_ms=frame;return f;}
int endpoint(uint16_t *port){
 int fd=socket(AF_INET,SOCK_DGRAM,0);assert(fd>=0);sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 assert(bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0);socklen_t n=sizeof(a);assert(getsockname(fd,reinterpret_cast<sockaddr*>(&a),&n)==0);*port=ntohs(a.sin_port);
 timeval timeout{1,0};assert(setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))==0);return fd;
}
uint32_t stamp(const uint8_t*p){return uint32_t(p[4])<<24|uint32_t(p[5])<<16|uint32_t(p[6])<<8|p[7];}
std::vector<uint8_t> samples(unsigned count,bool network){
 std::vector<uint8_t> bytes(count*2);
 for(unsigned n=0;n<count;++n){const uint16_t value=uint16_t(int(n)*101-16000);bytes[2*n+(network?1:0)]=value;bytes[2*n+(network?0:1)]=value>>8;}
 return bytes;
}
int main(int argc,char**argv){
 assert(argc==2);const std::string profile=argv[1];SipTransport s;unsigned tx=16000,rx=16000,frame=10;
 if(profile=="ha"||profile=="ha-asymmetric"||profile=="ha-legacy"){rx=48000;s.peer_directional_audio_v1_=true;}
 else if(profile=="ha-reverse"){tx=48000;s.peer_directional_audio_v1_=true;}
 else if(profile=="standard")frame=20;
 else if(profile=="legacy"||profile=="legacy-no-tx"){}
 else {assert(profile=="s3");AudioFormat baseline;assert(baseline.frame_ms==16);frame=baseline.frame_ms;}
 s.offer_tx_formats_.count=s.offer_rx_formats_.count=1;
 s.offer_tx_formats_.formats[0]=fmt(tx,frame);s.offer_rx_formats_.formats[0]=fmt(rx,frame);
 if(profile=="legacy"||profile=="legacy-no-tx"){s.offer_rx_formats_.count=2;s.offer_rx_formats_.formats[0]=fmt(48000,10);s.offer_rx_formats_.formats[1]=fmt(16000,10);}
 if(profile=="standard"){s.offer_tx_formats_.count=2;s.offer_tx_formats_.formats[0]=fmt(tx,10);s.offer_tx_formats_.formats[1]=fmt(tx,20);}
 const unsigned tx_frame=profile=="ha-asymmetric"?20:frame,rx_frame=frame;
 if(profile=="ha-asymmetric"||profile=="ha-legacy"){
  s.offer_tx_formats_.count=2;s.offer_tx_formats_.formats[0]=fmt(16000,20);s.offer_tx_formats_.formats[1]=fmt(16000,10);
  s.offer_rx_formats_.count=3;s.offer_rx_formats_.formats[0]=fmt(48000,10);
  s.offer_rx_formats_.formats[1]=fmt(16000,20);s.offer_rx_formats_.formats[2]=fmt(16000,10);
 }
 uint16_t local=0,remote=0;s.rtp_socket_=endpoint(&local);int peer=endpoint(&remote);s.rtp_port_=local;
 auto offer=s.build_sdp_offer_();assert(!offer.empty());
 assert(offer.find("a=x-voip-stack-independent-ptime:1\r\n")!=std::string::npos);
 const std::string base="v=0\r\nc=IN IP4 127.0.0.1\r\nm=audio "+std::to_string(remote)+" RTP/AVP 96";
 std::string answer=base+(tx!=rx?" 97":"")+"\r\na=rtpmap:96 L16/"+std::to_string(rx)+"/1\r\n";
 if(tx!=rx)answer+="a=rtpmap:97 L16/"+std::to_string(tx)+"/1\r\na=x-voip-stack-flow:96 send\r\na=x-voip-stack-flow:97 recv\r\n";
 answer+="a=ptime:"+std::to_string(tx_frame)+"\r\na=sendrecv\r\n";
 if(profile=="ha-asymmetric")answer+="a=x-voip-stack-independent-ptime:1\r\n";
 std::cout<<"PROFILE "<<profile<<"\nOFFER\n"<<offer<<"ANSWER\n"<<answer;
 if(profile=="legacy"||profile=="legacy-no-tx"){
  assert(offer.find("a=x-voip-stack-flow:")==std::string::npos);
  assert(offer.find("L16/48000/1")==std::string::npos&&offer.find("L16/16000/1")!=std::string::npos);
  assert(answer.find("L16/48000/1")==std::string::npos);
 }
 if(profile=="legacy-no-tx"){
  auto incompatible=answer;auto map=incompatible.find("L16/16000/1");incompatible.replace(map,11,"L16/48000/1");
  const auto before_tx=s.tx,before_rx=s.rx;const auto before_pt=s.tx_pt;const auto before_maps=s.accepted;
  assert(!s.learn_remote_rtp_from_sdp_(incompatible,INADDR_LOOPBACK,true));
  assert(s.tx==before_tx&&s.rx==before_rx&&s.tx_pt==before_pt&&s.accepted==before_maps&&s.rtp_tx_packets_==0);
  char byte;assert(recv(peer,&byte,1,MSG_DONTWAIT)<0);
  std::cout<<"REJECTED_NO_TX\n"<<incompatible<<"FALLBACK_TO_COMMON_TX\n";
 }
 assert(s.learn_remote_rtp_from_sdp_(answer,INADDR_LOOPBACK,true));
 assert(s.tx.sample_rate==tx&&s.rx.sample_rate==rx&&s.tx.frame_ms==tx_frame&&s.rx.frame_ms==rx_frame);
 std::cout<<"SELECTED tx="<<s.tx.sample_rate<<" rx="<<s.rx.sample_rate<<" tx_pt="<<unsigned(s.tx_pt)<<" rx_pt="<<unsigned(s.rx_pt)<<" frame_ms="<<frame<<"\n";
 const auto pcm=samples(tx*tx_frame/1000,false),network=samples(tx*tx_frame/1000,true);
 std::array<uint8_t,1500> packet{};uint32_t first=0;
 for(unsigned n=0;n<2;++n){
  s.send_audio_frame(pcm.data(),pcm.size());const auto length=recv(peer,packet.data(),packet.size(),0);
  assert(length==ssize_t(12+network.size())&&packet[0]==0x80&&packet[1]==s.tx_pt);
  assert(std::memcmp(packet.data()+12,network.data(),network.size())==0);
  std::cout<<"TX_RTP payload_bytes="<<length-12<<" pt="<<unsigned(packet[1])<<" timestamp="<<stamp(packet.data())<<"\n";
  if(n==0)first=stamp(packet.data());else assert(stamp(packet.data())-first==tx*tx_frame/1000);
 }
 assert(s.rtp_tx_packets_==2);
 // An independent peer authors the receive packet, including negotiated PT.
 const auto incoming=samples(rx*rx_frame/1000,true),expected=samples(rx*rx_frame/1000,false);
 std::vector<uint8_t> authored(12+incoming.size());authored[0]=0x80;authored[1]=s.rx_pt;
 std::copy(incoming.begin(),incoming.end(),authored.begin()+12);sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(local);
 assert(sendto(peer,authored.data(),authored.size(),0,reinterpret_cast<sockaddr*>(&address),sizeof(address))==ssize_t(authored.size()));
 const auto count=recv(s.rtp_socket_,packet.data(),packet.size(),0);assert(count==ssize_t(authored.size()));
 AudioFormat received;assert(s.accepted.find(packet[1]&0x7f,&received)&&received.sample_rate==rx);
 std::vector<uint8_t> decoded(expected.size());assert(rtp_payload_to_pcm(packet.data()+12,count-12,received,decoded.data(),decoded.size())==expected.size());
 assert(decoded==expected);std::cout<<"RX_RTP payload_bytes="<<count-12<<" pt="<<unsigned(packet[1])<<" decoded_bytes="<<decoded.size()<<"\n";close(peer);close(s.rtp_socket_);
}
'''
    cpp = folder / "media.cpp"
    cpp.write_text(code)
    exe = folder / "media"
    built = subprocess.run(["g++", "-std=c++17", "-g", "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-I", str(ROOT), str(cpp), "-o", str(exe)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    return exe


@pytest.mark.parametrize("profile", ["ha", "ha-asymmetric", "ha-legacy", "ha-reverse", "standard", "s3", "legacy", "legacy-no-tx"])
def test_selected_native_media_bytes_and_clock(media_binary, profile):
    result = subprocess.run([media_binary, profile], capture_output=True, text=True, timeout=6)
    assert result.returncode == 0, result.stderr
