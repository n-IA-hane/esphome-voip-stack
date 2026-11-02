"""Combined 5+7: real call, subscription and managed account owners on wire."""
import subprocess
import resource
import pytest
from test_refer_dialog_runtime import ROOT, SOURCE, block
from test_mwi_transport_runtime import mwi_transport_source


@pytest.fixture(scope="module")
def managed_mwi_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("managed-mwi")
    core = folder / "esphome/core"
    core.mkdir(parents=True)
    (core / "defines.h").write_text("")
    code = mwi_transport_source().split("int main(int argc,char**argv)")[0]
    adapter = (SOURCE / "sip_registration_transport.cpp").read_text()
    for marker in ["bool SipTransport::queue_managed_account_(", "bool SipTransport::managed_account_needs_main_loop_(",
                   "void SipTransport::notify_managed_account_main_loop_(", "void SipTransport::pump_managed_account_(",
                   "void SipTransport::registration_notify_("]:
        code += "\n" + block(adapter, marker)
    account = (ROOT / "tests/fixtures/registration_transport.cpp").read_text()
    for marker in ["void start_account_call(", "std::string pick(", "void confirm_account_call("]:
        code += "\n" + block(account, marker)
    code = code.replace('"office"', '"ha"')
    assert "uint32_t esp_random(){return 42;}" in code
    code = code.replace("uint32_t esp_random(){return 42;}", "uint32_t esp_random(){static uint32_t value=42;return ++value;}")
    code = '#include <thread>\n#define ESP_LOGE(...) host_log(__VA_ARGS__)\n' + code
    code += r'''
const auto application_thread=std::this_thread::get_id();
struct Saved {unsigned commits=0;SipAccountConfig config;};
bool commit_nvs_boundary(void*context,const std::string&id,const SipAccountConfig*config){
 assert(std::this_thread::get_id()==application_thread&&id=="ha"&&config);
 auto&saved=*static_cast<Saved*>(context);++saved.commits;saved.config=*config;return true;
}
struct Wake {std::atomic<bool>pending{false};unsigned count=0;};
void sip_step(SipTransport&phone,Wake&wake){
 for(unsigned pass=0;pass<3;++pass){
  std::thread worker([&]{phone.registration_.tick(clock_ms);phone.pump_registration_mwi_(clock_ms);phone.notify_managed_account_main_loop_();});worker.join();
  if(!wake.pending.exchange(false))break;
  phone.wake_registration_from_main_loop();
 }
}
void feed(SipTransport&phone,Peer&peer,const std::string&record){std::thread worker([&]{peer.send_to(phone,record);});worker.join();}
int main(int argc,char**argv){
 assert(argc==3);const bool tcp=std::string(argv[1])=="tcp";const std::string mode=argv[2];
 SipTransport phone;Peer old_peer(tcp),new_peer(tcp);bind_account(phone,old_peer);
 phone.managed_update_.reset(new SipTransport::ManagedAccountUpdate);
 SipMwi::Hooks hooks;hooks.context=&phone;hooks.send=SipTransport::registration_send_view_;hooks.reply=SipTransport::registration_mwi_reply_;
 hooks.local_ip=SipTransport::registration_local_ip_;hooks.random=esp_random;hooks.hash=SipTransport::registration_hash_;hooks.local_port=phone.sip_port_;
 phone.mwi_.set_hooks(hooks);phone.pump_registration_mwi_(clock_ms);const auto subscribe=one(old_peer,"SUBSCRIBE ");
 feed(phone,old_peer,notification(subscribe,1));one(old_peer,"SIP/2.0 200");feed(phone,old_peer,response(subscribe,200));
 assert(phone.mwi_.state(0).available&&phone.mwi_.state(0).new_messages==2);
 confirm_account_call(phone,old_peer);assert(phone.active_account_==0&&!phone.call_id_.empty());
 const auto revision=phone.registration_.account(0).config_revision;
 const int original_fd=tcp?phone.registration_flows_[0].socket.load():phone.sip_socket_;
 Saved saved;Wake wake;phone.registration_context_=&wake;
 phone.registration_wake_callback_=[](void*ctx,SipTransport*){assert(std::this_thread::get_id()!=application_thread);auto&w=*static_cast<Wake*>(ctx);++w.count;w.pending=true;};
 auto replacement=phone.registration_.account(0).config;replacement.username="new-managed-user";replacement.password="new-fixture-password";replacement.port=ntohs(new_peer.address.sin_port);
 assert(phone.set_account(replacement,commit_nvs_boundary,&saved));sip_step(phone,wake);
 assert(old_peer.drain().empty()&&new_peer.drain().empty()&&saved.commits==0);
 assert(phone.registration_.account(0).config_revision==revision&&phone.registration_.account(0).desired);
 assert(phone.mwi_.state(0).available&&!phone.registration_subscriptions_retired_(0));
 // End the actual call and its BYE transaction, without changing the guard.
 assert(phone.send_bye_unlocked_(phone.call_id_));const auto bye=one(old_peer,"BYE ");
 feed(phone,old_peer,response(bye,200));assert(phone.call_id_.empty()&&phone.active_account_==-1);
 sip_step(phone,wake);const auto retiring=old_peer.drain();assert(retiring.size()==2);
 const auto unregister=pick(retiring,"REGISTER"),unsubscribe=pick(retiring,"SUBSCRIBE");
 assert(header_value(unregister,"Expires")=="0"&&header_value(unsubscribe,"Expires")=="0");
 assert(header_value(unsubscribe,"Call-ID")==header_value(subscribe,"Call-ID"));
 assert(saved.commits==0&&phone.registration_.account(0).config_revision==revision);
 feed(phone,old_peer,response(unregister,200));feed(phone,old_peer,response(unsubscribe,200));sip_step(phone,wake);
 assert(!phone.registration_.account(0).bound&&!phone.registration_subscriptions_retired_(0));
 const auto wake_count=wake.count;for(unsigned pass=0;pass<3;++pass)sip_step(phone,wake);
 assert(wake.count==wake_count&&saved.commits==0&&phone.managed_update_->active);
 assert(phone.registration_.account(0).config.username=="100"&&phone.registration_.account(0).config_revision==revision);
 assert((tcp?phone.registration_flows_[0].socket.load():phone.sip_socket_)==original_fd);
 if(mode=="terminal"){
  feed(phone,old_peer,notification(subscribe,2,"terminated;reason=timeout"));one(old_peer,"SIP/2.0 200");
 }else if(mode=="timer-n")clock_ms+=32001;
 else{assert(tcp&&mode=="flow-loss");std::thread worker([&]{phone.handle_tcp_peer_loss_(&phone.registration_flows_[0]);});worker.join();}
 sip_step(phone,wake);assert(phone.registration_subscriptions_retired_(0));
 assert(saved.commits==1&&saved.config.username=="new-managed-user"&&!phone.managed_update_->active);
 assert(phone.registration_.account(0).config_revision!=revision&&phone.registration_.account(0).config.password=="new-fixture-password");
 // The host controls only connection completion. Production queued bytes,
 // retirement and TCP record send remain the actual methods under test.
 if(tcp){auto&flow=phone.registration_flows_[0];assert(!flow.pending.empty());
  phone.close_tcp_flow_(flow);flow.socket=new_peer.esp;flow.close_requested=false;flow.connect_requested=false;
  assert(phone.send_sip_tcp_record_(flow.pending.data(),flow.pending.size(),new_peer.esp));flow.pending.clear();}
 const auto new_register=one(new_peer,"REGISTER ");
 assert(header_value(new_register,"From").find("new-managed-user@")!=std::string::npos);
 assert(header_value(new_register,"Call-ID")!=header_value(unregister,"Call-ID"));
 const auto pending=phone.registration_.account(0).request;
 // An old final response cannot replace the new pending transaction.
 sockaddr_in old_address=old_peer.address;
 assert(phone.registration_ingress_(response(unregister,200),old_address,tcp?new_peer.esp:-1));
 assert(phone.registration_.account(0).request==pending&&saved.commits==1);
 for(auto&flow:phone.registration_flows_)phone.close_tcp_flow_(flow);close(phone.sip_socket_);
}
'''
    cpp = folder / "managed-mwi.cpp"
    cpp.write_text(code)
    binary = folder / "managed-mwi"
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation", "-g",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-pthread", "-DUSE_ESPHOME_VOIP_REGISTRATION", "-DUSE_ESPHOME_VOIP_MWI", "-DUSE_VOIP_HA_AUTO_REGISTER",
        "-I", str(folder), "-I", str(SOURCE), "-I", str(SOURCE.parents[2]), str(cpp), str(SOURCE / "sip_mwi.cpp"),
        str(SOURCE / "sip_registration.cpp"), "-lcrypto", "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("transport,mode", [(p,m) for p in ["udp","tcp"] for m in ["terminal","timer-n"]]+[("tcp","flow-loss")])
def test_managed_replacement_waits_for_real_call_and_mwi_retirement(managed_mwi_binary, transport, mode):
    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    result = subprocess.run([managed_mwi_binary, transport, mode], capture_output=True, text=True, timeout=10, preexec_fn=no_core)
    assert result.returncode == 0, result.stderr
