"""Two accepted TCP streams retain unread SIP records before a dialog exists."""
from pathlib import Path
import subprocess

import pytest

from test_sip_registration_transport import registration_transport_source
from test_refer_dialog_runtime import SOURCE, block


@pytest.fixture(scope="module")
def admission_binary(tmp_path_factory):
    folder=tmp_path_factory.mktemp("tcp-admission")
    defines=folder/"esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    code=registration_transport_source(True)
    code=code.replace('struct SipTransport: SipPhoneTransport {', '''struct SipTransport: SipPhoneTransport {
 void accept_tcp_client_(int,const sockaddr_in&);
 bool dialog_active_()const{return !call_id_.empty()||media_active_;}
''',1)
    # The existing transport fixture leaves the application INVITE callback
    # unimplemented. Observe dispatch after the actual TCP framing/parser.
    old=block(code,'bool handle_invite_(')
    code=code.replace(old,'bool handle_invite_(const std::string &m,const sockaddr_in&){call_id_=header_value(m,"Call-ID");return true;}',1)
    code='#include <netinet/tcp.h>\n'+code
    code+=block((SOURCE/'sip_transport.cpp').read_text(),'void SipTransport::accept_tcp_client_(')
    peers=(Path(__file__).parent/'fixtures/refer_wire_main.cpp').read_text().split('std::string begin(')[0]
    fixture=(Path(__file__).parent/'fixtures/registration_transport.cpp').read_text()
    code+='\n'+fixture[:fixture.index('void bind_account(')]
    code+='\n'+peers
    code+=r'''
int main(int argc,char**argv){assert(argc==3);const bool control_first=std::string(argv[2])=="control-first";const bool invite_first=std::string(argv[1])=="invite-first";
 SipTransport s;Peer first(true),second(true);
 s.call_id_.clear();s.media_active_=false;s.remote_sip_tcp_=false;
 SipMwi::Hooks hooks;hooks.context=&s;hooks.reply=SipTransport::registration_mwi_reply_;s.mwi_.set_hooks(hooks);
 auto &account=s.registration_.account(0);account.occupied=account.bound=account.desired=true;
 account.binding_ip=INADDR_LOOPBACK;
 auto a=first.address,b=second.address;
 s.accept_tcp_client_(first.esp,a);
 const std::string invite="INVITE sip:esp@127.0.0.1 SIP/2.0\r\nVia: SIP/2.0/TCP 127.0.0.1;branch=z9hG4bKone\r\nFrom: <sip:caller@127.0.0.1>;tag=one\r\nTo: <sip:esp@127.0.0.1>\r\nCall-ID: simultaneous\r\nCSeq: 1 INVITE\r\nContent-Length: 0\r\n\r\n";
 const std::string notify="NOTIFY sip:esp@127.0.0.1 SIP/2.0\r\nVia: SIP/2.0/TCP 127.0.0.1;branch=z9hG4bKtwo\r\nFrom: <sip:caller@127.0.0.1>;tag=two\r\nTo: <sip:esp@127.0.0.1>;tag=none\r\nCall-ID: control\r\nCSeq: 1 NOTIFY\r\nEvent: message-summary\r\nSubscription-State: terminated\r\nContent-Length: 0\r\n\r\n";
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
 assert(s.inbound_control_flow_.socket<0 || s.inbound_control_flow_.socket==first.esp);
 s.close_tcp_flow_(s.tcp_flow_);s.close_tcp_flow_(s.inbound_control_flow_);
}
'''
    cpp=folder/'admission.cpp'
    cpp.write_text(code)
    binary=folder/'admission'
    result=subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-g','-fsanitize=address,undefined',
        '-fno-pie','-no-pie','-DUSE_ESPHOME_VOIP_REGISTRATION','-DUSE_ESPHOME_VOIP_MWI',
        '-I',str(folder),'-I',str(SOURCE.parents[2]),'-I',str(SOURCE),str(cpp),str(SOURCE/'sip_registration.cpp'),str(SOURCE/'sip_mwi.cpp'),'-lcrypto','-o',str(binary)],capture_output=True,text=True)
    assert result.returncode==0,result.stderr
    return binary


@pytest.mark.parametrize('order',['invite-first','notify-first'])
@pytest.mark.parametrize('dispatch',['primary-first','control-first'])
def test_unread_stream_survives_concurrent_accept(admission_binary,order,dispatch):
    result=subprocess.run([admission_binary,order,dispatch],capture_output=True,text=True,timeout=8)
    assert result.returncode==0,result.stderr
