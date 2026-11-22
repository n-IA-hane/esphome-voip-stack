#include "sip_transport.h"
#include "sip_message.h"
#include "transport_host_sdk.h"
#include <fstream>
#include <poll.h>
#include <vector>

using namespace esphome::voip_stack;
struct Peer {
  int fd=socket(AF_INET,SOCK_DGRAM,0);sockaddr_in address{};
  explicit Peer(uint16_t port=0,uint32_t ip=INADDR_LOOPBACK) {
    assert(fd>=0);address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(ip);address.sin_port=htons(port);
    assert(bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
    socklen_t size=sizeof(address);assert(getsockname(fd,reinterpret_cast<sockaddr*>(&address),&size)==0);
  }
  ~Peer(){if(fd>=0)close(fd);}
  uint16_t port()const{return ntohs(address.sin_port);}
  std::string take(unsigned wait=2000)const {
    pollfd p{fd,POLLIN,0};assert(poll(&p,1,wait)==1);char data[8192];const auto count=recv(fd,data,sizeof(data),0);
    assert(count>0);return {data,static_cast<size_t>(count)};
  }
  void deliver(const std::string &data,uint16_t target)const {
    auto dest=address;dest.sin_addr.s_addr=htonl(INADDR_LOOPBACK);dest.sin_port=htons(target);
    assert(sendto(fd,data.data(),data.size(),0,reinterpret_cast<sockaddr*>(&dest),sizeof(dest))==static_cast<ssize_t>(data.size()));
  }
};
static uint16_t free_port(){Peer reservation;return reservation.port();}
static uint16_t free_pair() {
  for(uint16_t port=38000;port<45000;port+=2) {
    int a=socket(AF_INET,SOCK_DGRAM,0),b=socket(AF_INET,SOCK_DGRAM,0);sockaddr_in address{};
    address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(port);
    const bool first=bind(a,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0;address.sin_port=htons(port+1);
    const bool second=bind(b,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0;close(a);close(b);if(first&&second)return port;
  }
  std::abort();
}
static VideoCapability capability(){VideoCapability cap;cap.encoding="JPEG";cap.payload_type=26;cap.width=320;cap.height=240;cap.max_fps=10;return cap;}
struct Source : EncodedVideoSource {
  std::mutex mutex;EncodedVideoAccessUnitCallback callback=nullptr;void *context=nullptr;
  unsigned starts=0,stops=0;bool available=true;std::vector<uint8_t> frame;
  explicit Source(const char *path){std::ifstream input(path);std::string hex;input>>hex;for(size_t i=0;i<hex.size();i+=2)frame.push_back(std::stoul(hex.substr(i,2),nullptr,16));assert(frame.size()>100);}
  VideoCapability get_video_capability()const override{return available?capability():VideoCapability{};}
  size_t max_encoded_access_unit_bytes()const override{return frame.size();}
  bool prepare_video(const VideoCapability &cap)override{return cap.valid()&&cap.is_jpeg();}
  bool start_video(EncodedVideoAccessUnitCallback cb,void *ctx,const VideoCapability &cap)override{
    std::lock_guard<std::mutex> lock(mutex);assert(cap.valid()&&cb&&!callback);callback=cb;context=ctx;++starts;return true;
  }
  void stop_video()override{std::lock_guard<std::mutex> lock(mutex);callback=nullptr;++stops;}
  bool active(){std::lock_guard<std::mutex> lock(mutex);return callback!=nullptr;}
  void publish(uint32_t timestamp){std::lock_guard<std::mutex> lock(mutex);assert(callback);EncodedVideoAccessUnit unit;unit.data=frame.data();unit.size=frame.size();unit.timestamp_90khz=timestamp;unit.key_frame=true;callback(context,unit);}
};
struct Sink : EncodedVideoSink {
  VideoCapability get_receive_video_capability()const override{return capability();}
  size_t max_encoded_access_unit_bytes()const override{return 4096;}
  bool start_video(const VideoCapability&)override{return true;}
  bool set_video_active(bool)override{return true;}
  void stop_video()override{}
  bool consume_video_access_unit(const EncodedVideoAccessUnit&)override{return true;}
};
struct Phone : SipTransport {
  using SipTransport::SipTransport;
  void use_route(const Peer &route,const Peer &contact){
    esphome::LockGuard lock(dialog_mutex_);auto &account=registration_.account(0);
    account.config.id="fixture";account.config.registrar="127.0.0.1";account.config.domain="fixture.invalid";
    account.config.username="100";account.config.password="test-only-password";account.occupied=account.desired=account.bound=true;
    account.state=RegistrationState::REGISTERED;account.binding_expires_ms=account.keepalive_next_ms=millis()+3600000;
    active_account_=0;remote_target_uri_="sip:101@127.0.0.1:"+std::to_string(contact.port());
    dialog_route_count_=1;dialog_routes_[0]="<sip:proxy@127.0.0.2:"+std::to_string(route.port())+";lr>";
    assert(dialog_digest_.parse("Digest realm=\"fixture\", nonce=\"fixture-nonce\", algorithm=MD5, qop=\"auth\"",false));
  }
  void expire_video(bool retry=false){esphome::LockGuard lock(dialog_mutex_);assert(pending_video_direction_invite_.pending());
    if(retry)pending_video_direction_invite_.retry_at_ms=millis()-1;else pending_video_direction_invite_.response_deadline_ms=millis()-1;
    pump_video_direction_transaction_();}
};
static std::string reply(const std::string &request,unsigned status,const std::string &body="") {
  auto to=header_value(request,"To");if(to.find(";tag=")==to.npos)to+=";tag=desk";
  return "SIP/2.0 "+std::to_string(status)+" Reply\r\nVia: "+header_value(request,"Via")+"\r\nFrom: "+header_value(request,"From")+
      "\r\nTo: "+to+"\r\nCall-ID: "+header_value(request,"Call-ID")+"\r\nCSeq: "+header_value(request,"CSeq")+
      "\r\nContent-Type: application/sdp\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
}
static void port_in(std::string &sdp,const char *media,uint16_t port){const auto start=sdp.find(std::string("m=")+media+" ");assert(start!=sdp.npos);const auto number=sdp.find(' ',start)+1;sdp.replace(number,sdp.find(' ',number)-number,std::to_string(port));}
static void receive_only(std::string &sdp){const auto start=sdp.find("m=video ");assert(start!=sdp.npos);auto direction=sdp.find("a=sendrecv",start);if(direction==sdp.npos)direction=sdp.find("a=sendonly",start);assert(direction!=sdp.npos);sdp.replace(direction,10,"a=recvonly");}
static std::string audio_section(const std::string &sdp){const auto start=sdp.find("m=audio ");assert(start!=sdp.npos);const auto end=sdp.find("\r\nm=",start);return sdp.substr(start,end==sdp.npos?end:end+2-start);}
static uint32_t picture(const Peer &peer,const Source &source){
  std::vector<uint8_t> scan;uint16_t previous_sequence=0;uint32_t wire_timestamp=0;bool first=true;
  for(;;){
    const auto bytes=peer.take();const auto *packet=reinterpret_cast<const uint8_t*>(bytes.data());
    assert(bytes.size()>20&&packet[0]==0x80&&(packet[1]&127)==26);
    const uint32_t timestamp=(uint32_t(packet[4])<<24)|(uint32_t(packet[5])<<16)|(uint32_t(packet[6])<<8)|packet[7];
    if(first)wire_timestamp=timestamp;else assert(timestamp==wire_timestamp);const uint16_t sequence=(uint16_t(packet[2])<<8)|packet[3];
    if(!first)assert(sequence==uint16_t(previous_sequence+1));previous_sequence=sequence;
    const uint32_t offset=(uint32_t(packet[13])<<16)|(uint32_t(packet[14])<<8)|packet[15];
    assert(offset==scan.size()&&packet[16]==1&&packet[18]==40&&packet[19]==30);
    size_t payload=20;
    if(first&&packet[17]>=128){assert(bytes.size()>=24);const size_t quantizers=(size_t(packet[22])<<8)|packet[23];assert(quantizers==128);payload+=4+quantizers;}
    assert(payload<bytes.size());scan.insert(scan.end(),packet+payload,packet+bytes.size());first=false;
    if(packet[1]&0x80)break;
  }
  // Locate the independent JPEG source's SOS segment and compare every scan
  // byte against the complete RFC 2435 wire fragments through their marker.
  size_t start=0;for(size_t i=0;i+4<source.frame.size();++i)if(source.frame[i]==0xff&&source.frame[i+1]==0xda){start=i+2+(size_t(source.frame[i+2])<<8)+source.frame[i+3];break;}
  assert(start>0&&source.frame[source.frame.size()-2]==0xff&&source.frame.back()==0xd9);
  assert(scan==std::vector<uint8_t>(source.frame.begin()+start,source.frame.end()-2));return wire_timestamp;
}
template<class F>static void eventually(F predicate){for(unsigned i=0;i<200;++i){if(predicate())return;delay(5);}assert(predicate());}
int main(int argc,char **argv){
  assert(argc==3);const std::string mode=argv[1];Peer signaling,audio,video,routed(0,0x7f000002),contact;const auto local_sip=free_port(),local_audio=free_port(),local_video=free_pair();
  Source source(argv[2]);Sink sink;
  {
    Phone phone(local_sip,local_audio,1200,"127.0.0.1",false);phone.initialize_registration();phone.set_remote("127.0.0.1",signaling.port(),audio.port());
    phone.set_video_endpoints(&source,mode=="absent"?nullptr:&sink);phone.set_video_config(VideoCodec::JPEG,local_video,26,1200);
    std::atomic<unsigned> media_callbacks{0},active_callbacks{0};
    phone.set_video_active_state_callback([](void *p,bool active){if(active)++*static_cast<std::atomic<unsigned>*>(p);},&active_callbacks);
    phone.set_sip_signal_callback([](void *p,SipSignal signal){if(signal.type==SipSignalType::INVITE||signal.type==SipSignalType::STATUS_200_OK||signal.type==SipSignalType::MEDIA_READY)++*static_cast<std::atomic<unsigned>*>(p);},&media_callbacks);
    assert(phone.request_video_send(mode=="initial-on"));assert(phone.start());
    const auto establish=[&](const std::string &id,bool reject_video){
      assert(phone.send_invite(id,"100","Phone","101","Peer"));auto offer=signaling.take();auto body=message_body(offer);port_in(body,"audio",audio.port());
      const auto offset=body.find("m=video ");
      if(offset!=body.npos){port_in(body,"video",reject_video?0:video.port());const auto direction=body.find("a=sendrecv",offset);if(direction!=body.npos)body.replace(direction,10,"a=recvonly");}
      signaling.deliver(reply(offer,200,body),local_sip);assert(signaling.take().rfind("ACK ",0)==0);eventually([&]{return phone.snapshot().call_active;});assert(phone.start_audio_path());
      return offer;
    };
    const auto incoming=[&]{
      phone.set_accept_direct_calls(true);
      const std::string uri="sip:101@127.0.0.1:"+std::to_string(signaling.port());
      const auto request=[&](const std::string &method,const std::string &to,const std::string &body){
        return method+" sip:100@127.0.0.1 SIP/2.0\r\nVia: SIP/2.0/UDP 127.0.0.1:"+std::to_string(signaling.port())+
            ";branch=z9hG4bK-incoming-"+method+"\r\nFrom: <"+uri+">;tag=desk\r\nTo: "+to+"\r\nContact: <"+uri+
            ">\r\nCall-ID: video-activation\r\nCSeq: 1 "+method+"\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
      };
      const std::string body="v=0\r\no=- 77 1 IN IP4 127.0.0.1\r\ns=Peer\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio "+std::to_string(audio.port())+
          " RTP/AVP 96\r\na=rtpmap:96 L16/16000/1\r\na=ptime:16\r\na=sendrecv\r\nm=video "+std::to_string(video.port())+" RTP/AVP 26\r\na=rtpmap:26 JPEG/90000\r\na=recvonly\r\n";
      signaling.deliver(request("INVITE","<sip:100@127.0.0.1>",body),local_sip);assert(signaling.take().rfind("SIP/2.0 100",0)==0);
      assert(phone.send_answer("video-activation",DEFAULT_AUDIO_FORMAT,DEFAULT_AUDIO_FORMAT));const auto answer=signaling.take();assert(answer.rfind("SIP/2.0 200",0)==0);
      assert(message_body(answer).find("m=video 0 ")!=std::string::npos);
      signaling.deliver(request("ACK",header_value(answer,"To"),""),local_sip);eventually([&]{return media_callbacks==1&&!phone.snapshot().terminal_transaction_pending;});assert(phone.start_audio_path());return answer;
    };
    const auto initial=mode=="incoming"?incoming():establish("video-activation",mode!="initial-on");
    if(mode=="route")phone.use_route(routed,contact);
    Peer &peer=mode=="route"?routed:signaling;
    const auto check_audio=[&]{auto before=phone.snapshot();const auto cap=before.selected_tx_format;std::vector<uint8_t> pcm(cap.nominal_frame_bytes(),0);
      phone.send_audio_frame(pcm.data(),pcm.size());auto packet=audio.take();assert(packet.size()>12);auto after=phone.snapshot();assert(after.call_active&&after.rtp_running&&after.rtp_tx_packets>before.rtp_tx_packets);return packet;};
    check_audio();const auto initial_callbacks=media_callbacks.load();assert(initial_callbacks==1);
    if(mode=="initial-on")eventually([&]{return source.active()&&!phone.snapshot().video_send_change_pending;});
    else {
      assert(!source.active()&&!phone.snapshot().video_send_enabled);
      if(mode=="local-failure"){source.available=false;assert(!phone.request_video_send(true));assert(!phone.snapshot().video_send_change_pending&&!phone.snapshot().video_send_requested);check_audio();source.available=true;}
      assert(phone.request_video_send(true)&&"configured source must offer video after the initial audio-only answer");
      auto offer=peer.take();assert(offer.rfind("INVITE ",0)==0);assert(header_value(offer,"Call-ID")==header_value(initial,"Call-ID"));
      assert(mode=="incoming"||cseq_number(header_value(offer,"CSeq"))>cseq_number(header_value(initial,"CSeq")));
      if(mode=="route"){assert(!header_value(offer,"Route").empty()&&!header_value(offer,"Authorization").empty());assert(offer.find("INVITE sip:101@127.0.0.1:"+std::to_string(contact.port()))==0);}
      auto body=message_body(offer);assert(body.find("m=video "+std::to_string(local_video)+" RTP/AVP 26")!=body.npos);
      assert(body.find("a=rtpmap:26 JPEG/90000")!=body.npos);assert(audio_section(body)==audio_section(message_body(initial)));
      assert(body.find("m=video ",body.find("m=video ")+1)==body.npos);assert(!source.active());check_audio();
      if(mode=="timeout"){
        phone.expire_video();auto bye=peer.take();assert(bye.rfind("BYE ",0)==0);peer.deliver(reply(bye,200),local_sip);eventually([&]{const auto state=phone.snapshot();return !state.call_active&&!state.terminal_transaction_pending&&state.media_lifecycle_phase==0;});assert(!source.active());
      }else if(mode=="reject-488"){
        peer.deliver(reply(offer,488),local_sip);assert(peer.take().rfind("ACK ",0)==0);eventually([&]{return !phone.snapshot().video_send_change_pending;});assert(!phone.snapshot().video_send_requested&&!source.active());check_audio();
      }else{
        if(mode=="glare"){peer.deliver(reply(offer,491),local_sip);assert(peer.take().rfind("ACK ",0)==0);eventually([&]{return phone.snapshot().last_sip_status_code==491;});phone.expire_video(true);offer=peer.take();body=message_body(offer);}
        port_in(body,"audio",audio.port());port_in(body,"video",mode=="reject-video"?0:video.port());
        receive_only(body);
        peer.deliver(reply(offer,200,body),local_sip);assert(peer.take().rfind("ACK ",0)==0);eventually([&]{return !phone.snapshot().video_send_change_pending;});
        if(mode=="reject-video"){assert(!source.active()&&!phone.snapshot().video_send_enabled);check_audio();}
        else eventually([&]{return source.active()&&!phone.snapshot().video_send_change_pending;});
      }
    }
    if(source.active()){
      source.publish(90000);const auto timestamp=picture(video,source);check_audio();
      assert(phone.request_video_send(false));auto offer=peer.take();auto body=message_body(offer);port_in(body,"audio",audio.port());port_in(body,"video",video.port());
      peer.deliver(reply(offer,200,body),local_sip);assert(peer.take().rfind("ACK ",0)==0);eventually([&]{return !source.active()&&!phone.snapshot().video_send_change_pending;});check_audio();
      assert(phone.request_video_send(true));offer=peer.take();body=message_body(offer);port_in(body,"audio",audio.port());port_in(body,"video",video.port());
      receive_only(body);
      peer.deliver(reply(offer,200,body),local_sip);assert(peer.take().rfind("ACK ",0)==0);eventually([&]{return source.active()&&!phone.snapshot().video_send_change_pending;});source.publish(180000);assert(picture(video,source)-timestamp==90000);
      assert(active_callbacks==1);
    }
    assert(media_callbacks==initial_callbacks);if(mode!="timeout"){assert(phone.send_bye("video-activation"));auto bye=peer.take();assert(bye.rfind("BYE ",0)==0);peer.deliver(reply(bye,200),local_sip);eventually([&]{const auto state=phone.snapshot();return !state.call_active&&!state.terminal_transaction_pending&&state.media_lifecycle_phase==0;});eventually([&]{return !source.active();});}
    assert(phone.request_video_send(false));phone.set_remote("127.0.0.1",signaling.port(),audio.port());establish("redial",false);check_audio();assert(phone.send_bye("redial"));auto redial_bye=signaling.take();signaling.deliver(reply(redial_bye,200),local_sip);eventually([&]{const auto state=phone.snapshot();return !state.call_active&&!state.terminal_transaction_pending&&state.media_lifecycle_phase==0;});phone.stop();reap_host_tasks();
  }
  assert(video_probe::tasks==0&&video_probe::allocated==video_probe::freed&&video_probe::allocations.empty());
}
