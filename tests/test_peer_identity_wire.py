"""An accepted real UPDATE changes connected identity within its owned dialog."""
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
    code = code.replace('std::string sip_display_name_from_header(const std::string&,size_t){return "Room";}',
                        block((SOURCE / "sip_message.cpp").read_text(), "std::string sip_display_name_from_header("))
    code += block((SOURCE / "sip_transport.cpp").read_text(), "bool SipTransport::handle_update_(") + r'''
int main(int argc,char**argv){
 assert(argc==3);const bool tcp=std::string(argv[1])=="tcp";const std::string mode=argv[2];SipTransport phone;Peer peer(tcp);bind_account(phone,peer);
 confirm_account_call(phone,peer);const auto call=phone.call_id_;const auto count=phone.delivered.size();
 const auto old_uri=phone.remote_uri_,old_target=phone.remote_target_uri_;const auto seq=phone.remote_dialog_cseq_+1;
 auto update=request(peer,"UPDATE",seq,"Contact: <"+peer.uri()+">\r\n",call,phone.local_tag_,phone.remote_tag_);
 const auto from=update.find("\r\nFrom: ")+8,end=update.find("\r\n",from);
 update.replace(from,end-from,"\"Casa\" <sip:102@pbx.example>;tag="+phone.remote_tag_);
 if(mode=="foreign"){const auto pos=update.find("To: ");const auto tag=update.find(";tag=",pos);update.replace(tag+5,phone.local_tag_.size(),"foreign");}
 if(mode=="body"){update.replace(update.find("Content-Length: 0"),17,"Content-Length: 1");update+='x';}
 peer.send_to(phone,update);const auto answer=one(peer,"SIP/2.0 ");
 if(mode=="valid"){
  assert(answer.rfind("SIP/2.0 200",0)==0&&phone.delivered.size()==count+1);
  const auto&signal=phone.delivered.back();assert(signal.type==SipSignalType::CONNECTED_IDENTITY&&signal.call_id==call&&signal.connected_name=="Casa"&&signal.connected_route=="102"&&!signal.media_source_changed);
  assert(phone.remote_uri_.find("sip:102@pbx.example")!=std::string::npos&&phone.call_id_==call&&phone.media_active_);
  peer.send_to(phone,update);one(peer,"SIP/2.0 200");assert(phone.delivered.size()==count+1);
  auto stale=request(peer,"UPDATE",seq,"",call,phone.local_tag_,phone.remote_tag_);const auto branch=stale.find("branch=");stale.insert(branch+7,"other");
  peer.send_to(phone,stale);one(peer,"SIP/2.0 500");assert(phone.delivered.size()==count+1);
 }else{assert(answer.rfind(mode=="foreign"?"SIP/2.0 481":"SIP/2.0 488",0)==0);assert(phone.delivered.size()==count&&phone.remote_uri_==old_uri&&phone.remote_target_uri_==old_target);}
 assert(phone.send_bye_unlocked_(phone.call_id_));const auto bye=one(peer,"BYE ");
 if(mode=="valid")assert(header_value(bye,"To").find("sip:102@pbx.example")!=std::string::npos);
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
@pytest.mark.parametrize("mode", ["valid", "foreign", "body"])
def test_wire_identity_is_scoped_accepted_once_and_preserves_teardown(identity_wire_binary, transport, mode):
    result = subprocess.run([identity_wire_binary, transport, mode], capture_output=True, text=True, timeout=8)
    assert result.returncode == 0, result.stdout + result.stderr
