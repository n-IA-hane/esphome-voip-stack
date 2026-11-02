"""Shared record iteration preserves header order, folding and body boundary."""
from pathlib import Path
import subprocess


def test_record_fields_keep_compact_names_folding_and_body_boundary(tmp_path):
    source = Path(__file__).resolve().parents[1] / "esphome/components/voip_stack"
    cpp = tmp_path / "record.cpp"
    cpp.write_text(r'''
#include "sip_record.h"
#include <cassert>
#include <vector>
using namespace esphome::voip_stack;
int main(){
 const std::string record="NOTIFY sip:a@b SIP/2.0\r\nv: SIP/2.0/TCP first;branch=one\r\nVia: SIP/2.0/UDP second\r\no: message-summary\r\nRecord-Route: <sip:first;lr>,\r\n \t<sip:second;lr>\r\n\r\nVia: must-not-parse\r\n";
 std::vector<std::string>via;sip_record_fields(record,"Via",[&](std::string_view field){via.push_back(sip_record_unfold(field));return true;});
 assert(via.size()==2&&via[0]=="SIP/2.0/TCP first;branch=one"&&via[1]=="SIP/2.0/UDP second");
 assert(sip_record_first(record,"Event")=="message-summary");
 assert(sip_record_unfold(sip_record_first(record,"Record-Route"))=="<sip:first;lr>, <sip:second;lr>");
 assert(sip_record_first(record,"Call-ID").empty());
 unsigned count=0;sip_record_fields(record,"Via",[&](std::string_view){++count;return false;});assert(count==1);
}
''')
    binary = tmp_path / "record"
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-fno-pie", "-no-pie", "-I", str(source), str(cpp), "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
