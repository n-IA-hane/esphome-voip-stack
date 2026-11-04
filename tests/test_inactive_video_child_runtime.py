"""Prepared-child retarget and RTCP use real production fields and methods."""
import shutil
import subprocess

import pytest

from test_refer_dialog_runtime import block
from test_video_resource_lifecycle import VIDEO, RTOS, compile_harness, write_headers


@pytest.fixture(scope='module')
def child(tmp_path_factory):
    folder=tmp_path_factory.mktemp('inactive-video-child')
    for name in ('video_rtp.h','video.h','rtp_jpeg.h'):
        shutil.copyfile(VIDEO/name,folder/name)
    write_headers(folder,{
        'esphome/core/defines.h':'#pragma once\n', 'audio_core_task_utils.h':'#pragma once\n',
        'esphome/core/helpers.h':r'''
#pragma once
#include <functional>
#include <mutex>
namespace esphome {
struct Mutex;
inline std::function<void(Mutex*)> before_lock;
struct Mutex {std::mutex impl;void lock(){if(before_lock)before_lock(this);impl.lock();}void unlock(){impl.unlock();}};
using LockGuard=std::lock_guard<Mutex>;
}
''',
        'freertos/FreeRTOS.h':RTOS,'freertos/semphr.h':'#include "FreeRTOS.h"\n',
        'freertos/task.h':'#include "FreeRTOS.h"\n',
    })
    source=(VIDEO/'video_rtp.cpp').read_text()
    methods='\n'.join(block(source,marker) for marker in [
        'VideoRtpSession::VideoRtpSession(', 'bool VideoRtpSession::can_retarget_inactive_remote(',
        'bool VideoRtpSession::retarget_inactive_remote(', 'bool VideoRtpSession::handle_rtcp_from_(',
        'bool VideoRtpSession::handle_rtcp_packet_(', 'void VideoRtpSession::send_rtcp_packet_('])
    helpers=block(source,'uint32_t read_be32(')
    code=r'''
#include <cassert>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#define protected public
#include "video_rtp.h"
#undef protected
namespace esphome::voip_stack {
HELPERS
unsigned wake_calls=0;
METHODS
VideoRtpSession::~VideoRtpSession(){}
void VideoRtpSession::wake_task_(){++wake_calls;}
}
using namespace esphome::voip_stack;
void configure(VideoRtpSession&s){
 s.running_=true;s.terminate_=false;s.send_enabled_=false;s.receive_enabled_=false;s.source_started_=false;
 s.send_prepared_=s.receive_prepared_=true;s.remote_ip_v4_=0x7f000001;s.remote_rtcp_ip_v4_=0x7f000001;
 s.remote_rtp_port_=40040;s.remote_rtcp_port_=40041;s.latched_remote_rtp_port_=40050;s.latched_remote_rtcp_port_=40051;
 s.remote_ssrc_=0x12345678;s.remote_ssrc_latched_=true;s.tx_ssrc_=0x87654321;
}
const uint8_t report[]={0x80,201,0,1,0x12,0x34,0x56,0x78, 0x81,202,0,1,0x12,0x34,0x56,0x78};
int listener(uint32_t ip,uint16_t*port){
 int fd=socket(AF_INET,SOCK_DGRAM,0);assert(fd>=0);sockaddr_in address{};address.sin_family=AF_INET;
 address.sin_addr.s_addr=htonl(ip);assert(bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
 socklen_t n=sizeof(address);assert(getsockname(fd,reinterpret_cast<sockaddr*>(&address),&n)==0);*port=ntohs(address.sin_port);return fd;
}
int main(int argc,char**argv){
 assert(argc==2);const std::string mode=argv[1];VideoRtpSession s(false);configure(s);
 if(mode=="guards"){
  for(unsigned which=0;which<5;++which){
   configure(s);if(which==0)s.send_enabled_=true;if(which==1)s.receive_enabled_=true;
   if(which==2){s.source_started_=true;}
   if(which==3){s.terminate_=true;}
   if(which==4){s.running_=false;}
   assert(!s.can_retarget_inactive_remote());assert(!s.retarget_inactive_remote(0x7f000002,41000,0x7f000002,41001));
   assert(s.remote_rtp_port_==40040&&s.remote_rtcp_port_==40041&&s.latched_remote_rtcp_port_==40051);
  }
  assert(wake_calls==0);return 0;
 }
 assert(s.can_retarget_inactive_remote());
 if(mode=="commit"||mode=="RTCP"){
  assert(s.retarget_inactive_remote(0x7f000002,41000,0x7f000002,41001));
  assert(s.remote_ip_v4_==0x7f000002&&s.remote_rtp_port_==41000&&s.remote_rtcp_ip_v4_==0x7f000002&&s.remote_rtcp_port_==41001);
  assert(!s.remote_ssrc_latched_&&s.remote_ssrc_==0&&s.latched_remote_rtp_port_==0&&s.latched_remote_rtcp_port_==0);
  assert(s.rx_reset_requested_&&s.send_prepared_&&s.receive_prepared_&&s.tx_ssrc_==0x87654321&&wake_calls==1);
  if(mode=="commit")return 0;
  bool key=false;
  assert(!s.handle_rtcp_from_(report,sizeof(report),0x7f000001,40041,&key));
  assert(s.handle_rtcp_from_(report,sizeof(report),0x7f000002,40041,&key)&&s.latched_remote_rtcp_port_==0&&!key);
  assert(!s.handle_rtcp_from_(report,7,0x7f000002,41001,&key)&&s.latched_remote_rtcp_port_==0);
  s.receive_enabled_=true;
  assert(s.handle_rtcp_from_(report,sizeof(report),0x7f000002,41003,&key)&&s.latched_remote_rtcp_port_==41003);
  return 0;
 }
 assert(mode=="coherent-send");
 uint16_t ap,bp;int a=listener(0x7f000001,&ap),b=listener(0x7f000002,&bp);
 s.rtcp_socket_=socket(AF_INET,SOCK_DGRAM,0);assert(s.rtcp_socket_>=0);
 s.remote_rtcp_ip_v4_=0x7f000001;s.remote_rtcp_port_=ap;s.latched_remote_rtcp_port_=0;
 std::atomic<bool>sender_at_lock{false};
 esphome::before_lock=[&](esphome::Mutex*m){if(m==&s.socket_mutex_)sender_at_lock=true;};
 // Interleave a coherent SDP commit before the sender acquires the socket
 // lock. Reading the tuple before that lock would transmit to retired A.
 s.socket_mutex_.lock();sender_at_lock=false;
 std::thread sender([&]{s.send_rtcp_packet_(report,sizeof(report));});
 while(!sender_at_lock.load())std::this_thread::yield();
 s.remote_rtcp_ip_v4_=0x7f000002;s.remote_rtcp_port_=bp;s.socket_mutex_.unlock();
 sender.join();esphome::before_lock={};
 pollfd fd[2]={{a,POLLIN,0},{b,POLLIN,0}};assert(poll(fd,2,1000)==1&&!(fd[0].revents&POLLIN)&&(fd[1].revents&POLLIN));
 uint8_t bytes[32];assert(recv(b,bytes,sizeof(bytes),0)==sizeof(report)&&memcmp(bytes,report,sizeof(report))==0);
 // Concurrent real retargets and sends must always deliver to whole A or B.
 std::thread retarget([&]{for(unsigned n=0;n<40;++n)assert(s.retarget_inactive_remote(n&1?0x7f000001:0x7f000002,42000,n&1?0x7f000001:0x7f000002,n&1?ap:bp));});
 for(unsigned n=0;n<40;++n)s.send_rtcp_packet_(report,sizeof(report));
 retarget.join();unsigned received=0;
 while(received<40){fd[0].revents=fd[1].revents=0;assert(poll(fd,2,1000)>0);
  for(auto &f:fd)if(f.revents&POLLIN){assert(recv(f.fd,bytes,sizeof(bytes),0)==sizeof(report));++received;}}
 assert(received==40);close(a);close(b);close(s.rtcp_socket_);s.rtcp_socket_=-1;
}
'''.replace('HELPERS',helpers).replace('METHODS',methods)
    try:
        return compile_harness(folder,code,defines=('USE_ESP32','USE_ESPHOME_VOIP_STACK_VIDEO','USE_ESPHOME_VOIP_STACK_VIDEO_JPEG'))
    except subprocess.CalledProcessError as error:
        pytest.fail(error.stderr)


@pytest.mark.parametrize('scenario',['commit','guards','RTCP','coherent-send'])
def test_existing_child_retargets_only_inactive_media_and_keeps_RTCP_coherent(child,scenario):
    result=subprocess.run([str(child),scenario],capture_output=True,text=True,timeout=5)
    assert result.returncode==0,result.stdout+result.stderr
