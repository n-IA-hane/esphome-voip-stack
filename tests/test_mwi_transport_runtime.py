"""MWI and REGISTER share actual ingress, bytes and account-flow retirement."""
from pathlib import Path
import re
import subprocess
import pytest
from test_refer_dialog_runtime import SOURCE, ROOT, block
from test_sip_registration_transport import registration_transport_source


@pytest.fixture(scope="module")
def mwi_transport_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("mwi-transport")
    core = folder / "esphome/core"
    core.mkdir(parents=True)
    (core / "defines.h").write_text("")
    code = registration_transport_source(full_invite=True)
    adapter = (SOURCE / "sip_registration_transport.cpp").read_text()
    for marker in ["void SipTransport::registration_binding_accepted_(", "void SipTransport::pump_registration_mwi_(",
                   "bool SipTransport::registration_mwi_reply_(", "void SipTransport::registration_mwi_notify_(",
                   "std::string SipTransport::registration_local_ip_(", "bool SipTransport::copy_message_waiting_slot("]:
        code += "\n" + block(adapter, marker)
    peers = (ROOT / "tests/fixtures/refer_wire_main.cpp").read_text()
    code += "\n" + peers[:peers.index("std::string begin(")]
    account = (ROOT / "tests/fixtures/registration_transport.cpp").read_text()
    account_setup = account[:account.index("void start_account_call(")]
    account_setup = account_setup.replace("hooks.random = esp_random;", "hooks.binding_accepted=SipTransport::registration_binding_accepted_;hooks.random = esp_random;")
    account_setup = account_setup.replace("config.port = ntohs(peer.address.sin_port);", "config.port = ntohs(peer.address.sin_port);config.expires=60;")
    code += "\n" + account_setup
    code += r'''
std::string notification(const std::string&subscribe,unsigned sequence,const std::string&state="active;expires=60"){
 const std::string body=state.rfind("terminated",0)==0?"":"Messages-Waiting: yes\r\nMessage-Account: sip:mailbox.esphome%3Adevice123@pbx.example\r\nVoice-Message: 2/0\r\n";
 return "NOTIFY "+name_addr_uri(header_value(subscribe,"Contact"))+" SIP/2.0\r\nVia: SIP/2.0/UDP 127.0.0.1;branch=z9hG4bKn"+std::to_string(sequence)+
 "\r\nFrom: <sip:mailbox@pbx.example>;tag=server\r\nTo: "+header_value(subscribe,"From")+
 "\r\nCall-ID: "+header_value(subscribe,"Call-ID")+"\r\nCSeq: "+std::to_string(sequence)+" NOTIFY\r\nEvent: message-summary\r\nSubscription-State: "+state+
 "\r\nContact: <sip:mailbox@127.0.0.1>\r\nContent-Type: application/simple-message-summary\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
}
std::string one(Peer&p,const std::string&prefix){auto records=p.drain();assert(records.size()==1&&records[0].rfind(prefix,0)==0);return records[0];}
int main(int argc,char**argv){
 assert(argc==3);const bool tcp=std::string(argv[1])=="tcp";const std::string mode=argv[2];SipTransport phone;Peer peer(tcp);bind_account(phone,peer);
 SipMwi::Hooks hooks;hooks.context=&phone;hooks.send=SipTransport::registration_send_view_;hooks.reply=SipTransport::registration_mwi_reply_;
 hooks.local_ip=SipTransport::registration_local_ip_;hooks.random=esp_random;hooks.hash=SipTransport::registration_hash_;hooks.local_port=phone.sip_port_;
 phone.mwi_.set_hooks(hooks);phone.pump_registration_mwi_(clock_ms);const auto subscribe=one(peer,"SUBSCRIBE ");
 const auto original_fd=tcp?phone.registration_flows_[0].socket.load():phone.sip_socket_;
 const auto event=notification(subscribe,1);
 peer.send_to(phone,event);one(peer,"SIP/2.0 200");
 assert(phone.mwi_.state(0).available&&phone.mwi_.state(0).new_messages==2);
 assert(phone.mwi_.state(0).message_account==std::string("sip:mailbox.esphome%3Adevice123@pbx.example"));
 peer.send_to(phone,response(subscribe,200));assert(peer.drain().empty());
 if(mode=="refresh"||mode=="keepalive"){
  auto&account=phone.registration_.account(0);
  clock_ms=mode=="refresh"?account.next_ms:account.keepalive_next_ms;
  phone.registration_.tick(clock_ms);const auto transaction=one(peer,mode=="refresh"?"REGISTER ":"OPTIONS ");
  auto success=response(transaction,200);
  if(mode=="refresh")success.insert(success.find("Content-Length:"),"Contact: "+header_value(transaction,"Contact")+"\r\n");
  peer.send_to(phone,success);phone.pump_registration_mwi_(clock_ms);
  if(mode=="keepalive")assert(peer.drain().empty());
  else{
   const auto refreshed=one(peer,"SUBSCRIBE ");assert(header_value(refreshed,"Call-ID")==header_value(subscribe,"Call-ID"));
   assert(cseq_number(header_value(refreshed,"CSeq"))>cseq_number(header_value(subscribe,"CSeq")));
  }
 }else if(mode=="duplicate"){
  peer.send_to(phone,event);one(peer,"SIP/2.0 200");assert(phone.mwi_.state(0).new_messages==2);
  auto out_of_order=notification(subscribe,1);out_of_order.replace(out_of_order.find("z9hG4bKn1"),9,"z9hG4bKother");
  peer.send_to(phone,out_of_order);one(peer,"SIP/2.0 500");
 }else{
  assert(phone.register_account("office",false));phone.registration_.tick(clock_ms);
  const auto unregister=one(peer,"REGISTER ");peer.send_to(phone,response(unregister,200));
  assert(!phone.registration_.account(0).bound);phone.pump_registration_mwi_(clock_ms);
  const auto cancel=one(peer,"SUBSCRIBE ");assert(header_value(cancel,"Expires")=="0");
  assert(header_value(cancel,"Call-ID")==header_value(subscribe,"Call-ID"));
  auto replacement=phone.registration_.account(0).config;replacement.password="replacement";replacement.enabled=false;
  assert(!phone.registration_subscriptions_retired_(0));assert(!phone.set_account(replacement)&&!phone.remove_account("office"));
  assert(phone.registration_.account(0).config.password=="test-only-password");
  assert((tcp?phone.registration_flows_[0].socket.load():phone.sip_socket_)==original_fd);
  if(mode=="timeout"){
   clock_ms+=32001;phone.pump_registration_mwi_(clock_ms);
  }else if(mode=="flow-failure"){
   assert(tcp);phone.handle_tcp_peer_loss_(&phone.registration_flows_[0]);
  }else{
   peer.send_to(phone,response(cancel,200));assert(peer.drain().empty());assert(!phone.registration_subscriptions_retired_(0));
   peer.send_to(phone,notification(subscribe,2,"terminated;reason=timeout"));one(peer,"SIP/2.0 200");
  }
  assert(phone.registration_subscriptions_retired_(0));
  assert(phone.remove_account("office"));assert(phone.registration_.find("office")==-1);
  assert(phone.set_account(replacement));assert(phone.registration_.account(0).config.password=="replacement");
 }
 for(auto&flow:phone.registration_flows_)phone.close_tcp_flow_(flow);close(phone.sip_socket_);
}
'''
    code = code.replace("using SipTcpFlow=::SipTcpFlow;", "")
    code = code.replace("struct SipTransport: SipPhoneTransport {", "struct SipTransport: SipPhoneTransport {\nusing SipTcpFlow=::SipTcpFlow;", 1)
    # Retain use of log arguments, as production logging does.
    code = "template<class...T>void host_log(T&&...values){((void)values,...);}\n" + code
    for name in ["ESP_LOGW", "ESP_LOGE", "ESP_LOGD", "ESP_LOGI"]:
        code = re.sub(rf"^#define {name}\(\.\.\.\).*$", f"#define {name}(...) host_log(__VA_ARGS__)", code, flags=re.MULTILINE)
    cpp = folder / "mwi-transport.cpp"
    cpp.write_text(code)
    binary = folder / "mwi-transport"
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation", "-g",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_ESPHOME_VOIP_REGISTRATION", "-DUSE_ESPHOME_VOIP_MWI",
        "-I", str(folder), "-I", str(SOURCE), "-I", str(SOURCE.parents[2]), str(cpp), str(SOURCE / "sip_mwi.cpp"),
        str(SOURCE / "sip_registration.cpp"), "-lcrypto", "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("transport,mode", [(p,m) for p in ["udp","tcp"] for m in ["duplicate","terminal","timeout","refresh","keepalive"]]+[("tcp","flow-failure")])
def test_subscription_retires_before_account_credentials_or_flow_reuse(mwi_transport_binary, transport, mode):
    result = subprocess.run([mwi_transport_binary, transport, mode], capture_output=True, text=True, timeout=8)
    assert result.returncode == 0, result.stderr
