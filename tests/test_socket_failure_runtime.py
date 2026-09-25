"""Inject socket/bind/listen failures into production helpers, then retry."""

from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_socket_failures_release_descriptors_and_allow_retry(tmp_path):
    source = (ROOT / "esphome/components/voip_stack/sip_transport.cpp").read_text()
    methods = []
    for sig in ["bool SipTransport::bind_udp_", "bool SipTransport::bind_tcp_"]:
        start = source.index(sig)
        methods.append(source[start : source.index("\n}", start) + 2])
    prelude = r"""
#include <cassert>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <set>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <unistd.h>
constexpr const char *TAG="test";
const char* socket_errno_name(int){return "injected";}
const char* socket_errno_text(int){return "injected";}
#define ESP_LOGE(tag,...) do{(void)tag;if(false)std::printf(__VA_ARGS__);}while(false)
int fault=0;std::set<int> live;
int injected_socket(int domain,int type,int protocol){if(fault==1){errno=EMFILE;return -1;}int fd=::socket(domain,type,protocol);assert(fd>=0);assert(live.insert(fd).second);return fd;}
int injected_bind(int fd,const sockaddr*a,socklen_t n){if(fault==2){errno=EADDRINUSE;return -1;}return ::bind(fd,a,n);}
int injected_listen(int fd,int n){if(fault==3){errno=ENOBUFS;return -1;}return ::listen(fd,n);}
int injected_close(int fd){assert(live.erase(fd)==1);return ::close(fd);}
#define socket injected_socket
#define bind injected_bind
#define listen injected_listen
#define close injected_close
struct SipTransport {
 static constexpr int kRtpSocketRxBufferBytes=65536;
 bool bind_udp_(int*,uint16_t,const char*);bool bind_tcp_(int*,uint16_t,const char*);
};
"""
    tail = r"""
int main(){
 for(bool tcp:{false,true})for(int failure=1;failure<=(tcp?3:2);failure++) {
  SipTransport transport;int fd=-1;fault=failure;
  const auto call=[&](){return tcp?transport.bind_tcp_(&fd,0,"SIP"):transport.bind_udp_(&fd,0,"RTP");};
  assert(!call());assert(fd==-1);assert(live.empty());
  fault=0;assert(call());assert(fd>=0&&live.size()==1);assert(fcntl(fd,F_GETFD)>=0);
  assert(injected_close(fd)==0);fd=-1;assert(live.empty());
 }
}
"""
    cpp = tmp_path / "socket.cpp"
    cpp.write_text(prelude + "\n".join(methods) + tail)
    exe = tmp_path / "socket"
    subprocess.run(
        ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)],
        check=True,
        capture_output=True,
        text=True,
    )
    subprocess.run([str(exe)], check=True, capture_output=True, text=True)
