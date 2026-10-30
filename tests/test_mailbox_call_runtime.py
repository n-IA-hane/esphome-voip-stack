"""A standard MWI mailbox opens through the existing account call owner."""

import subprocess

import pytest

from test_refer_dialog_runtime import SOURCE, block


@pytest.fixture(scope="module", params=[False, True], ids=["internal", "signaling-psram"])
def mailbox_binary(tmp_path_factory, request):
    folder = tmp_path_factory.mktemp("mailbox-call")
    defines = folder / "esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    if request.param:
        (defines.parent / "helpers.h").write_text('''#pragma once
#include <cstdlib>
namespace esphome { template<class T> struct RAMAllocator {
using value_type=T;enum {ALLOC_EXTERNAL=1};explicit RAMAllocator(int=0){}
T*allocate(size_t n){return static_cast<T*>(std::malloc(n*sizeof(T)));}
void deallocate(T*p,size_t){std::free(p);}
}; }
''')
    transport = (SOURCE / "sip_registration_transport.cpp").read_text()
    waiting = (SOURCE / "voip_message_waiting.cpp").read_text()
    registration = (SOURCE / "voip_registration.cpp").read_text()
    message = (SOURCE / "sip_message.cpp").read_text()
    code = r'''
#include "sip_mwi.h"
#include "phonebook.h"
#include <algorithm>
#include <cctype>
#include <cassert>
using namespace esphome::voip_stack;
constexpr size_t VOIP_STACK_MAX_ROUTE_ID_LEN=64;
struct LockGuard{explicit LockGuard(int&){};};
'''
    for signature in ["std::string trim_copy(", "static bool is_sip_uri_unreserved(", "static uint8_t hex_value(",
                      "std::string sip_uri_user_encode(", "std::string sip_prefix_target_user(",
                      "std::string sip_normalize_dial_target("]:
        code += block(message, signature) + "\n"
    code += r'''
struct TestMwi:SipMwi {
 void store(size_t i,MessageWaitingState state){this->accounts_[i].state=std::move(state);}
 void publish(size_t i,MessageWaitingState state){this->publish_(i,std::move(state));}
};
struct SipTransport {
 int dialog_mutex_=0;SipRegistration registration_;TestMwi mwi_;
 bool get_message_waiting(const std::string&,MessageWaitingState*,const std::string&);
};
struct VoipStack {
 std::unique_ptr<SipTransport>transport_=std::make_unique<SipTransport>();
 SipTransport &owner=*transport_;
 std::string default_line_="office",pending_line_,pending_dialplan_target_,dialed,line;
 bool ready=true,force_direct_call_=false;unsigned calls=0;
 bool can_start_call_()const{return ready;}
 bool is_registered(const std::string&id){return owner.registration_.is_registered(id);}
 void call_(const std::string&t,const std::string&){++calls;dialed=t;line="direct";}
 void start_with_id_(const std::string&,const std::string& = {}){++calls;dialed=pending_dialplan_target_;line=pending_line_;}
 void start_call_on_line(const std::string&,const std::string&,const std::string& = {});
 MessageWaitingState get_message_waiting(const std::string&);
 bool mailbox_target_(const std::string&,const std::string&,std::string*,std::string*);
 bool can_call_mailbox(const std::string&="",const std::string&="");
 bool call_mailbox(const std::string&="",const std::string&="");
};
'''.replace('std::string&=""', 'std::string & = ""')
    code += block(transport, "bool SipTransport::get_message_waiting(") + "\n"
    code += block(registration, "void VoipStack::start_call_on_line(") + "\n"
    for signature in ["MessageWaitingState VoipStack::get_message_waiting(", "bool VoipStack::mailbox_target_(",
                      "bool VoipStack::can_call_mailbox(", "bool VoipStack::call_mailbox("]:
        code += block(waiting, signature) + "\n"
    code += r'''
MessageWaitingState mailbox(const std::string&id,const std::string&uri,bool has_new){
 MessageWaitingState state;state.account_id=id;
 assert(SipMwi::parse_summary("Messages-Waiting: "+std::string(has_new?"yes":"no")+"\r\n"+
  (uri.empty()?"":"Message-Account: "+uri+"\r\n")+"Voice-Message: "+std::string(has_new?"1":"0")+"/2\r\n",&state));return state;
}
int main(int argc,char**argv){
 assert(argc==2);const std::string scenario=argv[1];VoipStack phone;
 for(const auto*id:{"office","ha"}){SipAccountConfig c;c.id=id;c.registrar=c.domain="pbx.example";c.username=id;c.password="fixture";
  assert(phone.owner.registration_.set_account(c,0));auto&a=phone.owner.registration_.account(phone.owner.registration_.find(id));a.bound=true;a.state=RegistrationState::REGISTERED;}
 phone.owner.mwi_.store(0,mailbox("office","sip:*97@office.example;transport=tcp",false));
 phone.owner.mwi_.store(1,mailbox("ha","sip:mailbox.phone@home.example",true));
 if(scenario=="selected-waiting")phone.owner.mwi_.store(0,mailbox("office","sip:*97@office.example;transport=tcp",true));
 if(scenario=="none-waiting")phone.owner.mwi_.store(1,mailbox("ha","sip:mailbox.phone@home.example",false));
 if(scenario=="explicit"){
  assert(phone.can_call_mailbox("office")&&phone.calls==0);assert(phone.call_mailbox("office"));assert(phone.line=="office");return 0;
 }
 if(scenario=="override"){
  MessageWaitingState unavailable;unavailable.account_id="office";phone.owner.mwi_.store(0,unavailable);
  assert(!phone.can_call_mailbox("office"));assert(phone.can_call_mailbox("office","*97"));assert(phone.calls==0);
  assert(phone.call_mailbox("office","*97"));assert(phone.line=="office"&&phone.dialed=="*97");return 0;
 }
 if(scenario=="missing"){
  phone.owner.mwi_.store(0,mailbox("office","",true));assert(!phone.can_call_mailbox("office")&&!phone.call_mailbox("office")&&!phone.calls);return 0;
 }
 if(scenario=="invalid"){
  for(const auto*uri:{"sips:mailbox@pbx.example","https://pbx.example/vm","mailto:mailbox@pbx.example","sip:broken","sip:x@pbx.example;transport=udp;transport=tcp"}){
   phone.owner.mwi_.store(0,mailbox("office",uri,true));assert(!phone.can_call_mailbox("office")&&!phone.call_mailbox("office")&&!phone.calls);
  }return 0;
 }
 if(scenario=="busy"){
  phone.ready=false;assert(!phone.can_call_mailbox()&&!phone.call_mailbox()&&!phone.calls);return 0;
 }
 if(scenario=="uri-update"){
  unsigned updates=0;SipMwi::Hooks hooks;hooks.context=&updates;hooks.notify=[](void*ctx,const MessageWaitingState&){++*static_cast<unsigned*>(ctx);};phone.owner.mwi_.set_hooks(hooks);
  auto state=mailbox("office","sip:old@pbx.example",true);phone.owner.mwi_.publish(0,state);
  const auto before=updates;phone.owner.mwi_.publish(0,state);assert(updates==before);
  phone.owner.mwi_.publish(0,mailbox("office","sip:new@pbx.example",true));assert(updates==before+1);
  phone.owner.mwi_.publish(0,mailbox("office","",true));assert(updates==before+2&&phone.get_message_waiting("office").message_account.empty());return 0;
 }
 if(scenario=="parser"){
  MessageWaitingState state;assert(SipMwi::parse_summary("Messages-Waiting: yes\r\nMessage-Account: sip:x@pbx.example\r\n",&state));
  for(const auto&line:{std::string("Message-Account: <sip:x@pbx.example>\r\n"),std::string("Message-Account: not a uri\r\n"),std::string("Message-Account: sip:x@pbx.example\r\nMessage-Account: sip:y@pbx.example\r\n"),std::string("Message-Account: sip:")+std::string(513,'x')+"\r\n"})
   assert(!SipMwi::parse_summary("Messages-Waiting: yes\r\n"+line,&state));
  assert(state.message_account==std::string("sip:x@pbx.example"));return 0;
 }
 if(scenario=="direct-default")phone.default_line_="direct";
 assert(phone.can_call_mailbox()&&phone.calls==0);const auto previous=phone.default_line_;
 assert(phone.call_mailbox());assert(phone.calls==1&&phone.default_line_==previous);
 if(scenario=="selected-waiting"||scenario=="none-waiting")assert(phone.line=="office");else assert(phone.line=="ha");
}
'''
    source = folder / "mailbox.cpp"
    source.write_text(code)
    binary = folder / "mailbox"
    flags = ["-DUSE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM"] if request.param else []
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation", "-g",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_ESP32", "-DUSE_ESPHOME_VOIP_REGISTRATION",
        "-DUSE_ESPHOME_VOIP_MWI", "-DVOIP_REGISTRATION_CAPACITY=4", *flags, "-I", str(folder), "-I", str(SOURCE),
        str(source), str(SOURCE / "sip_mwi.cpp"), str(SOURCE / "sip_registration.cpp"), "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("scenario", ["aggregate", "selected-waiting", "none-waiting", "direct-default", "explicit", "override", "missing", "invalid", "busy", "uri-update", "parser"])
def test_mailbox_reuses_standard_account_call_and_one_state_owner(mailbox_binary, scenario):
    result = subprocess.run([mailbox_binary, scenario], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
