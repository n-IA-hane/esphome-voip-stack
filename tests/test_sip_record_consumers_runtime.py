"""Common SIP consumers agree on logical fields and the body boundary."""
import subprocess

import pytest

from test_refer_dialog_runtime import ROOT
from test_refer_wire_runtime import wire_source


def record_consumer_source():
    code = '#include "esphome/components/voip_stack/sip_record.h"\n' + wire_source()
    code += r'''
#include <new>
static bool measuring=false;static unsigned allocations=0;
void*operator new(size_t size){if(measuring)++allocations;auto*p=std::malloc(size);if(!p)throw std::bad_alloc();return p;}
void operator delete(void*p)noexcept{std::free(p);}
void operator delete(void*p,size_t)noexcept{std::free(p);}
int main(int argc,char**argv){
 assert(argc==2);const std::string mode=argv[1];
 const std::string message="INVITE sip:a@192.0.2.1 SIP/2.0\r\nVia \t: SIP/2.0/UDP first.example\r\n\t;branch=one\r\n"
  "v: SIP/2.0/TCP second.example;branch=two\r\nFrom:\r\n\t\"Name\" <sip:alice@example.test>;tag=remote\r\n"
  "Accept \t: text/plain,\r\n application/sdp;q=1\r\nl \t:\r\n\t4\r\n\r\nTEST\r\nVia: body-only\r\n";
 if(mode=="allocations"){
  std::string unrelated="OPTIONS sip:a@example.test SIP/2.0\r\n";
  for(unsigned n=0;n<80;++n)unrelated+="X-Unrelated: "+std::string(96,'x')+"\r\n";
  unrelated+="Content-Length: 0\r\n\r\n";
  measuring=true;for(unsigned n=0;n<100;++n)assert(header_value(unrelated,"Never-Present").empty());measuring=false;
  std::fprintf(stderr,"lookup_allocations=%u\n",allocations);
  assert(allocations==0&&"Unrelated header fields must remain borrowed views");return 0;
 }
 if(mode=="headers"){
  assert(header_value(message,"Via")=="SIP/2.0/UDP first.example ;branch=one");
  assert(header_values(message,"Via")=="SIP/2.0/UDP first.example ;branch=one\r\nVia: SIP/2.0/TCP second.example;branch=two");
  assert(name_addr_uri(header_value(message,"From"))=="sip:alice@example.test");
  assert(tag_from_header(header_value(message,"From"))=="remote");
  assert(header_value("SIP/2.0 200 OK\r\n\r\nVia: body-only\r\n","Via").empty());
 }else if(mode=="accept"){
  assert(sip_accepts_sdp(message));
  assert(!sip_accepts_sdp("OPTIONS sip:a@b SIP/2.0\r\nAccept \t: application/sdp;\r\n q=0\r\n\r\n"));
  assert(sip_accepts_sdp("OPTIONS sip:a@b SIP/2.0\r\n\r\nAccept: text/plain\r\n"));
 }else{
  size_t length=0;assert(sip_content_length(message,&length)&&length==4);
  assert(message_body(message)=="TEST");
  auto duplicate=message;duplicate.insert(duplicate.find("\r\n\r\n"),"\r\nContent-Length: 4");
  assert(!sip_content_length(duplicate,&length));
  assert(!sip_content_length("SIP/2.0 200 OK\r\nContent-Length: 4\r\n\t4\r\n\r\n",&length));
 }
}
'''
    return code


@pytest.fixture(scope='module')
def record_consumers(tmp_path_factory):
    code = record_consumer_source()
    path = tmp_path_factory.mktemp('sip-record-consumers')
    cpp = path / 'consumers.cpp'
    cpp.write_text(code)
    binary = path / 'consumers'
    built = subprocess.run(['g++', '-std=c++17', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                            '-I', str(ROOT), str(cpp), '-o', str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize('consumer', ['headers', 'accept', 'length', 'allocations'])
def test_logical_fields_preserve_syntax_and_body_boundary(record_consumers, consumer):
    run = subprocess.run([str(record_consumers), consumer], capture_output=True, text=True, timeout=5)
    assert run.returncode == 0, run.stdout + run.stderr
