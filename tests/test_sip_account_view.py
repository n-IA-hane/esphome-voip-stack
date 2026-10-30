"""Credential snapshots remain owned after account replacement or slot reuse."""

import subprocess

from test_refer_dialog_runtime import SOURCE, block


def test_account_snapshot_is_copied_and_callback_never_runs_under_sip_lock(tmp_path):
    header = tmp_path / "esphome/core/defines.h"
    header.parent.mkdir(parents=True)
    header.write_text("")
    transport = (SOURCE / "sip_registration_transport.cpp").read_text()
    component = (SOURCE / "voip_registration.cpp").read_text()
    code = r'''
#include "sip_account_view.h"
#include <cassert>
#include <functional>
#include <memory>
#include <vector>
using namespace esphome::voip_stack;
bool sip_lock=false;
struct LockGuard { explicit LockGuard(int &){assert(!sip_lock);sip_lock=true;}~LockGuard(){sip_lock=false;} };
struct SipTransport {
 SipRegistration registration_;int dialog_mutex_=0;
 bool get_account_snapshot(const std::string&,SipAccountSnapshot*);
};
struct VoipStack {
 std::unique_ptr<SipTransport> transport_{new SipTransport};
 std::vector<std::function<void()>> work;
 void defer(std::function<void()>fn){work.emplace_back(std::move(fn));}
 struct Trigger {void trigger(const std::string&,const char*,uint16_t){assert(!sip_lock);}} registration_state_trigger_;
 struct Callbacks {
  std::function<void(const std::string&)> observer;
  void call(const std::string&id){assert(!sip_lock);if(observer)observer(id);}
 } account_changed_callbacks_;
 bool get_account_snapshot(const std::string&,SipAccountSnapshot*);
 void publish_account_changed_(const std::string&);
 static void registration_state_callback_(void*,const std::string&,RegistrationState,uint16_t);
 void loop(){auto pending=std::move(work);work.clear();for(auto&fn:pending)fn();}
};
'''
    for text, marker in [
        (transport, "bool SipTransport::get_account_snapshot("),
        (component, "bool VoipStack::get_account_snapshot("),
        (component, "void VoipStack::publish_account_changed_("),
        (component, "void VoipStack::registration_state_callback_("),
    ]:
        code += block(text, marker) + "\n"
    code += r'''
int main(){
 VoipStack phone;SipAccountConfig cfg;cfg.id="ha";cfg.registrar=cfg.domain="pbx.example.test";
 cfg.username="technical-name";cfg.password="public-test-fixture";
 assert(phone.transport_->registration_.set_account(cfg,0));
 SipAccountSnapshot first;assert(phone.get_account_snapshot("ha",&first));
 assert(first.revision==1&&!first.registered&&first.config.username==cfg.username);
 cfg.password="replacement-test-fixture";assert(phone.transport_->registration_.set_account(cfg,1));
 SipAccountSnapshot second;assert(phone.get_account_snapshot("ha",&second));
 assert(second.revision>first.revision&&second.config.password!=first.config.password);
 assert(first.config.password=="public-test-fixture");
 unsigned calls=0;
 phone.account_changed_callbacks_.observer=[&](const std::string&id){++calls;assert(id=="ha");};
 sip_lock=true;VoipStack::registration_state_callback_(&phone,"ha",RegistrationState::REGISTERED,200);sip_lock=false;
 assert(calls==0);phone.loop();assert(calls==1);
 assert(phone.transport_->registration_.remove_account("ha"));
 phone.publish_account_changed_("ha");phone.loop();assert(calls==2);
 assert(!phone.get_account_snapshot("ha",&second));
 assert(first.config.username=="technical-name");
 assert(!phone.get_account_snapshot("ha",nullptr));
}
'''
    source = tmp_path / "view.cpp"
    source.write_text(code)
    binary = tmp_path / "view"
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_ESPHOME_VOIP_REGISTRATION",
        "-I", str(tmp_path), "-I", str(SOURCE), str(source), str(SOURCE / "sip_registration.cpp"),
        "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
