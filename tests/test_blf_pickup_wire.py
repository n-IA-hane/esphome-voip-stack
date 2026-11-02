"""Recipient-only pickup and auxiliary TCP NOTIFY use actual SIP owners."""
import subprocess
import resource
import pytest
from test_refer_dialog_runtime import ROOT, SOURCE, block
from test_mwi_transport_runtime import mwi_transport_source


def blf_pickup_source():
    code = mwi_transport_source().split("int main(int argc,char**argv)")[0]
    code = '#define USE_ESP32\n#include "phonebook.h"\n#undef USE_ESP32\n' + code
    code = code.replace("uint32_t esp_random(){return 42;}", "uint32_t esp_random(){static uint32_t value=42;return ++value;}")
    code += "\nusing esphome::voip_stack::VOIP_STACK_MAX_ROUTE_ID_LEN;\n"
    parser = (SOURCE / "sip_message.cpp").read_text()
    for marker in ["static bool is_sip_uri_unreserved(", "std::string sip_uri_user_encode(", "std::string sip_prefix_target_user(", "std::string sip_target_uri(", "std::string sip_normalize_dial_target("]:
        if marker not in code:
            code += "\n" + block(parser, marker)
    adapter = (SOURCE / "sip_registration_transport.cpp").read_text()
    for marker in ["bool SipTransport::configure_blf(", "bool SipTransport::set_blf_watches(",
                   "template<typename Result> bool SipTransport::copy_blf_state_(", "bool SipTransport::get_blf_info("]:
        code += "\n" + block(adapter, marker)
    call_fixture = (ROOT / "tests/fixtures/registration_transport.cpp").read_text()
    for marker in ["void start_account_call(", "std::string pick(", "void confirm_account_call("]:
        code += "\n" + block(call_fixture, marker)
    code += r'''
struct VoipStack {
 Phonebook phonebook_;std::unique_ptr<SipTransport>transport_=std::make_unique<SipTransport>();
 bool ready=true,blf_ready_=true;std::string blf_pickup_prefix_="*99";
 bool can_start_call_()const{return ready;}
 bool is_registered(const std::string&id){return transport_->is_registered(id);}
 bool pickup_contact(const std::string&);BlfStateInfo get_contact_blf_info_(std::string_view);
 void start_call_on_line(const std::string&target,const std::string&line){
  std::string host;uint16_t port;bool tcp;assert(transport_->prepare_account_line(line,&host,&port,&tcp));
  transport_->remote_ip_v4_=INADDR_LOOPBACK;transport_->remote_sip_port_=port;transport_->remote_sip_tcp_=tcp;
  assert(transport_->send_invite_unlocked_("picked", "picker", "Picker", target, "Recipient"));
 }
};
'''
    facade = (SOURCE / "voip_blf.cpp").read_text()
    for marker in ["BlfStateInfo VoipStack::get_contact_blf_info_(", "bool VoipStack::pickup_contact("]:
        code += "\n" + block(facade, marker)
    code += r'''
std::string dialog_document(const std::string&body,unsigned version=0,const char*state="full"){
 return "<dialog-info xmlns='urn:ietf:params:xml:ns:dialog-info' entity='sip:101@pbx.example' version='"+std::to_string(version)+"' state='"+state+"'>"+body+"</dialog-info>";
}
std::string notify_dialog(const std::string&subscribe,unsigned sequence,const std::string&body,const std::string&state="active;expires=60"){
 auto wire=notification(subscribe,sequence,state);wire.erase(wire.find("Content-Type:"));
 auto event=wire.find("Event: message-summary");wire.replace(event,22,"Event: dialog");
 return wire+"Content-Type: application/dialog-info+xml\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
}
int main(int argc,char**argv){
 assert(argc==3);const std::string mode=argv[1];const bool tcp=std::string(argv[2])=="tcp";
 VoipStack app;auto&phone=*app.transport_;Peer peer(tcp);bind_account(phone,peer);
 assert(app.phonebook_.add_one("Desk|101|127.0.0.1|5060|40000|sip_udp")==AddResult::Added);
 assert(app.phonebook_.set_metadata("Desk","endpoint-desk",ContactKind::SIP_ACCOUNT,"office",true));
 assert(phone.configure_blf(mode=="admission"?2:1,nullptr));
 SipMwi::Hooks hooks;hooks.context=&phone;hooks.send=SipTransport::registration_send_view_;hooks.reply=SipTransport::registration_mwi_reply_;
 hooks.local_ip=SipTransport::registration_local_ip_;hooks.random=esp_random;hooks.hash=SipTransport::registration_hash_;hooks.local_port=phone.sip_port_;
 phone.mwi_.set_hooks(hooks);assert(phone.set_blf_watches({{"endpoint-desk","office","101"}}));phone.pump_registration_mwi_(clock_ms);
 const auto packages=peer.drain();assert(packages.size()==2);std::string subscribe,mailbox;
 for(const auto&record:packages){assert(record.rfind("SUBSCRIBE ",0)==0);
  if(header_value(record,"Event")=="dialog")subscribe=record;
  else{assert(header_value(record,"Event")=="message-summary");mailbox=record;}}
 assert(!subscribe.empty()&&!mailbox.empty()&&header_value(subscribe,"Call-ID")!=header_value(mailbox,"Call-ID"));
 peer.send_to(phone,response(mailbox,200));peer.send_to(phone,notification(mailbox,1));one(peer,"SIP/2.0 200");
 assert(phone.mwi_.state(0).available&&phone.mwi_.state(0).new_messages==2);
 peer.send_to(phone,response(subscribe,200));
 const std::string direction=mode=="legacy"?"":mode=="initiator"?" direction='initiator'":" direction='recipient'";
 const std::string state=mode=="active"?"confirmed":"early";
 peer.send_to(phone,notify_dialog(subscribe,1,dialog_document("<dialog id='incoming'"+direction+"><state>"+state+"</state></dialog>")));
 one(peer,"SIP/2.0 200");
 if(mode=="admission"){
  const BlfWatchConfig valid{"endpoint-desk","office","101"};
  assert(!phone.set_blf_watches({valid,valid}));
  assert(!phone.set_blf_watches({valid,{"other","missing","102"}}));
  assert(!phone.set_blf_watches({valid,{"other","office","https://example.test"}}));
  assert(!phone.set_blf_watches({valid,{"other","office","tel:102"}}));
  assert(!phone.set_blf_watches({valid,{"other","office","sips:102@example.test"}}));
  assert(!phone.set_blf_watches({valid,{"other","office",std::string(512,'*')}}));
  assert(!phone.set_blf_watches({valid,{"other","office","bad\r\nInjected: yes"}}));
  assert(!phone.set_blf_watches({valid,{"other","office","102"},{"third","office","103"}}));
  BlfStateInfo info;assert(phone.get_blf_info("endpoint-desk",&info)&&info.available&&info.state==BlfState::RINGING);
  assert(phone.mwi_.blf_watch_id(0)=="endpoint-desk"&&peer.drain().empty());
  assert(phone.set_blf_watches({}));phone.pump_registration_mwi_(clock_ms);const auto stop=one(peer,"SUBSCRIBE ");
  assert(header_value(stop,"Event")=="dialog"&&header_value(stop,"Expires")=="0");
  peer.send_to(phone,response(stop,200));peer.send_to(phone,notify_dialog(stop,2,"","terminated;reason=deactivated"));one(peer,"SIP/2.0 200");
  assert(phone.get_blf_info("endpoint-desk",&info)&&!info.available&&phone.mwi_.state(0).available);
 }else if(mode=="auxiliary"){
  assert(!tcp);confirm_account_call(phone,peer);const auto call=phone.call_id_;
  const auto registered=phone.registration_.account(0).config_revision;
  Peer auxiliary(true);phone.inbound_tcp_flow_.socket=auxiliary.esp;
  const auto large=dialog_document("<dialog id='incoming' direction='recipient'><state>confirmed</state></dialog><extra xmlns='urn:ignored'>"+std::string(1800,'x')+"</extra>",1,"partial");
  const auto record=notify_dialog(subscribe,2,large);assert(record.size()>1300);
  auxiliary.send_to(phone,record);one(auxiliary,"SIP/2.0 200");
  BlfStateInfo info;assert(phone.get_blf_info("endpoint-desk",&info)&&info.state==BlfState::ACTIVE);
  assert(phone.call_id_==call&&phone.registration_.account(0).config_revision==registered&&phone.registration_.is_registered("office"));
  phone.handle_tcp_peer_loss_(&phone.inbound_tcp_flow_);assert(phone.inbound_tcp_flow_.socket==-1);
  assert(phone.call_id_==call&&phone.registration_.is_registered("office"));
 }else{
  if(mode=="prefix-empty")app.blf_pickup_prefix_.clear();
  if(mode=="busy")app.ready=false;
  if(mode=="unregistered")phone.registration_.account(0).bound=false;
  if(mode=="gap"){
   peer.send_to(phone,notify_dialog(subscribe,2,dialog_document("",2,"partial")));one(peer,"SIP/2.0 200");
  }
  const bool picked=app.pickup_contact("endpoint-desk");assert(picked==(mode=="recipient"));
  if(picked){const auto invite=one(peer,"INVITE ");assert(invite.rfind("INVITE sip:%2A99101@",0)==0);
   assert(header_value(invite,"From").find("100@pbx.example")!=std::string::npos&&header_value(invite,"Replaces").empty());}
  else assert(peer.drain().empty());
 }
 for(auto&flow:phone.registration_flows_)phone.close_tcp_flow_(flow);close(phone.sip_socket_);
}
'''
    return code


