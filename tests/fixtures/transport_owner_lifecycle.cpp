#include "sip_transport.h"
#include "sip_message.h"
#include "transport_host_sdk.h"
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace esphome::voip_stack;
inline thread_local bool fixture_stop=false;

struct Phone : SipTransport {
  Phone(uint16_t sip,uint16_t media) : SipTransport(sip,media,1200,"127.0.0.1",false) {}
  void *pending_mutex() {return &tcp_tx_pending_mutex_;}
  void observe_sip_exit() {
    // The scheduling adapter wakes the real task after stop has cleared
    // running_. Observe its real give without consuming the completion.
    wake_sip_task_();
    std::unique_lock<std::mutex> lock(sip_task_done_->mutex);
    assert(sip_task_done_->cv.wait_for(lock,std::chrono::seconds(2),[&]{return sip_task_done_->given;}));
  }
  bool resources_closed() const {
    return !running_ && sip_socket_<0 && sip_tcp_listener_socket_<0 && sip_wake_socket_<0 &&
        sip_task_handle_==nullptr && rtp_task_handle_==nullptr && rtp_socket_<0 && !rtp_running_;
  }
};
static uint16_t free_port() {
  int socket_fd=socket(AF_INET,SOCK_DGRAM,0);assert(socket_fd>=0);
  sockaddr_in local{};local.sin_family=AF_INET;local.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  assert(bind(socket_fd,reinterpret_cast<sockaddr*>(&local),sizeof(local))==0);
  socklen_t length=sizeof(local);assert(getsockname(socket_fd,reinterpret_cast<sockaddr*>(&local),&length)==0);
  close(socket_fd);return ntohs(local.sin_port);
}
static std::string receive(int socket_fd) {
  pollfd p{socket_fd,POLLIN,0};assert(poll(&p,1,2000)==1);
  char packet[8192];const auto bytes=recv(socket_fd,packet,sizeof(packet),0);assert(bytes>0);return {packet,static_cast<size_t>(bytes)};
}
int main(int argc,char **argv) {
  assert(argc==2);const std::string mode(argv[1]);
  const auto port=free_port(),media=free_port();
  int peer=socket(AF_INET,SOCK_DGRAM,0);assert(peer>=0);
  sockaddr_in remote{};remote.sin_family=AF_INET;remote.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  assert(bind(peer,reinterpret_cast<sockaddr*>(&remote),sizeof(remote))==0);
  socklen_t length=sizeof(remote);assert(getsockname(peer,reinterpret_cast<sockaddr*>(&remote),&length)==0);
  sockaddr_in local=remote;local.sin_port=htons(port);
  {
    Phone phone(port,media);phone.set_accept_direct_calls(true);phone.set_remote("127.0.0.1",ntohs(remote.sin_port),40002);
    phone.stop();assert(phone.resources_closed());  // No worker handles yet.
    if(mode=="startup-failure") {
      scheduler_fail_at=scheduler_creations+2;  // SIP starts, RTP admission fails.
      assert(!phone.start() && phone.resources_closed());reap_host_tasks();
      scheduler_fail_at=0;
    }
    for(unsigned generation=0;generation<3;++generation) {
      assert(phone.start() && phone.start());assert(video_probe::tasks==2);
      const auto options=[&](const std::string &transport) {
        return "OPTIONS sip:100@127.0.0.1 SIP/2.0\r\nVia: SIP/2.0/"+transport+" 127.0.0.1:"+
            std::to_string(ntohs(remote.sin_port))+";branch=z9hG4bK-options\r\nFrom: <sip:101@127.0.0.1>;tag=remote\r\n"
            "To: <sip:100@127.0.0.1>\r\nCall-ID: options"+std::to_string(generation)+
            "\r\nCSeq: 1 OPTIONS\r\nContent-Length: 0\r\n\r\n";
      };
      const auto udp=options("UDP");
      assert(sendto(peer,udp.data(),udp.size(),0,reinterpret_cast<sockaddr*>(&local),sizeof(local))==static_cast<ssize_t>(udp.size()));
      const auto response=receive(peer);assert(response.rfind("SIP/2.0 200",0)==0);
      int stream=socket(AF_INET,SOCK_STREAM,0);assert(stream>=0);
      assert(connect(stream,reinterpret_cast<sockaddr*>(&local),sizeof(local))==0);
      const auto tcp=options("TCP");assert(send(stream,tcp.data(),tcp.size(),0)==static_cast<ssize_t>(tcp.size()));
      assert(receive(stream).rfind("SIP/2.0 200",0)==0);close(stream);
      // Outgoing INVITE is read by the same SIP task; its terminal response
      // produces a real ACK before the owner joins both persistent workers.
      phone.set_sip_signaling_transport(false);
      assert(phone.send_invite("native-"+std::to_string(generation),"100","Phone","101","Peer"));
      const auto invite=receive(peer);assert(invite.rfind("INVITE ",0)==0);
      const auto rejected="SIP/2.0 486 Busy Here\r\nVia: "+header_value(invite,"Via")+"\r\nFrom: "+header_value(invite,"From")+
          "\r\nTo: "+header_value(invite,"To")+";tag=desk\r\nCall-ID: "+header_value(invite,"Call-ID")+
          "\r\nCSeq: "+header_value(invite,"CSeq")+"\r\nContent-Length: 0\r\n\r\n";
      assert(sendto(peer,rejected.data(),rejected.size(),0,reinterpret_cast<sockaddr*>(&local),sizeof(local))==static_cast<ssize_t>(rejected.size()));
      assert(receive(peer).rfind("ACK ",0)==0);
      bool early_observed=false;
      if(mode=="early-sip-exit") {
        const auto pending=phone.pending_mutex();
        esphome::before_mutex=[&,pending](void *mutex){
          if(fixture_stop && mutex==pending && !early_observed){early_observed=true;phone.observe_sip_exit();}
        };
      }
      fixture_stop=true;phone.stop();fixture_stop=false;
      assert(phone.resources_closed());reap_host_tasks();assert(video_probe::tasks==0);
      if(mode=="early-sip-exit")assert(early_observed);
      esphome::before_mutex={};
    }
  }
  close(peer);assert(video_probe::allocated==video_probe::freed && video_probe::allocations.empty());
}
