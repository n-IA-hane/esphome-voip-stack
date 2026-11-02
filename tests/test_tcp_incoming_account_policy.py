"""Actual account INVITE admission, dialog identity and socket ownership."""

from pathlib import Path
import subprocess

import pytest

from test_refer_dialog_runtime import SOURCE, block
from test_sip_registration_transport import registration_transport_source


def incoming_account_source():
    source = (SOURCE / "sip_transport.cpp").read_text()
    code = registration_transport_source(full_invite=True)
    code += "\n" + block(source, "void SipTransport::accept_tcp_client_(")
    helpers = (Path(__file__).parent / "fixtures/refer_wire_main.cpp").read_text()
    helpers = helpers[:helpers.index("std::string begin(")]
    registration = (Path(__file__).parent / "fixtures/registration_transport.cpp").read_text()
    registration = registration[:registration.index("void start_account_call(")]
    code += "\n" + helpers
    code += "\n" + registration
    return code


@pytest.fixture(scope="module")
def incoming_account_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("tcp-incoming-account")
    defines = folder / "esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    code = incoming_account_source() + r'''
int main(int argc,char**argv){
 assert(argc==3);const std::string scenario=argv[1];Peer registrar(std::string(argv[2])=="tcp");
 SipTransport s;bind_account(s,registrar);
 s.call_id_.clear();s.local_tag_.clear();s.remote_tag_.clear();s.media_active_=false;
 s.accept_direct_calls_=false;
 Peer options(true),caller(true);
 s.accept_tcp_client_(options.esp,options.address);
 options.send_to(s,request(options,"OPTIONS",1,"","probe","","probe"));
 auto probe=options.drain();assert(probe.size()==1&&probe[0].rfind("SIP/2.0 200",0)==0);
 const int untouched=s.tcp_flow_.socket;
 assert(untouched==options.esp && s.call_id_.empty());
 s.accept_tcp_client_(caller.esp,caller.address);
 assert(s.inbound_tcp_flow_.socket==caller.esp);
 std::string invite=request(caller,"INVITE",1,"","incoming-account","","caller");
 invite.replace(invite.find("To: <sip:esp@127.0.0.1>;tag=\r\n"),
                strlen("To: <sip:esp@127.0.0.1>;tag=\r\n"),"To: <sip:esp@127.0.0.1>\r\n");
 const auto at=invite.find("sip:esp@127.0.0.1");assert(at!=std::string::npos);
 invite.replace(at,strlen("sip:esp@127.0.0.1"),"sip:esphome-office@127.0.0.1");
 // A body delegates only SDP/media admission to the controlled media adapter.
 invite.replace(invite.find("Content-Length: 0"),strlen("Content-Length: 0"),"Content-Length: 3");
 invite+="v=0";
 auto incoming=caller.address;
 if(scenario=="unknown-user")invite.replace(invite.find("esphome-office"),14,"unknown-user");
 if(scenario=="foreign-ip")incoming.sin_addr.s_addr=htonl(0x7f000002);
 if(scenario=="wrong-flow")s.registration_flows_[1].socket=caller.esp;
 if(scenario=="unregistered")s.registration_.account(0).desired=false;
 if(scenario=="replaced"){
  auto config=s.registration_.account(0).config;config.password="new-fixture-value";
  assert(s.registration_.set_account(config,clock_ms));
  assert(!s.registration_.account(0).bound);
  assert(s.registration_.account(0).config.password=="new-fixture-value");
 }
 assert(send(caller.fd,invite.data(),invite.size(),MSG_NOSIGNAL)==ssize_t(invite.size()));
 s.handle_sip_stream_(caller.esp,incoming);
 auto replies=caller.drain();
 if(scenario!="accepted"){
  assert(replies.size()==1&&replies[0].rfind("SIP/2.0 403",0)==0);
  assert(s.call_id_.empty()&&s.active_account_==-1&&s.tcp_flow_.socket==untouched);
  assert(fcntl(options.esp,F_GETFD)>=0&&s.inbound_tcp_flow_.socket<0);
  if(scenario=="wrong-flow")s.registration_flows_[1].socket=-1;
 }else{
  assert(replies.size()==1&&replies[0].rfind("SIP/2.0 100",0)==0);
  assert(s.call_id_=="incoming-account"&&s.active_account_==0);
  assert(s.dialog_account_socket_==caller.esp&&s.tcp_flow_.socket==caller.esp);
  assert(s.inbound_tcp_flow_.socket==untouched&&fcntl(untouched,F_GETFD)>=0);
  assert(s.tcp_flow_.ingress_deadline==0);
  assert(s.registration_.account(0).config.tcp==registrar.tcp);
  assert(s.send_response_(200,"OK"));auto answered=caller.drain();assert(answered.size()==1);
  const auto tag=tag_from_header(header_value(answered[0],"To"));assert(!tag.empty());
  const auto contact=strip_angle_uri(header_value(answered[0],"Contact"));
  assert(contact.find("esphome-office@")!=std::string::npos);
  const bool contact_udp=contact.find("transport=udp")!=std::string::npos;
  assert(contact_udp!=registrar.tcp);
  Peer datagram(false);Peer&dialog_peer=contact_udp?datagram:caller;
  auto in_dialog=[&](const char*method,unsigned sequence){
   auto message=request(dialog_peer,method,sequence,"","incoming-account",tag,"caller");
   message.replace(message.find("sip:esp@127.0.0.1"),strlen("sip:esp@127.0.0.1"),contact);
   dialog_peer.send_to(s,message);
  };
  // A 2xx ACK is a separate dialog request and follows the advertised Contact.
  in_dialog("ACK",1);assert(!s.completed_invite_.awaiting_ack&&s.media_active_);
  in_dialog("BYE",2);
  auto ended=dialog_peer.drain();assert(ended.size()==1&&ended[0].rfind("SIP/2.0 200",0)==0);
  assert(s.call_id_.empty()&&!s.media_active_&&s.registration_.is_registered("office"));
 }
 s.close_tcp_flow_(s.tcp_flow_);s.close_tcp_flow_(s.inbound_tcp_flow_);
 for(auto&flow:s.registration_flows_)s.close_tcp_flow_(flow);
 close(s.sip_socket_);
}
'''
    cpp = folder / "incoming.cpp"
    cpp.write_text(code)
    binary = folder / "incoming"
    built = subprocess.run([
        "g++", "-std=c++17", "-g", "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
        "-DUSE_ESPHOME_VOIP_REGISTRATION", "-I", str(folder), "-I", str(SOURCE.parents[2]),
        "-I", str(SOURCE), str(cpp), str(SOURCE / "sip_registration.cpp"), "-lcrypto", "-o", str(binary),
    ], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize("registered_transport", ["udp", "tcp"])
@pytest.mark.parametrize("scenario", ["accepted", "unknown-user", "foreign-ip", "wrong-flow", "unregistered", "replaced"])
def test_current_account_owns_authorized_tcp_invite_only(incoming_account_binary, registered_transport, scenario):
    result = subprocess.run([incoming_account_binary, scenario, registered_transport],
                            capture_output=True, text=True, timeout=8)
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.fixture(scope="module")
def mixed_ingress_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("mixed-ingress")
    defines = folder / "esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    code = incoming_account_source() + r'''
std::string initial(Peer&peer,const std::string&id){
 auto message=request(peer,"INVITE",1,"",id,"","caller");
 message.replace(message.find("To: <sip:esp@127.0.0.1>;tag=\r\n"),
                 strlen("To: <sip:esp@127.0.0.1>;tag=\r\n"),"To: <sip:esp@127.0.0.1>\r\n");
#ifdef USE_ESPHOME_VOIP_REGISTRATION
 message.replace(message.find("sip:esp@127.0.0.1"),strlen("sip:esp@127.0.0.1"),"sip:esphome-office@127.0.0.1");
#endif
 message.replace(message.find("Content-Length: 0"),strlen("Content-Length: 0"),"Content-Length: 3");
 return message+"v=0";
}
void provisional(Peer&peer){auto records=peer.drain();assert(records.size()==1&&records[0].rfind("SIP/2.0 100",0)==0);}
std::string confirm(SipTransport&s,Peer&peer,const std::string&id){
 assert(s.send_response_(200,"OK"));auto records=peer.drain();
 assert(records.size()==1&&records[0].rfind("SIP/2.0 200",0)==0);
 const auto tag=tag_from_header(header_value(records[0],"To"));assert(!tag.empty());
 peer.send_to(s,request(peer,"ACK",1,"",id,tag,"caller"));return tag;
}
void hangup(SipTransport&s,Peer&peer,const std::string&id,const std::string&tag){
 peer.send_to(s,request(peer,"BYE",2,"",id,tag,"caller"));auto records=peer.drain();
 assert(records.size()==1&&records[0].rfind("SIP/2.0 200",0)==0);
 assert(s.call_id_.empty()&&!s.media_active_);
}
int main(int argc,char**argv){
 assert(argc==2);const std::string scenario=argv[1];SipTransport s;
#ifdef USE_ESPHOME_VOIP_REGISTRATION
 Peer registrar(false);bind_account(s,registrar);s.accept_direct_calls_=false;
#endif
 Peer udp(false),tcp(true);setup(s,udp);
 s.call_id_.clear();s.local_tag_.clear();s.remote_tag_.clear();s.media_active_=false;
 const auto tcp_invite=initial(tcp,"tcp-call"),udp_invite=initial(udp,"udp-call");

 if(scenario.rfind("immediate-redial",0)==0){
  Peer idle(true);s.accept_tcp_client_(idle.esp,idle.address);
  idle.send_to(s,request(idle,"OPTIONS",1,"","idle-probe","","probe"));
  auto probe=idle.drain();assert(probe.size()==1&&probe[0].rfind("SIP/2.0 200",0)==0);
  s.accept_tcp_client_(tcp.esp,tcp.address);tcp.send_to(s,tcp_invite);provisional(tcp);
  const auto first_tag=confirm(s,tcp,"tcp-call");
  if(scenario=="immediate-redial-terminal"){
   assert(s.send_bye_unlocked_("tcp-call"));auto closing=tcp.drain();
   assert(closing.size()==1&&closing[0].rfind("BYE ",0)==0);
   Peer blocked(true);s.accept_tcp_client_(blocked.esp,blocked.address);
   assert(fcntl(blocked.esp,F_GETFD)<0&&errno==EBADF);
   assert(fcntl(idle.esp,F_GETFD)>=0&&fcntl(tcp.esp,F_GETFD)>=0);
   tcp.send_to(s,response(closing[0],200));assert(s.call_id_.empty());
  }else hangup(s,tcp,"tcp-call",first_tag);
  assert(fcntl(idle.esp,F_GETFD)>=0&&fcntl(tcp.esp,F_GETFD)>=0);
  if(scenario=="immediate-redial-pending-output"){
   // A controlled queued record exercises the existing output-owner contract
   // without relying on timing-dependent TCP send-buffer pressure.
   const auto queued=request(tcp,"OPTIONS",9,"","queued-control","","probe");
   s.tcp_flow_.pending=queued;s.inbound_tcp_flow_.pending=queued;
   Peer blocked(true);s.accept_tcp_client_(blocked.esp,blocked.address);
   assert(fcntl(blocked.esp,F_GETFD)<0&&errno==EBADF);
   assert(fcntl(idle.esp,F_GETFD)>=0&&fcntl(tcp.esp,F_GETFD)>=0);
   assert(s.tcp_flow_.pending.str()==queued&&s.inbound_tcp_flow_.pending.str()==queued);
   reset_string(s.tcp_flow_.pending);reset_string(s.inbound_tcp_flow_.pending);
  }
  const auto other=initial(idle,"older-unread-call");
  const bool unread=scenario=="immediate-redial-unread",fragment=scenario=="immediate-redial-partial";
  if(unread){
   assert(send(idle.fd,other.data(),other.size(),0)==ssize_t(other.size()));
   pollfd pending{idle.esp,POLLIN,0};assert(poll(&pending,1,1000)==1);
  }else if(fragment)idle.send_to(s,other.substr(0,80));
  Peer next(true);s.accept_tcp_client_(next.esp,next.address);
  assert(fcntl(next.esp,F_GETFD)>=0&&"two fully idle TCP keepalives must not block immediate redial");
  if(unread||fragment)assert(fcntl(idle.esp,F_GETFD)>=0);
  next.send_to(s,initial(next,"next-call"));provisional(next);
  assert(s.call_id_=="next-call"&&s.tcp_flow_.socket==next.esp);
  const auto next_tag=confirm(s,next,"next-call");
  if(fragment)idle.send_to(s,other.substr(80));
  else if(unread)s.handle_sip_stream_(idle.esp,idle.address);
  if(unread||fragment){auto busy=idle.drain();assert(busy.size()==1&&busy[0].rfind("SIP/2.0 486",0)==0);}
  hangup(s,next,"next-call",next_tag);
 }else if(scenario=="active-udp-tcp-options"){
  udp.send_to(s,udp_invite);provisional(udp);const auto tag=confirm(s,udp,"udp-call");
  s.accept_tcp_client_(tcp.esp,tcp.address);
  tcp.send_to(s,request(tcp,"OPTIONS",1,"","tcp-probe","","probe"));
  auto replies=tcp.drain();assert(replies.size()==1&&replies[0].rfind("SIP/2.0 200",0)==0);
  assert(s.call_id_=="udp-call"&&!s.remote_sip_tcp_);
  hangup(s,udp,"udp-call",tag);
 }else{
  s.accept_tcp_client_(tcp.esp,tcp.address);
  const bool partial=scenario=="udp-options-partial";
  const auto first=partial?tcp_invite.substr(0,80):tcp_invite;
  if(scenario!="tcp-idle-eof")assert(send(tcp.fd,first.data(),first.size(),MSG_NOSIGNAL)==ssize_t(first.size()));
  if(partial)s.handle_sip_stream_(tcp.esp,tcp.address);
  if(scenario=="tcp-redial-deadline"){
   s.handle_sip_stream_(tcp.esp,tcp.address);provisional(tcp);
   const auto tag=confirm(s,tcp,"tcp-call");hangup(s,tcp,"tcp-call",tag);
   assert(s.tcp_flow_.ingress_deadline!=0&&"a retired call stream needs a bounded idle lifetime");
  }else if(scenario=="tcp-invite-first"){
   s.handle_sip_stream_(tcp.esp,tcp.address);provisional(tcp);
   const auto tag=confirm(s,tcp,"tcp-call");udp.send_to(s,udp_invite);
   const auto busy=udp.drain();assert(busy.size()==1&&busy[0].rfind("SIP/2.0 486",0)==0);
   assert(s.call_id_=="tcp-call"&&s.remote_sip_tcp_&&s.tcp_flow_.socket==tcp.esp);
   hangup(s,tcp,"tcp-call",tag);
  }else if(scenario=="udp-invite-first"||scenario=="tcp-idle-eof"){
   udp.send_to(s,udp_invite);provisional(udp);
   assert(fcntl(tcp.esp,F_GETFD)>=0&&"UDP INVITE must preserve the other unread TCP stream");
   assert(s.call_id_=="udp-call"&&!s.remote_sip_tcp_);
   const auto tag=confirm(s,udp,"udp-call");
   if(scenario=="tcp-idle-eof"){
    assert(shutdown(tcp.fd,SHUT_RDWR)==0);s.handle_sip_stream_(tcp.esp,tcp.address);
   }else{
    s.handle_sip_stream_(tcp.esp,tcp.address);
    const auto busy=tcp.drain();assert(busy.size()==1&&busy[0].rfind("SIP/2.0 486",0)==0);
   }
   assert(s.call_id_=="udp-call"&&!s.remote_sip_tcp_);
   hangup(s,udp,"udp-call",tag);
  }else{
   udp.send_to(s,request(udp,"OPTIONS",1,"","udp-probe","","probe"));
   const auto replies=udp.drain();assert(replies.size()==1&&replies[0].rfind("SIP/2.0 200",0)==0);
   assert(fcntl(tcp.esp,F_GETFD)>=0&&"UDP OPTIONS must preserve the unread TCP stream");
   assert(s.call_id_.empty()&&tcp.drain().empty());
   if(partial){
    assert(s.tcp_flow_.rx_buffer.str()==first);
    const auto rest=tcp_invite.substr(first.size());
    assert(send(tcp.fd,rest.data(),rest.size(),0)==ssize_t(rest.size()));
   }
   s.handle_sip_stream_(tcp.esp,tcp.address);provisional(tcp);
   assert(s.call_id_=="tcp-call"&&s.remote_sip_tcp_);
   const auto tag=confirm(s,tcp,"tcp-call");hangup(s,tcp,"tcp-call",tag);
  }
 }
 s.close_tcp_flow_(s.tcp_flow_);s.close_tcp_flow_(s.inbound_tcp_flow_);
#ifdef USE_ESPHOME_VOIP_REGISTRATION
 for(auto&flow:s.registration_flows_)s.close_tcp_flow_(flow);
#endif
 close(s.sip_socket_);
}
'''
    cpp = folder / "mixed.cpp"
    cpp.write_text(code)
    binary = folder / "mixed"
    built = subprocess.run([
        "g++", "-std=c++17", "-g", "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_ESPHOME_VOIP_REGISTRATION",
        "-I", str(folder), "-I", str(SOURCE.parents[2]), "-I", str(SOURCE), str(cpp),
        str(SOURCE / "sip_registration.cpp"), "-lcrypto", "-o", str(binary),
    ], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize("scenario", ["udp-options", "udp-options-partial", "udp-invite-first",
                                     "tcp-invite-first", "active-udp-tcp-options", "tcp-idle-eof", "tcp-redial-deadline",
                                     "immediate-redial", "immediate-redial-unread", "immediate-redial-partial",
                                     "immediate-redial-terminal", "immediate-redial-pending-output"])
def test_mixed_ingress_preserves_unread_stream_and_dialog_transport(mixed_ingress_binary, scenario):
    result = subprocess.run([mixed_ingress_binary, scenario], capture_output=True, text=True, timeout=8)
    assert result.returncode == 0, result.stdout + result.stderr
