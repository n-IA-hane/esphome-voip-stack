#include "sip_transport.h"
#include "sip_message.h"
#include "transport_host_sdk.h"
#include <atomic>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <tuple>
#include <unistd.h>

using namespace esphome::voip_stack;

struct ReplayPhone : SipTransport {
  ReplayPhone(uint16_t sip, uint16_t media) : SipTransport(sip, media, 1200, "127.0.0.1", false) {}
  auto resources() const {
    esphome::LockGuard dialog(dialog_mutex_);
    esphome::LockGuard media(media_lifecycle_mutex_);
    AudioFormat tx, rx; uint8_t tx_pt, rx_pt; RtpAudioPayloads payloads;
    get_media_config_(&tx, &rx, &tx_pt, &rx_pt, &payloads);
    return std::make_tuple(rtp_socket_, rtp_task_handle_, rtp_ssrc_, tx, rx, tx_pt, rx_pt);
  }
  bool proposed(const std::string &body, AudioFormat *tx, AudioFormat *rx) const {
    esphome::LockGuard dialog(dialog_mutex_);
    esphome::LockGuard media(media_lifecycle_mutex_);
    const auto retained = negotiated_media_(); NegotiatedMedia candidate;
    if (!parse_remote_media_(body, INADDR_LOOPBACK, false, retained, &candidate, true)) return false;
    *tx = candidate.tx; *rx = candidate.rx; return true;
  }
  DtmfConfig dtmf() const { DtmfConfig result;get_dtmf_config_(&result.tx,&result.rx);return result;}
  AudioFormat playout() const {
    AudioFormat rx; get_media_config_(nullptr, &rx, nullptr, nullptr);
    return pcm_playout_format(rx, offer_rx_formats_);
  }
};

