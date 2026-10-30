"""Independent presence and dialog subscriptions share one bounded SIP engine."""

from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "esphome/components/voip_stack"


@pytest.fixture(scope="module", params=["blf", "mwi", "psram"], ids=["presence-blf", "presence-blf-mwi", "presence-signaling-psram"])
def presence_binary(tmp_path_factory, request):
    folder = tmp_path_factory.mktemp("presence-wire")
    defines = folder / "esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    if request.param == "psram":
        (defines.parent / "helpers.h").write_text('''#pragma once
#include <cstdlib>
namespace esphome {
inline void* external_blocks[128]{};
inline unsigned external_allocations=0;
inline bool external_owns(const void*p){for(auto*block:external_blocks)if(block==p)return true;return false;}
template<class T> struct RAMAllocator {
 using value_type=T;
 enum {ALLOC_EXTERNAL=1};
 explicit RAMAllocator(int flags=0):flags_(flags){}
 T* allocate(size_t n){auto*p=static_cast<T*>(std::malloc(n*sizeof(T)));for(auto&block:external_blocks)if(!block){block=p;++external_allocations;return p;}std::abort();}
 void deallocate(T*p,size_t){for(auto&block:external_blocks)if(block==p){block=nullptr;break;}std::free(p);}
 int flags_;
};
}
''')
    harness = (ROOT / "tests/test_sip_mwi_engine.cpp").read_text()
    code = harness[:harness.index("int main(")]
    if request.param == "psram":
        inspect = r'''
struct AuditedClient : SipMwi {
 void verify_metadata_placement() const {
  for(size_t i=0;i<this->total_capacity_();++i){const auto&a=this->account_(i);if(!a.occupied)continue;
   for(const auto*field:{&a.contact,&a.call_id,&a.tag,&a.remote_tag,&a.branch,&a.remote_target,&a.routes,
                         &a.last_notify_id,&a.last_notify_tag,&a.last_notify_remote_tag,&a.last_notify_branch})
    if(field->size()>15)assert(esphome::external_owns(field->data()));
  }
 }
};
'''
        code = code.replace("struct Harness {", inspect + "\nstruct Harness {")
        code = code.replace("  SipMwi client;", "  AuditedClient client;")
    code = code.replace("MSG_DONTWAIT)", "MSG_DONTWAIT | MSG_PEEK)")
    # TCP records may be coalesced by the kernel. Frame complete SIP messages.
    start = code.index("  std::string receive() {")
    end = code.index("  std::string deliver(", start)
    code = code[:start] + r'''
  std::string buffered;
  std::string receive() {
    for (;;) {
      if (tcp) {
        const auto end=buffered.find("\r\n\r\n");
        if(end!=std::string::npos){const auto len=field(buffered,"Content-Length");
          const size_t count=end+4+(len.empty()?0:std::stoul(len));
          if(buffered.size()>=count){auto record=buffered.substr(0,count);buffered.erase(0,count);return record;}}
      }
      char bytes[8192];const auto count=recv(remote,bytes,sizeof(bytes),0);assert(count>0);
      if(!tcp)return std::string(bytes,size_t(count));buffered.append(bytes,size_t(count));
    }
  }
''' + code[end:]
    code = code.replace("  bool quiet() {", "  bool quiet() {\n    if(!buffered.empty()) return false;")
    have_presence = "enum class PresenceState" in (SOURCE / "sip_blf.h").read_text()
    code += r'''
#include <map>
std::vector<BlfStateInfo> events;
std::string event_notify(const std::string&r,unsigned seq,const std::string&body,const std::string&state="active;expires=60"){
 const auto event=field(r,"Event");
 auto text=notify(r,seq,body,state,event=="presence"?"application/pidf+xml":"application/dialog-info+xml");
 const auto at=text.find("Event: message-summary");text.replace(at,22,"Event: "+event);return text;
}
std::string pidf(const char*state){return std::string("<presence xmlns='urn:ietf:params:xml:ns:pidf' entity='sip:101@example.test'><tuple id='phone'><status><basic>")+state+"</basic></status></tuple></presence>";}
int main(int argc,char**argv){
 assert(argc==3);const std::string scenario=argv[1];(void)scenario;Harness h(std::string(argv[2])=="tcp");
 h.hooks.notify_blf=[](void*,const BlfStateInfo&s){events.push_back(s);};h.client.set_hooks(h.hooks);
 SipRegistration owner;assert(owner.set_account(h.config,0));auto&a=owner.account(0);
 a.bound=true;a.target_ip=INADDR_LOOPBACK;a.target_port=h.first.port;a.contact="sip:alice@127.0.0.1:5060";
#ifdef USE_ESPHOME_VOIP_MWI
 h.client.update_registered_account(0,a,0);
#endif
 assert(h.client.configure_blf(1));assert(h.client.set_blf_watch(0,0,a,"contact","sip:101@example.test",0));h.client.tick(0);
 std::map<std::string,std::string> requests;
 auto request=h.first.receive();requests[field(request,"Event")]=request;
 assert(!h.first.quiet()&&"dialog-info idle alone cannot establish endpoint availability; presence subscription is required");
 request=h.first.receive();requests[field(request,"Event")]=request;
#ifdef USE_ESPHOME_VOIP_MWI
 request=h.first.receive();requests[field(request,"Event")]=request;
 assert(requests.size()==3&&requests.count("message-summary"));
 h.answer(response(requests["message-summary"],200,"Expires: 60\r\n"),1);
 assert(h.notification(notify(requests["message-summary"]),2).rfind("SIP/2.0 200",0)==0);
#else
 assert(requests.size()==2);
#endif
 assert(requests.count("dialog")&&requests.count("presence"));
 auto dialog=requests["dialog"],presence=requests["presence"];
 assert(field(dialog,"Call-ID")!=field(presence,"Call-ID"));
 assert(field(presence,"Accept")=="application/pidf+xml");
 h.answer(response(dialog,200,"Expires: 60\r\n"),1);
 assert(h.notification(event_notify(dialog,1,"<dialog-info xmlns='urn:ietf:params:xml:ns:dialog-info' version='0' state='full' entity='sip:101@example.test'/>"),2).rfind("SIP/2.0 200",0)==0);
#ifdef TEST_PRESENCE_API
 assert(h.client.blf_state(0)->available&&h.client.blf_state(0)->state==BlfState::IDLE&&h.client.blf_state(0)->presence==PresenceState::UNKNOWN);
 if(scenario=="unsupported"){
  h.answer(response(presence,489),3);assert(h.client.blf_state(0)->available&&h.client.blf_state(0)->presence==PresenceState::UNKNOWN);
  h.client.tick(4);assert(h.first.quiet());return 0;
 }
 if(scenario=="auth"){
  h.answer(response(presence,401,"WWW-Authenticate: Digest realm=\"example.test\", nonce=\"presence\", qop=\"auth\", algorithm=SHA-256\r\n"),3);
  const auto authorized=h.first.receive();assert(field(authorized,"Event")=="presence"&&field(authorized,"Authorization").find("uri=\"sip:101@example.test\"")!=std::string::npos);
  assert(field(authorized,"Call-ID")==field(presence,"Call-ID"));presence=authorized;
 }
 h.answer(response(presence,200,"Expires: 60\r\n"),4);
 assert(h.notification(event_notify(presence,1,pidf("closed")),5).rfind("SIP/2.0 200",0)==0);
 assert(h.client.blf_state(0)->presence==PresenceState::CLOSED&&h.client.blf_state(0)->state==BlfState::IDLE);
 const auto before=events.size();
#ifdef TEST_METADATA_PSRAM
 h.client.verify_metadata_placement();
 const auto allocations_before=esphome::external_allocations;
#endif
 assert(h.notification(event_notify(presence,2,pidf("open")),6).rfind("SIP/2.0 200",0)==0);
 assert(h.client.blf_state(0)->presence==PresenceState::OPEN&&events.size()>before);
#ifdef TEST_METADATA_PSRAM
 h.client.verify_metadata_placement();
 assert(esphome::external_allocations==allocations_before&&"same-size NOTIFY must reuse retained external metadata");
#endif
 if(scenario=="malformed"){
  assert(h.notification(event_notify(presence,3,"<presence xmlns='wrong'><tuple id='x'><status><basic>closed</basic></status></tuple></presence>"),7).rfind("SIP/2.0 400",0)==0);
  assert(h.client.blf_state(0)->presence==PresenceState::OPEN&&h.client.blf_state(0)->available);
  return 0;
 }
 if(scenario=="source"){
  assert(h.notification(event_notify(presence,3,pidf("closed")),7,INADDR_LOOPBACK+1,h.first.port).rfind("SIP/2.0 481",0)==0);
  assert(h.client.blf_state(0)->presence==PresenceState::OPEN);return 0;
 }
 if(scenario=="restart"){
  h.client.on_registration_accepted(0,a,7);h.client.tick(7);
  std::map<std::string,std::string> refresh;for(size_t n=0;n<requests.size();++n){auto r=h.first.receive();refresh[field(r,"Event")]=r;}
  for(auto&pair:refresh){assert(field(pair.second,"Call-ID")==field(requests[pair.first],"Call-ID"));h.answer(response(pair.second,481),8);}
  assert(!h.client.blf_state(0)->available&&h.client.blf_state(0)->presence==PresenceState::UNKNOWN);
  h.client.tick(2008);for(size_t n=0;n<requests.size();++n){auto r=h.first.receive();assert(field(r,"Call-ID")!=field(requests[field(r,"Event")],"Call-ID"));}return 0;
 }
 if(scenario=="replace"){
  assert(h.client.set_blf_watch(0,0,a,"other","sip:102@example.test",7));h.client.tick(7);
  std::vector<std::string> retired;for(int n=0;n<2;++n)retired.push_back(h.first.receive());
  for(const auto&r:retired){assert(field(r,"Expires")=="0");h.answer(response(r,200,"Expires: 0\r\n"),8);assert(h.notification(event_notify(r,3,"","terminated;reason=deactivated"),9).rfind("SIP/2.0 200",0)==0);}
  h.client.update_blf_account(0,a,10);h.client.tick(10);
  for(int n=0;n<2;++n){auto r=h.first.receive();assert(field(r,"To").find("sip:102@")!=std::string::npos);}
  assert(h.client.blf_state(0)->presence==PresenceState::UNKNOWN);return 0;
 }
 if(scenario=="expire"){
  h.client.tick(60006);assert(h.client.blf_state(0)->presence==PresenceState::UNKNOWN);return 0;
 }
 h.client.invalidate_blf_account(0);assert(!h.client.blf_state(0)->available&&h.client.blf_state(0)->presence==PresenceState::UNKNOWN);
#endif
}
'''
    source = folder / "presence.cpp"
    source.write_text(code)
    binary = folder / "presence"
    flags = ["-DTEST_PRESENCE_API"] if have_presence else []
    if request.param != "blf":
        flags.append("-DUSE_ESPHOME_VOIP_MWI")
    if request.param == "psram":
        flags.extend(["-DUSE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM", "-DTEST_METADATA_PSRAM"])
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation", "-g",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie",
        "-DUSE_ESPHOME_VOIP_REGISTRATION", "-DUSE_ESPHOME_VOIP_BLF", "-DUSE_ESPHOME_VOIP_PRESENCE", *flags,
        "-I", str(folder), "-I", str(SOURCE), str(source), str(SOURCE / "sip_mwi.cpp"),
        str(SOURCE / "sip_blf.cpp"), str(SOURCE / "sip_registration.cpp"), "-lcrypto", "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("transport", ["udp", "tcp"])
@pytest.mark.parametrize("scenario", ["state", "auth", "unsupported", "malformed", "source", "restart", "replace", "expire"])
def test_presence_and_dialog_have_independent_real_sip_dialogs(presence_binary, scenario, transport):
    result = subprocess.run([presence_binary, scenario, transport], capture_output=True, text=True, timeout=8)
    assert result.returncode == 0, result.stderr
