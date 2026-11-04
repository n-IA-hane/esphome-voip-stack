"""One real INVITE owner admits a single call across registered accounts."""

import subprocess

import pytest

from test_refer_dialog_runtime import SOURCE
from test_tcp_incoming_account_policy import incoming_account_source


@pytest.fixture(scope="module")
def global_busy_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("multiaccount-global-busy")
    defines = folder / "esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    code = incoming_account_source()
    # The shared single-account fixture uses a constant hardware RNG. Give
    # distinct registrations distinct deterministic transaction identities.
    rng = "uint32_t esp_random(){return 42;}"
    assert code.count(rng) == 1
    code = code.replace(rng, "uint32_t esp_random(){static uint32_t next=42;return ++next;}")
    code += r'''
void bind_home(SipTransport &s, Peer &home) {
 if(home.tcp)s.registration_flows_[1].socket=home.esp;
 SipAccountConfig config;
 config.id="home";config.registrar="127.0.0.1";config.domain="ha.example";
 config.username="200";config.password="fixture-only";config.tcp=home.tcp;
 config.port=ntohs(home.address.sin_port);
 assert(s.registration_.set_account(config,clock_ms));s.registration_.tick(clock_ms);
 const auto sent=home.drain();assert(sent.size()==1&&sent[0].rfind("REGISTER ",0)==0);
 auto registered=response(sent[0],200);
 registered.insert(registered.find("Content-Length:"),"Contact: "+header_value(sent[0],"Contact")+"\r\n");
 home.send_to(s,registered);assert(s.registration_.is_registered("home"));
}
std::string initial_invite(Peer &peer,const std::string &account,const std::string &id) {
 auto message=request(peer,"INVITE",1,"",id,"","caller");
 const auto target=message.find("sip:esp@127.0.0.1");assert(target!=std::string::npos);
 message.replace(target,strlen("sip:esp@127.0.0.1"),"sip:esphome-"+account+"@127.0.0.1");
 const std::string old_to="To: <sip:esp@127.0.0.1>;tag=\r\n";
 const auto to=message.find(old_to);assert(to!=std::string::npos);
 message.replace(to,old_to.size(),"To: <sip:esp@127.0.0.1>\r\n");
 const auto length=message.find("Content-Length: 0");assert(length!=std::string::npos);
 message.replace(length,strlen("Content-Length: 0"),"Content-Length: 3");
 return message+"v=0";
}
struct ActiveSnapshot {
 std::string id,local,remote,target,response,via,cseq,account_socket_uri;
 int account,dialog_socket,tcp_socket,rtp_socket;
 bool media,tcp,awaiting_ack;
 size_t events;
 AudioFormat tx,rx;
 RtpAudioPayloads payloads;
 uint32_t ip,session,version;
 uint16_t port;
 explicit ActiveSnapshot(const SipTransport &s):
  id(s.call_id_),local(s.local_tag_),remote(s.remote_tag_),target(s.remote_target_uri_),
  response(s.last_invite_response_),via(s.last_invite_via_),cseq(s.last_invite_cseq_),
  account_socket_uri(s.local_contact_uri_),account(s.active_account_),
  dialog_socket(s.dialog_account_socket_),tcp_socket(s.tcp_flow_.socket),rtp_socket(s.rtp_socket_),
  media(s.media_active_),tcp(s.remote_sip_tcp_),awaiting_ack(s.completed_invite_.awaiting_ack),
  events(s.delivered.size()),tx(s.selected_tx_format_),rx(s.selected_rx_format_),
  payloads(s.negotiated_rx_payloads_),ip(s.remote_ip_v4_),session(s.sdp_session_id_),
  version(s.sdp_session_version_),port(s.remote_sip_port_) {}
 void unchanged(const SipTransport &s) const {
  assert(s.call_id_==id&&s.local_tag_==local&&s.remote_tag_==remote);
  assert(s.remote_target_uri_==target&&s.local_contact_uri_==account_socket_uri);
  assert(s.last_invite_response_==response&&s.last_invite_via_==via&&s.last_invite_cseq_==cseq);
  assert(s.active_account_==account&&s.dialog_account_socket_==dialog_socket);
  assert(s.tcp_flow_.socket==tcp_socket&&s.rtp_socket_==rtp_socket);
  assert(s.media_active_==media&&s.remote_sip_tcp_==tcp);
  assert(s.completed_invite_.awaiting_ack==awaiting_ack&&s.delivered.size()==events);
  assert(s.selected_tx_format_==tx&&s.selected_rx_format_==rx&&s.negotiated_rx_payloads_==payloads);
  assert(s.remote_ip_v4_==ip&&s.remote_sip_port_==port);
  assert(s.sdp_session_id_==session&&s.sdp_session_version_==version);
 }
};
void expect_response(Peer &peer,unsigned status,const std::string &id) {
 const auto records=peer.drain();assert(records.size()==1);
 assert(records[0].rfind("SIP/2.0 "+std::to_string(status)+" ",0)==0);
 assert(header_value(records[0],"Call-ID")==id);
}
void end_incoming(SipTransport &s,Peer &peer,const std::string &invite,bool connected) {
 const auto id=header_value(invite,"Call-ID");
 if(connected) {
  peer.send_to(s,request(peer,"BYE",2,"",id,s.local_tag_,s.remote_tag_));
  expect_response(peer,200,id);
 }else{
  auto cancel=invite.substr(0,invite.find("\r\n\r\n")+4);
  cancel.replace(0,6,"CANCEL");
  auto at=cancel.find("CSeq: 1 INVITE");assert(at!=std::string::npos);
  cancel.replace(at,strlen("CSeq: 1 INVITE"),"CSeq: 1 CANCEL");
  at=cancel.find("Content-Length: 3");assert(at!=std::string::npos);
  cancel.replace(at,strlen("Content-Length: 3"),"Content-Length: 0");
  peer.send_to(s,cancel);const auto records=peer.drain();assert(records.size()==2);
  assert(records[0].rfind("SIP/2.0 200 ",0)==0&&header_value(records[0],"CSeq")=="1 CANCEL");
  assert(records[1].rfind("SIP/2.0 487 ",0)==0&&header_value(records[1],"CSeq")=="1 INVITE");
  auto ack=cancel;ack.replace(0,6,"ACK");
  at=ack.find("CSeq: 1 CANCEL");ack.replace(at,strlen("CSeq: 1 CANCEL"),"CSeq: 1 ACK");
  const auto old_to=header_value(ack,"To");at=ack.find("To: "+old_to);
  ack.replace(at,strlen("To: ")+old_to.size(),"To: "+header_value(records[1],"To"));
  peer.send_to(s,ack);assert(!s.completed_invite_.awaiting_ack);
 }
 assert(s.call_id_.empty()&&!s.media_active_&&!s.terminal_transaction_pending_locked_());
}
int main(int argc,char **argv) {
 assert(argc==5);
 Peer office(std::string(argv[1])=="tcp"),home(std::string(argv[2])=="tcp");
 SipTransport s;bind_account(s,office);bind_home(s,home);
 assert(s.registration_.is_registered("office")&&s.registration_.is_registered("home"));
 s.call_id_.clear();s.local_tag_.clear();s.remote_tag_.clear();s.media_active_=false;
 s.accept_direct_calls_=false;
 const bool home_first=std::string(argv[3])=="home",connected=std::string(argv[4])=="in_call";
 Peer &first=home_first?home:office,&second=home_first?office:home;
 const std::string first_account=home_first?"home":"office",second_account=home_first?"office":"home";
 const auto invite=initial_invite(first,first_account,"owned-first");
 first.send_to(s,invite);expect_response(first,100,"owned-first");
 assert(s.active_account_==(home_first?1:0)&&s.call_id_=="owned-first");
 assert(s.delivered.size()==1&&s.delivered[0].type==SipSignalType::INVITE);
 assert(s.send_response_(connected?200:180,connected?"OK":"Ringing"));
 const auto initial_response=first.drain();assert(initial_response.size()==1);
 assert(initial_response[0].rfind(connected?"SIP/2.0 200 ":"SIP/2.0 180 ",0)==0);
 if(connected) {
  first.send_to(s,request(first,"ACK",1,"","owned-first",s.local_tag_,s.remote_tag_));
  assert(!s.completed_invite_.awaiting_ack&&s.media_active_);
 }else assert(!s.media_active_);
 const ActiveSnapshot owned(s);
 const auto competing=initial_invite(second,second_account,"owned-second");
 second.send_to(s,competing);expect_response(second,486,"owned-second");owned.unchanged(s);
 assert(first.drain().empty());
 // The same first transaction is a retransmission, never a second call.
 first.send_to(s,invite);
 const auto replay=first.drain();assert(replay.size()==1&&replay[0]==initial_response[0]);
 owned.unchanged(s);assert(second.drain().empty());
 // A repeated competing INVITE must still leave the original owner intact.
 second.send_to(s,competing);expect_response(second,486,"owned-second");owned.unchanged(s);
 assert(first.drain().empty());
 // End only the original dialog, then prove that the other account can call.
 end_incoming(s,first,invite,connected);
 assert(s.registration_.is_registered("office")&&s.registration_.is_registered("home"));
 const auto next=initial_invite(second,second_account,"after-busy");
 second.send_to(s,next);expect_response(second,100,"after-busy");
 assert(s.call_id_=="after-busy"&&s.active_account_==(home_first?0:1));
 end_incoming(s,second,next,false);
 for(auto &flow:s.registration_flows_)s.close_tcp_flow_(flow);
 s.close_tcp_flow_(s.tcp_flow_);s.close_tcp_flow_(s.inbound_tcp_flow_);
 close(s.sip_socket_);
}
'''
    cpp = folder / "global_busy.cpp"
    cpp.write_text(code)
    binary = folder / "global_busy"
    result = subprocess.run([
        "g++", "-std=c++17", "-g", "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
        "-DUSE_ESPHOME_VOIP_REGISTRATION", "-I", str(folder), "-I", str(SOURCE.parents[2]),
        "-I", str(SOURCE), str(cpp), str(SOURCE / "sip_registration.cpp"), "-lcrypto", "-o", str(binary),
    ], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("office_transport", ["udp", "tcp"])
@pytest.mark.parametrize("home_transport", ["udp", "tcp"])
@pytest.mark.parametrize("first_account", ["home", "office"])
@pytest.mark.parametrize("phase", ["ringing", "in_call"])
def test_accounts_share_one_global_call_and_replay_only_the_owned_transaction(
    global_busy_binary, office_transport, home_transport, first_account, phase,
):
    result = subprocess.run([
        global_busy_binary, office_transport, home_transport, first_account, phase,
    ], capture_output=True, text=True, timeout=8)
    assert result.returncode == 0, result.stdout + result.stderr
