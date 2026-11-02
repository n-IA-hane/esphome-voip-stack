"""BLF and MWI share real subscription/auth/deadline code over UDP/TCP sockets."""

from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "esphome/components/voip_stack"


@pytest.fixture(scope="module", params=[True, False], ids=["with-mwi", "blf-only"])
def blf_binary(tmp_path_factory, request):
    folder = tmp_path_factory.mktemp("blf-wire")
    defines = folder / "esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    harness = (ROOT / "tests/test_sip_mwi_engine.cpp").read_text()
    code = harness[:harness.index("int main(")] + r'''
std::string xml(unsigned version,const char *state,const char *content) {
 return "<dialog-info xmlns='urn:ietf:params:xml:ns:dialog-info' version='"+
  std::to_string(version)+"' state='"+state+"' entity='sip:101@example.test'>"+content+"</dialog-info>";
}
std::string dialog_notify(const std::string &request,unsigned seq,const std::string &body,
                          const std::string &state="active;expires=60") {
 auto wire=notify(request,seq,body,state);
 auto at=wire.find("Event: message-summary");assert(at!=std::string::npos);
 wire.replace(at,22,"Event: dialog");
 at=wire.find("application/simple-message-summary");assert(at!=std::string::npos);
 wire.replace(at,34,"application/dialog-info+xml");
 return wire;
}
std::vector<BlfStateInfo> blf_updates;
int main(int argc,char **argv) {
 assert(argc==3);const std::string scenario=argv[1];Harness h(std::string(argv[2])=="tcp");
 h.hooks.notify_blf=[](void *,const BlfStateInfo &state){blf_updates.push_back(state);};
 h.client.set_hooks(h.hooks);
 SipRegistration owner;assert(owner.set_account(h.config,0));
 auto &account=owner.account(0);account.bound=true;
 account.target_ip=INADDR_LOOPBACK;account.target_port=h.first.port;
 account.contact="sip:alice@127.0.0.1:5060";
#ifdef USE_ESPHOME_VOIP_MWI
 h.client.update_registered_account(0,account,0);h.client.tick(0);
 auto mwi=h.first.receive();h.answer(response(mwi,200,"Expires: 60\r\n"));
 assert(h.notification(notify(mwi)).rfind("SIP/2.0 200",0)==0);
 assert(h.client.state(0).waiting);
#endif
 assert(h.client.configure_blf(2));assert(h.client.configure_blf(2));assert(!h.client.configure_blf(3));
 const auto *stable=h.client.blf_state(0);
 if(scenario=="flow") {
  auto second=h.config;second.id="second";second.username="bob";second.port=h.second.port;
  assert(owner.set_account(second,3));auto &a=owner.account(1);a.bound=true;a.target_ip=INADDR_LOOPBACK;
  a.target_port=h.second.port;a.contact="sip:bob@127.0.0.1:5060";
  assert(h.client.set_blf_watch(0,1,a,"contact","sip:101@example.test",3));h.client.tick(3);
  auto sub=h.second.receive();assert(field(sub,"Event")=="dialog");
  assert(h.client.handle_response(h.second.deliver(response(sub,200,"Expires: 60\r\n")),INADDR_LOOPBACK,h.second.port,h.second.tcp,1,4));
  auto update=dialog_notify(sub,1,xml(0,"full","<dialog id='one'><state>early</state></dialog>"));
  assert(h.client.handle_notify(h.second.deliver(update),INADDR_LOOPBACK,h.second.port,h.second.tcp,1,5));
  assert(h.second.receive().rfind("SIP/2.0 200",0)==0);
  if(h.second.tcp) {
   auto changed=dialog_notify(sub,2,xml(1,"partial","<dialog id='one'><state>confirmed</state></dialog>"));
   assert(h.client.handle_notify(h.second.deliver(changed),INADDR_LOOPBACK,h.second.port,true,0,6));
   assert(h.second.receive().rfind("SIP/2.0 481",0)==0);
  }
  assert(h.client.blf_state(0)->state==BlfState::RINGING);return 0;
 }
 assert(h.client.set_blf_watch(0,0,account,"contact","sip:101@example.test",3));h.client.tick(3);
 auto sub=h.first.receive();assert(field(sub,"Event")=="dialog");
 assert(field(sub,"From").find("sip:alice@example.test")!=std::string::npos);
 assert(field(sub,"To").find("sip:101@example.test")!=std::string::npos);
 const auto early=xml(0,"full","<dialog id='one'><state>early</state></dialog>");
 if(scenario=="calling") {
  h.answer(response(sub,200,"Expires: 60\r\n"),4);
  assert(h.notification(dialog_notify(sub,1,xml(0,"full","<dialog id='one' direction='initiator'><state>trying</state></dialog>")),5).rfind("SIP/2.0 200",0)==0);
  assert(h.client.blf_state(0)->state==BlfState::CALLING&&h.client.blf_state(0)->ringing_dialogs==0);
  assert(h.notification(dialog_notify(sub,2,xml(1,"partial","<dialog id='one'><state>early</state></dialog>")),6).rfind("SIP/2.0 200",0)==0);
  assert(h.client.blf_state(0)->state==BlfState::CALLING&&h.client.blf_state(0)->ringing_dialogs==0);
  assert(h.notification(dialog_notify(sub,3,xml(2,"partial","<dialog id='one' direction='recipient'/>")),7).rfind("SIP/2.0 200",0)==0);
  assert(h.client.blf_state(0)->state==BlfState::RINGING&&h.client.blf_state(0)->ringing_dialogs==1);return 0;
 }
 if(scenario=="auth") {
  h.answer(response(sub,401,"WWW-Authenticate: Digest realm=\"example.test\", nonce=\"blf\", qop=\"auth\", algorithm=SHA-256\r\n"),4);
  auto authorized=h.first.receive();const auto auth=field(authorized,"Authorization");
  assert(auth.find("uri=\"sip:101@example.test\"")!=std::string::npos);
  assert(field(authorized,"Call-ID")==field(sub,"Call-ID")&&field(authorized,"CSeq")!=field(sub,"CSeq"));
  assert(field(authorized,"From").find("sip:alice@example.test")!=std::string::npos);
  const auto at=auth.find("cnonce=\"");assert(at!=std::string::npos);const auto end=auth.find('"',at+8);
  const auto cnonce=auth.substr(at+8,end-at-8);
  const auto ha1=hash_value("SHA-256",h.config.username+":example.test:"+h.config.password);
  const auto ha2=hash_value("SHA-256","SUBSCRIBE:sip:101@example.test");
  const auto expected=hash_value("SHA-256",ha1+":blf:00000001:"+cnonce+":auth:"+ha2);
  assert(auth.find("response=\""+expected+'"')!=std::string::npos);
  h.answer(response(authorized,200,"Expires: 60\r\n"),5);
  assert(h.notification(dialog_notify(authorized,1,early),6).rfind("SIP/2.0 200",0)==0);
  assert(h.client.blf_state(0)->state==BlfState::RINGING);return 0;
 }
 if(scenario=="early-gap") {
  assert(h.notification(dialog_notify(sub,1,early),4).rfind("SIP/2.0 200",0)==0);
  h.answer(response(sub,200,"Expires: 60\r\n"),5);
 } else {
  h.answer(response(sub,200,"Expires: 60\r\n"),4);
  assert(h.notification(dialog_notify(sub,1,early),5).rfind("SIP/2.0 200",0)==0);
 }
 assert(h.client.blf_state(0)->available&&h.client.blf_state(0)->state==BlfState::RINGING);
 const auto mwi_updates=h.updates.size();
 if(scenario=="shared") {
  assert(h.notification(dialog_notify(sub,2,xml(1,"partial","<dialog id='one'><state>confirmed</state></dialog>")),6).rfind("SIP/2.0 200",0)==0);
  assert(h.client.blf_state(0)->state==BlfState::ACTIVE&&h.updates.size()==mwi_updates);
#ifdef USE_ESPHOME_VOIP_MWI
  assert(h.notification(notify(mwi,2,"Messages-Waiting: no\r\n"),7).rfind("SIP/2.0 200",0)==0);
  assert(!h.client.state(0).waiting&&h.client.blf_state(0)->state==BlfState::ACTIVE);
#endif
  h.client.clear_blf_watch(0,20);h.client.tick(20);auto stop=h.first.receive();
  assert(field(stop,"Expires")=="0"&&field(stop,"Event")=="dialog");
  h.answer(response(stop,200,"Expires: 0\r\n"),21);
  assert(h.notification(dialog_notify(stop,3,"","terminated;reason=timeout"),22).rfind("SIP/2.0 200",0)==0);
  assert(!h.client.blf_state(0)->available);
 } else if(scenario=="early-gap") {
  assert(h.notification(dialog_notify(sub,2,xml(3,"partial","<dialog id='one'><state>terminated</state></dialog>")),6).rfind("SIP/2.0 200",0)==0);
  assert(!h.client.blf_state(0)->available);h.client.tick(7);auto refresh=h.first.receive();
  assert(field(refresh,"Call-ID")==field(sub,"Call-ID"));h.answer(response(refresh,200,"Expires: 60\r\n"),8);
  assert(h.notification(dialog_notify(refresh,3,xml(4,"partial","<dialog id='one'><state>confirmed</state></dialog>")),9).rfind("SIP/2.0 200",0)==0);
  h.client.tick(10);assert(h.first.quiet()&&!h.client.blf_state(0)->available);
  assert(h.notification(dialog_notify(refresh,4,xml(5,"full","")),11).rfind("SIP/2.0 200",0)==0);
  assert(h.client.blf_state(0)->available&&h.client.blf_state(0)->state==BlfState::IDLE);
 } else if(scenario=="replace") {
  assert(h.client.set_blf_watch(0,0,account,"replacement","sip:102@example.test",6));
  assert(h.client.blf_watch_id(0)=="replacement");h.client.tick(6);auto stop=h.first.receive();
  assert(field(stop,"Expires")=="0"&&field(stop,"To").find("sip:101@")!=std::string::npos);
  h.answer(response(stop,200,"Expires: 0\r\n"),7);
  assert(h.notification(dialog_notify(stop,2,"","terminated;reason=timeout"),8).rfind("SIP/2.0 200",0)==0);
  h.client.update_blf_account(0,account,9);h.client.tick(9);auto fresh=h.first.receive();
  assert(field(fresh,"To").find("sip:102@")!=std::string::npos&&field(fresh,"Call-ID")!=field(sub,"Call-ID"));
  h.answer(response(fresh,200,"Expires: 60\r\n"),10);
  assert(h.notification(dialog_notify(fresh,1,xml(0,"full","")),11).rfind("SIP/2.0 200",0)==0);
  assert(h.client.blf_state(0)->contact_id=="replacement"&&h.client.blf_state(0)->available);
  h.client.invalidate_blf_account(0);assert(owner.remove_account(h.config.id));
  auto foreign=h.config;foreign.id="foreign";assert(owner.set_account(foreign,12));
  h.client.update_blf_account(0,owner.account(0),13);h.client.tick(13);
  assert(!h.client.blf_state(0)->available&&h.first.quiet());
 } else return 2;
 assert(h.client.blf_state(0)==stable);
}
'''
    source = folder / "blf.cpp"
    source.write_text(code)
    binary = folder / "blf"
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie",
        "-DUSE_ESPHOME_VOIP_REGISTRATION", "-DUSE_ESPHOME_VOIP_BLF",
        *(["-DUSE_ESPHOME_VOIP_MWI"] if request.param else []),
        "-I", str(folder), "-I", str(SOURCE), str(source), str(SOURCE / "sip_mwi.cpp"),
        str(SOURCE / "sip_blf.cpp"), str(SOURCE / "sip_registration.cpp"), "-lcrypto", "-o", str(binary)],
        capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("scenario", ["shared", "early-gap", "replace", "flow", "auth", "calling"])
@pytest.mark.parametrize("transport", ["udp", "tcp"])
def test_shared_subscriptions_preserve_wire_and_lifecycle_ownership(blf_binary, scenario, transport):
    result = subprocess.run([blf_binary, scenario, transport], capture_output=True, text=True, timeout=8)
    assert result.returncode == 0, result.stderr
