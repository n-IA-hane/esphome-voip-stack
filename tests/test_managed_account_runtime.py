"""Managed config changes retire old REGISTER generations on the same worker."""

import subprocess

import pytest

from test_refer_dialog_runtime import ROOT, SOURCE, block


@pytest.fixture(scope="module")
def managed_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("managed-account")
    defines = folder / "esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    header = (SOURCE / "sip_transport.h").read_text()
    adapter = (SOURCE / "sip_registration_transport.cpp").read_text()
    worker = block((SOURCE / "sip_transport.cpp").read_text(), "void SipTransport::sip_task_()")
    start = worker.index("#ifdef USE_VOIP_HA_AUTO_REGISTER") + len("#ifdef USE_VOIP_HA_AUTO_REGISTER")
    worker_stage = worker[start:worker.index("#endif", start)]
    harness = (ROOT / "tests/fixtures/registration_engine.cpp").read_text()
    code = harness[:harness.index("int main(")] + r'''
#include <atomic>
#include <cerrno>
#include <memory>
#include <thread>
const std::thread::id main_owner=std::this_thread::get_id();
uint32_t clock_ms=0;
uint32_t millis(){return clock_ms;}
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
struct LockGuard {explicit LockGuard(int&){};};
struct SipTransport {
 using AccountCommit=bool(*)(void*,const std::string&,const SipAccountConfig*);
 SipRegistration &registration_;int dialog_mutex_=0;
 bool active_call=false,terminal=false;
 unsigned wakes=0,flow_retirements=0,dns_invalidations=0,notifications=0;
 std::atomic<uint16_t> managed_retirement_status_{0};
 std::atomic<bool> running_{true},transport_stopping_{false},main_wake{false};
 void (*registration_wake_callback_)(void*,SipTransport*)=nullptr;void *registration_context_=nullptr;
 REPLACE_PENDING
 std::unique_ptr<ManagedAccountUpdate> managed_update_{new ManagedAccountUpdate};
 bool account_in_use_(int i)const{return i>=0&&(active_call||terminal);}
 void wake_sip_task_(){++wakes;}
 void retire_registration_flow_(size_t){++flow_retirements;}
 void invalidate_registration_dns_(size_t){++dns_invalidations;}
 static void registration_notify_(void*ctx,const std::string&,RegistrationState,uint16_t){++static_cast<SipTransport*>(ctx)->notifications;}
 bool set_account(const SipAccountConfig&,AccountCommit=nullptr,void*=nullptr);
 bool remove_account(const std::string&,AccountCommit=nullptr,void*=nullptr,bool=false);
 bool queue_managed_account_(const SipAccountConfig&,bool,AccountCommit,void*);
 bool managed_account_needs_main_loop_()const;
 void notify_managed_account_main_loop_();
 void pump_managed_account_(uint32_t);
 REPLACE_RESUME
 void step(uint32_t now){clock_ms=now;
  for(unsigned stage=0;stage<3;++stage){
   std::thread worker([&]{registration_.tick(now);WORKER_MANAGED_STAGE});worker.join();
   if(!main_wake.exchange(false))break;
   wake_registration_from_main_loop();
  }
 }
};
'''.replace("REPLACE_PENDING", block(header, "struct ManagedAccountUpdate {") + ";").replace("void*=", "void * =").replace(
    "REPLACE_RESUME", block(header, "void wake_registration_from_main_loop()")).replace("WORKER_MANAGED_STAGE", worker_stage)
    for name in ["set_account", "remove_account", "queue_managed_account_", "managed_account_needs_main_loop_", "notify_managed_account_main_loop_", "pump_managed_account_"]:
        marker = next(line for line in adapter.splitlines() if "SipTransport::" + name + "(" in line)
        code += block(adapter, marker) + "\n"
    code += r'''
struct Saved {unsigned count=0;bool fail=false,removed=false;SipAccountConfig config;};
bool persist(void*context,const std::string&id,const SipAccountConfig*config){
 assert(std::this_thread::get_id()==main_owner);
 auto&s=*static_cast<Saved*>(context);assert(id=="ha");if(s.fail)return false;
 ++s.count;s.removed=config==nullptr;if(config)s.config=*config;return true;
}
bool quiet(Peer &peer){char c;return recv(peer.peer,&c,1,MSG_DONTWAIT)<0&&(errno==EAGAIN||errno==EWOULDBLOCK);}
int main(int argc,char**argv){
 assert(argc==3);const std::string scenario=argv[1];Harness h(std::string(argv[2])=="tcp");
 h.hooks.send=[](void*ctx,size_t,const std::string&wire,uint32_t ip,uint16_t port,bool tcp){
  assert(std::this_thread::get_id()!=main_owner);
  auto&h=*static_cast<Harness*>(ctx);auto&peer=port==h.first.port?h.first:h.second;
  assert(ip==INADDR_LOOPBACK&&port==peer.port&&tcp==peer.tcp);
  return send(peer.esp,wire.data(),wire.size(),0)==ssize_t(wire.size());
 };
 h.manager.set_hooks(h.hooks);auto original=h.config();original.id="ha";
 const auto feed=[&](Peer&peer,const std::string&wire,uint32_t now){
  assert(h.manager.handle_response(peer.answer(wire),INADDR_LOOPBACK,peer.port,peer.tcp,0,now));
 };
 assert(h.manager.set_account(original,0));std::thread initial_worker([&]{h.manager.tick(0);});initial_worker.join();
 auto initial=h.first.receive();feed(h.first,h.success(initial),1);
 assert(h.manager.is_registered("ha"));const auto revision=h.manager.account(0).config_revision;
 SipTransport transport{h.manager};Saved saved;
 transport.registration_context_=&transport;
 transport.registration_wake_callback_=[](void*context,SipTransport*){
  assert(std::this_thread::get_id()!=main_owner);
  static_cast<SipTransport*>(context)->main_wake=true;
 };
 if(scenario=="noop"){
  transport.active_call=true;
  assert(transport.set_account(original,persist,&saved));transport.step(2);
  assert(!transport.managed_update_->active&&saved.count==1&&transport.flow_retirements==0);
  assert(h.manager.account(0).config_revision==revision&&h.manager.is_registered("ha"));
  assert(quiet(h.first)&&transport.active_call);return 0;
 }
 auto replacement=original;replacement.username="new-name";replacement.password="replacement-test-value";replacement.port=h.second.port;
 transport.active_call=true;assert(transport.set_account(replacement,persist,&saved));transport.step(3);
 assert(quiet(h.first)&&h.manager.account(0).config_revision==revision&&saved.count==0);
 assert(!transport.main_wake.load()&&transport.wakes==1); // queue wake only, no main polling while a call owns it
 transport.active_call=false;transport.terminal=true;transport.step(4);assert(quiet(h.first)&&transport.wakes==1);
 transport.terminal=false;transport.step(5);auto unregister=h.first.receive();
 assert(field(unregister,"Expires")=="0"&&field(unregister,"From").find(original.username+"@")!=std::string::npos);
 const unsigned waiting_wakes=transport.wakes;
 for(unsigned n=0;n<4;++n)transport.step(5);
 assert(transport.wakes==waiting_wakes&&quiet(h.first)); // no main-context polling during the outstanding unregister
 auto latest=replacement;latest.username="latest-name";
 assert(transport.set_account(latest,persist,&saved));
 if(scenario=="remove")assert(transport.remove_account("ha",persist,&saved));
 if(scenario=="persist-fail")saved.fail=true;
 if(scenario=="timeout"){
  transport.step(32005);
  transport.step(32006);assert(transport.managed_retirement_status_.load()==408);
 }else{
  feed(h.first,h.success(unregister,0),6);transport.step(7);
 }
 if(scenario=="remove"){
  assert(saved.count==1&&saved.removed&&h.manager.find("ha")<0);
  assert(!transport.managed_update_->active&&quiet(h.second));return 0;
 }
 if(scenario=="persist-fail"){
  assert(saved.count==0&&h.manager.account(0).config.username==original.username&&quiet(h.second));
  saved.fail=false;assert(transport.set_account(latest,persist,&saved));transport.step(8);
 }
 assert(saved.count==1&&!saved.removed&&saved.config.username=="latest-name");
 auto request=h.second.receive();assert(field(request,"From").find("latest-name@")!=std::string::npos);
 assert(field(request,"Call-ID")!=field(initial,"Call-ID"));
 assert(h.manager.account(0).config_revision!=revision&&transport.flow_retirements==1&&transport.dns_invalidations==1);
 const auto late=h.first.answer(h.success(initial));
 const uint32_t now=scenario=="timeout"?32007:9;
 const auto current_request=h.manager.account(0).request;
 assert(h.manager.handle_response(late,INADDR_LOOPBACK,h.first.port,h.first.tcp,0,now));
 assert(h.manager.account(0).request==current_request&&quiet(h.first)&&quiet(h.second));
 assert(h.manager.account(0).pending&&h.manager.account(0).config.username=="latest-name");
 feed(h.second,h.success(request),now+1);assert(h.manager.is_registered("ha"));
 assert(!transport.managed_update_->active);
}
'''
    source = folder / "managed.cpp"
    source.write_text(code)
    binary = folder / "managed"
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-pthread", "-DUSE_ESPHOME_VOIP_REGISTRATION", "-DUSE_VOIP_HA_AUTO_REGISTER",
        "-I", str(folder), "-I", str(SOURCE), str(source), str(SOURCE / "sip_registration.cpp"),
        "-lcrypto", "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("scenario", ["noop", "replace", "timeout", "remove", "persist-fail"])
@pytest.mark.parametrize("transport", ["udp", "tcp"])
def test_managed_updates_are_idempotent_or_retire_one_old_generation(managed_binary, scenario, transport):
    result = subprocess.run([managed_binary, scenario, transport], capture_output=True, text=True, timeout=8)
    assert result.returncode == 0, result.stderr
