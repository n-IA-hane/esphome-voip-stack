"""Provider snapshots cannot partially replace manual or another owner's data."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_provider_validation_capacity_metadata_and_manual_contacts(tmp_path):
    source = tmp_path / "atomic.cpp"
    source.write_text(r'''
#include "esphome/components/voip_stack/phonebook.h"
#include <cassert>
using namespace esphome::voip_stack;
ContactEntry row(const char*id,const char*name,const char*route){
 ContactEntry e;e.id=id;e.name=name;e.route=route;return e;
}
int main(){
 Phonebook book;int ldap,other,ha;
 auto manual=row("manual-id","Manual","100");
 assert(book.add_entry(manual)==AddResult::Added);
 auto old=row("old-id","Old","101");old.kind=ContactKind::ESP;old.auto_blf=true;old.account_id="ha";
 assert(book.replace_directory(&ldap,{old})==DirectoryCommitResult::APPLIED);
 assert(book.select("old-id"));
 auto renamed=old;renamed.name="Renamed";
 assert(book.replace_directory(&ldap,{renamed})==DirectoryCommitResult::APPLIED);
 assert(book.current_name()=="Renamed"&&book.current()->stable_id()=="old-id");
 assert(book.replace_directory(&ldap,{renamed})==DirectoryCommitResult::UNCHANGED);
 auto invalid=renamed;invalid.name="bad\nname";
 assert(book.replace_directory(&ldap,{renamed,invalid})==DirectoryCommitResult::INVALID);
 assert(book.size()==2&&book.current_name()=="Renamed");
 auto duplicate=renamed;duplicate.route="102";
 assert(book.replace_directory(&ldap,{renamed,duplicate})==DirectoryCommitResult::INVALID);
 assert(book.replace_directory(&other,{renamed})==DirectoryCommitResult::CONFLICT);
 assert(book.size()==2&&book.current_name()=="Renamed");
 auto shadow=row("different-id","Manual","103");
 assert(book.replace_directory(&ldap,{renamed,shadow})==DirectoryCommitResult::UNCHANGED);
 assert(book.find("Manual")->route=="100"&&book.find("Manual")->origin==ContactOrigin::MANUAL);
 auto managed=row("ha-id","Managed","104");
 assert(book.replace_provider(ContactOrigin::HA,&ha,{managed})==DirectoryCommitResult::APPLIED);
 assert(book.replace_provider(ContactOrigin::HA,&ha,{})==DirectoryCommitResult::APPLIED);
 assert(book.size()==2&&book.find("Manual")&&book.find_id("old-id"));
 std::vector<ContactEntry> too_many;
 for(unsigned i=0;i<64;++i){auto n=std::to_string(i);ContactEntry e;e.id="capacity-"+n;e.name="Capacity"+n;e.route="route-"+n;too_many.push_back(e);}
 assert(book.replace_directory(&ldap,too_many)==DirectoryCommitResult::CAPACITY);
 assert(book.size()==2&&book.current_name()=="Renamed");
 // A metadata-only change must be visible to the existing BLF observer.
 renamed.auto_blf=false;
 assert(book.replace_directory(&ldap,{renamed})==DirectoryCommitResult::APPLIED);
 assert(!book.find_id("old-id")->auto_blf);
 assert(book.replace_directory(&ldap,{})==DirectoryCommitResult::APPLIED);
 assert(book.size()==1&&book.find("Manual"));
 assert(book.set_metadata("Manual","alias",ContactKind::CONTACT,"office",true));
 assert(book.find_id("alias")->auto_blf&&book.find_id("alias")->account_id=="office");
 assert(!book.set_metadata("Manual","alias",ContactKind::CONTACT,"office",true));
 assert(!book.set_metadata("Manual","bad\nname",ContactKind::CONTACT,"office",true));
 assert(book.find_id("alias"));
}
''')
    binary = tmp_path / "atomic"
    built = subprocess.run(["g++", "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_ESP32", "-DUSE_ESPHOME_VOIP_DIRECTORY",
        "-DUSE_ESPHOME_VOIP_CONTACT_METADATA", "-I", str(ROOT), str(source), "-o", str(binary)],
        capture_output=True, text=True, timeout=30)
    assert built.returncode == 0, built.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr
