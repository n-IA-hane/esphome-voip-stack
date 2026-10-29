"""Keep authoritative roster semantics while reusing owned normalization storage."""

from pathlib import Path
import subprocess

from test_refer_dialog_runtime import block

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "esphome/components/voip_stack"


def test_phonebook_normalizes_owned_vector_and_streams_valid_csv_rows(tmp_path):
    source = (SOURCE / "voip_settings.cpp").read_text()
    helpers = "\n".join(block(source, marker) for marker in (
        "struct ParsedPhonebookSlot", "bool parse_slot_for_normalize(",
        "void append_csv(", "std::string serialize_endpoint(",
    ))
    helpers = helpers.replace("struct ParsedPhonebookSlot {\n  ContactEntry entry;\n}",
                              "struct ParsedPhonebookSlot {\n  ContactEntry entry;\n};")
    method = block(source, "std::string VoipStack::normalize_phonebook_for_transport_(")
    cpp = tmp_path / "phonebook.cpp"
    cpp.write_text(r'''
#include <cassert>
#include <string>
#include <vector>
#include "esphome/components/voip_stack/phonebook.h"
using namespace esphome::voip_stack;
struct Book:Phonebook {const ContactEntry* storage()const{return entries_.data();}size_t capacity()const{return entries_.capacity();}};
struct VoipStack {std::string normalize_phonebook_for_transport_(const std::string&);};
''' + helpers + "\n" + method + r'''
ContactEntry entry(std::string route,std::string name){
 ContactEntry e;e.route=std::move(route);e.name=std::move(name);return e;
}
int main(){
 Book book;book.set_self_name("self");
 std::vector<ContactEntry> rows;
 rows.push_back(entry("self","self"));
 rows.push_back(entry("first","First"));
 rows.push_back(entry("second","Second"));
 rows.push_back(entry("first","Updated"));
 const auto* storage=rows.data();
 assert(book.replace_all(std::move(rows)));
 assert(book.storage()==storage);assert(book.size()==2);
 assert(book.current_name()=="Updated");assert(book.select("second"));
 std::vector<ContactEntry> same{entry("first","Updated"),entry("second","Second")};
 assert(!book.replace_all(std::move(same)));assert(book.current_route()=="second");
 std::vector<ContactEntry> full;
 for(unsigned n=0;n<80;n++)full.push_back(entry("r"+std::to_string(n),"N"+std::to_string(n)));
 assert(book.replace_all(std::move(full)));assert(book.size()==64);
 assert(book.find("r63")&&!book.find("r64"));assert(book.capacity()<=64);
 std::vector<ContactEntry> overreserved;overreserved.reserve(64);
 overreserved.push_back(entry("small","Small"));
 assert(book.replace_all(std::move(overreserved)));assert(book.capacity()<=1);
 assert(book.replace_all({}));assert(book.empty());
 VoipStack v;
 const std::string malformed="bad|row|with|wrong|format";
 assert(v.normalize_phonebook_for_transport_(malformed)==malformed);
 // Invalid rows must not consume the maximum number of valid contacts.
 std::string csv;
 for(unsigned n=0;n<80;n++){if(!csv.empty())csv+=',';csv+="bad||invalid";}
 csv+=",First|192.0.2.1|5060|40000|sip_udp";
 assert(v.normalize_phonebook_for_transport_(csv)=="First|192.0.2.1|5060|40000|sip_udp");
 const std::string legacy="First|192.0.2.1|5060|40000|sip_udp,"
  "Second|route2|192.0.2.2|5060|40000|sip_tcp,Third";
 assert(v.normalize_phonebook_for_transport_(legacy)==legacy);
 csv.clear();std::string expected;
 for(unsigned n=0;n<80;n++){
  if(!csv.empty())csv+=',';csv+="N"+std::to_string(n);
  if(n<64){if(!expected.empty())expected+=',';expected+="N"+std::to_string(n);}
 }
 assert(v.normalize_phonebook_for_transport_(csv)==expected);
}
''')
    binary = tmp_path / "phonebook"
    result = subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-Wno-misleading-indentation", "-fsanitize=address,undefined",
        "-DUSE_ESP32", "-I", str(ROOT), str(cpp), "-o", str(binary),
    ], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
