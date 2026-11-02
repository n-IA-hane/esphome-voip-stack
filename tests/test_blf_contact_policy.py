"""Real HA7B JSON fields map to the existing Phonebook and automatic watches."""
import json
import subprocess
from test_refer_dialog_runtime import ROOT, SOURCE, block


def test_real_ha_roster_metadata_and_explicit_ldap_contract_drive_watch_policy(tmp_path):
    core = tmp_path / "esphome/core"
    core.mkdir(parents=True)
    (core / "defines.h").write_text("")
    # Python's independent JSON decoder supplies the unchanged cJSON tree
    # boundary. The field mapping, Phonebook and watch selection are actual code.
    document = json.loads((ROOT / "tests/fixtures/blf/public-roster-v2.json").read_text())
    lines = []
    def tree(value):
        name = f"node{len(lines)}"
        lines.append(f"auto {name}=std::make_unique<cJSON>();")
        if isinstance(value, dict):
            lines.append(f"{name}->type=6;")
            for key, child in value.items():
                nested = tree(child)
                lines.append(f"{name}->members[{json.dumps(key)}]=std::move({nested});")
        elif isinstance(value, list):
            lines.append(f"{name}->type=5;")
            for child in value:
                nested = tree(child)
                lines.append(f"{name}->elements.push_back(std::move({nested}));")
            lines.append(f"for(size_t i=0;i<{name}->elements.size();++i){{if(i+1<{name}->elements.size()){name}->elements[i]->next={name}->elements[i+1].get();}}")
            lines.append(f"if(!{name}->elements.empty()){name}->child={name}->elements[0].get();")
        elif isinstance(value, str):
            lines.append(f"{name}->type=4;{name}->backing={json.dumps(value)};{name}->valuestring={name}->backing.c_str();")
        elif isinstance(value, bool):
            lines.append(f"{name}->type={2 if value else 1};")
        elif value is not None:
            lines.append(f"{name}->type=3;{name}->valuedouble={value};")
        return name
    root = tree(document)
    code = r'''
#include "phonebook.h"
#include "sip_blf.h"
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
using namespace esphome::voip_stack;
struct cJSON {int type=0;std::string backing;const char*valuestring=nullptr;double valuedouble=0;cJSON*next=nullptr,*child=nullptr;
 std::map<std::string,std::unique_ptr<cJSON>>members;std::vector<std::unique_ptr<cJSON>>elements;};
const cJSON*cJSON_GetObjectItemCaseSensitive(const cJSON*p,const char*key){if(!p)return nullptr;auto i=p->members.find(key);return i==p->members.end()?nullptr:i->second.get();}
bool cJSON_IsObject(const cJSON*p){return p&&p->type==6;}bool cJSON_IsArray(const cJSON*p){return p&&p->type==5;}
bool cJSON_IsString(const cJSON*p){return p&&p->type==4;}bool cJSON_IsNumber(const cJSON*p){return p&&p->type==3;}
bool cJSON_IsBool(const cJSON*p){return p&&(p->type==1||p->type==2);}bool cJSON_IsTrue(const cJSON*p){return p&&p->type==2;}
#define cJSON_ArrayForEach(item,array) for(item=(array)?(array)->child:nullptr;item;item=item->next)
'''
    settings = (SOURCE / "voip_settings.cpp").read_text()
    code += block(settings, "struct JsonRosterSlot {") + ";\n"
    for marker in ["const char *json_string(", "bool json_bool(", "uint16_t json_u16(", "const cJSON *json_metadata(",
                   "std::string json_metadata_string(", "uint16_t json_metadata_u16(", "bool json_metadata_bool(",
                   "bool json_metadata_string_array_contains(", "bool parse_json_roster_slot("]:
        code += block(settings, marker) + "\n"
    code += r'''
struct SipTransport{std::vector<BlfWatchConfig>desired;bool set_blf_watches(const std::vector<BlfWatchConfig>&v){desired=v;return true;}};
struct VoipStack{Phonebook phonebook_;bool blf_ready_=true,blf_watch_contacts_=true;uint8_t blf_capacity_=16;
 std::string blf_account_id_="ha",default_line_="qa";std::unique_ptr<SipTransport>transport_=std::make_unique<SipTransport>();
 std::string get_contact_account(std::string_view)const;bool set_blf_watches(const std::vector<BlfWatchConfig>&);void sync_blf_contacts_();};
'''
    facade = (SOURCE / "voip_blf.cpp").read_text()
    code += block(settings, "std::string VoipStack::get_contact_account(") + "\n"
    for marker in ["bool VoipStack::set_blf_watches(", "void VoipStack::sync_blf_contacts_("]:
        code += block(facade, marker) + "\n"
    code += "int main(){\n" + "\n".join(lines) + f"\nconst auto*contacts=cJSON_GetObjectItemCaseSensitive({root}.get(),\"contacts\");\n"
    code += r'''
 VoipStack phone;std::vector<ContactEntry>rows;
 for(const auto&item:contacts->elements){JsonRosterSlot slot;assert(parse_json_roster_slot(item.get(),&slot));ContactEntry e;
  e.name=slot.name;e.route=slot.route;e.id=slot.stable_id;e.kind=slot.kind;e.account_id=slot.account_id;e.auto_blf=slot.auto_blf;rows.push_back(e);}
 assert(phone.phonebook_.replace_all(rows));phone.sync_blf_contacts_();const auto&w=phone.transport_->desired;
 assert(w.size()==3);std::map<std::string,std::string>expected{{"browser-primary","Casa"},{"esphome:d74c0c09d06d0a7433e003f341e42a6b","esp_84c7bb000001"},{"sip-qa","qa"}};
 for(const auto&row:w){assert(expected.at(row.contact_id)==row.target&&row.account_id=="ha");}
 assert(phone.phonebook_.find_id("group:ring:Family")&&!contact_can_auto_blf(*phone.phonebook_.find_id("group:ring:Family")));
 ContactEntry external;external.id="ldap-phone";external.name="External phone";external.route="103";external.kind=ContactKind::CONTACT;external.auto_blf=true;external.account_id="office";
 assert(phone.phonebook_.add_entry(external)==AddResult::Added);phone.sync_blf_contacts_();assert(phone.transport_->desired.size()==4&&phone.transport_->desired.back().account_id=="office");
 ContactEntry unknown=external;unknown.id="unknown";unknown.name="Unknown";unknown.route="104";unknown.kind=ContactKind::UNKNOWN;
 assert(phone.phonebook_.add_entry(unknown)==AddResult::Added);phone.sync_blf_contacts_();assert(phone.transport_->desired.size()==4);
}
'''
    cpp = tmp_path / "policy.cpp"
    cpp.write_text(code)
    binary = tmp_path / "policy"
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation", "-g",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_ESP32", "-DUSE_ESPHOME_VOIP_BLF", "-DUSE_ESPHOME_VOIP_REGISTRATION", "-DUSE_ESPHOME_VOIP_CONTACT_METADATA",
        "-I", str(tmp_path), "-I", str(SOURCE), str(cpp), "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
