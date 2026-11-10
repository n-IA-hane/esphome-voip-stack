"""Real packetizers keep wire bytes, pacing and bounded retry while copying once."""
import json
import shutil
import subprocess

import pytest

from test_refer_dialog_runtime import SOURCE, block
from test_video_resource_lifecycle import RTOS, write_headers


def packet_source(implementation, codec):
    helpers = '\n'.join(block(implementation, marker) for marker in ('void write_be16(', 'void write_be32('))
    if codec == 'H264':
        helpers += '\n' + block(implementation, 'bool next_annex_b_nal(')
    markers = ['VideoRtpSession::VideoRtpSession(', f'void VideoRtpSession::send_{codec.lower()}_access_unit_(']
    for marker in ('bool VideoRtpSession::send_rtp_payload_(', 'bool VideoRtpSession::send_rtp_packet_('):
        if marker in implementation:
            markers.append(marker)
    methods = '\n'.join(block(implementation, marker) for marker in markers)
    return r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#define protected public
#include "video_rtp.h"
#undef protected
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
uint32_t millis(){return 1234;}
unsigned copied_bytes=0,send_calls=0,pacing_calls=0,resets=0;std::string mode;
void*tracked_copy(void*out,const void*input,size_t bytes){copied_bytes+=bytes;return std::memcpy(out,input,bytes);}
ssize_t tracked_send(int fd,const void*p,size_t n,int flags,const sockaddr*address,socklen_t size){
 ++send_calls;if((mode=="retry"&&send_calls==1)||(mode=="failure"&&send_calls==2)){
  errno=mode=="retry"?EAGAIN:EINVAL;return -1;
 }
 return ::sendto(fd,p,n,flags,address,size);
}
#define memcpy tracked_copy
#define sendto tracked_send
namespace esphome::voip_stack {
@HELPERS@
@METHODS@
VideoRtpSession::~VideoRtpSession(){}
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_HOSTED_AUDIO_PACING
bool VideoRtpSession::wait_for_audio_pacing_(){
 ++pacing_calls;if(mode=="pacing-stop"&&pacing_calls>1)return false;
 if(mode=="stop-before-send")this->send_enabled_=false;
 return true;
}
void VideoRtpSession::reset_audio_pacing_burst_(){++resets;}
#endif
}
#undef memcpy
#undef sendto
using namespace esphome::voip_stack;
uint32_t number(const uint8_t*p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
int main(int argc,char**argv){
 assert(argc==3);mode=argv[1];VideoRtpSession session(false);session.running_=true;session.send_enabled_=mode!="paused";
 session.max_payload_=400;session.tx_sequence_=1000;session.tx_timestamp_offset_=123;session.tx_ssrc_=0x01020304;
 session.rtp_socket_=socket(AF_INET,SOCK_DGRAM,0);const int peer=socket(AF_INET,SOCK_DGRAM,0);assert(peer>=0&&session.rtp_socket_>=0);
 sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 assert(bind(peer,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);socklen_t length=sizeof(address);
 assert(getsockname(peer,reinterpret_cast<sockaddr*>(&address),&length)==0);
 session.remote_ip_v4_=INADDR_LOOPBACK;session.remote_rtp_port_=ntohs(address.sin_port);
 std::vector<uint8_t>input,expected;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
 session.capability_.payload_type=26;
 std::vector<uint8_t>fragment(8+4096);fragment[4]=1;fragment[5]=50;fragment[6]=fragment[7]=8;
 for(unsigned n=0;n<4096;++n)fragment[n+8]=uint8_t(1+n%200);
 expected.assign(fragment.begin()+8,fragment.end());input.resize(8192);size_t completed=0;
 RtpJpegDepacketizer dep;assert(dep.push(fragment.data(),fragment.size(),true,90000,input.data(),input.size(),&completed)==RtpJpegPushResult::COMPLETE);
 input.resize(completed);
#else
 session.capability_.payload_type=103;
 input={0,0,0,1,0x67,0x42,0xe0,0x1e,0,0,0,1,0x65};expected={0x67,0x42,0xe0,0x1e,0x65};
 for(unsigned n=0;n<4096;++n){input.push_back(uint8_t(1+n%200));expected.push_back(uint8_t(1+n%200));}
#endif
 const auto original_input=input;const EncodedVideoAccessUnit unit{input.data(),input.size(),90000,true};
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
 session.send_jpeg_access_unit_(unit);
#else
 session.send_h264_access_unit_(unit);
#endif
 assert(input==original_input&&"Packetization cannot modify an access unit owned by the source");
 bool pacing_abort=false;
#if defined(USE_ESPHOME_VOIP_STACK_VIDEO_JPEG) && defined(USE_ESPHOME_VOIP_STACK_VIDEO_HOSTED_AUDIO_PACING)
 pacing_abort=mode=="pacing-stop"||mode=="stop-before-send";
#endif
 const bool failed=mode=="failure"||mode=="paused"||pacing_abort;
 std::vector<uint8_t>reconstructed;std::ofstream wire(argv[2],std::ios::binary);unsigned packets=0,markers=0;
 while(true){
  uint8_t packet[1600];const auto bytes=recv(peer,packet,sizeof(packet),MSG_DONTWAIT);
  if(bytes<0){assert(errno==EAGAIN||errno==EWOULDBLOCK);break;}
  assert(bytes>12&&bytes<=412&&packet[0]==0x80&&(packet[1]&127)==session.capability_.payload_type);
  assert((unsigned(packet[2])*256+packet[3])==1000+packets&&number(packet+4)==90123&&number(packet+8)==0x01020304);
  if(packet[1]&128)++markers;
  const uint8_t size[2]{uint8_t(bytes>>8),uint8_t(bytes)};wire.write(reinterpret_cast<const char*>(size),2);wire.write(reinterpret_cast<const char*>(packet),bytes);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  const size_t offset=(size_t(packet[13])<<16)|(size_t(packet[14])<<8)|packet[15];assert(offset==reconstructed.size());
  assert(packet[16]==1&&packet[18]==8&&packet[19]==8);size_t header=20;
  if(offset==0&&packet[17]>=128){assert(packet[20]==0&&packet[21]==0);header+=4+unsigned(packet[22])*256+packet[23];}
  assert(header<size_t(bytes));reconstructed.insert(reconstructed.end(),packet+header,packet+bytes);
#else
  const auto type=packet[12]&31;
  if(type==28){assert(bytes>=15);if(packet[13]&128)reconstructed.push_back((packet[12]&0xe0)|(packet[13]&31));
   reconstructed.insert(reconstructed.end(),packet+14,packet+bytes);
  }else{assert(type==7);reconstructed.insert(reconstructed.end(),packet+12,packet+bytes);}
#endif
  ++packets;
 }
 assert(session.tx_packets_==packets&&session.tx_sequence_==1000+packets+unsigned(mode=="failure"));
 assert(session.dropped_access_units_==unsigned(failed)&&session.tx_access_units_ok_==unsigned(!failed));
 if(!failed){assert(markers==1&&reconstructed==expected&&packets>2);}
 else{assert(markers==0);assert(std::equal(reconstructed.begin(),reconstructed.end(),expected.begin()));}
 if(mode=="paused"||(pacing_abort&&mode=="stop-before-send"))assert(packets==0);
 if(mode=="failure"||(pacing_abort&&mode=="pacing-stop"))assert(packets==1);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
 assert(session.tx_resync_needed_==failed);
#endif
 std::printf("{\"packets\":%u,\"copied_bytes\":%u,\"input_bytes\":%zu,\"send_calls\":%u,\"pacing_calls\":%u,\"resets\":%u}\n",packets,copied_bytes,input.size(),send_calls,pacing_calls,resets);
 wire.close();std::fflush(stdout);
 if(mode=="normal"||mode=="retry")assert(copied_bytes<=input.size()&&"Packetization must copy each input byte into its final RTP packet only once");
 close(peer);close(session.rtp_socket_);session.rtp_socket_=-1;
}
'''.replace('@HELPERS@', helpers).replace('@METHODS@', methods)


@pytest.fixture(scope='module', params=[('JPEG', False), ('JPEG', True), ('H264', False), ('H264', True)])
def packet_writer(tmp_path_factory, request):
    codec, paced = request.param
    folder = tmp_path_factory.mktemp(f'packet-{codec}-{paced}')
    for name in ('video_rtp.h', 'video.h', 'rtp_jpeg.h'):
        shutil.copyfile(SOURCE / name, folder / name)
    write_headers(folder, {
        'esphome/core/defines.h': '#pragma once\n', 'audio_core_task_utils.h': '#pragma once\n',
        'esphome/core/helpers.h': '#pragma once\n#include <mutex>\nnamespace esphome {using Mutex=std::mutex;using LockGuard=std::lock_guard<Mutex>;}\n',
        'freertos/FreeRTOS.h': RTOS, 'freertos/semphr.h': '#include "FreeRTOS.h"\n', 'freertos/task.h': '#include "FreeRTOS.h"\n',
    })
    cpp = folder / 'packet.cpp'
    cpp.write_text(packet_source((SOURCE / 'video_rtp.cpp').read_text(), codec))
    binary = folder / 'packet'
    defines = ['USE_ESP32', 'USE_ESPHOME_VOIP_STACK_VIDEO', f'USE_ESPHOME_VOIP_STACK_VIDEO_{codec}']
    if paced:
        defines.append('USE_ESPHOME_VOIP_STACK_VIDEO_HOSTED_AUDIO_PACING')
    built = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-variable',
                            '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-fstack-usage', '-O1', '-fno-inline',
                            *[f'-D{x}' for x in defines], '-I', str(folder), str(cpp),
                            *([str(SOURCE / 'rtp_jpeg.cpp')] if codec == 'JPEG' else []), '-o', str(binary)],
                           capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize('mode', ['normal', 'retry', 'failure', 'paused', 'pacing-stop', 'stop-before-send'])
def test_real_packetizers_preserve_media_and_retry_contract(packet_writer, mode):
    run = subprocess.run([str(packet_writer), mode, str(packet_writer.parent / f'{mode}.wire')],
                         capture_output=True, text=True, timeout=5)
    assert run.returncode == 0, run.stdout + run.stderr
    receipt = json.loads(run.stdout)
    (packet_writer.parent / f'{mode}.json').write_text(json.dumps(receipt, indent=2) + '\n')