@pytest.fixture(scope="module")
def pickup_wire_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("blf-pickup")
    core = folder / "esphome/core"
    core.mkdir(parents=True)
    (core / "defines.h").write_text("")
    code = blf_pickup_source()
    cpp = folder / "pickup.cpp"
    cpp.write_text(code)
    binary = folder / "pickup"
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation", "-g", "-fsanitize=address,undefined",
        "-fno-pie", "-no-pie", "-DUSE_ESPHOME_VOIP_REGISTRATION", "-DUSE_ESPHOME_VOIP_MWI", "-DUSE_ESPHOME_VOIP_BLF", "-DUSE_ESPHOME_VOIP_CONTACT_METADATA",
        "-I", str(folder), "-I", str(SOURCE), "-I", str(SOURCE.parents[2]), str(cpp), str(SOURCE / "sip_mwi.cpp"),
        str(SOURCE / "sip_blf.cpp"), str(SOURCE / "sip_registration.cpp"), "-lcrypto", "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("mode,transport", [(m,p) for m in ["recipient","initiator","legacy","active","gap","prefix-empty","busy","unregistered","admission"] for p in ["udp","tcp"]]+[("auxiliary","udp")])
def test_only_recipient_pickup_originates_and_auxiliary_notify_preserves_call(pickup_wire_binary, mode, transport):
    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    result = subprocess.run([pickup_wire_binary, mode, transport], capture_output=True, text=True, timeout=8, preexec_fn=no_core)
    assert result.returncode == 0, result.stderr
