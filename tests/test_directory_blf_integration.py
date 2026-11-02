"""Actual provider commit, Phonebook selection and subscription owner compose."""
import subprocess
import pytest
from test_refer_dialog_runtime import SOURCE, block
from test_blf_pickup_wire import blf_pickup_source


@pytest.fixture(scope="module", params=[False, True], ids=["internal-model", "psram-string-model"])
def directory_blf_binary(tmp_path_factory, request):
    folder = tmp_path_factory.mktemp("directory-blf")
    core = folder / "esphome/core"
    core.mkdir(parents=True)
    (core / "defines.h").write_text("")
    (core / "helpers.h").write_text('''#pragma once
#include <cstdlib>
namespace esphome{template<class T>struct RAMAllocator{using value_type=T;enum{ALLOC_EXTERNAL=1};int flags_;explicit RAMAllocator(int f=0):flags_(f){}template<class U>RAMAllocator(const RAMAllocator<U>&o):flags_(o.flags_){}T*allocate(size_t n){return static_cast<T*>(std::malloc(n*sizeof(T)));}void deallocate(T*p,size_t){std::free(p);}template<class U>bool operator==(const RAMAllocator<U>&o)const{return flags_==o.flags_;}template<class U>bool operator!=(const RAMAllocator<U>&o)const{return flags_!=o.flags_;}};}
''')
    code = '#include "sip_account_view.h"\nusing esphome::voip_stack::SipAccountSnapshot;\n' + blf_pickup_source().split("struct VoipStack {")[0]
    if request.param:
        code = code.replace('struct SipSignalingString:std::string {using std::string::string;using std::string::operator=;std::string str()const{return *this;}};',
                            'using esphome::voip_stack::SipSignalingString;')
    adapter = (SOURCE / "sip_registration_transport.cpp").read_text()
    code += block(adapter, "bool SipTransport::get_account_snapshot(") + r'''
struct VoipStack {
 Phonebook phonebook_;std::unique_ptr<SipTransport>transport_=std::make_unique<SipTransport>();
 bool blf_ready_=true,blf_watch_contacts_=true;uint8_t blf_capacity_=3;
 std::string blf_account_id_="fallback",default_line_="office",device_route_id_="self";unsigned publications=0;
 struct DirectoryProvider{const void*owner=nullptr;std::string account_id;};std::array<DirectoryProvider,4>directory_providers_{};
 bool get_account_snapshot(const std::string&,SipAccountSnapshot*);
 std::string get_contact_account(std::string_view)const;bool set_blf_watches(const std::vector<BlfWatchConfig>&);void sync_blf_contacts_();
 DirectoryCommitResult commit_contact_directory(const void*,ContactEntry*,size_t,const std::string&);
 void publish_phonebook_changed_(){++publications;sync_blf_contacts_();}
};
'''
    settings = (SOURCE / "voip_settings.cpp").read_text()
    code += block((SOURCE / "voip_registration.cpp").read_text(), "bool VoipStack::get_account_snapshot(") + "\n"
    code += block(settings, "std::string VoipStack::get_contact_account(") + "\n"
    code += block(settings, "DirectoryCommitResult VoipStack::commit_contact_directory(const void *provider, ContactEntry *entries") + "\n"
    for marker in ["bool VoipStack::set_blf_watches(", "void VoipStack::sync_blf_contacts_("]:
        code += block((SOURCE / "voip_blf.cpp").read_text(), marker) + "\n"
    pickup = blf_pickup_source()
    for marker in ["std::string dialog_document(", "std::string notify_dialog("]:
        code += block(pickup, marker) + "\n"
    code += r'''
ContactEntry row(const char*id,const char*name,const char*route,const char*account=""){
 ContactEntry e;e.id=id;e.name=name;e.route=route;e.account_id=account;e.kind=ContactKind::CONTACT;e.auto_blf=true;return e;
}
void add_line(SipTransport&s,Peer&p,const char*id,const char*user){
 auto config=s.registration_.account(0).config;config.id=id;config.username=user;assert(s.registration_.set_account(config,clock_ms));s.registration_.tick(clock_ms);
 const auto reg=one(p,"REGISTER ");auto answer=response(reg,200);answer.insert(answer.find("Content-Length:"),"Contact: "+header_value(reg,"Contact")+"\r\n");
 p.send_to(s,answer);assert(s.registration_.is_registered(id));
}
int main(){
 VoipStack app;auto&phone=*app.transport_;Peer peer(false);bind_account(phone,peer);add_line(phone,peer,"explicit","200");add_line(phone,peer,"fallback","300");
 assert(app.phonebook_.prepare_directory_storage());assert(phone.configure_blf(3,nullptr));
 SipMwi::Hooks hooks;hooks.context=&phone;hooks.send=SipTransport::registration_send_view_;hooks.reply=SipTransport::registration_mwi_reply_;
 hooks.local_ip=SipTransport::registration_local_ip_;hooks.random=esp_random;hooks.hash=SipTransport::registration_hash_;hooks.local_port=phone.sip_port_;phone.mwi_.set_hooks(hooks);
 auto manual=row("manual","Manual fallback","103");assert(app.phonebook_.add_entry(manual)==AddResult::Added);
 int provider;ContactEntry initial[]={row("ldap-explicit","Directory explicit phone","101","explicit"),row("ldap-provider","Directory provider phone","102")};
 assert(app.commit_contact_directory(&provider,initial,2,"office")==DirectoryCommitResult::APPLIED);
 assert(app.get_contact_account("ldap-explicit")=="explicit"&&app.get_contact_account("ldap-provider")=="office"&&app.get_contact_account("manual")=="fallback");
 assert(app.phonebook_.select("ldap-provider"));phone.pump_registration_mwi_(clock_ms);auto initial_wire=peer.drain();assert(initial_wire.size()==6);
 std::string old_provider,explicit_sub;unsigned dialogs=0,mailboxes=0;
 for(const auto&sub:initial_wire){assert(sub.rfind("SUBSCRIBE ",0)==0);const auto event=header_value(sub,"Event");
  peer.send_to(phone,response(sub,200));
  if(event=="dialog"){
   ++dialogs;const auto to=header_value(sub,"To"),from=header_value(sub,"From");
   if(to.find("sip:101@")!=std::string::npos){assert(from.find("sip:200@")!=std::string::npos);explicit_sub=sub;}
   else if(to.find("sip:102@")!=std::string::npos){assert(from.find("sip:100@")!=std::string::npos);old_provider=sub;}
   else{assert(to.find("sip:103@")!=std::string::npos&&from.find("sip:300@")!=std::string::npos);}
   peer.send_to(phone,notify_dialog(sub,1,dialog_document("")));one(peer,"SIP/2.0 200");
  }else{assert(event=="message-summary");++mailboxes;peer.send_to(phone,notification(sub,1));one(peer,"SIP/2.0 200");}
 }
 assert(dialogs==3&&mailboxes==3&&!old_provider.empty()&&!explicit_sub.empty());
 ContactEntry renamed[]={row("ldap-provider","Provider renamed label","102"),row("ldap-explicit","Explicit renamed label","101","explicit")};
 assert(app.commit_contact_directory(&provider,renamed,2,"office")==DirectoryCommitResult::APPLIED);
 assert(app.phonebook_.current()->stable_id()=="ldap-provider"&&app.phonebook_.current_name()=="Provider renamed label");
 phone.pump_registration_mwi_(clock_ms);assert(peer.drain().empty());
 ContactEntry duplicate[]={row("duplicate","Bad","201"),row("duplicate","Other","202")};const auto publications=app.publications;
 assert(app.commit_contact_directory(&provider,duplicate,2,"office")==DirectoryCommitResult::INVALID&&app.publications==publications);
 assert(app.phonebook_.current()->stable_id()=="ldap-provider");
 ContactEntry changed[]={row("ldap-provider","Provider renamed label","202"),row("ldap-explicit","Explicit renamed label","101","explicit")};
 assert(app.commit_contact_directory(&provider,changed,2,"office")==DirectoryCommitResult::APPLIED);phone.pump_registration_mwi_(clock_ms);
 const auto stop=one(peer,"SUBSCRIBE ");assert(header_value(stop,"Expires")=="0"&&header_value(stop,"Call-ID")==header_value(old_provider,"Call-ID"));
 assert(app.phonebook_.current()->stable_id()=="ldap-provider"&&app.phonebook_.current_route()=="202");
 peer.send_to(phone,response(stop,200));phone.pump_registration_mwi_(clock_ms);assert(peer.drain().empty());
 peer.send_to(phone,notify_dialog(stop,2,"","terminated;reason=deactivated"));one(peer,"SIP/2.0 200");phone.pump_registration_mwi_(clock_ms);
 const auto replacement=one(peer,"SUBSCRIBE ");assert(header_value(replacement,"To").find("sip:202@")!=std::string::npos&&header_value(replacement,"From").find("sip:100@")!=std::string::npos&&header_value(replacement,"Call-ID")!=header_value(old_provider,"Call-ID"));
 peer.send_to(phone,response(replacement,200));peer.send_to(phone,notify_dialog(replacement,1,dialog_document("")));one(peer,"SIP/2.0 200");
 peer.send_to(phone,notify_dialog(old_provider,9,dialog_document("<dialog id='stale'><state>confirmed</state></dialog>",99)));one(peer,"SIP/2.0 481");
 BlfStateInfo info;assert(phone.get_blf_info("ldap-provider",&info)&&info.available&&info.state==BlfState::IDLE&&info.account_id=="office");
 assert(app.commit_contact_directory(&provider,nullptr,0,"office")==DirectoryCommitResult::APPLIED);phone.pump_registration_mwi_(clock_ms);auto removed=peer.drain();assert(removed.size()==2);
 for(const auto&sub:removed){assert(header_value(sub,"Expires")=="0"&&header_value(sub,"Event")=="dialog");peer.send_to(phone,response(sub,200));peer.send_to(phone,notify_dialog(sub,4,"","terminated;reason=deactivated"));one(peer,"SIP/2.0 200");}
 assert(app.phonebook_.size()==1&&app.phonebook_.current()->stable_id()=="manual"&&app.get_contact_account("manual")=="fallback");
 assert(phone.get_blf_info("manual",&info)&&info.available&&info.account_id=="fallback"&&phone.registration_.is_registered("office"));
 for(auto&flow:phone.registration_flows_)phone.close_tcp_flow_(flow);close(phone.sip_socket_);
}
'''
    cpp = folder / "directory.cpp"
    cpp.write_text(code)
    binary = folder / "directory"
    built = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation", "-g", "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
        "-DUSE_ESPHOME_VOIP_REGISTRATION", "-DVOIP_REGISTRATION_CAPACITY=4", "-DUSE_ESPHOME_VOIP_MWI", "-DUSE_ESPHOME_VOIP_BLF", "-DUSE_ESPHOME_VOIP_DIRECTORY", "-DUSE_ESPHOME_VOIP_CONTACT_METADATA",
        *(["-DUSE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM"] if request.param else []), "-I", str(folder), "-I", str(SOURCE), "-I", str(SOURCE.parents[2]),
        str(cpp), str(SOURCE / "sip_mwi.cpp"), str(SOURCE / "sip_blf.cpp"), str(SOURCE / "sip_registration.cpp"), "-lcrypto", "-o", str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    return binary


def test_provider_commit_selection_account_precedence_and_watch_retirement(directory_blf_binary):
    result = subprocess.run([directory_blf_binary], capture_output=True, text=True, timeout=8)
    assert result.returncode == 0, result.stdout + result.stderr
