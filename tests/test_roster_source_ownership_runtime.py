"""One canonical roster preserves local dial plans without enabling LDAP."""

from pathlib import Path
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module", params=[(False, False), (False, True), (True, False), (True, True)],
                ids=["base", "base-blf", "directory", "directory-blf"])
def roster_binary(tmp_path_factory, request):
    folder = tmp_path_factory.mktemp("roster-source")
    source = folder / "roster.cpp"
    source.write_text(r'''
#include "esphome/components/voip_stack/phonebook.h"
#include <cassert>
#include <cstdlib>
#include <iostream>
using namespace esphome::voip_stack;
bool measuring=false;size_t allocations=0;
void *operator new(size_t n){if(measuring)++allocations;auto*p=std::malloc(n);if(!p)throw std::bad_alloc();return p;}
void operator delete(void*p)noexcept{std::free(p);}
void operator delete(void*p,size_t)noexcept{std::free(p);}
ContactEntry remote(const std::string &route,const std::string &name="Remote"){
 ContactEntry e;e.name=name;e.route=route;e.ip="192.0.2.1";
 e.port=5060;e.rtp_port=40000;e.endpoint_type=ContactEndpointType::SIP;
#ifdef USE_ESPHOME_VOIP_DIRECTORY
 e.id=route;
#endif
 return e;
}
void add_local(Phonebook &book){assert(book.add_one("P4Direct|192.168.1.191|5060|40000|sip_tcp")==AddResult::Added);}
void local_intact(const Phonebook &book){
 auto *entry=book.find("P4Direct");assert(entry&&entry->origin==ContactOrigin::MANUAL);
 assert(entry->ip=="192.168.1.191"&&entry->sip_transport_tcp&&entry->port==5060&&entry->missing_count==0);
}
struct LegacyLayout {
 ContactText name,route,ip;ContactEndpointType endpoint_type;uint16_t port,rtp_port;
 bool sip_transport_tcp,directional_audio_v1;ContactText number;uint8_t missing_count;ContactKind kind;
#ifdef USE_ESPHOME_VOIP_BLF
 bool auto_blf;
#endif
#ifdef USE_ESPHOME_VOIP_DIRECTORY
 ContactText id;ContactOrigin origin;const void *provider;
#endif
};
static_assert(sizeof(ContactEntry)<=sizeof(LegacyLayout));
int main(int argc,char **argv){
 assert(argc==2);std::string scenario=argv[1];Phonebook book;int provider;
 const auto apply=[&](std::vector<ContactEntry> rows){return book.replace_provider(ContactOrigin::HA,&provider,std::move(rows));};
 if(scenario=="snapshot"){
  add_local(book);assert(apply({remote("r1"),remote("r2")})==DirectoryCommitResult::APPLIED);local_intact(book);
  assert(book.size()==3&&book.select("r2"));
  assert(apply({remote("r1"),remote("r2")})==DirectoryCommitResult::UNCHANGED);assert(book.current_route()=="r2");
  assert(apply({remote("r2","Renamed")})==DirectoryCommitResult::APPLIED);
  assert(!book.find("r1")&&book.current_name()=="Renamed"&&book.current_route()=="r2");local_intact(book);
  assert(apply({})==DirectoryCommitResult::APPLIED);local_intact(book);assert(book.size()==1&&book.current_route()=="P4Direct");
  assert(apply({})==DirectoryCommitResult::UNCHANGED);
 }else if(scenario=="local_priority"){
  add_local(book);
  assert(apply({remote("different-route","P4Direct"),remote("P4Direct","Remote name")})==DirectoryCommitResult::UNCHANGED);
  assert(book.size()==1);local_intact(book);
  assert(!book.add_batch("P4Direct|192.0.2.99|5060|40000|sip_udp",ContactOrigin::HA));local_intact(book);
 }else if(scenario=="manual_after_roster"){
  assert(apply({remote("old-route","P4Direct")})==DirectoryCommitResult::APPLIED);
  assert(book.add_one("P4Direct|192.168.1.191|5060|40000|sip_tcp")==AddResult::EndpointReplaced);
  assert(book.size()==1&&!book.find("old-route"));local_intact(book);
  assert(apply({})==DirectoryCommitResult::UNCHANGED);local_intact(book);
 }else if(scenario=="csv_cycle"){
  add_local(book);assert(book.add_batch("Csv|192.0.2.1|5060|40000|sip_udp",ContactOrigin::HA));
  assert(!book.commit_cycle({"Csv"},1));local_intact(book);
  assert(book.commit_cycle({},1));assert(!book.find("Csv"));local_intact(book);
 }else if(scenario=="prune_duplicate_names"){
  assert(apply({remote("ha-one","P4Direct"),remote("ha-two","P4Direct")})==DirectoryCommitResult::APPLIED);
  assert(book.add_one("P4Direct|192.168.1.191|5060|40000|sip_tcp")==AddResult::EndpointReplaced);
  assert(book.commit_cycle({},1));local_intact(book);
  assert(book.size()==1&&book.current_route()=="P4Direct");
 }else if(scenario=="prune_cursor"){
  assert(apply({remote("ha-one","One"),remote("ha-two","Two")})==DirectoryCommitResult::APPLIED);
  add_local(book);assert(book.select("P4Direct"));
  assert(book.commit_cycle({},1));local_intact(book);assert(book.current_route()=="P4Direct"&&book.size()==1);
  assert(apply({remote("ha-one","One"),remote("ha-two","Two")})==DirectoryCommitResult::APPLIED);
  assert(book.select("ha-one"));assert(book.commit_cycle({"Two"},1));
  assert(book.current_route()=="ha-two");local_intact(book);
 }else if(scenario=="capacity_atomic"){
  add_local(book);std::vector<ContactEntry> rows;
  for(unsigned i=0;i<63;++i)rows.push_back(remote("r"+std::to_string(i),"N"+std::to_string(i)));
  assert(apply(rows)==DirectoryCommitResult::APPLIED);assert(book.size()==64&&book.select("r62"));
  rows.push_back(remote("overflow"));assert(apply(rows)==DirectoryCommitResult::CAPACITY);
  assert(book.size()==64&&!book.find("overflow")&&book.current_route()=="r62");local_intact(book);
 }else if(scenario=="invalid_atomic"){
  add_local(book);assert(apply({remote("old")})==DirectoryCommitResult::APPLIED);assert(book.select("old"));
  auto invalid=remote("invalid");invalid.name=std::string(65,'x');
  assert(apply({remote("new"),invalid})==DirectoryCommitResult::INVALID);
  assert(apply({remote("duplicate"),remote("duplicate")})==DirectoryCommitResult::INVALID);
  assert(book.size()==2&&book.find("old")&&!book.find("new")&&book.current_route()=="old");local_intact(book);
 }else if(scenario=="provider_conflict"){
#ifdef USE_ESPHOME_VOIP_DIRECTORY
  int ldap;assert(book.replace_directory(&ldap,{remote("owned")})==DirectoryCommitResult::APPLIED);
  assert(apply({remote("owned")})==DirectoryCommitResult::CONFLICT);
  assert(book.size()==1&&book.current()->origin==ContactOrigin::LDAP&&book.current()->provider==&ldap);
#else
  assert(book.replace_provider(ContactOrigin::LDAP,&provider,{remote("owned")})==DirectoryCommitResult::INVALID);
  assert(book.empty());
#endif
 }else if(scenario=="csv_snapshot_shared_owner"){
  assert(book.add_batch("Csv|same-route|192.0.2.9|5060|40000|sip_udp",ContactOrigin::HA,&provider));
  assert(book.add_batch("Changed|same-route|192.0.2.10|5060|40000|sip_tcp",ContactOrigin::HA,&provider));
  assert(book.find("same-route")->ip=="192.0.2.10");
  assert(apply({remote("same-route","Snapshot")})==DirectoryCommitResult::APPLIED);
  assert(book.size()==1&&book.current_name()=="Snapshot");
  assert(apply({})==DirectoryCommitResult::APPLIED&&book.empty());
 }else if(scenario=="csv_cannot_acquire_directory"){
#ifdef USE_ESPHOME_VOIP_DIRECTORY
  int ldap;assert(book.replace_directory(&ldap,{remote("owned")})==DirectoryCommitResult::APPLIED);
  assert(!book.add_batch("Remote|owned|192.0.2.9|5060|40000|sip_udp",ContactOrigin::HA));
  assert(book.current()->origin==ContactOrigin::LDAP&&book.current()->provider==&ldap&&book.current()->ip=="192.0.2.1");
#else
  assert(book.add_batch("Csv|192.0.2.9|5060|40000|sip_udp",ContactOrigin::HA));
  assert(book.current()->origin==ContactOrigin::HA);
#endif
 }else if(scenario=="warm_no_allocation"){
  add_local(book);assert(apply({remote("one"),remote("two")})==DirectoryCommitResult::APPLIED);
  ContactEntry rows[]={remote("one","Changed"),remote("two")};allocations=0;measuring=true;
  const auto result=book.replace_provider(ContactOrigin::HA,&provider,rows,2);
  measuring=false;assert(result==DirectoryCommitResult::APPLIED&&allocations==0);local_intact(book);
 }else if(scenario=="explicit_remove_flush"){
  add_local(book);assert(apply({remote("one")})==DirectoryCommitResult::APPLIED);
  assert(book.remove_one("P4Direct"));assert(book.size()==1);book.clear();assert(book.empty());
 }else{assert(false);}
 std::cout<<"entry="<<sizeof(ContactEntry)<<" legacy="<<sizeof(LegacyLayout)<<std::endl;
}
''')
    binary = folder / "roster"
    command = ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
               "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_ESP32"]
    directory, blf = request.param
    if directory:
        command.append("-DUSE_ESPHOME_VOIP_DIRECTORY")
    if blf:
        command.append("-DUSE_ESPHOME_VOIP_BLF")
    command += ["-I", str(ROOT), str(source), "-o", str(binary)]
    built = subprocess.run(command, capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize("scenario", [
    "snapshot", "local_priority", "manual_after_roster", "csv_cycle", "prune_duplicate_names", "prune_cursor", "capacity_atomic",
    "invalid_atomic", "provider_conflict", "csv_snapshot_shared_owner", "csv_cannot_acquire_directory",
    "warm_no_allocation", "explicit_remove_flush",
])
def test_real_phonebook_source_ownership(roster_binary, scenario):
    result = subprocess.run([str(roster_binary), scenario], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.mark.parametrize("legacy", [False, True])
def test_noreg_static_dialplan_codegen_keeps_optional_services_out(tmp_path, legacy):
    from test_native_ha_config import validate

    result = validate(tmp_path, "\napi:\n  custom_services: true\n", generate=True, voip_settings=f"""
  static_contacts:
    - name: P4Direct
      ip: 192.0.2.191
      port: 5060
      rtp_port: 40000
      transport: tcp
  ha_integration:
    auto_register: false
    endpoint: true
    legacy_roster: {str(legacy).lower()}
""")
    assert result.returncode == 0, result.stdout + result.stderr
    generated = next(tmp_path.rglob("main.cpp")).read_text()
    defines = next(tmp_path.rglob("defines.h")).read_text()
    assert 'phone->add_contact("P4Direct|192.0.2.191|5060|40000|sip_tcp");' in generated
    assert ("#define USE_VOIP_HA_LEGACY_ROSTER" in defines) is legacy
    for feature in ("DIRECTORY", "REGISTRATION", "MWI", "BLF"):
        assert f"#define USE_ESPHOME_VOIP_{feature}" not in defines


@pytest.mark.parametrize("directory", [False, True])
def test_rejected_json_keeps_last_good_source_through_update_cycle(tmp_path, directory):
    settings = (ROOT / "esphome/components/voip_stack/voip_settings.cpp").read_text()
    method = settings[settings.index("void VoipStack::set_contacts("):
                      settings.index("\nstd::string VoipStack::normalize_phonebook_for_transport_(")]
    source = tmp_path / "rejected-json.cpp"
    source.write_text(r'''
#include "esphome/components/voip_stack/phonebook.h"
#include <cassert>
using namespace esphome::voip_stack;
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
struct VoipStack {
 Phonebook phonebook_;std::string device_name_="Self";bool cycle_active_=true;unsigned json_calls=0;
 std::unordered_set<std::string> seen_in_cycle_;
 void set_contacts(const std::string&);
 std::string normalize_phonebook_for_transport_(const std::string&s){return s;}
 bool apply_roster_json_contacts_(const std::string&){++json_calls;return false;}
 bool maybe_auto_select_ha_first_(){return false;}
 void publish_phonebook_changed_(){}
 void track_csv_(const std::string&){}
};
''' + method + r'''
int main(){
 VoipStack phone;assert(phone.phonebook_.add_one("Local|192.0.2.1|5060|40000|sip_tcp")==AddResult::Added);
 assert(phone.phonebook_.add_one("Old HA|192.0.2.2|5060|40000|sip_udp",ContactOrigin::HA,&phone)==AddResult::Added);
 phone.set_contacts("{rejected JSON}");assert(phone.json_calls==1);
 assert(!phone.phonebook_.commit_cycle(phone.seen_in_cycle_,1));
 assert(phone.phonebook_.size()==2&&phone.phonebook_.find("Old HA")&&phone.phonebook_.find("Local"));
}
''')
    binary = tmp_path / "rejected-json"
    command = ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g", "-fsanitize=address,undefined",
               "-fno-pie", "-no-pie", "-DUSE_ESP32", "-DUSE_VOIP_HA_LEGACY_ROSTER"]
    if directory:
        command.append("-DUSE_ESPHOME_VOIP_DIRECTORY")
    built = subprocess.run([*command, "-I", str(ROOT), str(source), "-o", str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
