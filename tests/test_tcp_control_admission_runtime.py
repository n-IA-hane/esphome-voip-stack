"""TCP storage admission stays separate from actual INVITE/subscription policy."""

from pathlib import Path
import subprocess

import pytest

from test_refer_dialog_runtime import SOURCE, block
from test_sip_registration_transport import registration_transport_source


@pytest.fixture(scope="module")
def control_admission_binary(tmp_path_factory, request):
    folder = tmp_path_factory.mktemp("tcp-control-admission")
    defines = folder / "esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    code = registration_transport_source("MWI" in request.param, "BLF" in request.param)
    gate = "bool should_accept_session_()const{return true;}"
    assert gate in code
    code = code.replace(gate, "bool app_accept=true;bool should_accept_session_()const{return app_accept;}")
    code += "\n" + block((SOURCE / "sip_transport.cpp").read_text(), "void SipTransport::accept_tcp_client_(")
    peers = (Path(__file__).parent / "fixtures/refer_wire_main.cpp").read_text()
    code += "\n" + peers[:peers.index("std::string begin(")]
    account = (Path(__file__).parent / "fixtures/registration_transport.cpp").read_text()
    code += "\n" + account[:account.index("std::string account_refer(")]
    code += r'''
std::string notification(const std::string &subscribe,unsigned sequence,bool blf){
 const std::string body=blf?
  "<dialog-info xmlns='urn:ietf:params:xml:ns:dialog-info' version='0' state='full' entity='sip:101@pbx.example'><dialog id='one'><state>early</state></dialog></dialog-info>":
  "Messages-Waiting: yes\r\nVoice-Message: 2/1 (0/0)\r\n";
 return "NOTIFY "+strip_angle_uri(header_value(subscribe,"Contact"))+" SIP/2.0\r\n"+
  "Via: SIP/2.0/TCP 127.0.0.1;branch=z9hG4bKnotify"+std::to_string(sequence)+"\r\nFrom: "+
  header_value(subscribe,"To")+";tag=destination\r\nTo: "+header_value(subscribe,"From")+
  "\r\nCall-ID: "+header_value(subscribe,"Call-ID")+"\r\nCSeq: "+std::to_string(sequence)+
  " NOTIFY\r\nContact: <sip:notifier@127.0.0.1>\r\nEvent: "+(blf?"dialog":"message-summary")+"\r\nSubscription-State: active;expires=3600\r\n"+
  "Content-Type: "+(blf?"application/dialog-info+xml":"application/simple-message-summary")+
  "\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
}
void expect_status(Peer &peer,unsigned status){
 const auto replies=peer.drain();assert(replies.size()==1);
 if(replies[0].rfind("SIP/2.0 "+std::to_string(status),0)!=0)
  fprintf(stderr,"expected status %u, received %s\n",status,replies[0].substr(0,replies[0].find("\r\n")).c_str());
 assert(replies[0].rfind("SIP/2.0 "+std::to_string(status),0)==0);
}
int main(int argc,char **argv){
 assert(argc==2);const std::string scenario=argv[1];SipTransport s;Peer pbx(true);bind_account(s,pbx);
 s.call_id_.clear();s.media_active_=false;
 if(scenario=="subscription-call" || scenario=="subscription-idle"){
#if defined(USE_ESPHOME_VOIP_MWI) || defined(USE_ESPHOME_VOIP_BLF)
  SipMwi::Hooks hooks;hooks.context=&s;hooks.send=SipTransport::registration_send_view_;
  hooks.reply=SipTransport::registration_mwi_reply_;hooks.random=esp_random;hooks.hash=SipTransport::registration_hash_;
  hooks.local_ip=[](void*,uint32_t){return std::string("127.0.0.1");};hooks.local_port=s.sip_port_;
  s.mwi_.set_hooks(hooks);const auto &account=s.registration_.account(0);
#ifdef USE_ESPHOME_VOIP_MWI
  constexpr bool blf=false;s.mwi_.update_registered_account(0,account,clock_ms);
#else
  constexpr bool blf=true;assert(s.mwi_.configure_blf(1));
  assert(s.mwi_.set_blf_watch(0,0,account,"kitchen","sip:101@pbx.example",clock_ms));
#endif
  s.mwi_.tick(clock_ms);auto records=pbx.drain();assert(records.size()==1);
  auto subscribe=records[0];pbx.send_to(s,response(subscribe,200));
  Peer initial(true);s.accept_tcp_client_(initial.esp,initial.address);
  initial.send_to(s,notification(subscribe,1,blf));expect_status(initial,200);
  assert(s.tcp_flow_.socket==initial.esp);
  if(scenario=="subscription-call"){
   confirm_account_call(s,pbx);s.app_accept=false;
   assert(s.active_account_==0&&s.registration_flows_[0].socket==pbx.esp);
  }
  // The first control connection expires independently while the account call
  // stays on its registration socket, exactly as in the physical TCP witness.
  const auto deadline=s.tcp_flow_.ingress_deadline.load();assert(deadline!=0);
  clock_ms=deadline;s.handle_tcp_peer_loss_(&s.tcp_flow_,deadline);
  assert(s.tcp_flow_.socket<0&&s.inbound_tcp_flow_.socket<0);
  const auto call=s.call_id_;const bool media=s.media_active_;
  s.mwi_.on_registration_accepted(0,account,clock_ms);s.mwi_.tick(clock_ms);
  records=pbx.drain();assert(records.size()==1);subscribe=records[0];
  assert(header_value(subscribe,"CSeq")=="2 SUBSCRIBE");pbx.send_to(s,response(subscribe,200));
  Peer renewed(true);assert(renewed.address.sin_port!=initial.address.sin_port);
  s.accept_tcp_client_(renewed.esp,renewed.address);
  assert(s.tcp_flow_for_socket_(renewed.esp)!=nullptr&&"valid NOTIFY connection rejected before SIP dispatch");
  renewed.send_to(s,notification(subscribe,2,blf));expect_status(renewed,200);
  assert(s.call_id_==call&&s.media_active_==media&&s.registration_flows_[0].socket==pbx.esp);
  if(scenario=="subscription-call")assert(s.tcp_flow_.socket<0&&s.inbound_tcp_flow_.socket<0);
#ifdef USE_ESPHOME_VOIP_MWI
  assert(s.mwi_.state(0).available&&s.mwi_.state(0).new_messages==2);
#else
  assert(s.mwi_.blf_state(0)->available&&s.mwi_.blf_state(0)->state==esphome::voip_stack::BlfState::RINGING);
#endif
#endif
 }else if(scenario=="direct-primary"){
  Peer original(true);setup(s,original);s.app_accept=false;
  Peer control(true);s.accept_tcp_client_(control.esp,control.address);
  assert(s.inbound_tcp_flow_.socket==control.esp&&s.tcp_flow_.socket==original.esp);
  control.send_to(s,request(control,"OPTIONS",1,"","probe","","probe"));expect_status(control,200);
  assert(s.call_id_=="original"&&s.media_active_&&s.tcp_flow_.socket==original.esp);
  assert(s.inbound_tcp_flow_.socket<0&&fcntl(original.esp,F_GETFD)>=0);
 }else if(scenario=="partial-primary" || scenario=="unread-primary"){
  Peer first(true);s.accept_tcp_client_(first.esp,first.address);
  const auto options=request(first,"OPTIONS",1,"","probe","","probe");
  const auto prefix=options.substr(0,8);
  if(scenario=="partial-primary"){
   assert(send(first.fd,prefix.data(),prefix.size(),0)==ssize_t(prefix.size()));
   s.handle_sip_stream_(first.esp,first.address);assert(s.tcp_flow_.rx_buffer==prefix);
  }else assert(send(first.fd,options.data(),options.size(),0)==ssize_t(options.size()));
  s.app_accept=false;Peer second(true);s.accept_tcp_client_(second.esp,second.address);
  assert(s.tcp_flow_.socket==first.esp&&s.inbound_tcp_flow_.socket==second.esp);
  Peer third(true);s.accept_tcp_client_(third.esp,third.address);
  assert(fcntl(third.esp,F_GETFD)<0&&errno==EBADF);
  assert(s.tcp_flow_.socket==first.esp&&s.inbound_tcp_flow_.socket==second.esp);
  if(scenario=="partial-primary")assert(s.tcp_flow_.rx_buffer==prefix);
  else assert(!s.tcp_flow_.received_record);
  second.send_to(s,request(second,"OPTIONS",1,"","second","","probe"));expect_status(second,200);
  assert(s.tcp_flow_.socket==first.esp&&fcntl(first.esp,F_GETFD)>=0&&s.call_id_.empty());
 }else{
  s.app_accept=false;Peer incoming(true);s.accept_tcp_client_(incoming.esp,incoming.address);
  assert(s.tcp_flow_for_socket_(incoming.esp)!=nullptr&&"busy application rejected TCP before method admission");
  if(scenario=="busy-options"){
   incoming.send_to(s,request(incoming,"OPTIONS",1,"","probe","","probe"));expect_status(incoming,200);
  }else{
   assert(scenario=="busy-invite");
   auto invite=request(incoming,"INVITE",1,"","unowned","","caller");
   invite.replace(invite.find("To: <sip:esp@127.0.0.1>;tag=\r\n"),strlen("To: <sip:esp@127.0.0.1>;tag=\r\n"),"To: <sip:esp@127.0.0.1>\r\n");
   incoming.send_to(s,invite);expect_status(incoming,486);
  }
  assert(s.call_id_.empty()&&!s.media_active_&&s.active_account_==-1);
  assert(s.registration_flows_[0].socket==pbx.esp&&s.registration_.is_registered("office"));
 }
 s.close_tcp_flow_(s.tcp_flow_);s.close_tcp_flow_(s.inbound_tcp_flow_);
 for(auto &flow:s.registration_flows_)s.close_tcp_flow_(flow);close(s.sip_socket_);
}
'''
    cpp = folder / "control.cpp"
    cpp.write_text(code)
    binary = folder / "control"
    built = subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra", "-g", "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer", "-fno-pie", "-no-pie", "-DUSE_ESPHOME_VOIP_REGISTRATION",
        *[f"-DUSE_ESPHOME_VOIP_{feature}" for feature in request.param],
        "-I", str(folder), "-I", str(SOURCE.parents[2]), "-I", str(SOURCE), str(cpp),
        str(SOURCE / "sip_registration.cpp"), str(SOURCE / "sip_mwi.cpp"),
        str(SOURCE / "sip_blf.cpp"), "-lcrypto", "-o", str(binary),
    ], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize(
    ("control_admission_binary", "scenario"),
    [pytest.param(features, scenario, id=f"{name}-{scenario}")
     for name, features in [("neither", ()), ("mwi-only", ("MWI",)),
                            ("blf-only", ("BLF",)), ("both", ("MWI", "BLF"))]
     for scenario in ["subscription-idle", "subscription-call", "busy-options", "busy-invite",
                      "direct-primary", "partial-primary", "unread-primary"]
     if features or not scenario.startswith("subscription-")],
    indirect=["control_admission_binary"],
)
def test_control_tcp_admission_preserves_method_owners(control_admission_binary, scenario):
    result = subprocess.run([control_admission_binary, scenario], capture_output=True, text=True, timeout=8)
    assert result.returncode == 0, result.stdout + result.stderr
