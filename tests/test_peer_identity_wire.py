"""An accepted real UPDATE changes connected identity within its owned dialog."""

from production_sip_source import read_production_source
import subprocess
import pytest
from test_refer_dialog_runtime import SOURCE, block
from test_blf_pickup_wire import blf_pickup_source


@pytest.fixture(scope="module")
def identity_wire_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("peer-identity-wire")
    core = folder / "esphome/core"
    core.mkdir(parents=True)
    (core / "defines.h").write_text("")
    code = blf_pickup_source().split("struct VoipStack {")[0]
    code = code.replace("bool handle_update_(const std::string&,const sockaddr_in&){assert(false);return false;}", "bool handle_update_(const std::string&,const sockaddr_in&);")
    code += block(read_production_source(SOURCE / "sip_transport.cpp"), "bool SipTransport::handle_update_(") + r'''
int main(int argc,char**argv){
 assert(argc==3);const bool tcp=std::string(argv[1])=="tcp";const std::string mode=argv[2];SipTransport phone;Peer peer(tcp);bind_account(phone,peer);
 if(mode=="to-name"||mode=="to-bare"||mode=="to-number"){
  if(mode=="to-number"){
   std::string host;uint16_t port;bool stream;
   assert(phone.prepare_account_line("office",&host,&port,&stream));
   phone.remote_ip_v4_=INADDR_LOOPBACK;phone.remote_sip_port_=port;phone.remote_sip_tcp_=stream;
   assert(phone.send_invite_unlocked_("account-call","esp","ESP","101",""));
  }else start_account_call(phone,peer);
  auto invite=pick(peer.drain(),"INVITE");
  peer.send_to(phone,response(invite,401));auto retry=peer.drain();invite=pick(retry,"INVITE");
  assert(!header_value(invite,"Authorization").empty());
  const auto account=phone.active_account_;const auto local_uri=phone.local_uri_,contact=phone.local_contact_uri_;
  const auto call=phone.call_id_,local_tag=phone.local_tag_;const auto count=phone.delivered.size();
  auto accepted=response(invite,200);const auto old_to=header_value(accepted,"To");
  const auto to=sip_name_addr(name_addr_uri(old_to),mode=="to-name"?"Connected reception":"")+";tag=destination";
  accepted.replace(accepted.find("\r\nTo: ")+6,old_to.size(),to);
  peer.send_to(phone,accepted);one(peer,"ACK ");
  assert(phone.delivered[count].type==SipSignalType::STATUS_200_OK);
  if(mode!="to-bare"){
   assert(phone.delivered.size()==count+2);const auto &identity=phone.delivered.back();
   assert(identity.type==SipSignalType::CONNECTED_IDENTITY&&identity.call_id==call&&identity.connected_route=="101"&&identity.connected_name==(mode=="to-name"?"Connected reception":"")&&!identity.media_source_changed);
  }else assert(phone.delivered.size()==count+1);
  assert(phone.destination_.name==(mode=="to-name"?"Connected reception":mode=="to-bare"?"Peer":"101"));
  assert(phone.active_account_==account&&phone.local_uri_==local_uri&&phone.local_contact_uri_==contact&&phone.local_tag_==local_tag&&phone.call_id_==call&&phone.media_active_);
  assert(phone.send_bye_unlocked_(call));const auto bye=one(peer,"BYE ");
  assert(header_value(bye,"From").find("sip:100@pbx.example")!=std::string::npos);
  assert(header_value(bye,"Contact")==header_value(invite,"Contact"));
  assert(name_addr_uri(header_value(bye,"To"))==name_addr_uri(header_value(invite,"To")));
  peer.send_to(phone,response(bye,200));assert(phone.call_id_.empty()&&phone.active_account_==-1);
  for(auto&flow:phone.registration_flows_)phone.close_tcp_flow_(flow);close(phone.sip_socket_);return 0;
 }
 confirm_account_call(phone,peer);const auto call=phone.call_id_;const auto count=phone.delivered.size();
 const auto account=phone.active_account_;const auto local_uri=phone.local_uri_,local_contact=phone.local_contact_uri_,local_tag=phone.local_tag_,remote_tag=phone.remote_tag_;
 const auto old_uri=phone.remote_uri_,old_target=phone.remote_target_uri_;const auto seq=phone.remote_dialog_cseq_+1;
 auto update=request(peer,"UPDATE",seq,
  mode=="name-only"||mode=="number-only"?"":"Contact: <"+peer.uri()+">\r\n",call,phone.local_tag_,phone.remote_tag_);
 const auto from=update.find("\r\nFrom: ")+8,end=update.find("\r\n",from);
 const auto identity_uri=mode=="name-only"?name_addr_uri(old_uri):std::string("sip:102@pbx.example");
 const auto identity_route=mode=="name-only"?sip_user_from_header(old_uri):std::string("102");
 const auto identity_name=mode=="number-only"?identity_route:std::string("Casa");
 update.replace(from,end-from,sip_name_addr(identity_uri,mode=="number-only"?"":"Casa")+";tag="+phone.remote_tag_);
 if(mode=="foreign"){const auto pos=update.find("To: ");const auto tag=update.find(";tag=",pos);update.replace(tag+5,phone.local_tag_.size(),"foreign");}
 if(mode=="body"){update.replace(update.find("Content-Length: 0"),17,"Content-Length: 1");update+='x';}
 peer.send_to(phone,update);const auto answer=one(peer,"SIP/2.0 ");
 if(mode=="valid"||mode=="name-only"||mode=="number-only"){
  assert(answer.rfind("SIP/2.0 200",0)==0&&phone.delivered.size()==count+1);
  const auto&signal=phone.delivered.back();assert(signal.type==SipSignalType::CONNECTED_IDENTITY&&signal.call_id==call&&signal.connected_name==identity_name&&signal.connected_route==identity_route&&!signal.media_source_changed);
  assert(name_addr_uri(phone.remote_uri_)==identity_uri&&phone.call_id_==call&&phone.media_active_);
  assert(phone.active_account_==account&&phone.local_uri_==local_uri&&phone.local_contact_uri_==local_contact&&phone.local_tag_==local_tag&&phone.remote_tag_==remote_tag);
  assert(phone.remote_target_uri_==(mode=="valid"?peer.uri():old_target));
  peer.send_to(phone,update);one(peer,"SIP/2.0 200");assert(phone.delivered.size()==count+1);
  auto stale=request(peer,"UPDATE",seq,"",call,phone.local_tag_,phone.remote_tag_);const auto branch=stale.find("branch=");stale.insert(branch+7,"other");
  peer.send_to(phone,stale);one(peer,"SIP/2.0 500");assert(phone.delivered.size()==count+1);
 }else{assert(answer.rfind(mode=="foreign"?"SIP/2.0 481":"SIP/2.0 488",0)==0);assert(phone.delivered.size()==count&&phone.remote_uri_==old_uri&&phone.remote_target_uri_==old_target);}
 assert(phone.send_bye_unlocked_(phone.call_id_));const auto bye=one(peer,"BYE ");
 if(mode=="valid"||mode=="name-only"||mode=="number-only")assert(name_addr_uri(header_value(bye,"To"))==identity_uri);
 peer.send_to(phone,response(bye,200));assert(phone.call_id_.empty()&&phone.active_account_==-1);
 for(auto&flow:phone.registration_flows_)phone.close_tcp_flow_(flow);close(phone.sip_socket_);
}
'''
    cpp = folder / "identity.cpp"
    cpp.write_text(code)
    binary = folder / "identity"
    built = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation", "-g", "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
        "-DUSE_ESPHOME_VOIP_REGISTRATION", "-DUSE_ESPHOME_VOIP_MWI", "-DUSE_ESPHOME_VOIP_BLF", "-DUSE_ESPHOME_VOIP_CONTACT_METADATA",
        "-I", str(folder), "-I", str(SOURCE), "-I", str(SOURCE.parents[2]), str(cpp), str(SOURCE / "sip_mwi.cpp"), str(SOURCE / "sip_blf.cpp"),
        str(SOURCE / "sip_registration.cpp"), "-lcrypto", "-o", str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize("transport", ["udp", "tcp"])
@pytest.mark.parametrize("mode", ["valid", "foreign", "body", "name-only", "number-only", "to-name", "to-bare", "to-number"])
def test_wire_identity_is_scoped_accepted_once_and_preserves_teardown(identity_wire_binary, transport, mode):
    result = subprocess.run([identity_wire_binary, transport, mode], capture_output=True, text=True, timeout=8)
    assert result.returncode == 0, result.stdout + result.stderr
