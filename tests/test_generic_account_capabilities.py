"""Ordinary account offers follow peer SDP, never a managed account name."""

import subprocess

import pytest

from test_sdp_receive_payloads import ROOT, sdp_fixture_source


@pytest.mark.parametrize("account", ["qualification", "office", "ha", "direct"])
@pytest.mark.parametrize("directional", [True, False])
def test_registered_account_uses_wire_directional_capability(
    tmp_path, account, directional
):
    owner = (ROOT / "esphome/components/voip_stack/voip_fsm.cpp").read_text()
    start = owner.index(
        "  const std::string outgoing_line =",
        owner.index("void VoipStack::start_call_attempt_"),
    )
    stop = owner.index("  if (via_ha)", start)
    fragment = owner[start:stop]
    # The enclosing REGISTER gate is supplied by the compiler invocation.
    fragment = fragment[: fragment.rfind("#endif")]
    source = sdp_fixture_source().replace(
        "struct SipTransport {",
        r"""struct SipTransport {
 bool advertised_d1=false;
 std::string registration_account_domain(const std::string&){return "example.org";}
 bool prepare_account_line(const std::string&,std::string*,uint16_t*,bool*,bool *d1=nullptr){if(d1)*d1=advertised_d1;return true;}
 void clear_account_line(){}
""",
    )
    source += (
        r"""
#include <functional>
struct Failure {void trigger(const std::string&,const std::string&) {assert(false);}};
struct Pointer {SipTransport *p;SipTransport *get(){return p;}SipTransport *operator->(){return p;}bool operator!=(std::nullptr_t){return p!=nullptr;}};
struct VoipStack {
 Pointer transport_;std::string pending_line_,default_line_="direct",pending_dialplan_target_;
 bool force_direct_call_=false,configured_d1=false;Failure call_failed_trigger_;
 void defer(std::function<void()> fn){fn();}
 void run(){bool via_ha=false;std::string requested_call_id,dial_ip;uint16_t dial_port=0,dial_rtp_port=0;
 bool dial_sip_tcp=false,dial_directional_audio_v1=configured_d1;
"""
        + fragment
        + r"""
 transport_->peer_directional_audio_v1_=dial_directional_audio_v1;
 }
};
AudioFormat fmt(unsigned rate){AudioFormat f;f.sample_rate=rate;f.frame_ms=10;return f;}
int main(int argc,char **argv){assert(argc==3);const bool d1=std::string(argv[2])=="yes";
 SipTransport transport;transport.advertised_d1=d1;
 transport.offer_tx_formats_.count=1;transport.offer_tx_formats_.formats[0]=fmt(16000);
 transport.offer_rx_formats_.count=2;transport.offer_rx_formats_.formats[0]=fmt(48000);transport.offer_rx_formats_.formats[1]=fmt(16000);
 VoipStack phone{&transport};phone.pending_line_=argv[1];phone.configured_d1=phone.pending_line_=="direct"&&d1;phone.run();
 auto offer=transport.build_sdp_offer_();
 assert((offer.find("a=x-voip-stack-flow:")!=std::string::npos)==d1);
 assert((offer.find("L16/48000/1")!=std::string::npos)==d1);
 assert(offer.find("L16/16000/1")!=std::string::npos);
 assert(offer.find("a=ptime:10")!=std::string::npos);
}
"""
    )
    cpp = tmp_path / "ordinary.cpp"
    cpp.write_text(source)
    binary = tmp_path / "ordinary"
    built = subprocess.run(
        [
            "g++",
            "-std=c++17",
            "-DUSE_ESPHOME_VOIP_REGISTRATION",
            "-I",
            str(ROOT),
            str(cpp),
            "-o",
            str(binary),
        ],
        capture_output=True,
        text=True,
    )
    assert built.returncode == 0, built.stderr
    result = subprocess.run(
        [binary, account, "yes" if directional else "no"],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr


@pytest.fixture(scope="module")
def capability_wire_binary(tmp_path_factory):
    from test_refer_dialog_runtime import block

    folder = tmp_path_factory.mktemp("generic-capability-wire")
    include = folder / "esphome/core/defines.h"
    include.parent.mkdir(parents=True)
    include.write_text("")
    base = ROOT / "esphome/components/voip_stack"
    harness = (ROOT / "tests/fixtures/registration_engine.cpp").read_text()
    code = harness[: harness.index("int main(")]
    code += '\n#include "sip_sdp_features.h"\n'
    code += sdp_fixture_source().replace("SipTransport", "SdpTransport")
    code += r"""
struct LockGuard {explicit LockGuard(int&) {}};
const char *inet_ntoa_r(in_addr a,char *out,size_t n){return inet_ntop(AF_INET,&a,out,n);}
struct SipTransport {
 SipRegistration &registration_;int dialog_mutex_=0,next_account_=-1;std::string call_id_{};
 bool terminal_transaction_pending_locked_()const{return false;}
 static bool registration_directional_audio_supported_(std::string_view);
 bool prepare_account_line(const std::string&,std::string*,uint16_t*,bool*,bool* =nullptr);
};
"""
    adapter = (base / "sip_registration_transport.cpp").read_text()
    code += block(
        adapter, "bool SipTransport::registration_directional_audio_supported_("
    )
    code += block(adapter, "bool SipTransport::prepare_account_line(")
    code += r"""
bool quiet(Peer &p){char c;return recv(p.peer,&c,1,MSG_DONTWAIT)<0;}
AudioFormat pcm(unsigned rate){AudioFormat f;f.sample_rate=rate;f.frame_ms=10;return f;}
std::string caps(bool d1){return std::string("v=0\r\no=- 1 1 IN IP4 192.0.2.1\r\ns=Ordinary peer\r\nc=IN IP4 192.0.2.1\r\nt=0 0\r\n")+
 "m=audio 0 RTP/AVP 96 97\r\na=rtpmap:96 L16/16000/1\r\na=rtpmap:97 L16/48000/1\r\n"+
 (d1?"a=x-voip-stack-flow:96 sendrecv\r\na=x-voip-stack-flow:97 recv\r\n":"");}
std::string body_response(const std::string &request,const std::string &body,unsigned status=200,const std::string&type="application/sdp"){
 auto result=response(request,status,"Content-Type: "+type+"\r\n");auto at=result.find("Content-Length: 0");
 result.replace(at,17,"Content-Length: "+std::to_string(body.size()));return result+body;
}
bool offer(SipTransport &line,const std::string &id,bool expected){
 std::string host;uint16_t port=0;bool tcp=false,d1=false;
 assert(line.prepare_account_line(id,&host,&port,&tcp,&d1));assert(d1==expected);
 SdpTransport s;s.peer_directional_audio_v1_=d1;
 s.offer_tx_formats_.count=1;s.offer_tx_formats_.formats[0]=pcm(16000);
 s.offer_rx_formats_.count=2;s.offer_rx_formats_.formats[0]=pcm(48000);s.offer_rx_formats_.formats[1]=pcm(16000);
 auto wire=s.build_sdp_offer_();assert(!wire.empty());
 assert((wire.find("L16/48000/1")!=std::string::npos)==expected);
 assert((wire.find("a=x-voip-stack-flow:")!=std::string::npos)==expected);
 // The answer is independently framed and remains authoritative.
 const std::string body="v=0\r\nc=IN IP4 192.0.2.1\r\nm=audio 40002 RTP/AVP 96"+
  std::string(expected?" 97":"")+"\r\na=rtpmap:96 L16/"+std::string(expected?"48000":"16000")+"/1\r\n"+
  (expected?"a=rtpmap:97 L16/16000/1\r\na=x-voip-stack-flow:96 send\r\na=x-voip-stack-flow:97 recv\r\n":"")+
  "a=ptime:10\r\na=sendrecv\r\n";
 assert(s.learn_remote_rtp_from_sdp_(body,0,true));
 assert(s.tx.sample_rate==16000 && s.rx.sample_rate==(expected?48000:16000));return d1;
}
int main(int argc,char**argv){assert(argc==3);const std::string scenario=argv[1];Harness h(std::string(argv[2])=="tcp");
 h.hooks.directional_audio_supported=SipTransport::registration_directional_audio_supported_;
 h.manager.set_hooks(h.hooks);auto config=h.config("VideoPhoneDemo");config.username="ordinary-user";
 assert(h.manager.set_account(config,0));h.manager.tick(0);auto registration=h.first.receive();h.feed(h.first,h.success(registration),1);
 SipTransport line{h.manager};assert(h.manager.is_registered(config.id));
 offer(line,config.id,false); // Immediate call works before the optional query.
 if(scenario=="send-failure"){
  h.hooks.send=[](void*,size_t,std::string_view,uint32_t,uint16_t,bool){return false;};h.manager.set_hooks(h.hooks);
  h.manager.tick(2);assert(h.manager.is_registered(config.id)&&quiet(h.first));offer(line,config.id,false);return 0;
 }
 h.manager.tick(2);auto query=h.first.receive();assert(query.rfind("OPTIONS ",0)==0);
 offer(line,config.id,false);
 assert(field(query,"Accept")=="application/sdp"&&field(query,"Content-Length")=="0");
 if(scenario=="timeout"){
  h.manager.tick(32002);assert(h.manager.is_registered(config.id));assert(quiet(h.first));
  offer(line,config.id,false);return 0;
 }
 std::string body=caps(scenario!="legacy"&&scenario!="401"&&scenario!="405"&&scenario!="empty"&&scenario!="keepalive-legacy");
 if(scenario=="empty")body.clear();
 if(scenario=="bad-payload")body="v=0\r\nm=audio 0 RTP/AVP 96\r\na=x-voip-stack-flow:119 recv\r\n";
 if(scenario=="bad-flow")body="v=0\r\nm=audio 0 RTP/AVP 96\r\na=x-voip-stack-flow:96 duplex\r\n";
 if(scenario=="malformed")body="v=0\r\nm=video 0 RTP/AVP 96\r\na=x-voip-stack-flow:96 send\r\n";
 auto answer=body_response(query,body,scenario=="405"?405:scenario=="401"?401:200,scenario=="type"?"text/plain":"application/sdp");
 if(scenario=="identity"){
  auto wrong=answer;auto at=wrong.find("z9hG4bK");wrong[at+7]='x';h.feed(h.first,wrong,3);
  assert((h.manager.account(0).pending && h.manager.account(0).request_kind==SipRegistration::RequestKind::CAPABILITIES));offer(line,config.id,false);
  assert(h.manager.handle_response(answer,INADDR_LOOPBACK,h.first.port+1,h.first.tcp,h.first.tcp?0:-1,3));
  assert((h.manager.account(0).pending && h.manager.account(0).request_kind==SipRegistration::RequestKind::CAPABILITIES));
 }
 h.feed(h.first,answer,4);
 const bool supported=scenario!="legacy"&&scenario!="401"&&scenario!="405"&&scenario!="empty"&&scenario!="malformed"&&scenario!="type"&&scenario!="keepalive-legacy"&&scenario!="bad-payload"&&scenario!="bad-flow";
 offer(line,config.id,supported);assert(h.manager.is_registered(config.id));
 assert(!h.manager.account(0).pending&&h.manager.account(0).state==RegistrationState::REGISTERED);
 if(scenario=="multiple"){
  auto second=h.config("second-ordinary",1);assert(h.manager.set_account(second,5));h.manager.tick(5);
  auto reg2=h.second.receive();h.feed(h.second,h.success(reg2),6,1);h.manager.tick(7);
  auto query2=h.second.receive();h.feed(h.second,answer,8,1);
  assert((h.manager.account(1).pending && h.manager.account(1).request_kind==SipRegistration::RequestKind::CAPABILITIES));offer(line,config.id,true);
  h.feed(h.second,body_response(query2,caps(false)),9,1);offer(line,second.id,false);offer(line,config.id,true);
 }else if(scenario=="renew"){
  const auto refresh_at=h.manager.account(0).next_ms;h.manager.tick(refresh_at);auto refresh=h.first.receive();
  assert(refresh.rfind("REGISTER ",0)==0);h.feed(h.first,h.success(refresh),refresh_at+1);
  offer(line,config.id,true);h.manager.tick(refresh_at+2);assert(quiet(h.first));
 }else if(scenario=="keepalive"||scenario=="keepalive-legacy"){
  h.manager.set_keepalive_interval(2000);h.manager.tick(5);auto keep=h.first.receive();
  assert(field(keep,"Accept").empty());h.feed(h.first,body_response(keep,caps(!supported)),6);
  offer(line,config.id,supported);
 }else if(scenario=="expiry"){
  h.manager.tick(3600002);assert(!h.manager.is_registered(config.id));
  assert(!h.manager.account(0).peer_directional_audio_v1);
 }else if(scenario=="rebind"){
  h.manager.connection_lost(0,5);assert(!h.manager.account(0).peer_directional_audio_v1);
  h.manager.tick(5);auto refresh=h.first.receive();h.feed(h.first,h.success(refresh),6);
  offer(line,config.id,false);h.manager.tick(7);auto next=h.first.receive();h.feed(h.first,body_response(next,caps(false)),8);
  offer(line,config.id,false);
 }else if(scenario=="replace"){
  auto newer=config;newer.username="replaced-user";assert(h.manager.set_account(newer,5));
  assert(!h.manager.account(0).peer_directional_audio_v1);
  assert(!h.manager.handle_response(answer,INADDR_LOOPBACK,h.first.port,h.first.tcp,h.first.tcp?0:-1,6));
  assert(!h.manager.account(0).peer_directional_audio_v1);
 }else if(scenario=="unregister"){
  assert(h.manager.request_registration(config.id,false,5));assert(!h.manager.account(0).peer_directional_audio_v1);
  assert(!h.manager.is_registered(config.id));h.manager.tick(5);auto remove=h.first.receive();assert(field(remove,"Expires")=="0");
  h.feed(h.first,h.success(remove,0),6);assert(!h.manager.account(0).bound);
 }
 std::cout<<"account_bytes="<<sizeof(SipRegistration::Account)<<" hooks_bytes="<<sizeof(SipRegistration::Hooks)<<"\n";
}
"""
    cpp = folder / "wire.cpp"
    cpp.write_text(code)
    binary = folder / "wire"
    result = subprocess.run(
        [
            "g++",
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-DUSE_ESPHOME_VOIP_REGISTRATION",
            "-I",
            str(folder),
            "-I",
            str(ROOT),
            "-I",
            str(base),
            str(cpp),
            str(base / "sip_registration.cpp"),
            "-lcrypto",
            "-o",
            str(binary),
        ],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("transport", ["udp", "tcp"])
@pytest.mark.parametrize(
    "scenario",
    [
        "d1",
        "legacy",
        "empty",
        "401",
        "405",
        "renew",
        "timeout",
        "identity",
        "malformed",
        "type",
        "expiry",
        "rebind",
        "replace",
        "unregister",
        "keepalive",
        "keepalive-legacy",
        "multiple",
        "send-failure",
        "bad-payload",
        "bad-flow",
    ],
)
def test_real_registration_query_flow_and_authoritative_offer(
    capability_wire_binary, transport, scenario
):
    result = subprocess.run(
        [capability_wire_binary, scenario, transport],
        capture_output=True,
        text=True,
        timeout=10,
    )
    assert result.returncode == 0, result.stderr
