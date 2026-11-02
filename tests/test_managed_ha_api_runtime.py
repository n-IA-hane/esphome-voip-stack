"""Run the real native API adapter with the Home provisioning payload."""
import subprocess

import pytest

from test_refer_dialog_runtime import SOURCE


@pytest.fixture(scope="module")
def managed_api_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("managed-native-api")
    header = (SOURCE / "ha_integration.h").read_text()
    adapter = header[header.index("class VoipHAIntegration"):header.index("}  // namespace")]
    source = folder / "adapter.cpp"
    source.write_text(r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
using Value = std::variant<std::string,int32_t,bool>;
namespace esphome {
namespace setup_priority { constexpr float AFTER_CONNECTION=0; }
struct Component {virtual void setup(){};virtual float get_setup_priority()const{return 0;}
 template<class F>void defer(F f){f();}};
template<class T>struct Parented {T*parent_=nullptr;void set_parent(T*p){parent_=p;}};
namespace api {
struct CustomAPIDevice {
 struct Service {std::vector<std::string> args;std::function<void(const std::vector<Value>&)> invoke;};
 std::map<std::string,Service> services;
 template<class T,class...Args,size_t...I>
 void dispatch(T*owner,void(T::*fn)(Args...),const std::vector<Value>&values,std::index_sequence<I...>){
  (owner->*fn)(std::get<Args>(values[I])...);
 }
 template<class T,class...Args>
 void register_service(void(T::*fn)(Args...),const std::string&name,std::initializer_list<std::string>args={}){
  auto owner=static_cast<T*>(this);
  services[name]={{args},[this,owner,fn](const std::vector<Value>&values){
   assert(values.size()==sizeof...(Args));dispatch(owner,fn,values,std::index_sequence_for<Args...>{});}};
 }
};
}
namespace voip_stack {
struct SipAccountConfig {std::string id,registrar,domain,username,password,auth_username,proxy;
 uint16_t port=5060,proxy_port=5060;uint32_t expires=3600;bool tcp=false;};
struct VoipStack {
 SipAccountConfig account;int writes=0;bool persisted=false,tcp=false;std::string display;
 const char* configured_sip_transport_name()const{return tcp?"tcp":"udp";}
 void set_account(const SipAccountConfig&c,bool persist){account=c;persisted=persist;++writes;}
 void set_ha_display_name(const std::string&s){display=s;}
 void publish_entity_states(){}
 void remove_account(std::string,bool){}void reset_account(std::string){}void register_account(std::string){}
 void unregister_account(std::string){}void select_line(std::string){}void start_call_on_line(std::string,std::string){}
 void call(std::string){}void call_via_ha(std::string,std::string){}void hangup_call_if_id(std::string){}
 void answer_call(){}void decline_call(std::string){}void stop(){}void send_dtmf(char){}
 void set_media_route(std::string,std::string){}void set_ha_peer_name(std::string){}
 void set_contacts(std::string){}void add_contact(std::string){}void remove_contact(std::string){}
 void flush_contacts(){}void update_contacts(){}
};
''' + adapter + r'''
}}
using namespace esphome::voip_stack;
int main(int argc,char**argv){
 assert(argc==3);const bool tcp=std::string(argv[1])=="tcp";const std::string scenario=argv[2];
 VoipStack parent;parent.tcp=tcp;VoipHAIntegration adapter;adapter.set_parent(&parent);adapter.setup();
 assert(adapter.services.count("set_account"));
 auto&service=adapter.services.at("set_account");
 const std::vector<std::string>expected{"account_id","registrar","port","domain","username","password","auth_username","proxy","proxy_port","transport","expires","persist"};
 assert(service.args==expected);
 const std::string id=scenario=="manual-auto"?"qa":"ha";
 std::vector<Value>payload{std::string(id),std::string("192.0.2.10"),int32_t(5060),std::string("192.0.2.10"),
  std::string("controlled-user"),std::string("controlled-private-value"),std::string("controlled-user"),std::string(""),
  int32_t(5060),std::string(scenario=="explicit"?"udp":"auto"),int32_t(3600),true};
 service.invoke(payload);
 if(scenario=="manual-auto"){assert(parent.writes==0);return 0;}
 if(scenario=="auto"){
#ifdef USE_VOIP_HA_AUTO_REGISTER
  assert(parent.writes==1);
  assert(parent.account.tcp==tcp);
#else
  assert(parent.writes==0);return 0;
#endif
 }else{assert(parent.writes==1&&!parent.account.tcp);}
 assert(parent.persisted&&parent.account.id=="ha"&&parent.account.password=="controlled-private-value");
 assert(parent.account.registrar=="192.0.2.10"&&parent.account.domain=="192.0.2.10"&&parent.account.port==5060);
 assert(parent.account.username==parent.account.auth_username&&parent.account.expires==3600);
#ifdef USE_VOIP_HA_AUTO_REGISTER
 assert(adapter.services.count("set_ha_display_name"));
 auto&name=adapter.services.at("set_ha_display_name");assert(name.args==std::vector<std::string>{"name"});
 name.invoke({std::string("Ingresso")});assert(parent.display=="Ingresso");
#else
 assert(!adapter.services.count("set_ha_display_name"));
#endif
}
''')
    binaries = {}
    for managed in (False, True):
        binary = folder / ("managed" if managed else "legacy")
        args = ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_ESPHOME_VOIP_REGISTRATION"]
        if managed:
            args.append("-DUSE_VOIP_HA_AUTO_REGISTER")
        result = subprocess.run(args + [str(source), "-o", str(binary)], capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        binaries[managed] = binary
    return binaries


@pytest.mark.parametrize("managed", [True, False])
@pytest.mark.parametrize("transport", ["udp", "tcp"])
@pytest.mark.parametrize("scenario", ["auto", "explicit", "manual-auto"])
def test_inventory_callback_executes_home_account_payload(managed_api_binary, managed, transport, scenario):
    run = subprocess.run([managed_api_binary[managed], transport, scenario], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr
