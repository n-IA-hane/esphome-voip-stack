"""Real REGISTER -> shared MWI/BLF recovery over framed UDP/TCP records."""
from pathlib import Path
import subprocess

import pytest

from test_refer_dialog_runtime import SOURCE, block

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope='module')
def recovery_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp('subscription-recovery')
    stub = folder / 'esphome/core/defines.h'
    stub.parent.mkdir(parents=True)
    stub.write_text('')
    adapter = (SOURCE / 'sip_registration_transport.cpp').read_text()
    harness = (ROOT / 'tests/test_sip_mwi_engine.cpp').read_text()
    code = harness[:harness.index('int main(')]
    old_receive = block(code, 'std::string receive()')
    code = code.replace('  std::string receive() {', '  std::string buffered;\n  std::string receive() {', 1)
    code = code.replace(old_receive, r'''std::string receive() {
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
    }''', 1)
    code = code.replace('    char c;\n    return recv(remote,', '    if(!buffered.empty())return false;\n    char c;\n    return recv(remote,', 1)
    code = code.replace('recv(remote, &c, 1, MSG_DONTWAIT)', 'recv(remote, &c, 1, MSG_DONTWAIT | MSG_PEEK)', 1)
    code += r'''
#include <map>
uint32_t clock_ms=0;uint32_t millis(){return clock_ms;}
struct LockGuard {explicit LockGuard(int&){};};
struct SipTransport {
 SipRegistration registration_;SipMwi &mwi_;Harness &wire;int dialog_mutex_=0;
 unsigned state_changes=0,wakes=0;
 explicit SipTransport(Harness&h):mwi_(h.client),wire(h){
  SipRegistration::Hooks hooks;hooks.context=this;hooks.local_port=5060;hooks.random=random_value;hooks.hash=hash_value;
  hooks.send=[](void*ctx,size_t i,const std::string&s,uint32_t ip,uint16_t port,bool tcp){auto&self=*static_cast<SipTransport*>(ctx);return self.wire.hooks.send(&self.wire,i,s,ip,port,tcp);};
  hooks.resolve=[](void*,size_t,const std::string&){return INADDR_LOOPBACK;};
  hooks.local_ip=[](void*,uint32_t){return std::string("127.0.0.1");};
  hooks.notify=[](void*ctx,const std::string&,RegistrationState,uint16_t){++static_cast<SipTransport*>(ctx)->state_changes;};
  REGISTER_HOOK
  registration_.set_hooks(hooks);
 }
 void wake_sip_task_(){++wakes;}
 bool register_account(const std::string&,bool);
 void pump_registration_mwi_(uint32_t);
 REGISTER_CALLBACK_DECL
 void step(uint32_t now){clock_ms=now;registration_.tick(now);pump_registration_mwi_(now);}
 void accept_register(const std::string&request,uint32_t now){
  const auto reply=response(request,200,"Contact: "+field(request,"Contact")+"\r\nExpires: 3600\r\n");
  assert(registration_.handle_response(wire.first.deliver(reply),INADDR_LOOPBACK,wire.first.port,wire.first.tcp,0,now));
 }
};
'''
    marker = 'void SipTransport::registration_binding_accepted_('
    present = marker in adapter
    code = code.replace('REGISTER_HOOK', 'hooks.binding_accepted = registration_binding_accepted_;' if present else '')
    code = code.replace('REGISTER_CALLBACK_DECL', 'static void registration_binding_accepted_(void*,size_t,uint32_t);' if present else '')
    for marker in ['bool SipTransport::register_account(', 'void SipTransport::pump_registration_mwi_(']:
        code += '\n' + block(adapter, marker)
    if present:
        initializer = block(adapter, 'void SipTransport::initialize_registration()')
        assert 'hooks.binding_accepted = registration_binding_accepted_;' in initializer
        code += '\n' + block(adapter, 'void SipTransport::registration_binding_accepted_(')
    code += r'''
std::string dialog_notify(const std::string&request,unsigned seq=1,const std::string&substate="active;expires=60"){
 auto result=notify(request,seq,"<dialog-info xmlns='urn:ietf:params:xml:ns:dialog-info' version='0' state='full' entity='sip:101@example.test'/>",substate);
 auto at=result.find("Event: message-summary");result.replace(at,22,"Event: dialog");
 at=result.find("application/simple-message-summary");result.replace(at,34,"application/dialog-info+xml");return result;
}
void accept(Harness&h,const std::string&request,uint32_t now,unsigned sequence=1){
 h.answer(response(request,200,"Expires: 60\r\n"),now);
 const auto event=field(request,"Event");
 const auto message=event=="dialog"?dialog_notify(request,sequence):notify(request,sequence,"Messages-Waiting: no\r\nVoice-Message: 0/0\r\n");
 assert(h.notification(message,now+1).rfind("SIP/2.0 200",0)==0);
}
std::map<std::string,std::string> subscriptions(Harness&h){
 std::map<std::string,std::string> records;
 for(unsigned n=0;n<2;++n){auto request=h.first.receive();assert(request.rfind("SUBSCRIBE ",0)==0);records[field(request,"Event")]=request;}
 assert(records.size()==2&&records.count("dialog")&&records.count("message-summary"));return records;
}
int main(int argc,char**argv){
 assert(argc==3);const std::string scenario=argv[1];Harness h(std::string(argv[2])=="tcp");
 if(scenario=="404"||scenario=="retry-after"){
  const auto first=h.start(0);h.answer(response(first,404,scenario=="retry-after"?"Retry-After: 7\r\n":""),1);
  assert(!h.client.state(0).available);const auto changes=h.updates.size();
  const uint32_t due=scenario=="retry-after"?7001:2001;
  h.client.tick(due-1);assert(h.first.quiet()&&h.updates.size()==changes);
  h.client.tick(due);assert(!h.first.quiet()&&"an enabled mailbox must be rediscovered after a transient 404");
  const auto retry=h.first.receive();assert(field(retry,"Call-ID")!=field(first,"Call-ID")&&field(retry,"From")!=field(first,"From"));
  accept(h,retry,due+1);assert(h.client.state(0).available&&!h.client.state(0).waiting);return 0;
 }
 SipTransport transport(h);assert(transport.registration_.set_account(h.config,0));
 assert(h.client.configure_blf(1));assert(h.client.set_blf_watch(0,0,transport.registration_.account(0),"contact","sip:101@example.test",0));
 transport.step(0);auto reg=h.first.receive();assert(reg.rfind("REGISTER ",0)==0&&h.first.quiet());
 transport.accept_register(reg,1);transport.step(2);auto initial=subscriptions(h);
 for(const auto&pair:initial)accept(h,pair.second,3);
 assert(h.client.state(0).available&&h.client.blf_state(0)->available);
 const auto revision=transport.registration_.account(0).config_revision;const unsigned states=transport.state_changes;
 clock_ms=10;assert(transport.register_account(h.config.id,true));transport.step(10);
 reg=h.first.receive();assert(reg.rfind("REGISTER ",0)==0&&h.first.quiet());
 transport.step(11);assert(h.first.quiet()); // recovery SUBSCRIBE must follow the accepted REGISTER
 transport.accept_register(reg,12);assert(transport.state_changes==states);transport.step(13);
 assert(!h.first.quiet()&&"successful renewed REGISTER must refresh existing subscriptions");
 auto refreshed=subscriptions(h);
 for(const auto&pair:refreshed){assert(field(pair.second,"Call-ID")==field(initial[pair.first],"Call-ID"));assert(field(pair.second,"From")==field(initial[pair.first],"From"));}
 if(scenario=="inflight"){
  clock_ms=14;assert(transport.register_account(h.config.id,true));transport.step(14);reg=h.first.receive();
  assert(reg.rfind("REGISTER ",0)==0);transport.accept_register(reg,15);transport.step(15);assert(h.first.quiet());
  for(const auto&pair:refreshed)accept(h,pair.second,16,2);
  transport.step(18);auto deferred=subscriptions(h);
  for(const auto&pair:deferred){assert(field(pair.second,"Call-ID")==field(initial[pair.first],"Call-ID"));accept(h,pair.second,19,3);}
  transport.step(21);assert(h.first.quiet());return 0;
 }
 if(scenario=="healthy"){
  const auto public_changes=h.updates.size();
  for(const auto&pair:refreshed)accept(h,pair.second,14,2);
  transport.step(16);assert(h.first.quiet()&&h.updates.size()==public_changes);return 0;
 }
 if(scenario=="backoff"){
  for(const auto&pair:refreshed)h.answer(response(pair.second,503,"Retry-After: 7\r\n"),14);
  clock_ms=20;assert(transport.register_account(h.config.id,true));transport.step(20);reg=h.first.receive();
  transport.accept_register(reg,21);transport.step(22);assert(h.first.quiet());
  uint32_t remaining=0;assert(h.client.next_deadline(22,&remaining)&&remaining==6992);
  transport.step(7013);assert(h.first.quiet());transport.step(7014);auto delayed=subscriptions(h);
  for(const auto&pair:delayed){assert(field(pair.second,"Call-ID")==field(initial[pair.first],"Call-ID"));accept(h,pair.second,7015,3);}
  transport.step(7017);assert(h.first.quiet());return 0;
 }
 if(scenario=="unsubscribe"){
  h.client.clear_blf_watch(0,14);
  clock_ms=14;assert(transport.register_account(h.config.id,true));transport.step(14);reg=h.first.receive();
  transport.accept_register(reg,15);transport.step(15);assert(h.first.quiet());
  for(const auto&pair:refreshed)accept(h,pair.second,16,2);
  transport.step(18);auto final=subscriptions(h);
  assert(field(final["dialog"],"Expires")=="0"&&field(final["message-summary"],"Expires")!="0");
  accept(h,final["message-summary"],19,3);
  h.answer(response(final["dialog"],200,"Expires: 0\r\n"),19);
  assert(h.notification(dialog_notify(final["dialog"],3,"terminated;reason=deactivated"),20).rfind("SIP/2.0 200",0)==0);
  clock_ms=30;assert(transport.register_account(h.config.id,true));transport.step(30);reg=h.first.receive();
  transport.accept_register(reg,31);transport.step(32);auto only=h.first.receive();
  assert(field(only,"Event")=="message-summary"&&h.first.quiet()&&!h.client.blf_state(0)->available);
  accept(h,only,33,4);return 0;
 }
 // The restarted PBX retained accounts but lost its subscription pool. It rejects
 // old dialog refreshes; recovery creates fresh dialogs through the same engine.
 for(const auto&pair:refreshed)h.answer(response(pair.second,481),14);
 assert(!h.client.state(0).available&&!h.client.blf_state(0)->available);
 transport.step(2013);assert(h.first.quiet());transport.step(2014);auto rebuilt=subscriptions(h);
 for(const auto&pair:rebuilt){assert(field(pair.second,"Call-ID")!=field(initial[pair.first],"Call-ID"));assert(field(pair.second,"From")!=field(initial[pair.first],"From"));accept(h,pair.second,2015);}
 assert(transport.registration_.account(0).config_revision==revision&&transport.registration_.is_registered(h.config.id));
 assert(h.client.state(0).available&&h.client.blf_state(0)->available);transport.step(2017);assert(h.first.quiet());
}
'''
    source = folder / 'recovery.cpp'
    source.write_text(code)
    binary = folder / 'recovery'
    result = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation', '-g', '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-DUSE_ESPHOME_VOIP_REGISTRATION', '-DUSE_ESPHOME_VOIP_MWI', '-DUSE_ESPHOME_VOIP_BLF', '-I', str(folder), '-I', str(SOURCE), str(source), str(SOURCE / 'sip_registration.cpp'), str(SOURCE / 'sip_mwi.cpp'), str(SOURCE / 'sip_blf.cpp'), '-lcrypto', '-o', str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize('scenario', ['404', 'retry-after', 'restart', 'healthy', 'inflight', 'backoff', 'unsubscribe'])
@pytest.mark.parametrize('transport', ['udp', 'tcp'])
def test_subscription_recovery_uses_real_registration_completion(recovery_binary, scenario, transport):
    result = subprocess.run([recovery_binary, scenario, transport], capture_output=True, text=True, timeout=8)
    assert result.returncode == 0, result.stderr
