"""The original offer owns one kernel RTP socket through final preparation."""

from pathlib import Path
import subprocess

import pytest

from test_refer_dialog_runtime import SOURCE, block
from test_refer_wire_runtime import wire_source


@pytest.fixture(scope="module")
def reservation_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("rtp-offer-reservation")
    source = (SOURCE / "sip_transport.cpp").read_text()
    code = wire_source()
    code = code.replace("#define ESP_LOGI", "#define ESP_LOGE(...) ((void)0)\n#define ESP_LOGI", 1)
    code = code.replace("enum class MediaLifecyclePhase {IDLE}",
                        "enum class MediaLifecyclePhase {IDLE,PREPARED,ACTIVE,CLEANING,SHUTTING_DOWN}")
    stub = "bool bind_udp_(int*fd,uint16_t,const char*){*fd=0;return true;}"
    assert stub in code
    code = code.replace(stub, "bool bind_udp_(int*,uint16_t,const char*);")
    stop = "void request_audio_path_stop_locked_(){media_active_=false;rtp_socket_=-1;}"
    assert stop in code
    code = code.replace(stop, "void request_audio_path_stop_locked_();")
    additions = r'''
 static constexpr unsigned kRtpSocketRxBufferBytes=65536;
 std::atomic<bool>rtp_running_{false},rtp_task_terminate_{false},rtp_task_quiesced_{true};
 std::atomic<uint16_t>rtp_sequence_{0};std::atomic<uint32_t>rtp_timestamp_{0};uint32_t rtp_ssrc_=0;
 void *rtp_task_handle_=this,*rtp_cleanup_done_=this;
 struct Converter {
  unsigned prepares=0;uint32_t last_rate=0;
  bool prepare(const RtpAudioPayloads&,const AudioFormat&format){++prepares;last_rate=format.sample_rate;return true;}
  bool prepare_additional(const RtpAudioPayloads&,const AudioFormat&){assert(false);return false;}
 }pcm_receive_converter_;
 bool prepare_media_path_locked_();bool commit_media_path_locked_();void finish_audio_path_stop_();
 void drain_cleanup(){
  assert(media_lifecycle_phase_==MediaLifecyclePhase::CLEANING);
  finish_audio_path_stop_();media_lifecycle_phase_=MediaLifecyclePhase::IDLE;rtp_task_quiesced_=true;
 }
'''
    code = code.replace("struct SipTransport: SipPhoneTransport {",
                        "struct SipTransport: SipPhoneTransport {\n" + additions, 1)
    code = code.replace("Mutex tcp_send_mutex_,tcp_tx_pending_mutex_,dialog_mutex_,media_lifecycle_mutex_;",
                        "Mutex tcp_send_mutex_,tcp_tx_pending_mutex_,dialog_mutex_,media_lifecycle_mutex_,rtp_socket_mutex_;")
    code = code.replace("constexpr uint32_t SIP_T1_MS=500;", """
int xSemaphoreTake(void*,int){return 1;}
unsigned notifications=0;void xTaskNotifyGive(void*){++notifications;}
ssize_t observed_send(int,const void*,size_t,int);
ssize_t observed_sendto(int,const void*,size_t,int,const sockaddr*,socklen_t);
#define send observed_send
#define sendto observed_sendto
constexpr uint32_t SIP_T1_MS=500;""", 1)
    old = block(code, "std::string build_sdp_offer_()")
    code = code.replace(old, r'''std::string build_sdp_offer_(){return
      "v=0\r\nm=audio "+std::to_string(rtp_port_)+" RTP/AVP 96 97\r\n"+
      "a=rtpmap:96 L16/48000/1\r\na=rtpmap:97 L16/16000/1\r\na=ptime:10\r\n";}''')
    for marker in ["bool SipTransport::bind_udp_(", "bool SipTransport::prepare_media_path_locked_(",
                   "bool SipTransport::commit_media_path_locked_(",
                   "void SipTransport::request_audio_path_stop_locked_(", "void SipTransport::finish_audio_path_stop_("]:
        code += "\n" + block(source, marker)
    peers = (Path(__file__).parent / "fixtures/refer_wire_main.cpp").read_text()
    code += "\n" + peers[:peers.index("std::string begin(")]
    code += r'''
#undef send
#undef sendto
SipTransport *observed=nullptr;unsigned offers=0;bool fail_offer=false;
uint16_t socket_port(int fd){sockaddr_in a{};socklen_t n=sizeof(a);assert(getsockname(fd,reinterpret_cast<sockaddr*>(&a),&n)==0);return ntohs(a.sin_port);}
void check_offer_boundary(const void *data,size_t size){
 if(size<7||memcmp(data,"INVITE ",7)!=0)return;
 assert(observed&&observed->rtp_socket_>=0&&"RTP port must be reserved before transmitting the original INVITE");
 assert(socket_port(observed->rtp_socket_)==observed->rtp_port_);
 assert(observed->media_lifecycle_phase_==SipTransport::MediaLifecyclePhase::IDLE&&!observed->rtp_running_);
 assert(observed->pcm_receive_converter_.prepares==0&&"No decoder is prepared before the SDP answer");++offers;
}
bool reject_offer(const void*p,size_t n){if(fail_offer&&n>=7&&!memcmp(p,"INVITE ",7)){errno=EIO;return true;}return false;}
ssize_t observed_send(int fd,const void *p,size_t n,int flags){check_offer_boundary(p,n);if(reject_offer(p,n))return -1;return ::send(fd,p,n,flags);}
ssize_t observed_sendto(int fd,const void *p,size_t n,int flags,const sockaddr *a,socklen_t l){check_offer_boundary(p,n);if(reject_offer(p,n))return -1;return ::sendto(fd,p,n,flags,a,l);}
int bind_exclusive(uint16_t port){
 int fd=socket(AF_INET,SOCK_DGRAM,0);assert(fd>=0);sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_ANY);a.sin_port=htons(port);
 if(bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))<0){const auto error=errno;close(fd);errno=error;return -1;}return fd;
}
void cleaned(SipTransport &s,int original){
 assert(s.media_lifecycle_phase_==SipTransport::MediaLifecyclePhase::CLEANING);
 // Model only the existing persistent worker's deferred cleanup scheduling.
 s.drain_cleanup();assert(s.rtp_socket_<0&&fcntl(original,F_GETFD)<0&&errno==EBADF);
 int probe=bind_exclusive(s.rtp_port_);assert(probe>=0&&"Abort must release the exact port without SO_REUSEADDR");close(probe);
}
void released(SipTransport &s,int original){
 const auto before=notifications;s.request_audio_path_stop_locked_();
 assert(s.media_lifecycle_phase_==SipTransport::MediaLifecyclePhase::CLEANING&&notifications==before+1);cleaned(s,original);
}
int main(int argc,char **argv){
 assert(argc==3);const std::string scenario=argv[1];Peer peer(std::string(argv[2])=="tcp");SipTransport s;setup(s,peer);
 s.call_id_.clear();s.media_active_=false;observed=&s;
 int probe=bind_exclusive(0);assert(probe>=0);s.rtp_port_=socket_port(probe);close(probe);
 const unsigned repeats=scenario=="repeat"?10:1;
 for(unsigned i=0;i<repeats;++i){
  s.pcm_receive_converter_.prepares=0;const auto before=offers;
  if(scenario=="bind-failure"){
   int occupied=bind_exclusive(s.rtp_port_);assert(occupied>=0);
   assert(!s.send_invite_unlocked_("unbound","esp","ESP","peer","Peer"));
   assert(s.rtp_socket_<0&&peer.drain().empty()&&offers==before);
   assert(s.media_lifecycle_phase_==SipTransport::MediaLifecyclePhase::IDLE);
   close(occupied);probe=bind_exclusive(s.rtp_port_);assert(probe>=0);close(probe);return 0;
  }
  if(scenario=="send-failure"){
   fail_offer=true;const bool sent=s.send_invite_unlocked_("unsent","esp","ESP","peer","Peer");
   if(peer.tcp)assert(!sent);
   else{
    // UDP keeps the original transaction eligible for retransmission. Its
    // actual timeout, rather than an initial send error, releases the port.
    assert(sent&&!s.pending_invite_.empty()&&s.rtp_socket_>=0);
    clock_ms=s.pending_invite_.deadline_ms;s.pump_udp_retransmits_unlocked_();
   }
   assert(offers==before+1&&peer.drain().empty()&&s.call_id_.empty());
   cleaned(s,s.rtp_socket_);return 0;
  }
  assert(s.send_invite_unlocked_("offer-"+std::to_string(i),"esp","ESP","peer","Peer"));
  auto wire=peer.drain();assert(wire.size()==1&&wire[0].rfind("INVITE ",0)==0&&offers==before+1);
  assert(wire[0].find("m=audio "+std::to_string(s.rtp_port_)+" ")!=std::string::npos);
  const int original=s.rtp_socket_;assert(original>=0);
  int tos=0;socklen_t tos_size=sizeof(tos);assert(getsockopt(original,IPPROTO_IP,IP_TOS,&tos,&tos_size)==0&&tos==0xB8);
  assert(bind_exclusive(s.rtp_port_)<0&&errno==EADDRINUSE);
  if(scenario=="decline"||scenario=="cancel"){
   if(scenario=="cancel"){
    assert(s.send_cancel_unlocked_(s.call_id_));auto cancel=peer.drain();assert(cancel.size()==1&&cancel[0].rfind("CANCEL ",0)==0);
    peer.send_to(s,response(cancel[0],200));assert(peer.drain().empty());
   }
   peer.send_to(s,response(wire[0],scenario=="cancel"?487:486));
   auto ack=peer.drain();assert(ack.size()==1&&ack[0].rfind("ACK ",0)==0&&s.call_id_.empty());
   cleaned(s,original);return 0;
  }
  if(scenario=="overlapping-offer"){
   const auto owner=s.call_id_;assert(!s.send_invite_unlocked_("intruder","esp","ESP","peer","Peer"));
   assert(s.rtp_socket_==original&&s.call_id_==owner&&offers==before+1&&peer.drain().empty());
   released(s,original);return 0;
  }
  if(scenario=="early-data"){
   // Model the application's offered 48 kHz preference before its final
   // receive geometry is selected. This must not prepare a decoder yet.
   s.selected_rx_format_.sample_rate=48000;
   const std::string answer_sdp="v=0\r\nc=IN IP4 127.0.0.1\r\nm=audio 41000 RTP/AVP 97\r\na=rtpmap:97 L16/16000/1\r\na=ptime:10\r\n";
   auto answer=response(wire[0],200);answer.replace(answer.find("Content-Length: 0"),17,"Content-Type: application/sdp\r\nContent-Length: "+std::to_string(answer_sdp.size()));
   peer.send_to(s,answer+answer_sdp);auto acks=peer.drain();assert(acks.size()==1&&acks[0].rfind("ACK ",0)==0);
   assert(s.rtp_socket_==original&&s.pcm_receive_converter_.prepares==0&&!s.rtp_running_);
   int sender=socket(AF_INET,SOCK_DGRAM,0);assert(sender>=0);sockaddr_in dst{};dst.sin_family=AF_INET;dst.sin_addr.s_addr=htonl(INADDR_LOOPBACK);dst.sin_port=htons(s.rtp_port_);
   std::vector<std::array<uint8_t,332>> original_packets;
   for(uint16_t seq=60506;seq<=60510;++seq){std::array<uint8_t,332>packet{};packet[0]=0x80;packet[1]=97;packet[2]=seq>>8;packet[3]=seq;
    const uint32_t stamp=(seq-60506)*160;packet[4]=stamp>>24;packet[5]=stamp>>16;packet[6]=stamp>>8;packet[7]=stamp;
    packet[8]=0x12;packet[9]=0x34;packet[10]=0x56;packet[11]=0x78;
    for(unsigned n=0;n<160;++n){const uint16_t sample=uint16_t(int(seq-60506)*1000+int(n)-80);packet[12+n*2]=sample>>8;packet[13+n*2]=sample;}
    original_packets.push_back(packet);
    assert(sendto(sender,packet.data(),packet.size(),0,reinterpret_cast<sockaddr*>(&dst),sizeof(dst))==ssize_t(packet.size()));}
   // The speaker's preferred 48 kHz format can change to the selected 16 kHz
   // without recreating or draining the transport's reserved UDP socket.
   assert(s.pcm_receive_converter_.prepares==0);s.selected_rx_format_.sample_rate=16000;s.selected_rx_format_.frame_ms=10;
   s.selected_tx_format_.sample_rate=16000;s.selected_tx_format_.frame_ms=10;s.negotiated_rx_payloads_.add(97,s.selected_rx_format_);
   assert(s.prepare_media_path_locked_());assert(s.rtp_socket_==original&&socket_port(original)==s.rtp_port_);
   assert(s.pcm_receive_converter_.prepares==1&&s.pcm_receive_converter_.last_rate==16000);
   for(uint16_t seq=60506;seq<=60510;++seq){std::array<uint8_t,512>packet{};assert(recv(original,packet.data(),packet.size(),MSG_DONTWAIT)==332);assert((unsigned(packet[2])<<8|packet[3])==seq);assert(std::memcmp(packet.data(),original_packets[seq-60506].data(),332)==0);}
   close(sender);
  }else assert(scenario=="abort"||scenario=="repeat"||scenario=="active-close");
  if(scenario=="active-close"){
   assert(s.prepare_media_path_locked_());assert(s.rtp_socket_==original);
   assert(s.commit_media_path_locked_());assert(s.rtp_running_&&s.media_lifecycle_phase_==SipTransport::MediaLifecyclePhase::ACTIVE);
  }
  released(s,original);s.reset_dialog_();s.media_lifecycle_phase_=SipTransport::MediaLifecyclePhase::IDLE;
 }
 s.close_tcp_flow_(s.tcp_flow_);close(s.sip_socket_);assert(offers==repeats);
}
'''
    # Active media uses the transport's existing wake, represented by the same
    # scheduler notification counter, never by another socket or task.
    code = code.replace("void wake_sip_task_(){}", "void wake_sip_task_(){}\nvoid wake_rtp_task_(){++notifications;}")
    cpp = folder / "reservation.cpp"
    cpp.write_text(code)
    binary = folder / "reservation"
    result = subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra", "-g", "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer", "-fno-pie", "-no-pie", "-DUSE_ESPHOME_VOIP_STACK_SPEAKER",
        "-I", str(SOURCE.parents[2]), "-I", str(SOURCE), str(cpp), "-o", str(binary),
    ], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("transport", ["udp", "tcp"])
@pytest.mark.parametrize("scenario", ["early-data", "abort", "repeat", "active-close", "bind-failure",
                                      "send-failure", "decline", "cancel", "overlapping-offer"])
def test_original_offer_reserves_and_releases_its_rtp_socket(reservation_binary, transport, scenario):
    result = subprocess.run([reservation_binary, scenario, transport], capture_output=True, text=True, timeout=8)
    assert result.returncode == 0, result.stdout + result.stderr
