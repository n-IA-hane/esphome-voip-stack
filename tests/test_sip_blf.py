"""Real bounded RFC4235 parser: version recovery, namespaces and aggregate state."""

from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "esphome/components/voip_stack"


@pytest.fixture(scope="module")
def parser_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("blf-parser")
    defines = folder / "esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    source = folder / "parser.cpp"
    source.write_text(r'''
#include "sip_blf.h"
#include <cassert>
#include <string>
using namespace esphome::voip_stack;
std::string doc(unsigned version,const char *state,const std::string &body) {
 return "<dialog-info xmlns='urn:ietf:params:xml:ns:dialog-info' version='"+
   std::to_string(version)+"' state='"+state+"' entity='sip:101@example.test'>"+body+"</dialog-info>";
}
int main(int argc,char **argv) {
 assert(argc==2);const std::string scenario=argv[1];BlfDialogCache cache;BlfStateInfo info;
 cache.snapshot(&info);assert(!info.available&&info.state==BlfState::UNKNOWN);
 if(scenario=="partial") {
  assert(cache.apply(doc(0,"full","<dialog id='first'><state>confirmed</state></dialog><dialog id='second'><state>early</state></dialog>"))==BlfDocumentResult::APPLIED);
  cache.snapshot(&info);assert(info.available&&info.state==BlfState::RINGING&&info.ringing_dialogs==1&&info.active_dialogs==1);
  assert(cache.apply(doc(1,"partial","<dialog id='second'><state>terminated</state></dialog>"))==BlfDocumentResult::APPLIED);
  cache.snapshot(&info);assert(info.state==BlfState::ACTIVE&&info.active_dialogs==1&&!info.ringing_dialogs);
  assert(cache.apply(doc(2,"partial","<dialog id='first'><local><target><param pname='+sip.rendering' pval='no'/></target></local></dialog>"))==BlfDocumentResult::APPLIED);
  cache.snapshot(&info);assert(info.state==BlfState::HELD&&info.held_dialogs==1);
  assert(cache.apply(doc(4,"partial","<dialog id='first'><state>terminated</state></dialog>"))==BlfDocumentResult::NEED_FULL);
  cache.snapshot(&info);assert(info.version==2&&info.state==BlfState::HELD);
  assert(cache.apply(doc(8,"full",""))==BlfDocumentResult::APPLIED);
  cache.snapshot(&info);assert(info.state==BlfState::IDLE&&info.version==8);
  assert(cache.apply(doc(7,"full","<dialog id='old'><state>early</state></dialog>"))==BlfDocumentResult::IGNORED);
 } else if(scenario=="namespace") {
  const std::string xml="<?xml version='1.0'?><d:dialog-info xmlns:d='urn:ietf:params:xml:ns:dialog-info' version='9' state='full' entity='sip:x@example.test'><!--comment--><d:dialog id='a&amp;b'><s:state xmlns:s='urn:ietf:params:xml:ns:dialog-info'><![CDATA[confirmed]]></s:state></d:dialog></d:dialog-info>";
  assert(cache.apply(xml)==BlfDocumentResult::APPLIED);
  assert(cache.apply(doc(10,"partial","<dialog id='a&#38;b'><state>terminated</state></dialog>"))==BlfDocumentResult::APPLIED);
  cache.snapshot(&info);assert(info.available&&info.state==BlfState::IDLE);
 } else if(scenario=="unicode") {
  assert(cache.apply(doc(0,"full","<dialog id='caf&#233;'><state>early</state></dialog>"))==BlfDocumentResult::APPLIED);
  assert(cache.apply(doc(1,"partial",u8"<dialog id='café'><state>terminated</state></dialog>"))==BlfDocumentResult::APPLIED);
  cache.snapshot(&info);assert(info.state==BlfState::IDLE);
 } else if(scenario=="first-partial") {
  assert(cache.apply(doc(0,"partial","<dialog id='x'><state>early</state></dialog>"))==BlfDocumentResult::NEED_FULL);
  cache.snapshot(&info);assert(!info.available);
  assert(cache.apply(doc(100,"full","<dialog id='x'><state>confirmed</state></dialog>"))==BlfDocumentResult::APPLIED);
  cache.invalidate();cache.snapshot(&info);assert(!info.available&&info.state==BlfState::UNKNOWN);
  assert(cache.apply(doc(0,"full",""))==BlfDocumentResult::APPLIED);
 } else if(scenario=="limits") {
  assert(cache.apply(doc(0,"full",""))==BlfDocumentResult::APPLIED);
  std::string body;for(unsigned i=0;i<5;++i)body+="<dialog id='"+std::to_string(i)+"'><state>confirmed</state></dialog>";
  assert(cache.apply(doc(1,"full",body))==BlfDocumentResult::LIMIT);
  cache.snapshot(&info);assert(info.state==BlfState::IDLE&&info.version==0);
  assert(cache.apply(std::string(4097,'x'))==BlfDocumentResult::LIMIT);
  assert(cache.apply(doc(1,"full","<dialog id='"+std::string(129,'x')+"'><state>early</state></dialog>"))==BlfDocumentResult::INVALID);
 } else if(scenario=="invalid") {
  for(const auto &body:{"<!DOCTYPE x [<!ENTITY foo SYSTEM 'file:///no'>]><dialog/>","<dialog id='x'><state>early</dialog>","<dialog id='x'><state>invalid</state></dialog>","<dialog id='x'><state>early</state></dialog><dialog id='x'><state>confirmed</state></dialog>","<dialog id='x' id='y'><state>early</state></dialog>","<dialog id='&evil;'><state>early</state></dialog>"})
   assert(cache.apply(doc(0,"full",body))==BlfDocumentResult::INVALID);
  assert(cache.apply("<dialog-info xmlns='wrong' version='0' state='full' entity='sip:x'/>")==BlfDocumentResult::INVALID);
  assert(cache.apply("<dialog-info xmlns='urn:ietf:params:xml:ns:dialog-info' version='4294967296' state='full' entity='sip:x'/>")==BlfDocumentResult::INVALID);
 } else return 2;
}
''')
    binary = folder / "parser"
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie",
        "-DUSE_ESPHOME_VOIP_BLF", "-I", str(folder), "-I", str(SOURCE),
        str(source), str(SOURCE / "sip_blf.cpp"), "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("scenario", ["partial", "namespace", "unicode", "first-partial", "limits", "invalid"])
def test_dialog_info_updates_are_bounded_and_transactional(parser_binary, scenario):
    result = subprocess.run([parser_binary, scenario], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
