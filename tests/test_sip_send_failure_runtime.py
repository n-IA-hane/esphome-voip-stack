"""A failed partial TCP SIP record closes the stream; a fresh stream can send."""

from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_partial_sip_record_failure_and_retry(tmp_path):
    source = (ROOT / "esphome/components/voip_stack/sip_transport.cpp").read_text()
    start = source.index("bool SipTransport::send_sip_tcp_record_(const char *data")
    method = source[start : source.index("\n}", start) + 2]
    cpp = tmp_path / "send.cpp"
    cpp.write_text(
        r"""
#include <cassert>
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <string>
constexpr char TAG[]="test";
#define ESP_LOGI(tag,...) do{(void)tag;if(false)std::printf(__VA_ARGS__);}while(false)
#define ESP_LOGW(tag,...) ESP_LOGI(tag,__VA_ARGS__)
const char* socket_errno_name(int){return "injected";}
const char* socket_errno_text(int){return "injected";}
constexpr int SHUT_RDWR=2;int fail_at=0,calls=0,shutdowns=0;std::string wire;
int send(int,const char*p,size_t n,int){if(++calls==fail_at){errno=EPIPE;return -1;}size_t count=n>3?3:n;wire.append(p,count);return count;}
int shutdown(int,int){++shutdowns;return 0;}
struct SipTransport{bool send_sip_tcp_record_(const char*,size_t,int);};
"""
        + method
        + r"""
int main(){
 const char message[]="INVITE sip:peer SIP/2.0\r\n\r\n";
 for(int failed_write:{1,2,3}){
  SipTransport transport;fail_at=failed_write;calls=shutdowns=0;wire.clear();
  assert(!transport.send_sip_tcp_record_(message,sizeof(message)-1,5));assert(shutdowns==1);assert(wire.size()==size_t(failed_write-1)*3);
  fail_at=0;calls=shutdowns=0;wire.clear();
  assert(transport.send_sip_tcp_record_(message,sizeof(message)-1,6));assert(wire==message&&shutdowns==0);
 }
}
"""
    )
    exe = tmp_path / "send"
    subprocess.run(
        ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)],
        check=True,
        capture_output=True,
        text=True,
    )
    subprocess.run([str(exe)], check=True, capture_output=True, text=True)
