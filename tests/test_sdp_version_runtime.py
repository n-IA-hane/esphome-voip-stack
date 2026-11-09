"""SDP origin versions belong to audio dialogs with or without video support."""
import subprocess

import pytest

from test_audio_resume_runtime import resume_source
from test_refer_dialog_runtime import ROOT, SOURCE, block


def version_source(video):
    code = resume_source().split('\n    int main(', 1)[0]
    if not video:
        # Retain the hardware fixture types, then compile every production
        # method with the video feature disabled, as in an audio-only build.
        code = code.replace('#include "esphome/components/voip_stack/video.h"',
                            '#include "esphome/components/voip_stack/video.h"\n'
                            '#undef USE_ESPHOME_VOIP_STACK_VIDEO')
    header = (SOURCE / 'sip_transport.h').read_text()
    owner = (SOURCE / 'sip_transport.cpp').read_text()
    parser = (SOURCE / 'sip_message.cpp').read_text()
    completed = block(header, 'struct CompletedServerTransaction {') + ';'
    code = code.replace(completed + 'CompletedServerTransaction completed_invite_,completed_control_;',
                        'CompletedServerTransaction completed_invite_,completed_control_;')
    code = code.replace('struct RetainedCall {', completed + '\nstruct RetainedCall {CompletedServerTransaction accepted_refer;')
    code = code.replace('struct SipTransport {', r'''
struct SipTransport {
 struct CompletedRefer {CompletedServerTransaction accepted,control;uint32_t expires_ms=0;};
 std::array<std::unique_ptr<CompletedRefer>,4>completed_refers_;
 bool replay_completed_response_(const std::string&,const sockaddr_in&,const std::string&,bool);
 bool send_sip_udp_bytes_(const char*p,size_t n,uint32_t,uint16_t){return sendto(sip_server_,p,n,0,reinterpret_cast<const sockaddr*>(&sip_peer_),sizeof(sip_peer_))==ssize_t(n);}
 bool send_sip_(const std::string&s,uint32_t i,uint16_t p){return send_sip_udp_bytes_(s.data(),s.size(),i,p);}
''')
    code = '#include <array>\n#include <memory>\nconstexpr unsigned MAX_SIP_TCP_RX_BUFFER=8192;\n' + code
    code += '\n' + block(parser, 'bool sip_content_length(')
    code += '\n' + block(owner, 'bool SipTransport::replay_completed_response_(')
    return code + r'''
int main(){
 SipTransport t;AudioFormat pcm;pcm.sample_rate=16000;pcm.frame_ms=10;
 t.offer_tx_formats_.formats[0]=t.offer_rx_formats_.formats[0]=pcm;
 t.remote_ip_v4_=INADDR_LOOPBACK;t.sdp_session_id_=123;t.sdp_session_version_=7;
 t.sip_server_=socket(AF_INET,SOCK_DGRAM,0);const int peer=socket(AF_INET,SOCK_DGRAM,0);
 assert(t.sip_server_>=0&&peer>=0);
 sockaddr_in local{};local.sin_family=AF_INET;local.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 assert(bind(peer,reinterpret_cast<sockaddr*>(&local),sizeof(local))==0);
 socklen_t length=sizeof(local);assert(getsockname(peer,reinterpret_cast<sockaddr*>(&local),&length)==0);
 t.sip_peer_=local;t.current_peer_ip=INADDR_LOOPBACK;t.current_peer_port=ntohs(local.sin_port);
 const std::string initial="v=0\r\nc=IN IP4 127.0.0.1\r\nm=audio 40004 RTP/AVP 96\r\na=rtpmap:96 L16/16000/1\r\na=ptime:10\r\na=sendrecv\r\n";
 assert(t.learn_remote_rtp_from_sdp_(initial,INADDR_LOOPBACK));
 const auto original=t.build_sdp_answer_();assert(original.find("o=- 123 7 IN IP4 ")!=std::string::npos);
 const auto request=[&](const std::string&body,unsigned sequence,const char*method="INVITE"){
  return std::string(method)+" sip:esp@127.0.0.1 SIP/2.0\r\nCall-ID: "+t.call_id_+
   "\r\nFrom: <sip:peer@127.0.0.1>;tag=remote\r\nTo: <sip:esp@127.0.0.1>;tag=local\r\nCSeq: "+std::to_string(sequence)+" "+method+
   "\r\nVia: SIP/2.0/UDP 127.0.0.1;branch=z9hG4bK-version-"+std::to_string(sequence)+
   "\r\nContent-Type: application/sdp\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;};
 const auto receive=[&](){char data[8192];const auto bytes=recv(peer,data,sizeof(data),MSG_DONTWAIT);assert(bytes>0);return std::string(data,bytes);};
 auto held=initial;held.replace(held.find("a=sendrecv"),10,"a=inactive");
 for(unsigned sequence:{2U,3U}){
  t.current_request=request(sequence==2?held:initial,sequence);
  assert(t.handle_reinvite_(t.current_request,local));
  const auto response=receive();const auto body=message_body(response);
  const auto version=sequence+6;
  assert(body.find("o=- 123 "+std::to_string(version)+" IN IP4 ")!=std::string::npos);
  assert(t.sdp_session_version_==version);
  const auto committed=t.sdp_session_version_;
  assert(t.replay_completed_response_(t.current_request,local,"INVITE",true));
  assert(receive()==response&&t.sdp_session_version_==committed);
  bool terminate=false;assert(t.acknowledge_completed_invite_(request("",sequence,"ACK"),local,&terminate)==200&&!terminate);
 }
 t.fail_send=true;t.current_request=request(held,4);
 assert(!t.handle_reinvite_(t.current_request,local));
 assert(t.sdp_session_version_==9&&t.audio_send_enabled_&&t.audio_receive_enabled_);
 t.fail_send=false;assert(t.handle_reinvite_(t.current_request,local));
 assert(message_body(receive()).find("o=- 123 10 IN IP4 ")!=std::string::npos&&t.sdp_session_version_==10);
 close(peer);close(t.sip_server_);
}
'''


@pytest.mark.parametrize('video', [False, True], ids=['audio-only', 'video-enabled'])
def test_hold_resume_versions_commit_only_after_publication(tmp_path, video):
    stub = tmp_path / 'esphome/core/defines.h'
    stub.parent.mkdir(parents=True)
    stub.write_text('#pragma once\n')
    (stub.parent / 'helpers.h').write_text('#pragma once\n#include <mutex>\nnamespace esphome {using Mutex=std::mutex;using LockGuard=std::lock_guard<Mutex>;}\n')
    cpp = tmp_path / 'versions.cpp'
    cpp.write_text(version_source(video))
    binary = tmp_path / 'versions'
    built = subprocess.run(['g++', '-std=c++17',
                            '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-pthread',
                            '-I', str(tmp_path), '-I', str(ROOT), str(cpp), '-o', str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
    assert run.returncode == 0, run.stdout + run.stderr
