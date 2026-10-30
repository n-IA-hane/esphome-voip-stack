"""The JSON option preserves manual CSV contacts without a JSON parser."""

from pathlib import Path
import subprocess

import pytest

from test_refer_dialog_runtime import SOURCE

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("legacy", [False, True])
def test_real_contact_dispatch_preserves_csv_and_gates_json(tmp_path, legacy):
    settings = (SOURCE / "voip_settings.cpp").read_text()
    method = settings[settings.index("void VoipStack::set_contacts("):
                      settings.index("\nstd::string VoipStack::normalize_phonebook_for_transport_(")]
    code = r'''
#include "esphome/components/voip_stack/phonebook.h"
#include <cassert>
using namespace esphome::voip_stack;
#define ESP_LOGI(...)
#define ESP_LOGW(...)
struct VoipStack {
 Phonebook phonebook_;std::string device_name_="Self";bool cycle_active_=false;
 unsigned updates=0,json_calls=0;
 void set_contacts(const std::string&);
 std::string normalize_phonebook_for_transport_(const std::string&s){return s;}
 bool maybe_auto_select_ha_first_(){return false;}
 void publish_phonebook_changed_(){++updates;}
 void track_csv_(const std::string&){}
#ifdef USE_VOIP_HA_LEGACY_ROSTER
 bool apply_roster_json_contacts_(const std::string&){++json_calls;return false;}
#endif
};
'''
    code += method + r'''
int main(){
 VoipStack phone;phone.set_contacts("Desk");assert(phone.phonebook_.size()==1&&phone.updates==1);
 phone.set_contacts(" \t [\"not a CSV contact\"] ");
 assert(phone.phonebook_.size()==1&&phone.updates==1);
#ifdef USE_VOIP_HA_LEGACY_ROSTER
 assert(phone.json_calls==1);
#else
 assert(phone.json_calls==0);
#endif
 phone.set_contacts("Kitchen");assert(phone.phonebook_.size()==2&&phone.updates==2);
}
'''
    source = tmp_path / "roster.cpp"
    source.write_text(code)
    binary = tmp_path / "roster"
    flags = ["-DUSE_VOIP_HA_LEGACY_ROSTER"] if legacy else []
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_ESP32", *flags,
        "-I", str(ROOT), str(source), "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
