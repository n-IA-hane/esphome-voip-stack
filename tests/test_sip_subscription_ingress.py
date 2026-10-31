"""Production subscription ingress across independent BLF/MWI feature gates."""

from pathlib import Path
import subprocess

import pytest

from test_refer_dialog_runtime import SOURCE, block


@pytest.fixture(scope="module", params=[(), ("MWI",), ("BLF",), ("MWI", "BLF")],
                ids=["neither", "mwi-only", "blf-only", "both"])
def ingress_binary(tmp_path_factory, request):
    folder = tmp_path_factory.mktemp("subscription-ingress")
    header = folder / "esphome/core/defines.h"
    header.parent.mkdir(parents=True)
    header.write_text("")
    parser = (SOURCE / "sip_message.cpp").read_text()
    adapter = (SOURCE / "sip_registration_transport.cpp").read_text()
    fixture = (Path(__file__).parent / "test_sip_mwi_engine.cpp").read_text()
    code = '#include "sip_registration.h"\n'
    # The socket peer and independent SIP response builder have no engine owner.
    code += fixture[:fixture.index("struct Harness {")]
    code += '''
#if __has_include("sip_header_name.h")
#include "sip_header_name.h"
#endif
constexpr size_t MAX_SIP_TCP_RX_BUFFER=8192;
uint32_t millis(){return 1001;}
'''
    for marker in ["std::string trim_copy(", "std::string header_value(",
                   "bool sip_content_length(", "std::string cseq_method("]:
        code += block(parser, marker) + "\n"
    code += r'''
struct SipTransport {
 SipRegistration registration_;
#if defined(USE_ESPHOME_VOIP_MWI) || defined(USE_ESPHOME_VOIP_BLF)
 SipMwi mwi_;
#endif
 Peer *peer=nullptr;
 unsigned mwi_updates=0,blf_updates=0;
 int registration_socket_index_(int socket){return socket>=0?0:-1;}
 bool registration_ingress_(const std::string&,const sockaddr_in&,int);
};
'''
    code += block(adapter, "bool SipTransport::registration_ingress_(") + "\n"
    code += r'''
int main(int argc,char**argv){
 assert(argc==2);Peer peer(std::string(argv[1])=="tcp");SipTransport transport;transport.peer=&peer;
 SipAccountConfig config;config.id="desk";config.registrar="127.0.0.1";config.domain="example.test";
 config.username="alice";config.password="fixture-only";config.port=peer.port;config.tcp=peer.tcp;
 assert(transport.registration_.set_account(config,0));
 auto &account=transport.registration_.account(0);account.bound=true;
 account.target_ip=INADDR_LOOPBACK;account.target_port=peer.port;
 account.contact="sip:alice@127.0.0.1:5060";
 sockaddr_in source{};source.sin_addr.s_addr=htonl(INADDR_LOOPBACK);source.sin_port=htons(peer.port);
 const int socket=peer.tcp?peer.client:-1;
#if defined(USE_ESPHOME_VOIP_MWI) || defined(USE_ESPHOME_VOIP_BLF)
 SipMwi::Hooks hooks;hooks.context=&transport;hooks.random=random_value;hooks.hash=hash_value;
 hooks.local_ip=[](void*,uint32_t){return std::string("127.0.0.1");};
 hooks.send=[](void*ctx,size_t,const std::string&wire,uint32_t ip,uint16_t port,bool tcp){
  auto &owner=*static_cast<SipTransport*>(ctx);auto &peer=*owner.peer;
  assert(ip==INADDR_LOOPBACK&&port==peer.port&&tcp==peer.tcp);
  return send(peer.client,wire.data(),wire.size(),0)==ssize_t(wire.size());
 };
 hooks.reply=[](void*ctx,const std::string&wire,uint32_t,uint16_t,bool,int){
  auto &peer=*static_cast<SipTransport*>(ctx)->peer;
  return send(peer.client,wire.data(),wire.size(),0)==ssize_t(wire.size());
 };
#ifdef USE_ESPHOME_VOIP_MWI
 hooks.notify=[](void*ctx,const MessageWaitingState&){++static_cast<SipTransport*>(ctx)->mwi_updates;};
#endif
#ifdef USE_ESPHOME_VOIP_BLF
 hooks.notify_blf=[](void*ctx,const BlfStateInfo&){++static_cast<SipTransport*>(ctx)->blf_updates;};
#endif
 transport.mwi_.set_hooks(hooks);
#endif
#ifdef USE_ESPHOME_VOIP_MWI
 transport.mwi_.update_registered_account(0,account,0);transport.mwi_.tick(0);
 auto mwi=peer.receive();
 assert(transport.registration_ingress_(peer.deliver(response(mwi,200,"Expires: 60\r\n")),source,socket));
 const auto before_mwi=transport.mwi_updates;
 assert(transport.registration_ingress_(peer.deliver(notify(mwi)),source,socket));
 assert(peer.receive().rfind("SIP/2.0 200",0)==0);
 assert(transport.mwi_.state(0).available&&transport.mwi_.state(0).waiting);
 assert(transport.mwi_updates==before_mwi+1&&transport.blf_updates==0);
#endif
#ifdef USE_ESPHOME_VOIP_BLF
 assert(transport.mwi_.configure_blf(1));
 assert(transport.mwi_.set_blf_watch(0,0,account,"kitchen","sip:101@example.test",1002));
 transport.mwi_.tick(1002);auto blf=peer.receive();assert(field(blf,"Event")=="dialog");
 const auto previous_mwi=transport.mwi_updates,previous_blf=transport.blf_updates;
 assert(transport.registration_ingress_(peer.deliver(response(blf,200,"Expires: 60\r\n")),source,socket));
 auto update=notify(blf,1,"<dialog-info xmlns='urn:ietf:params:xml:ns:dialog-info' version='0' state='full' entity='sip:101@example.test'><dialog id='one'><state>early</state></dialog></dialog-info>",
                    "active;expires=60","application/dialog-info+xml");
 auto event=update.find("Event: message-summary");assert(event!=std::string::npos);
 update.replace(event,22,"Event: dialog");
 assert(transport.registration_ingress_(peer.deliver(update),source,socket));
 assert(peer.receive().rfind("SIP/2.0 200",0)==0);
 assert(transport.mwi_.blf_state(0)->available&&transport.mwi_.blf_state(0)->state==BlfState::RINGING);
 assert(transport.blf_updates==previous_blf+1&&transport.mwi_updates==previous_mwi);
#endif
#if !defined(USE_ESPHOME_VOIP_MWI) && !defined(USE_ESPHOME_VOIP_BLF)
 const std::string unsolicited="NOTIFY sip:phone@example.test SIP/2.0\r\nEvent: dialog\r\nCSeq: 1 NOTIFY\r\nContent-Length: 0\r\n\r\n";
 assert(!transport.registration_ingress_(peer.deliver(unsolicited),source,socket));
 assert(transport.mwi_updates==0&&transport.blf_updates==0&&peer.quiet());
#endif
}
'''
    cpp = folder / "ingress.cpp"
    cpp.write_text(code)
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie",
             "-DUSE_ESPHOME_VOIP_REGISTRATION",
             *[f"-DUSE_ESPHOME_VOIP_{feature}" for feature in request.param],
             "-I", str(folder), "-I", str(SOURCE)]
    obj, dependencies = folder / "subscriptions.o", folder / "subscriptions.d"
    built = subprocess.run(["g++", *flags, "-MD", "-MF", str(dependencies), "-c",
                            str(SOURCE / "sip_mwi.cpp"), "-o", str(obj)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    symbols = subprocess.run(["nm", "-C", "--defined-only", str(obj)],
                             capture_output=True, text=True, check=True).stdout
    if not request.param:
        assert "esphome::voip_stack::" not in symbols
        assert str(SOURCE / "sip_registration.h") not in dependencies.read_text()
    if "MWI" not in request.param:
        assert "SipMwi::parse_summary" not in symbols
    if "BLF" not in request.param:
        assert "SipMwi::set_blf_watch" not in symbols
    binary = folder / "ingress"
    built = subprocess.run(["g++", *flags, "-no-pie", str(cpp), str(obj),
                            str(SOURCE / "sip_registration.cpp"), str(SOURCE / "sip_blf.cpp"),
                            "-lcrypto", "-o", str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize("transport", ["udp", "tcp"])
def test_subscription_ingress_matches_the_enabled_protocol_owners(ingress_binary, transport):
    result = subprocess.run([ingress_binary, transport], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr
