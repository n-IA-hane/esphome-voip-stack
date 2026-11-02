"""TCP storage admission stays separate from INVITE method admission."""

from pathlib import Path
import subprocess

import pytest

from test_refer_dialog_runtime import SOURCE, block
from test_sip_registration_transport import registration_transport_source


@pytest.fixture(scope="module")
def control_admission_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("tcp-control-admission")
    defines = folder / "esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    code = registration_transport_source(full_invite=True)
    gate = "bool should_accept_session_()const{return true;}"
    assert gate in code
    code = code.replace(gate, "bool app_accept=true;bool should_accept_session_()const{return app_accept;}")
    code += "\n" + block((SOURCE / "sip_transport.cpp").read_text(), "void SipTransport::accept_tcp_client_(")
    peers = (Path(__file__).parent / "fixtures/refer_wire_main.cpp").read_text()
    code += "\n" + peers[:peers.index("std::string begin(")]
    account = (Path(__file__).parent / "fixtures/registration_transport.cpp").read_text()
    code += "\n" + account[:account.index("std::string account_refer(")]
    code += r'''
void expect_status(Peer &peer,unsigned status){
 const auto replies=peer.drain();assert(replies.size()==1);
 if(replies[0].rfind("SIP/2.0 "+std::to_string(status),0)!=0)
  fprintf(stderr,"expected status %u, received %s\n",status,replies[0].substr(0,replies[0].find("\r\n")).c_str());
 assert(replies[0].rfind("SIP/2.0 "+std::to_string(status),0)==0);
}
int main(int argc,char **argv){
 assert(argc==2);const std::string scenario=argv[1];SipTransport s;Peer pbx(true);bind_account(s,pbx);
 s.call_id_.clear();s.media_active_=false;
 if(scenario=="direct-primary"){
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
        "-I", str(folder), "-I", str(SOURCE.parents[2]), "-I", str(SOURCE), str(cpp),
        str(SOURCE / "sip_registration.cpp"), "-lcrypto", "-o", str(binary),
    ], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize("scenario", ["busy-options", "busy-invite", "direct-primary", "partial-primary", "unread-primary"])
def test_control_tcp_admission_preserves_method_owners(control_admission_binary, scenario):
    result = subprocess.run([control_admission_binary, scenario], capture_output=True, text=True, timeout=8)
    assert result.returncode == 0, result.stdout + result.stderr
