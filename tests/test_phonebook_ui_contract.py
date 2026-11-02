"""Shipped phone_lists lambdas consume real Phonebook with optional features off."""
from pathlib import Path
import json
import os
import subprocess
import sys

import pytest
from esphome import yaml_util
from esphome.components.substitutions import do_substitution_pass

from test_refer_dialog_runtime import block

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / "esphome/components/voip_stack"


@pytest.mark.parametrize("mode", ["off", "metadata", "directory", "directory-psram"])
@pytest.mark.parametrize("registration", [False, True], ids=["direct", "default-line"])
def test_shipped_rows_manual_route_selection_and_single_call_owner(tmp_path, mode, registration):
    source_path = Path(os.environ.get("VOIP_PHONE_LISTS_SOURCE", ROOT / "tests/fixtures/phone_lists.yaml"))
    document = yaml_util.load_yaml(source_path)
    view = do_substitution_pass({"substitutions": {
        **document["defaults"], "firmware_features": ["voip", "display"],
    }, "view": document["phone_list_view"][0]}, {})["view"]
    header = (CORE / "voip_stack.h").read_text()
    declarations = header[header.index("  size_t get_contact_count() const"):header.index("  std::string get_contacts_csv() const;")]
    settings = (CORE / "voip_settings.cpp").read_text()
    code = r'''
#include "esphome/components/voip_stack/phonebook.h"
#include "phone_list_rows.h"
#include <cassert>
#include <iostream>
#include <type_traits>
template<class T,class=void>struct has_id:std::false_type{};
template<class T>struct has_id<T,std::void_t<decltype(T{}.id)>>:std::true_type{};
template<class T,class=void>struct has_account:std::false_type{};
template<class T>struct has_account<T,std::void_t<decltype(T{}.account_id)>>:std::true_type{};
template<class T,class=void>struct has_provider:std::false_type{};
template<class T>struct has_provider<T,std::void_t<decltype(T{}.provider)>>:std::true_type{};
namespace esphome::voip_stack {
class VoipStack {
 public:
 Phonebook phonebook_;bool idle=true;unsigned starts=0,publishes=0;
 std::string pending_dialplan_target_,observed_route,observed_line,default_line_="ha",pending_line_;
 struct Provider{const void*owner=nullptr;std::string account_id;};std::array<Provider,4>directory_providers_{};
 bool is_idle()const{return idle;}bool can_start_call_()const{return idle;}
 void publish_destination_(){++publishes;}
 // Observe the existing call-owner boundary, never simulate SIP success.
 void start(){++starts;idle=false;observed_route=phonebook_.current_route();
#ifdef USE_ESPHOME_VOIP_REGISTRATION
 observed_line=pending_line_.empty()?default_line_:pending_line_;
#else
 observed_line="direct";
#endif
 }
DECLARATIONS
};
METHODS
}
using namespace esphome;using phone_list_view::RowData;using phone_list_view::RowKey;
voip_stack::VoipStack phone;
#define id(x) x
uint32_t count(){COUNT}
RowKey key(uint32_t index){KEY}
uint8_t group(uint32_t index){GROUP}
void render(uint32_t index,RowData&row){RENDER}
void select(const std::string&row_id,[[maybe_unused]]uint8_t action){SELECT}
int main(){
 using namespace voip_stack;
#ifdef USE_ESPHOME_VOIP_CONTACT_METADATA
 static_assert(has_id<ContactEntry>::value&&has_account<ContactEntry>::value);
#else
 static_assert(!has_id<ContactEntry>::value&&!has_account<ContactEntry>::value);
#endif
#ifdef USE_ESPHOME_VOIP_DIRECTORY
 static_assert(has_provider<ContactEntry>::value);
#else
 static_assert(!has_provider<ContactEntry>::value);
#endif
 assert(phone.phonebook_.add_one("Legacy|192.0.2.1|5060|40000|full_duplex|48000|10|sip_udp")==AddResult::Added);
 assert(phone.phonebook_.at(0)->number.empty());
 RowData legacy;render(0,legacy);assert(std::string(legacy.detail.data()).empty());
 ContactEntry parsed;
 assert(!Phonebook::parse_entry("Legacy|192.0.2.1|5060|40000|bad;hint|48000|10|sip_udp",&parsed));
 const std::string legacy_prefix="Legacy|192.0.2.1|5060|40000|",legacy_suffix="|48000|10|sip_udp";
 assert(Phonebook::parse_entry(legacy_prefix+std::string(160,'x')+legacy_suffix,&parsed)&&parsed.number.empty());
 assert(!Phonebook::parse_entry(legacy_prefix+std::string(161,'x')+legacy_suffix,&parsed));
 ContactEntry manual;manual.name="Manual display";manual.route="qa-manual-7";
 manual.ip="192.0.2.7";manual.port=5067;manual.endpoint_type=ContactEndpointType::SIP;
 manual.kind=ContactKind::CONTACT;manual.number="+39000107";
#ifdef USE_ESPHOME_VOIP_CONTACT_METADATA
 manual.id="stable-manual-id";manual.account_id="office";manual.auto_blf=false;
#endif
 assert(phone.phonebook_.add_entry(manual)==AddResult::Added);
 assert(count()==2&&phone.get_contact(2)==nullptr&&group(1)==3);
 const std::string stable(key(1).id);assert(key(1).name=="Manual display");
 RowData row;render(1,row);assert(std::string(row.id.data())==stable&&std::string(row.name.data())==manual.name);
 assert(std::string(row.detail.data())=="+39000107");
 assert(phone.phonebook_.select(stable));
 manual.name="Renamed display";
 assert(phone.phonebook_.replace_all({manual}));
 assert(phone.phonebook_.current_name()=="Renamed display"&&key(0).id==stable);
 select(stable,0);assert(phone.starts==1&&phone.publishes==1&&phone.observed_route=="qa-manual-7");
#ifdef USE_ESPHOME_VOIP_REGISTRATION
#ifdef USE_ESPHOME_VOIP_CONTACT_METADATA
 assert(phone.observed_line=="office");
#else
 assert(phone.observed_line=="ha");
#endif
#else
 assert(phone.observed_line=="direct");
#endif
 assert(phone.pending_line_.empty());
 select(stable,0);assert(phone.starts==1); // Busy presenter never starts another call.
 phone.idle=true;phone.phonebook_.clear();select(stable,0);assert(phone.starts==1); // Stale row ID.
 assert(phone.phonebook_.add_entry(manual)==AddResult::Added);
 RowData again;render(0,again);assert(std::string(again.detail.data())=="+39000107");
 std::cout<<"sizeof_contact="<<sizeof(ContactEntry)<<" count="<<phone.get_contact_count()<<" route="<<phone.observed_route<<" line="<<phone.observed_line<<"\n";
}
'''
    code = code.replace("DECLARATIONS", declarations)
    code = code.replace("METHODS", "\n".join(block(settings, name) for name in [
        "bool VoipStack::call_contact(", "std::string VoipStack::get_contact_account(",
    ]))
    for key, name in [("COUNT", "row_count"), ("KEY", "row_key"), ("GROUP", "row_group"), ("RENDER", "render_row")]:
        code = code.replace(key, str(view[name]))
    code = code.replace("SELECT", str(view["on_select"][0]["lambda"]))
    include = tmp_path / "include"
    core = include / "esphome/core"
    core.mkdir(parents=True)
    (core / "defines.h").write_text("")
    (core / "helpers.h").write_text('''#pragma once
#include <cstdlib>
namespace esphome {template<class T>struct RAMAllocator {
using value_type=T;enum{ALLOC_EXTERNAL=1};uint8_t flags;explicit RAMAllocator(uint8_t value=0):flags(value){}
T*allocate(size_t n){return static_cast<T*>(std::malloc(n*sizeof(T)));}
void deallocate(T*p,size_t){std::free(p);}
};}
''')
    cpp = tmp_path / "ui.cpp"
    cpp.write_text(code)
    binary = tmp_path / "ui"
    flags = []
    if mode != "off":
        flags.append("-DUSE_ESPHOME_VOIP_CONTACT_METADATA")
    if mode.startswith("directory"):
        flags.append("-DUSE_ESPHOME_VOIP_DIRECTORY")
    if mode.endswith("psram"):
        flags.append("-DUSE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM")
    if registration:
        flags.append("-DUSE_ESPHOME_VOIP_REGISTRATION")
    result = subprocess.run(["g++", "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_ESP32", *flags,
        "-I", str(include), "-I", str(ROOT), "-I", str(ROOT / "tests/fixtures"), str(cpp), "-o", str(binary)],
        capture_output=True, text=True, timeout=45)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    (tmp_path / "receipt.json").write_text(json.dumps({"mode": mode, "registration": registration,
        "source": str(source_path), "stdout": result.stdout, "stderr": result.stderr, "exit": result.returncode}, indent=2) + "\n")
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.mark.parametrize("profile", ["direct", "registered", "managed-ha"])
def test_native_codegen_exposes_ui_facade_without_directory_or_subscriptions(tmp_path, profile):
    from test_sip_registration_config import ACCOUNT
    source = (ROOT / "tests/minimal_no_text.yaml").read_text()
    source = source.replace("../esphome/components", str(ROOT / "esphome/components"))
    options = "  ha_integration: false\n" + (ACCOUNT if profile == "registered" else "")
    if profile == "managed-ha":
        options = "  ha_integration:\n    auto_register: true\n" + ACCOUNT
        source += "\napi:\n  custom_services: true\n"
    source = source.replace("voip_stack:\n", "voip_stack:\n" + options + """  static_contacts:
    - entry: 'Manual display|qa-manual-7|192.0.2.7|5067|40000|sip_udp'
""")
    source = source.replace("  name: voip-minimal-no-text", """  name: voip-minimal-no-text
  on_boot:
    then:
      - lambda: |-
          const auto *contact = id(phone).get_contact(0);
          if (contact != nullptr && id(phone).is_idle())
            id(phone).call_contact(std::string(contact->stable_id()));
""")
    path = tmp_path / "ui-contract.yaml"
    path.write_text(source)
    result = subprocess.run([sys.executable, "-m", "esphome", "compile", str(path), "--only-generate"],
                            capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stdout + result.stderr
    main = next(tmp_path.rglob("main.cpp")).read_text()
    defines = next(tmp_path.rglob("defines.h")).read_text()
    assert "get_contact(0)" in main and "call_contact" in main and "stable_id()" in main
    assert "Manual display|qa-manual-7|192.0.2.7|5067|40000|sip_udp" in main
    for feature in ["USE_ESPHOME_VOIP_DIRECTORY", "USE_ESPHOME_VOIP_CONTACT_METADATA", "USE_ESPHOME_VOIP_BLF",
                    "USE_ESPHOME_VOIP_MWI", "USE_PHONE_CLIENT_TLS"]:
        assert feature not in defines
    assert ("USE_ESPHOME_VOIP_REGISTRATION" in defines) == (profile != "direct")
    assert ("USE_VOIP_HA_AUTO_REGISTER" in defines) == (profile == "managed-ha")
