"""Managed config changes retire old REGISTER generations on the same worker."""

from production_sip_source import read_production_source

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
    worker = block(read_production_source(SOURCE / "sip_transport.cpp"), "void SipTransport::sip_task_()")
    start = worker.index("#ifdef USE_VOIP_HA_AUTO_REGISTER") + len("#ifdef USE_VOIP_HA_AUTO_REGISTER")
    worker_stage = worker[start:worker.index("#endif", start)]
    harness = (ROOT / "tests/fixtures/registration_engine.cpp").read_text()
    code = harness[:harness.index("int main(")] + r'''
#include <atomic>
#include <cerrno>
#include <memory>
#include <thread>
#include "sip_mwi.h"
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
 SipMwi mwi_{};
 REPLACE_SUBSCRIPTIONS
 void pump_registration_mwi_(uint32_t);
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
 bool register_account(const std::string&,bool);
 bool queue_managed_account_(const SipAccountConfig&,bool,AccountCommit,void*);
 bool managed_account_needs_main_loop_()const;
 void notify_managed_account_main_loop_();
 void pump_managed_account_(uint32_t);
 REPLACE_RESUME
 void step(uint32_t now){clock_ms=now;
  for(unsigned stage=0;stage<3;++stage){
   std::thread worker([&]{registration_.tick(now);pump_registration_mwi_(now);WORKER_MANAGED_STAGE});worker.join();
   if(!main_wake.exchange(false))break;
   wake_registration_from_main_loop();
  }
 }
};
'''.replace("REPLACE_PENDING", block(header, "struct ManagedAccountUpdate {") + ";").replace(
    "REPLACE_SUBSCRIPTIONS", block(header, "bool registration_subscriptions_retired_(")).replace("void*=", "void * =").replace(
    "REPLACE_RESUME", block(header, "void wake_registration_from_main_loop()")).replace("WORKER_MANAGED_STAGE", worker_stage)
    for name in ["pump_registration_mwi_", "set_account", "remove_account", "queue_managed_account_", "managed_account_needs_main_loop_", "notify_managed_account_main_loop_", "pump_managed_account_", "register_account"]:
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
#include "managed_subscription_handover.h"
int main(int argc,char**argv){
 assert(argc==3);const std::string scenario=argv[1];Harness h(std::string(argv[2])=="tcp");
 h.hooks.send=[](void*ctx,size_t,std::string_view wire,uint32_t ip,uint16_t port,bool tcp){
  assert(std::this_thread::get_id()!=main_owner);
  auto&h=*static_cast<Harness*>(ctx);auto&peer=port==h.first.port?h.first:h.second;
  assert(ip==INADDR_LOOPBACK&&port==peer.port&&tcp==peer.tcp);
  return send(peer.esp,wire.data(),wire.size(),0)==ssize_t(wire.size());
 };
 h.manager.set_hooks(h.hooks);auto original=h.config();original.id="ha";
 if(scenario.rfind("watch-",0)==0) original.username="Waveshare S3 Audio";
 const auto feed=[&](Peer&peer,const std::string&wire,uint32_t now,int index=0){
  assert(h.manager.handle_response(peer.answer(wire),INADDR_LOOPBACK,peer.port,peer.tcp,index,now));
 };
 if(scenario=="bootstrap"){
  SipTransport transport{h.manager};Saved saved;
  transport.registration_context_=&transport;
  transport.registration_wake_callback_=[](void*ctx,SipTransport*){static_cast<SipTransport*>(ctx)->main_wake=true;};
  auto manual=original;manual.id="qa";manual.username="qa-user";manual.port=h.second.port;
  assert(transport.set_account(manual));
  auto excess=manual;excess.id="too-many";assert(!transport.set_account(excess));
  assert(transport.set_account(original,persist,&saved));transport.step(0);
  auto ha_request=h.first.receive(),qa_request=h.second.receive();
  assert(field(qa_request,"From").find("qa-user@")!=std::string::npos);
  assert(field(ha_request,"From").find(original.username+"@")!=std::string::npos);
  assert(field(ha_request,"Expires")=="3600"&&field(ha_request,"Via").find(h.first.tcp?"TCP":"UDP")!=std::string::npos);
  feed(h.first,h.success(ha_request),1,h.manager.find("ha"));
  feed(h.second,h.success(qa_request),1,h.manager.find("qa"));
  assert(h.manager.is_registered("ha")&&h.manager.is_registered("qa")&&saved.count==1);
  assert(transport.register_account("ha",true)); // the API follow-up remains a real account operation
  return 0;
 }
 assert(h.manager.set_account(original,0));std::thread initial_worker([&]{h.manager.tick(0);});initial_worker.join();
 auto initial=h.first.receive();feed(h.first,h.success(initial),1);
 assert(h.manager.is_registered("ha"));const auto revision=h.manager.account(0).config_revision;
 SipTransport transport{h.manager};Saved saved;
 transport.registration_context_=&transport;
 transport.registration_wake_callback_=[](void*context,SipTransport*){
  assert(std::this_thread::get_id()!=main_owner);
  static_cast<SipTransport*>(context)->main_wake=true;
 };
 if(scenario.rfind("watch-",0)==0){
  managed_subscription_handover(h, transport, original, saved, scenario);return 0;
 }
 if(scenario=="contact-retirement-replace"||scenario=="contact-retirement-remove"){
  assert(!h.first.tcp);
  assert(transport.register_account("ha",true));transport.step(2);
  const auto refresh=h.first.receive();
  auto mapped=h.success(refresh);
  mapped.replace(mapped.find(";rport"),6,";received=198.51.100.1;rport=41000");
  std::thread map_worker([&]{feed(h.first,mapped,3);});map_worker.join();
  const auto retirement=h.first.receive();assert(field(retirement,"Expires")=="0");
  feed(h.first,response(retirement,503),4);
  const auto retained=h.manager.account(0).retiring_contact;
  assert(!retained.empty()&&!h.manager.account(0).bound&&!h.manager.account(0).pending);
  auto replacement=original;replacement.username="updated-name";replacement.port=h.second.port;
  if(scenario=="contact-retirement-replace")assert(transport.set_account(replacement,persist,&saved));
  else assert(transport.remove_account("ha",persist,&saved));
  transport.step(5);
  assert(saved.count==0&&transport.managed_update_->active);
  assert(h.manager.account(0).config_revision==revision&&h.manager.account(0).retiring_contact==retained);
  assert(!h.manager.account(0).desired&&quiet(h.second));
  const auto retry=h.first.receive();assert(field(retry,"Expires")=="0");
  assert(field(retry,"Contact")==field(retirement,"Contact"));
  feed(h.first,response(retry,200),6);transport.step(7);
  assert(saved.count==1&&!transport.managed_update_->active);
  if(scenario=="contact-retirement-remove"){
   assert(saved.removed&&h.manager.find("ha")<0&&quiet(h.second));return 0;
  }
  assert(saved.config.username=="updated-name");
  const auto replacement_request=h.second.receive();
  assert(field(replacement_request,"From").find("updated-name@")!=std::string::npos);
  feed(h.second,h.success(replacement_request),8);assert(h.manager.is_registered("ha"));
  return 0;
 }
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
 if(scenario=="persist-fail-inactive")assert(transport.register_account("ha",false));
 transport.terminal=false;transport.step(5);auto unregister=h.first.receive();
 assert(field(unregister,"Expires")=="0"&&field(unregister,"From").find(original.username+"@")!=std::string::npos);
 const unsigned waiting_wakes=transport.wakes;
 for(unsigned n=0;n<4;++n)transport.step(5);
 assert(transport.wakes==waiting_wakes&&quiet(h.first)); // no main-context polling during the outstanding unregister
 auto latest=replacement;latest.username="latest-name";
 assert(transport.set_account(latest,persist,&saved));
 if(scenario=="remove")assert(transport.remove_account("ha",persist,&saved));
 const bool failed_write=scenario.rfind("persist-fail",0)==0;
 if(failed_write)saved.fail=true;
 if(scenario=="persist-fail-opt-out")assert(transport.register_account("ha",false));
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
 if(failed_write){
  assert(saved.count==0&&h.manager.account(0).config_revision==revision&&quiet(h.second));
  assert(!transport.managed_update_->active&&h.manager.account(0).config.username==original.username);
  if(scenario!="persist-fail"){
   assert(!h.manager.account(0).desired&&quiet(h.first));
   transport.step(1000);assert(quiet(h.first)&&quiet(h.second));return 0;
  }
  assert(h.manager.account(0).desired);
  const auto resume=h.first.receive();
  assert(field(resume,"Expires")=="3600"&&field(resume,"From")==field(initial,"From"));
  feed(h.first,h.success(resume),8);assert(h.manager.is_registered("ha"));
  const auto wakes=transport.wakes;
  transport.step(9);transport.step(10);
  assert(quiet(h.first)&&quiet(h.second)&&wakes==transport.wakes);
  saved.fail=false;assert(transport.set_account(latest,persist,&saved));transport.step(11);
  const auto retire_again=h.first.receive();assert(field(retire_again,"Expires")=="0");
  feed(h.first,h.success(retire_again,0),12);transport.step(13);
 }
 assert(saved.count==1&&!saved.removed&&saved.config.username=="latest-name");
 auto request=h.second.receive();assert(field(request,"From").find("latest-name@")!=std::string::npos);
 assert(field(request,"Call-ID")!=field(initial,"Call-ID"));
 assert(h.manager.account(0).config_revision!=revision&&transport.flow_retirements==1&&transport.dns_invalidations==1);
 const auto late=h.first.answer(h.success(initial));
 const uint32_t now=scenario=="timeout"?32007:14;
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
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-pthread", "-DUSE_ESPHOME_VOIP_REGISTRATION", "-DUSE_VOIP_HA_AUTO_REGISTER", "-DUSE_ESPHOME_VOIP_MWI",
        "-DUSE_ESPHOME_VOIP_BLF", "-DUSE_ESPHOME_VOIP_PRESENCE", "-I", str(ROOT / "tests/fixtures"),
        "-I", str(folder), "-I", str(SOURCE), str(source), str(SOURCE / "sip_registration.cpp"),
        str(SOURCE / "sip_mwi.cpp"), str(SOURCE / "sip_blf.cpp"),
        "-lcrypto", "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("scenario", [
    "bootstrap", "noop", "replace", "timeout", "remove",
    "persist-fail", "persist-fail-inactive", "persist-fail-opt-out",
    "watch-rejected", "watch-missing", "watch-timeout", "watch-notify", "watch-remove", "watch-refresh",
])
@pytest.mark.parametrize("transport", ["udp", "tcp"])
def test_managed_updates_are_idempotent_or_retire_one_old_generation(managed_binary, scenario, transport):
    result = subprocess.run([managed_binary, scenario, transport], capture_output=True, text=True, timeout=8)
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("operation", ["replace", "remove"])
def test_managed_changes_wait_for_contact_retirement_before_persisting(managed_binary, operation):
    result = subprocess.run([managed_binary, "contact-retirement-" + operation, "udp"],
                            capture_output=True, text=True, timeout=8)
    assert result.returncode == 0, result.stderr
