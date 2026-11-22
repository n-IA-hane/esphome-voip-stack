#include "video_rtp.h"
#include "video_host_sdk.h"
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace esphome::voip_stack;
inline thread_local bool releasing=false;
struct Owner : VideoRtpSession {
  using VideoRtpSession::VideoRtpSession;
  void *socket_lock() {return &socket_mutex_;}
};

static VideoCapability capability() {
  VideoCapability value; value.encoding="H264"; value.payload_type=105;
  value.packetization_mode=1; value.width=value.height=16; value.max_fps=10; value.profile_level_id="42c00a";
  return value;
}
struct Sink : EncodedVideoSink {
  std::mutex mutex; std::condition_variable cv;
  std::vector<EncodedVideoAccessUnit> leases;
  unsigned stopped=0; size_t limit=0;
  size_t max_encoded_access_unit_bytes() const override {return limit;}
  VideoCapability get_receive_video_capability() const override {return capability();}
  bool start_video(const VideoCapability&) override {return true;}
  bool set_video_active(bool) override {return true;}
  void stop_video() override {{std::lock_guard<std::mutex> lock(mutex);++stopped;}cv.notify_all();}
  bool consume_video_access_unit(const EncodedVideoAccessUnit &unit) override {
    {std::lock_guard<std::mutex> lock(mutex);assert(unit.release_callback);leases.push_back(unit);}cv.notify_all();return true;
  }
  EncodedVideoAccessUnit take(unsigned index) {
    std::unique_lock<std::mutex> lock(mutex);
    assert(cv.wait_for(lock,std::chrono::seconds(2),[&]{return leases.size()>index;}));return leases[index];
  }
  void wait_stop(unsigned count) {
    std::unique_lock<std::mutex> lock(mutex);
    assert(cv.wait_for(lock,std::chrono::seconds(2),[&]{return stopped>=count;}));
  }
};
static uint16_t free_pair() {
  for(uint16_t port=38000;port<45000;port+=2) {
    int first=socket(AF_INET,SOCK_DGRAM,0),second=socket(AF_INET,SOCK_DGRAM,0);
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(port);
    const bool one=bind(first,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0;address.sin_port=htons(port+1);
    const bool two=bind(second,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0;
    close(first);close(second);if(one&&two)return port;
  }
  std::abort();
}
static void picture(int sender,uint16_t port,unsigned seq) {
  const uint8_t sps[]={0x67,0x42,0xc0,0x0a,0xd9,0x1e,0xc0,0x44,0,0,3,0,4,0,0,3,0,0x50,0x3c,0x48,0x99,0x20};
  const uint8_t pps[]={0x68,0xcb,0x83,0xcb,0x20};
  const uint8_t idr[]={0x65,0x88,0x84,0x0f,0xf2,0x62,0x80,0,0x80,0x3e};
  sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(port);
  const auto send=[&](const uint8_t *data,size_t size,bool marker) {
    uint8_t packet[1500]{};packet[0]=0x80;packet[1]=105|(marker?0x80:0);packet[2]=seq>>8;packet[3]=seq++;packet[11]=1;
    std::memcpy(packet+12,data,size);
    assert(sendto(sender,packet,size+12,0,reinterpret_cast<sockaddr*>(&address),sizeof(address))==static_cast<ssize_t>(size+12));
  };
  send(sps,sizeof(sps),false);send(pps,sizeof(pps),false);send(idr,sizeof(idr),true);
}
static void setup(VideoRtpSession &owner,Sink &sink,uint16_t port,uint16_t sender_port) {
  owner.set_sink(&sink);owner.set_local_port(port);assert(owner.prepare_buffers());
  const auto cap=capability();assert(owner.set_negotiated(cap,cap,cap,0x7f000001,sender_port,0x7f000001,sender_port+1,false,true));
  assert(owner.start());
}
int main(int argc,char **argv) {
  assert(argc==2);const std::string mode(argv[1]);
  int sender=socket(AF_INET,SOCK_DGRAM,0);assert(sender>=0);
  sockaddr_in sender_address{};sender_address.sin_family=AF_INET;sender_address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  assert(bind(sender,reinterpret_cast<sockaddr*>(&sender_address),sizeof(sender_address))==0);
  socklen_t length=sizeof(sender_address);assert(getsockname(sender,reinterpret_cast<sockaddr*>(&sender_address),&length)==0);
  const auto port=free_pair();Sink sink;
  auto *owner=new Owner(false);setup(*owner,sink,port,ntohs(sender_address.sin_port));
  picture(sender,port,0);auto held=sink.take(0);assert(held.size>20);
  std::vector<uint8_t> retained(held.data,held.data+held.size);
  if(mode=="stop-redial-resize") {
    const auto allocations=video_probe::allocated.load(),frees=video_probe::freed.load();
    owner->stop();assert(video_probe::tasks==0);
    assert(std::memcmp(held.data,retained.data(),held.size)==0 && video_probe::freed==frees);
    const auto cap=capability();assert(owner->set_negotiated(cap,cap,cap,0x7f000001,ntohs(sender_address.sin_port),0,0,false,true));
    assert(owner->start());picture(sender,port,10);auto redial=sink.take(1);redial.release();owner->stop();
    assert(std::memcmp(held.data,retained.data(),held.size)==0 && video_probe::allocated==allocations);
    Sink resized;resized.limit=8192;owner->set_sink(&resized);
    assert(!owner->prepare_buffers() && video_probe::freed==frees);
    held.release();assert(owner->prepare_buffers());delete owner;
  } else if(mode=="destroy-release-tail") {
    owner->stop();
    std::mutex gate;std::condition_variable cv;bool entered=false,proceed=false;
    const auto socket_lock=owner->socket_lock();
    esphome::before_mutex=[&](void *mutex) {
      if(!releasing || mutex!=socket_lock)return;
      std::unique_lock<std::mutex> lock(gate);entered=true;cv.notify_all();cv.wait(lock,[&]{return proceed;});
    };
    std::thread release([&]{releasing=true;held.release();});
    {std::unique_lock<std::mutex> lock(gate);assert(cv.wait_for(lock,std::chrono::seconds(2),[&]{return entered;}));}
    std::atomic<bool> destroyed{false};std::thread destroy([&]{delete owner;destroyed=true;});
    delay(50);
    {std::lock_guard<std::mutex> lock(gate);proceed=true;}cv.notify_all();
    release.join();destroy.join();assert(destroyed);esphome::before_mutex={};
  } else {
    std::atomic<bool> destroyed{false};
    std::thread destroy([&]{delete owner;destroyed=true;});
    sink.wait_stop(1);delay(50);
    // Before the fix both the encoded bytes and callback context are freed.
    assert(std::memcmp(held.data,retained.data(),held.size)==0);
    held.release();destroy.join();assert(destroyed);
  }
  close(sender);
  assert(video_probe::tasks==0 && video_probe::allocated==video_probe::freed && video_probe::allocations.empty());
}
