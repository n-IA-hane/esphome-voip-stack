"""Actual command admission and socket wake publish a bounded generation tag."""
import subprocess

from test_refer_dialog_runtime import SOURCE, block


def test_fifo_backpressure_and_failed_wake_never_publish_a_digit(tmp_path):
    implementation = (SOURCE / 'sip_dtmf.cpp').read_text()
    header = (SOURCE / 'sip_transport.h').read_text()
    code = r'''
#include "rtp_dtmf.h"
#include <array>
#include <atomic>
#include <cassert>
#include <cstring>
#include <mutex>
#include <thread>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using namespace esphome::voip_stack;
struct LockGuard{std::mutex&m;explicit LockGuard(std::mutex&v):m(v){m.lock();}~LockGuard(){m.unlock();}};
#define portENTER_CRITICAL(p) (p)->lock()
#define portEXIT_CRITICAL(p) (p)->unlock()
'''
    code += block(header, 'struct DtmfRequest {') + ';\n'
    code += r'''
struct Queue{DtmfRequest data[8]{};unsigned count=0;};constexpr int pdTRUE=1;
unsigned uxQueueSpacesAvailable(Queue*q){return 8-q->count;}
int xQueueSend(Queue*q,const void*p,int){if(q->count==8)return 0;q->data[q->count++]=*static_cast<const DtmfRequest*>(p);return pdTRUE;}
struct SipTransport {
 mutable std::mutex dialog_mutex_,rtp_socket_mutex_,media_config_lock_;
 DtmfFormat dtmf_tx_format_{8000,0xFFFF,109},dtmf_rx_format_{};
 std::atomic<uint32_t>media_proposal_epoch_{8},remote_rtp_ip_v4_{INADDR_LOOPBACK};
 std::atomic<uint16_t>remote_rtp_port_{40000};std::atomic<bool>media_active_{true},rtp_running_{true};
 int rtp_socket_=-1;uint16_t rtp_port_=0;Queue queue;Queue*dtmf_queue_=&queue;uint8_t direction=3;
 uint8_t get_audio_direction_()const{return direction;}
 void get_dtmf_config_(DtmfFormat*,DtmfFormat*,uint32_t*)const;
 bool can_send_dtmf_locked_(const DtmfFormat&,uint32_t)const;
 bool can_send_dtmf()const;bool send_dtmf(char,uint16_t=160);
};
'''
    for signature in ['void SipTransport::get_dtmf_config_(', 'bool SipTransport::can_send_dtmf_locked_(',
                      'bool SipTransport::can_send_dtmf() const', 'bool SipTransport::send_dtmf(']:
        code += block(implementation, signature) + '\n'
    code += r'''
void socket_for(SipTransport&s){s.rtp_socket_=socket(AF_INET,SOCK_DGRAM,0);assert(s.rtp_socket_>=0);sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);assert(bind(s.rtp_socket_,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0);socklen_t len=sizeof(addr);assert(getsockname(s.rtp_socket_,reinterpret_cast<sockaddr*>(&addr),&len)==0);s.rtp_port_=ntohs(addr.sin_port);}
int main(){
 SipTransport phone;socket_for(phone);assert(phone.can_send_dtmf());
 assert(!phone.send_dtmf('x')&&!phone.send_dtmf('1',39)&&phone.queue.count==0);
 phone.dtmf_tx_format_.clock=48000;assert(!phone.send_dtmf('1',8000)&&phone.queue.count==0);phone.dtmf_tx_format_.clock=8000;
 phone.dtmf_tx_format_.events=0x03FF;assert(!phone.send_dtmf('#')&&phone.queue.count==0);phone.dtmf_tx_format_.events=0xFFFF;
 phone.media_proposal_epoch_=9;assert(!phone.send_dtmf('1'));phone.media_proposal_epoch_=8;
 phone.direction=2;assert(!phone.can_send_dtmf()&&!phone.send_dtmf('1'));phone.direction=3;
 for(char c='1';c<='8';++c)assert(phone.send_dtmf(c));assert(!phone.can_send_dtmf()&&!phone.send_dtmf('9'));
 for(unsigned i=0;i<8;++i){assert(phone.queue.data[i].event==i+1&&phone.queue.data[i].epoch==8&&phone.queue.data[i].duration_ms==160);char wake;assert(recv(phone.rtp_socket_,&wake,1,0)==1);}
 close(phone.rtp_socket_);phone.queue.count=0;assert(!phone.send_dtmf('1')&&phone.queue.count==0);
 SipTransport concurrent;socket_for(concurrent);std::atomic<bool>ready=false;
 std::thread reader([&](){ready=true;fd_set fds;FD_ZERO(&fds);FD_SET(concurrent.rtp_socket_,&fds);timeval timeout{1,0};assert(select(concurrent.rtp_socket_+1,&fds,nullptr,nullptr,&timeout)==1);LockGuard lock(concurrent.rtp_socket_mutex_);assert(concurrent.queue.count==1&&concurrent.queue.data[0].event==1);});
 while(!ready.load())std::this_thread::yield();assert(concurrent.send_dtmf('1'));reader.join();close(concurrent.rtp_socket_);
}
'''
    cpp = tmp_path / 'enqueue.cpp'
    cpp.write_text(code)
    binary = tmp_path / 'enqueue'
    result = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation', '-g',
        '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-pthread', '-I', str(SOURCE), str(cpp), '-o', str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr
