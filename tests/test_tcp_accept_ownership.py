"""Two accepted TCP streams retain unread SIP records before a dialog exists."""
from pathlib import Path
import subprocess

import pytest

from test_sip_registration_transport import registration_transport_source
from test_refer_dialog_runtime import SOURCE, block
from test_refer_wire_runtime import wire_source


@pytest.fixture(scope="module", params=["direct", "registration", "mwi", "blf", "both", "psram", "video"])
def admission_binary(tmp_path_factory, request):
    folder=tmp_path_factory.mktemp("tcp-admission")
    defines=folder/"esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    feature = request.param
    registered = feature != "direct"
    mwi = feature in {"mwi", "both"}
    blf = feature in {"blf", "both"}
    code=registration_transport_source(mwi, blf) if registered else wire_source(full_invite=True)
    if feature == "psram":
        helpers = folder / "esphome/core/helpers.h"
        helpers.write_text("""#pragma once
#include <memory>
namespace esphome {
template<class T>struct RAMAllocator{
 using value_type=T;enum {ALLOC_EXTERNAL=1};RAMAllocator(int=0){}
 template<class U>RAMAllocator(const RAMAllocator<U>&){}
 T*allocate(size_t n){return static_cast<T*>(::operator new(n*sizeof(T)));}
 void deallocate(T*p,size_t){::operator delete(p);}
 template<class U>bool operator==(const RAMAllocator<U>&)const{return true;}
 template<class U>bool operator!=(const RAMAllocator<U>&)const{return false;}
};}
""")
        stub = 'struct SipSignalingString:std::string {using std::string::string;using std::string::operator=;std::string str()const{return *this;}};'
        assert stub in code
        code = '#include "sip_signaling_string.h"\n' + code.replace(
            stub, 'using SipSignalingString=esphome::voip_stack::SipSignalingString;')
    transport_source = (SOURCE/'sip_transport.cpp').read_text()
    expiry_begin = transport_source.index('    for (auto *flow : {&this->tcp_flow_, &this->inbound_tcp_flow_})',
                                          transport_source.index('const int ready = ::select'))
    expiry = transport_source[expiry_begin:transport_source.index('    bool connect_expired', expiry_begin)]
    extra = 'void expire_inbound_(){\n' + expiry + '}\n'
    if feature == "video":
        extra += 'struct PendingVideo {bool active=false;bool pending()const{return active;}} pending_video_direction_invite_;\n'
    code=code.replace('struct SipTransport: SipPhoneTransport {', '''struct SipTransport: SipPhoneTransport {
 bool dialog_active_()const{return !call_id_.empty()||media_active_;}
''' + extra,1)
    # The existing transport fixture leaves the application INVITE callback
    # unimplemented. Observe dispatch after the actual TCP framing/parser.
    old=block(code,'bool SipTransport::handle_invite_(')
    code=code.replace(old,'bool SipTransport::handle_invite_(const std::string &m,const sockaddr_in&){if(!claim_inbound_tcp_(dispatch_socket_))return false;call_id_=header_value(m,"Call-ID");tcp_flow_.ingress_deadline=0;return true;}',1)
    code='#include <netinet/tcp.h>\n'+code
    if feature == "video":
        # Compile the complete ingress methods with their video ownership gate.
        # Media codecs remain the existing controlled adapter in this fixture.
        gated = []
        for marker in ('bool SipTransport::claim_inbound_tcp_(', 'bool SipTransport::reclaim_idle_inbound_tcp_(',
                       'void SipTransport::handle_sip_stream_('):
            body = block(transport_source, marker)
            assert body in code
            code = code.replace(body, '', 1)
            gated.append(body)
        code += '\n#define USE_ESPHOME_VOIP_STACK_VIDEO\n' + '\n'.join(gated)
    code+=block(transport_source,'void SipTransport::accept_tcp_client_(')
    peers=(Path(__file__).parent/'fixtures/refer_wire_main.cpp').read_text().split('std::string begin(')[0]
    fixture=(Path(__file__).parent/'fixtures/registration_transport.cpp').read_text()
    if registered:
        code+='\n'+fixture[:fixture.index('void bind_account(')]
    code+='\n'+peers
    code+=r'''
int main(int argc,char**argv){assert(argc==4);const bool control_first=std::string(argv[2])=="control-first";const bool invite_first=std::string(argv[1])=="invite-first";
 SipTransport s;Peer first(true),second(true);
 assert(s.inbound_tcp_flow_.rx_buffer.capacity()<MAX_SIP_TCP_RX_BUFFER);
 assert(s.inbound_tcp_flow_.pending.capacity()<MAX_SIP_TCP_RX_BUFFER);
 s.call_id_.clear();s.media_active_=false;s.remote_sip_tcp_=false;
#if defined(USE_ESPHOME_VOIP_MWI) || defined(USE_ESPHOME_VOIP_BLF)
 SipMwi::Hooks hooks;hooks.context=&s;hooks.reply=SipTransport::registration_mwi_reply_;s.mwi_.set_hooks(hooks);
#endif
#ifdef USE_ESPHOME_VOIP_REGISTRATION
 auto &account=s.registration_.account(0);account.occupied=account.bound=account.desired=true;
 account.binding_ip=std::string(argv[3])=="registered"?INADDR_LOOPBACK:0x7f000002;
#endif
 auto a=first.address,b=second.address;
 s.accept_tcp_client_(first.esp,a);
 const std::string invite="INVITE sip:esp@127.0.0.1 SIP/2.0\r\nVia: SIP/2.0/TCP 127.0.0.1;branch=z9hG4bKone\r\nFrom: <sip:caller@127.0.0.1>;tag=one\r\nTo: <sip:esp@127.0.0.1>\r\nCall-ID: simultaneous\r\nCSeq: 1 INVITE\r\nContent-Length: 0\r\n\r\n";
 const std::string notify="NOTIFY sip:esp@127.0.0.1 SIP/2.0\r\nVia: SIP/2.0/TCP 127.0.0.1;branch=z9hG4bKtwo\r\nFrom: <sip:caller@127.0.0.1>;tag=two\r\nTo: <sip:esp@127.0.0.1>;tag=none\r\nCall-ID: control\r\nCSeq: 1 NOTIFY\r\nEvent: message-summary\r\nSubscription-State: terminated\r\nContent-Length: 0\r\n\r\n";

 const std::string scenario=argv[1];
 if(scenario=="pool-full"){
  s.accept_tcp_client_(second.esp,b);Peer third(true);s.accept_tcp_client_(third.esp,third.address);
  assert(fcntl(third.esp,F_GETFD)<0&&errno==EBADF);
  assert(fcntl(first.esp,F_GETFD)>=0&&fcntl(second.esp,F_GETFD)>=0);
  assert(s.call_id_.empty());
 }else if(scenario=="deadline"){
  s.accept_tcp_client_(second.esp,b);const auto deadline=s.tcp_flow_.ingress_deadline.load();
  clock_ms+=1000;assert(send(first.fd,"INVITE ",7,0)==7);s.handle_sip_stream_(first.esp,a);
  assert(s.tcp_flow_.ingress_deadline==deadline);
  clock_ms=deadline;s.expire_inbound_();
  assert(s.tcp_flow_.socket<0&&s.inbound_tcp_flow_.socket<0&&s.call_id_.empty());
 }else if(scenario=="oversized"){
  s.accept_tcp_client_(second.esp,b);
  const std::string malformed="INVITE sip:esp@local SIP/2.0\r\nContent-Length: 4097\r\n\r\n";
  assert(send(second.fd,malformed.data(),malformed.size(),0)==ssize_t(malformed.size()));
  s.handle_sip_stream_(second.esp,b);
  assert(s.inbound_tcp_flow_.socket<0&&s.tcp_flow_.socket==first.esp&&s.call_id_.empty());
 }else if(scenario=="partial"){
  const std::string prefix=invite.substr(0,20);
  assert(send(first.fd,prefix.data(),prefix.size(),0)==ssize_t(prefix.size()));s.handle_sip_stream_(first.esp,a);
  s.accept_tcp_client_(second.esp,b);second.send_to(s,invite);
  assert(s.call_id_=="simultaneous"&&s.tcp_flow_.socket==second.esp);
  assert(s.inbound_tcp_flow_.socket==first.esp&&s.inbound_tcp_flow_.rx_buffer.str()==prefix);
 }else if(scenario=="options"){
  const std::string options="OPTIONS sip:esp@127.0.0.1 SIP/2.0\r\nVia: SIP/2.0/TCP 127.0.0.1;branch=probe\r\nFrom: <sip:peer@local>;tag=probe\r\nTo: <sip:esp@local>\r\nCall-ID: probe\r\nCSeq: 1 OPTIONS\r\nContent-Length: 0\r\n\r\n";
  first.send_to(s,options);const auto replies=first.drain();assert(replies.size()==1&&replies[0].rfind("SIP/2.0 200",0)==0);
  s.accept_tcp_client_(second.esp,b);second.send_to(s,invite);
  assert(s.call_id_=="simultaneous"&&s.tcp_flow_.socket==second.esp&&s.inbound_tcp_flow_.socket==first.esp);
 }else{
 const auto &one=invite_first?invite:notify;const auto &two=invite_first?notify:invite;
 assert(send(first.fd,one.data(),one.size(),0)==ssize_t(one.size()));
 s.accept_tcp_client_(second.esp,b);
 assert(fcntl(first.esp,F_GETFD)>=0 && "second accept destroyed the first unread SIP stream");
 assert(s.tcp_flow_for_socket_(first.esp)!=nullptr);
 assert(s.tcp_flow_for_socket_(second.esp)!=nullptr);
 assert(send(second.fd,two.data(),two.size(),0)==ssize_t(two.size()));
 if(control_first){s.handle_sip_stream_(second.esp,b);s.handle_sip_stream_(first.esp,a);}
 else{s.handle_sip_stream_(first.esp,a);s.handle_sip_stream_(second.esp,b);}
 assert(s.call_id_=="simultaneous");
 assert(s.tcp_flow_for_socket_(invite_first?first.esp:second.esp)!=nullptr);
 assert(s.inbound_tcp_flow_.socket<0 || s.inbound_tcp_flow_.socket==first.esp);
 }
 s.close_tcp_flow_(s.tcp_flow_);s.close_tcp_flow_(s.inbound_tcp_flow_);
}
'''
    cpp=folder/'admission.cpp'
    cpp.write_text(code)
    binary=folder/'admission'
    flags = []
    if registered:
        flags.append('-DUSE_ESPHOME_VOIP_REGISTRATION')
    if mwi:
        flags.append('-DUSE_ESPHOME_VOIP_MWI')
    if blf:
        flags.append('-DUSE_ESPHOME_VOIP_BLF')
    if feature == "psram":
        flags.append('-DUSE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM')
    result=subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-g','-fsanitize=address,undefined',
        '-fno-pie','-no-pie', *flags,
        '-I',str(folder),'-I',str(SOURCE.parents[2]),'-I',str(SOURCE),str(cpp),
        str(SOURCE/'sip_registration.cpp'),str(SOURCE/'sip_mwi.cpp'),str(SOURCE/'sip_blf.cpp'),
        '-lcrypto','-o',str(binary)],capture_output=True,text=True)
    assert result.returncode==0,result.stderr
    return binary


@pytest.mark.parametrize('order',['invite-first','notify-first'])
@pytest.mark.parametrize('dispatch',['primary-first','control-first'])
@pytest.mark.parametrize('peer',['registered','direct'])
def test_unread_stream_survives_concurrent_accept(admission_binary,order,dispatch,peer):
    result=subprocess.run([admission_binary,order,dispatch,peer],capture_output=True,text=True,timeout=8)
    assert result.returncode==0,result.stderr


@pytest.mark.parametrize('scenario', ['options', 'partial', 'pool-full', 'deadline', 'oversized'])
def test_admission_pool_keeps_bounded_independent_streams(admission_binary, scenario):
    result = subprocess.run([admission_binary, scenario, 'primary-first', 'direct'],
                            capture_output=True, text=True, timeout=8)
    assert result.returncode == 0, result.stderr
