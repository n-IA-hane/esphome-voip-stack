"""Public ESP-TLS I/O contract with real socket readiness and deadlines.

The adapter models IDF 5.5.6's documented BIO contract. This is not a TLS
handshake interoperability test and does not replace target qualification.
"""

from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def tls_client(tmp_path_factory):
    folder = tmp_path_factory.mktemp("phone-client-tls")
    (folder / "esp_tls.h").write_text(r'''#pragma once
#include <cstddef>
#include <sys/types.h>
constexpr int ESP_OK=0;
constexpr int ESP_TLS_ERR_SSL_WANT_READ=-0x6900;
constexpr int ESP_TLS_ERR_SSL_WANT_WRITE=-0x6880;
struct esp_tls { int fd[2]{-1,-1}; unsigned reads=0,writes=0; };
using esp_tls_t=esp_tls;
struct esp_tls_cfg_t { int timeout_ms=0; const unsigned char*cacert_buf=nullptr;size_t cacert_bytes=0;int(*crt_bundle_attach)(void*)=nullptr; };
esp_tls_t*esp_tls_init();
int esp_tls_conn_new_sync(const char*,int,int,const esp_tls_cfg_t*,esp_tls_t*);
int esp_tls_get_conn_sockfd(esp_tls_t*,int*);
void esp_tls_conn_destroy(esp_tls_t*);
ssize_t esp_tls_conn_read(esp_tls_t*,void*,size_t);
ssize_t esp_tls_conn_write(esp_tls_t*,const void*,size_t);
''')
    (folder / "esp_crt_bundle.h").write_text("#pragma once\ninline int esp_crt_bundle_attach(void*){return 0;}\n")
    source = folder / "tls.cpp"
    source.write_text(r'''#include "esp_tls.h"
#include "esphome/components/phone_client/net_stream.h"
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
std::string scenario;
esp_tls_t*active=nullptr;
unsigned contexts=0;
esp_tls_t*esp_tls_init(){++contexts;return new esp_tls_t;}
int esp_tls_conn_new_sync(const char*,int,int,const esp_tls_cfg_t*cfg,esp_tls_t*t){
 assert(socketpair(AF_UNIX,SOCK_STREAM,0,t->fd)==0);
 timeval timeout{cfg->timeout_ms/1000,(cfg->timeout_ms%1000)*1000};
 assert(setsockopt(t->fd[0],SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout)==0);
 active=t;return 1;
}
int esp_tls_get_conn_sockfd(esp_tls_t*t,int*out){*out=t->fd[0];return ESP_OK;}
void esp_tls_conn_destroy(esp_tls_t*t){close(t->fd[0]);close(t->fd[1]);delete t;active=nullptr;--contexts;}
ssize_t esp_tls_conn_read(esp_tls_t*t,void*out,size_t length){
 ++t->reads;
 if(scenario=="fatal")return -12345;
 if(scenario=="buffered"||(scenario=="read-want-write"&&t->reads>1)){assert(length>=3);std::memcpy(out,"abc",3);return 3;}
 if(scenario=="read-want-write")return ESP_TLS_ERR_SSL_WANT_WRITE;
 const auto n=recv(t->fd[0],out,length,0);
 return n<0&&(errno==EAGAIN||errno==EWOULDBLOCK)?ESP_TLS_ERR_SSL_WANT_READ:n;
}
ssize_t esp_tls_conn_write(esp_tls_t*t,const void*,size_t length){
 ++t->writes;
 if(scenario=="write-want-read"&&t->writes==1)return ESP_TLS_ERR_SSL_WANT_READ;
 if(scenario=="partial-write")return std::min<size_t>(2,length);
 return static_cast<ssize_t>(length);
}
int main(int argc,char**argv){
 assert(argc==2);scenario=argv[1];{
 esphome::phone_client::TcpStream stream;assert(stream.connect("fixture.test",993,400,true));
 uint8_t data[16]{};const auto start=std::chrono::steady_clock::now();int n=0;
 if(scenario=="deadline") {n=stream.read(data,sizeof data,20);assert(n<0);}
 else if(scenario=="read-want-write"||scenario=="buffered"){n=stream.read(data,sizeof data,150);assert(n==3&&!std::memcmp(data,"abc",3));}
 else if(scenario=="write-want-read"){
   const int fd=active->fd[1];std::thread peer([fd](){std::this_thread::sleep_for(std::chrono::milliseconds(20));assert(send(fd,"x",1,0)==1);});
   n=stream.write(data,8,150);peer.join();assert(n==8);
 }else if(scenario=="partial-write"){assert(esphome::phone_client::write_all(stream,data,9,150));assert(active->writes==5);}
 else if(scenario=="fatal"){n=stream.read(data,sizeof data,150);assert(n<0);}
 else if(scenario=="interrupt"){
   std::thread control([&stream](){std::this_thread::sleep_for(std::chrono::milliseconds(20));stream.interrupt();});
   n=stream.read(data,sizeof data,150);control.join();assert(n<=0);
 }else assert(false);
 const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
 assert(elapsed<120&&"caller deadline/readiness was replaced by connection timeout");
 stream.close();stream.close();assert(contexts==0);
 }assert(contexts==0);
}
''')
    binary = folder / "tls"
    result = subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g", "-pthread",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_PHONE_CLIENT_TLS",
        "-I", str(folder), "-I", str(ROOT), str(source),
        str(ROOT / "esphome/components/phone_client/net_stream.cpp"), "-o", str(binary),
    ], capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("scenario", [
    "deadline", "read-want-write", "write-want-read", "buffered", "partial-write", "fatal", "interrupt",
])
def test_tls_respects_caller_deadline_and_readiness(tls_client, scenario):
    result = subprocess.run([str(tls_client), scenario], capture_output=True, text=True, timeout=3)
    assert result.returncode == 0, result.stderr
