"""Managed names and main-context wakeups keep stable endpoint ownership."""
import subprocess

from test_refer_dialog_runtime import SOURCE, block


def test_managed_name_and_correlated_line_keep_routing_and_callback_owner(tmp_path):
    header = (SOURCE / "voip_stack.h").read_text()
    settings = (SOURCE / "voip_settings.cpp").read_text()
    registration = (SOURCE / "voip_registration.cpp").read_text()
    methods = block(header, "const std::string &get_caller_display_name()")
    methods += "\n" + block(settings, "void VoipStack::set_ha_display_name(")
    methods += "\n" + block(settings, "void VoipStack::call_via_ha(")
    methods += "\n" + block(registration, "void VoipStack::registration_dns_ready_callback_(")
    methods += "\n" + block(registration, "void VoipStack::pump_registration_wake_()")
    methods = methods.replace("const std::string &get_caller_display_name()", "const std::string &VoipStack::get_caller_display_name()")
    source = tmp_path / "identity.cpp"
    source.write_text(r'''
#include <atomic>
#include <cassert>
#include <string>
#include <memory>
#include <thread>
struct Phonebook {static constexpr size_t MAX_NAME_BYTES=64;
 static std::string trim(const std::string&s){auto b=s.find_first_not_of(" \t\r\n");return b==std::string::npos?"":s.substr(b,s.find_last_not_of(" \t\r\n")-b+1);}};
struct SipTransport {unsigned wakes=0;void wake_registration_from_main_loop(){++wakes;}};
struct VoipStack {
 std::string device_name_="Physical name",ha_display_name_,device_route_id_="stable-route",line,target,call_id;
 std::unique_ptr<SipTransport> transport_{new SipTransport};
 std::atomic<SipTransport*> registration_dns_owner_{nullptr};unsigned loop_wakes=0;
 const std::string&get_caller_display_name()const;
 void set_ha_display_name(const std::string&);
 void call_via_ha(const std::string&,const std::string&);
 static void registration_dns_ready_callback_(void*,SipTransport*);
 void pump_registration_wake_();
 void enable_loop_soon_any_context(){++loop_wakes;}
 void start_call_on_line_(const std::string&t,const std::string&l,const std::string&id){target=t;line=l;call_id=id;}
 void call_(const std::string&t,const std::string&id){target=t;line="legacy";call_id=id;}
};
''' + methods + r'''
int main(){
 VoipStack phone;assert(phone.get_caller_display_name()=="Physical name");
 phone.set_ha_display_name("  Ingresso  ");assert(phone.get_caller_display_name()=="Ingresso");
 for(const auto&bad:{std::string(""),std::string(65,'x'),std::string("Injected\nName"),std::string("a\x7f")}){
  phone.set_ha_display_name(bad);assert(phone.get_caller_display_name()=="Ingresso");
 }
 phone.set_ha_display_name(std::string(64,'x'));assert(phone.get_caller_display_name().size()==64);
 assert(phone.device_name_=="Physical name"&&phone.device_route_id_=="stable-route");
 phone.call_via_ha("qa-target","Ha-Correlated@controller");
 assert(phone.line=="ha"&&phone.target=="qa-target"&&phone.call_id=="Ha-Correlated@controller");
 phone.call_via_ha("other","bad\nCallId");assert(phone.target=="qa-target");
 auto*old=phone.transport_.get();
 std::thread worker([&]{VoipStack::registration_dns_ready_callback_(&phone,old);});worker.join();
 assert(phone.loop_wakes==1&&old->wakes==0);phone.pump_registration_wake_();assert(old->wakes==1);
 phone.pump_registration_wake_();assert(old->wakes==1);
 // A late callback keeps only identity; a replaced transport is never invoked.
 VoipStack::registration_dns_ready_callback_(&phone,old);
 auto replacement=std::make_unique<SipTransport>();phone.transport_.swap(replacement);
 phone.pump_registration_wake_();assert(replacement->wakes==1&&phone.transport_->wakes==0);
 VoipStack::registration_dns_ready_callback_(&phone,phone.transport_.get());
 phone.pump_registration_wake_();assert(phone.transport_->wakes==1);
}
''')
    binary = tmp_path / "identity"
    compile = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                              "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                              "-DUSE_VOIP_HA_AUTO_REGISTER", str(source), "-o", str(binary)],
                             capture_output=True, text=True)
    assert compile.returncode == 0, compile.stderr
    run = subprocess.run([binary], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr
