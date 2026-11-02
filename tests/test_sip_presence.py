"""PIDF uses bounded standard XML and never aliases dialog-idle to online."""

from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "esphome/components/voip_stack"


@pytest.fixture(scope="module")
def presence_parser(tmp_path_factory):
    folder = tmp_path_factory.mktemp("pidf-parser")
    header = folder / "esphome/core/defines.h"
    header.parent.mkdir(parents=True)
    header.write_text("")
    source = folder / "pidf.cpp"
    source.write_text(r'''
#include "sip_blf.h"
#include <cassert>
using namespace esphome::voip_stack;
std::string doc(const std::string&body){return "<presence xmlns='urn:ietf:params:xml:ns:pidf' entity='sip:x@example.test'>"+body+"</presence>";}
std::string tuple(const char*id,const char*state){return std::string("<tuple id='")+id+"'><status><basic>"+state+"</basic></status></tuple>";}
int main(int argc,char**argv){
 assert(argc==2);const std::string scenario=argv[1];PresenceState state=PresenceState::OPEN;
 if(scenario=="aggregate"){
  assert(parse_presence(doc(tuple("one","closed")+tuple("two","open")),&state)&&state==PresenceState::OPEN);
  assert(parse_presence(doc(tuple("one","open")+tuple("two","closed")),&state)&&state==PresenceState::OPEN);
  assert(parse_presence(doc(tuple("one","closed")+tuple("two","closed")),&state)&&state==PresenceState::CLOSED);
 }else if(scenario=="namespace"){
  assert(parse_presence("<?xml version='1.0'?><p:presence xmlns:p='urn:ietf:params:xml:ns:pidf' entity='sip:x'><p:tuple id='x'><p:status><s:basic xmlns:s='urn:ietf:params:xml:ns:pidf'><![CDATA[open]]></s:basic></p:status></p:tuple></p:presence>",&state)&&state==PresenceState::OPEN);
  assert(!parse_presence("<presence xmlns='wrong' entity='sip:x'><tuple id='x'><status><basic>closed</basic></status></tuple></presence>",&state)&&state==PresenceState::OPEN);
 }else if(scenario=="extensions"){
  assert(parse_presence(doc("<tuple id='x'><status><x:available xmlns:x='urn:other'>yes</x:available></status></tuple>"),&state)&&state==PresenceState::UNKNOWN);
  assert(parse_presence(doc("<tuple id='x'><status><basic xmlns='wrong'>closed</basic></status></tuple>"),&state)&&state==PresenceState::UNKNOWN);
  assert(parse_presence(doc(tuple("one","closed")+"<tuple id='two'><status><x:s xmlns:x='other'/></status></tuple>"),&state)&&state==PresenceState::UNKNOWN);
 }else if(scenario=="empty"){
  assert(parse_presence(doc(""),&state)&&state==PresenceState::UNKNOWN);
  assert(!parse_presence("<presence xmlns='urn:ietf:params:xml:ns:pidf'/>",&state));
 }else if(scenario=="invalid"){
  for(const auto&body:{"<tuple><status><basic>closed</basic></status></tuple>","<tuple id='x'/>","<tuple id='x'><status><basic>closed</basic><basic>open</basic></status></tuple>","<tuple id='x'><status/><status><basic>closed</basic></status></tuple>","<tuple id='x'><status/><foo><basic>closed</basic></foo></tuple>","<tuple id='x'><status><basic>unknown</basic></status></tuple>","<tuple id='x'><status><basic>closed</status></tuple>","<tuple id='x'><status><basic><x/></basic></status></tuple>","<tuple id='x'><status><basic>closed</basic></status></tuple><tuple id='&#120;'><status><basic>open</basic></status></tuple>","<!DOCTYPE x [<!ENTITY e SYSTEM 'file:///no'>]><tuple id='x'><status><basic>open</basic></status></tuple>"}){
   state=PresenceState::OPEN;assert(!parse_presence(doc(body),&state)&&state==PresenceState::OPEN);
  }
 }else if(scenario=="limits"){
  assert(!parse_presence(std::string(4097,'x'),&state));
  std::string body;for(unsigned n=0;n<17;++n)body+=tuple(std::to_string(n).c_str(),"closed");
  assert(!parse_presence(doc(body),&state));
  assert(!parse_presence(doc(tuple(std::string(129,'x').c_str(),"open")),&state));
  assert(!parse_presence(doc(tuple("x","open")),nullptr));
 }else return 2;
}
''')
    binary = folder / "pidf"
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_ESPHOME_VOIP_BLF", "-DUSE_ESPHOME_VOIP_PRESENCE",
        "-I", str(folder), "-I", str(SOURCE), str(source), str(SOURCE / "sip_blf.cpp"), "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("scenario", ["aggregate", "namespace", "extensions", "empty", "invalid", "limits"])
def test_pidf_is_bounded_and_transactional(presence_parser, scenario):
    result = subprocess.run([presence_parser, scenario], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