static uint16_t reserve_udp(int *fd) {
  *fd = socket(AF_INET, SOCK_DGRAM, 0); assert(*fd >= 0);
  sockaddr_in address{}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  assert(bind(*fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
  socklen_t bytes=sizeof(address); assert(getsockname(*fd,reinterpret_cast<sockaddr*>(&address),&bytes)==0);
  return ntohs(address.sin_port);
}
static uint16_t reserve_sip(int *udp, int *tcp) {
  for(unsigned n=0;n<64;++n){const auto port=reserve_udp(udp);*tcp=socket(AF_INET,SOCK_STREAM,0);assert(*tcp>=0);
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(port);
    if(bind(*tcp,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0)return port;
    assert(errno==EADDRINUSE);close(*udp);close(*tcp);
  } assert(false);return 0;
}
static std::string take(int fd) {
  pollfd ready{fd,POLLIN,0};assert(poll(&ready,1,2500)==1);char bytes[16384];
  auto count=recv(fd,bytes,sizeof(bytes),0);assert(count>0);return {bytes,static_cast<size_t>(count)};
}
static std::string read(const char *path) {std::ifstream in(path,std::ios::binary);assert(in);return {std::istreambuf_iterator<char>(in),{}};}
static std::string retarget(const std::string &body,uint16_t port) {
  std::istringstream input(body);std::string line,result;
  while(std::getline(input,line)){
    if(!line.empty()&&line.back()=='\r')line.pop_back();
    if(line.rfind("c=IN IP4 ",0)==0)line="c=IN IP4 127.0.0.1";
    if(line.rfind("m=audio ",0)==0){const auto end=line.find(' ',8);assert(end!=line.npos);line.replace(8,end-8,std::to_string(port));}
    result+=line+"\r\n";
  }return result;
}
static void format(const char *name,const AudioFormat &f){
  std::cout<<'"'<<name<<"\":{\"rate\":"<<f.sample_rate<<",\"channels\":"<<unsigned(f.channels)
           <<",\"ms\":"<<unsigned(f.frame_ms)<<",\"pcm\":"<<unsigned(f.pcm_format)<<",\"codec\":"<<unsigned(f.codec)<<'}';
}
struct CallEvents {
  std::atomic<bool> accepted{false}, incoming{false}, ready{false}, incompatible{false};
};
int main(int argc,char **argv) {
  assert(argc==7);AudioFormatList tx,rx;tx.count=rx.count=0;
  const std::string mode=argv[5];const unsigned expected=std::stoul(argv[6]);
  const bool incoming=mode.rfind("incoming",0)==0,delayed=mode.find("offerless")!=mode.npos;
  std::ifstream config(argv[1]);assert(config);std::string flow,codec,pcm;unsigned rate,channels,ms;
  while(config>>flow>>codec>>rate>>channels>>ms>>pcm){
    assert(flow=="tx"||flow=="rx");assert(codec=="pcm"||codec=="g722"||codec=="opus");
    AudioFormat f;f.sample_rate=rate;f.channels=channels;f.frame_ms=ms;
    f.codec=codec=="pcm"?AudioCodec::PCM:codec=="g722"?AudioCodec::G722:AudioCodec::OPUS;
    f.pcm_format=pcm=="s16le"?PcmFormat::S16LE:pcm=="s24le"?PcmFormat::S24LE:pcm=="s24-in-s32"?PcmFormat::S24LE_IN_S32:PcmFormat::S32LE;
    auto &list=flow=="tx"?tx:rx;assert(list.count<VOIP_STACK_MAX_AUDIO_FORMATS);list.formats[list.count++]=f;
  }
  int peer,media_old,media_new,reserved,sip_udp,sip_tcp;
  const auto peer_port=reserve_udp(&peer),old_port=reserve_udp(&media_old),new_port=reserve_udp(&media_new);
  const auto sip_port=reserve_sip(&sip_udp,&sip_tcp),local_media_port=reserve_udp(&reserved);close(reserved);
  const auto initial_answer=retarget(read(argv[2]),old_port),reoffer=retarget(read(argv[3]),new_port);
  unsigned wire_status=0;bool unchanged=false,media_usable=false,parsed=false,offerless_ok=!delayed;
  AudioFormat initial_tx=disabled_audio_format(),initial_rx=initial_tx,proposed_tx=initial_tx,proposed_rx=initial_tx,playout=initial_tx;
  {
    ReplayPhone phone(sip_port,local_media_port);phone.set_audio_formats(tx,rx);phone.initialize_registration();
    phone.set_remote("127.0.0.1",peer_port,old_port);
    SipAccountConfig account;account.id="protocol-witness";account.registrar="127.0.0.1";account.domain="127.0.0.1";
    account.username="protocol-witness";account.port=peer_port;account.expires=30;assert(phone.set_account(account));
    CallEvents events;phone.set_sip_signal_callback([](void *p,SipSignal signal){auto &e=*static_cast<CallEvents*>(p);
      if(signal.type==SipSignalType::STATUS_200_OK)e.accepted=true;
      if(signal.type==SipSignalType::INVITE)e.incoming=true;
      if(signal.type==SipSignalType::MEDIA_READY)e.ready=true;
      if(signal.type==SipSignalType::MEDIA_INCOMPATIBLE)e.incompatible=true;
    },&events);
    close(sip_udp);close(sip_tcp);assert(phone.start());
    sockaddr_in endpoint{};endpoint.sin_family=AF_INET;endpoint.sin_addr.s_addr=htonl(INADDR_LOOPBACK);endpoint.sin_port=htons(sip_port);
    const auto deliver=[&](const std::string &msg){assert(sendto(peer,msg.data(),msg.size(),0,reinterpret_cast<sockaddr*>(&endpoint),sizeof(endpoint))==static_cast<ssize_t>(msg.size()));};
    const auto response=[&](const std::string &request,unsigned status,const std::string &body=std::string()){
      auto to=header_value(request,"To");if(to.find(";tag=")==to.npos)to+=";tag=winner-b";
      std::string msg="SIP/2.0 "+std::to_string(status)+" Replay\r\nVia: "+header_value(request,"Via")+"\r\nFrom: "+header_value(request,"From")+
          "\r\nTo: "+to+"\r\nCall-ID: "+header_value(request,"Call-ID")+"\r\nCSeq: "+header_value(request,"CSeq")+"\r\nContact: "+header_value(request,"Contact")+"\r\n";
      if(!body.empty())msg+="Content-Type: application/sdp\r\n";
      return msg+"Content-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
    };
    const auto wait=[&](auto predicate){const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);while(!predicate()&&std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(1));assert(predicate());};
    const auto reg=take(peer);assert(reg.rfind("REGISTER ",0)==0);deliver(response(reg,200));
    wait([&]{return phone.is_registered(account.id);});
    const auto peer_uri="<sip:fork@127.0.0.1:"+std::to_string(peer_port)+">";
    std::string peer_from,peer_to,local_uri;
    if(incoming){
      local_uri="sip:protocol-witness@127.0.0.1:"+std::to_string(sip_port);
      peer_from=peer_uri+";tag=winner-b";
      std::string invite="INVITE "+local_uri+" SIP/2.0\r\nVia: SIP/2.0/UDP 127.0.0.1:"+std::to_string(peer_port)+
          ";branch=z9hG4bK-incoming-1\r\nFrom: "+peer_from+"\r\nTo: <"+local_uri+">\r\nContact: "+peer_uri+
          "\r\nCall-ID: capture-replay\r\nCSeq: 1 INVITE\r\nContent-Type: application/sdp\r\nContent-Length: "+std::to_string(initial_answer.size())+"\r\n\r\n"+initial_answer;
      deliver(invite);assert(take(peer).rfind("SIP/2.0 100",0)==0);wait([&]{return events.incoming.load();});
      const auto selected=phone.snapshot();assert(phone.prepare_media_path());
      assert(phone.send_answer("capture-replay",selected.selected_rx_format,selected.selected_tx_format));
      const auto ok=take(peer);assert(ok.rfind("SIP/2.0 200",0)==0);std::ofstream(argv[4],std::ios::binary)<<message_body(ok);
      peer_to=header_value(ok,"To");
      const auto ack="ACK "+local_uri+" SIP/2.0\r\nVia: SIP/2.0/UDP 127.0.0.1:"+std::to_string(peer_port)+
          ";branch=z9hG4bK-incoming-ack-1\r\nFrom: "+peer_from+"\r\nTo: "+peer_to+"\r\nCall-ID: capture-replay\r\nCSeq: 1 ACK\r\nContent-Length: 0\r\n\r\n";
      deliver(ack);assert(phone.commit_media_path());
    }else{
      std::string host;uint16_t port;bool tcp;assert(phone.prepare_account_line(account.id,&host,&port,&tcp)&&!tcp);
      assert(phone.send_invite("capture-replay","100","Phone","fork","Peer"));
      const auto invite=take(peer);assert(invite.rfind("INVITE ",0)==0);std::ofstream(argv[4],std::ios::binary)<<message_body(invite);
      auto ok=response(invite,200,initial_answer);const auto contact=header_value(ok,"Contact");
      ok.replace(ok.find("\r\nContact: ")+11,contact.size(),peer_uri);
      deliver(ok);assert(take(peer).rfind("ACK ",0)==0);wait([&]{return events.accepted.load();});
      assert(phone.prepare_media_path()&&phone.commit_media_path());
      peer_from=header_value(invite,"To")+";tag=winner-b";peer_to=header_value(invite,"From");local_uri=name_addr_uri(peer_to);
    }
    const auto request=[&](const char *method,unsigned number,const std::string &body,bool success_ack=false){
      const auto branch=std::string("z9hG4bK-")+(success_ack?"ack-":"reoffer-")+std::to_string(number);
      auto msg=std::string(method)+" "+local_uri+" SIP/2.0\r\nVia: SIP/2.0/UDP 127.0.0.1:"+std::to_string(peer_port)+
          ";branch="+branch+"\r\nFrom: "+peer_from+"\r\nTo: "+peer_to+"\r\nCall-ID: capture-replay\r\nCSeq: "+std::to_string(number)+" "+method+"\r\nContact: "+peer_uri+"\r\n";
      if(!body.empty())msg+="Content-Type: application/sdp\r\n";
      return msg+"Content-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
    };
    auto before=phone.resources();initial_tx=std::get<3>(before);initial_rx=std::get<4>(before);playout=phone.playout();
    if(mode=="alias-delayed") {
      const auto original=phone.dtmf();assert(original.tx.payload_type==106&&original.rx.payload_type==122&&original.tx.clock==16000&&original.rx.clock==16000&&original.tx.events==0x0fff&&original.rx.events==0x0fff);
      deliver(request("INVITE",100,""));const auto offered=take(peer);assert(offered.rfind("SIP/2.0 200",0)==0);
      const auto body=message_body(offered);std::ofstream(std::string(argv[4])+".delayed",std::ios::binary)<<body;
      assert(body.find("a=rtpmap:122 telephone-event/16000")!=body.npos&&body.find("a=rtpmap:106 telephone-event/16000")!=body.npos);
      deliver(request("ACK",100,initial_answer,true));wait([&]{return events.ready.load()||events.incompatible.load();});assert(events.ready.load()&&!events.incompatible.load());
      const auto after=phone.dtmf();assert(after==original&&phone.resources()==before);assert(phone.can_send_dtmf()&&phone.send_dtmf('5',160));unsigned ends=0,packets=0;
      while(ends<3){const auto event=take(media_old);assert(event.size()==16);const auto *data=reinterpret_cast<const uint8_t*>(event.data());assert((data[1]&127)==106&&data[12]==5);if(data[13]&0x80){assert(((unsigned(data[14])<<8)|data[15])==2560);++ends;}++packets;}
      std::vector<uint8_t> samples(original.tx.clock/1000*20*2,0x20);phone.send_audio_frame(samples.data(),samples.size());assert(take(media_old).size()>12);
      assert(phone.send_bye("capture-replay"));const auto bye=take(peer);assert(bye.rfind("BYE ",0)==0);deliver(response(bye,200));wait([&]{auto state=phone.snapshot();return !state.call_active&&!state.terminal_transaction_pending&&state.media_lifecycle_phase==0;});
      assert(phone.register_account(account.id,false));const auto unreg=take(peer);deliver(response(unreg,200));wait([&]{return !phone.is_registered(account.id);});phone.stop();reap_host_tasks();
      std::cout<<"{\"actual_delayed_offer_has_both_aliases\":true,\"tx_pt\":106,\"rx_pt\":122,\"events\":4095,\"dtmf_wire_packets\":"<<packets<<",\"dtmf_end_reports\":"<<ends<<",\"audio_resources_unchanged\":true,\"cleanup_complete\":true}"<<std::endl;return 0;
    }

    parsed=phone.proposed(reoffer,&proposed_tx,&proposed_rx);
    std::vector<uint8_t> samples(initial_tx.nominal_frame_bytes(),0x20);phone.send_audio_frame(samples.data(),samples.size());assert(take(media_old).size()>12);
    deliver(request("INVITE",100,reoffer));const auto final=take(peer);wire_status=std::stoul(final.substr(8,3));
    deliver(request("ACK",100,"",wire_status<300));unchanged=before==phone.resources();
    phone.send_audio_frame(samples.data(),samples.size());media_usable=take(wire_status==200?media_new:media_old).size()>12;
    bool peer_bye=false;
    if(wire_status==200&&delayed){
      deliver(request("INVITE",101,""));const auto offered=take(peer);assert(offered.rfind("SIP/2.0 200",0)==0);
      std::ofstream(std::string(argv[4])+".offerless",std::ios::binary)<<message_body(offered);
      deliver(request("ACK",101,retarget(read(argv[2]),new_port),true));
      wait([&]{return events.ready.load()||events.incompatible.load();});
      offerless_ok=events.ready.load()&&!events.incompatible.load()&&before==phone.resources();
      if(!offerless_ok){const auto bye=take(peer);assert(bye.rfind("BYE ",0)==0);deliver(response(bye,200));peer_bye=true;}
      else{phone.send_audio_frame(samples.data(),samples.size());assert(take(media_new).size()>12);}
    }
    if(!peer_bye){assert(phone.send_bye("capture-replay"));const auto bye=take(peer);assert(bye.rfind("BYE ",0)==0);deliver(response(bye,200));}
    wait([&]{auto state=phone.snapshot();return !state.call_active&&!state.terminal_transaction_pending&&state.media_lifecycle_phase==0;});
    assert(phone.register_account(account.id,false));const auto unreg=take(peer);assert(unreg.rfind("REGISTER ",0)==0);deliver(response(unreg,200));
    wait([&]{return !phone.is_registered(account.id);});phone.stop();reap_host_tasks();
  }
  close(peer);close(media_old);close(media_new);assert(video_probe::allocated==video_probe::freed&&video_probe::allocations.empty());
  std::cout<<'{';format("initial_tx",initial_tx);std::cout<<',';format("initial_rx",initial_rx);std::cout<<',';format("initial_playout",playout);std::cout<<',';
  format("proposed_tx",proposed_tx);std::cout<<',';format("proposed_rx",proposed_rx);
  std::cout<<",\"proposal_parsed\":"<<(parsed?"true":"false")<<",\"wire_status\":"<<wire_status<<",\"same_media_resources\":"<<(unchanged?"true":"false")
           <<",\"media_usable\":"<<(media_usable?"true":"false")<<",\"offerless_ok\":"<<(offerless_ok?"true":"false")<<",\"cleanup_complete\":true}"<<std::endl;
  return wire_status==expected&&unchanged&&media_usable&&offerless_ok?0:2;
}
