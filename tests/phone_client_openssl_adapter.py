"""Host adapter for the ESP-TLS public API, backed by real OpenSSL TLS.

This tests real peer/CA/hostname behavior through production TcpStream. It does
not claim to execute Espressif's TLS backend or its scheduler on a host.
"""
from pathlib import Path


def write_adapter(folder: Path):
    (folder / "esp_tls.h").write_text(r'''#pragma once
#include <cstddef>
#include <sys/types.h>
#include <openssl/ssl.h>
constexpr int ESP_OK=0;
constexpr int ESP_TLS_ERR_SSL_WANT_READ=-0x6900;
constexpr int ESP_TLS_ERR_SSL_WANT_WRITE=-0x6880;
struct esp_tls {int fd=-1; SSL_CTX*ctx=nullptr; SSL*ssl=nullptr; bool started=false;};
using esp_tls_t=esp_tls;
struct esp_tls_cfg_t {int timeout_ms=0;bool non_block=false;const unsigned char*cacert_buf=nullptr;size_t cacert_bytes=0;int(*crt_bundle_attach)(void*)=nullptr;};
esp_tls_t*esp_tls_init();
int esp_tls_conn_new_sync(const char*,int,int,const esp_tls_cfg_t*,esp_tls_t*);
int esp_tls_conn_new_async(const char*,int,int,const esp_tls_cfg_t*,esp_tls_t*);
int esp_tls_get_conn_sockfd(esp_tls_t*,int*);
void esp_tls_conn_destroy(esp_tls_t*);
ssize_t esp_tls_conn_read(esp_tls_t*,void*,size_t);
ssize_t esp_tls_conn_write(esp_tls_t*,const void*,size_t);
''')
    (folder / "esp_crt_bundle.h").write_text("#pragma once\ninline int esp_crt_bundle_attach(void*){return 0;}\n")
    cpp = folder / "openssl_adapter.cpp"
    cpp.write_text(r'''#include "esp_tls.h"
#include <algorithm>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
esp_tls_t*esp_tls_init(){return new esp_tls_t;}
static bool start(const char*host,int length,int port,const esp_tls_cfg_t*cfg,esp_tls_t*t){
 std::string hostname(host,length);addrinfo hints{},*addresses=nullptr;
 hints.ai_socktype=SOCK_STREAM;hints.ai_family=AF_INET;
 if(getaddrinfo(hostname.c_str(),std::to_string(port).c_str(),&hints,&addresses))return false;
 t->fd=socket(addresses->ai_family,addresses->ai_socktype,addresses->ai_protocol);
 bool ok=t->fd>=0&&connect(t->fd,addresses->ai_addr,addresses->ai_addrlen)==0;freeaddrinfo(addresses);
 if(!ok)return false;
 timeval timeout{cfg->timeout_ms/1000,(cfg->timeout_ms%1000)*1000};
 setsockopt(t->fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
 setsockopt(t->fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
 t->ctx=SSL_CTX_new(TLS_client_method());if(!t->ctx)return false;
 SSL_CTX_set_verify(t->ctx,SSL_VERIFY_PEER,nullptr);
 if(cfg->cacert_buf){
  BIO*b=BIO_new_mem_buf(cfg->cacert_buf,cfg->cacert_bytes);X509*ca=PEM_read_bio_X509(b,nullptr,nullptr,nullptr);BIO_free(b);
  if(!ca)return false;ok=X509_STORE_add_cert(SSL_CTX_get_cert_store(t->ctx),ca)==1;X509_free(ca);if(!ok)return false;
 }else SSL_CTX_set_default_verify_paths(t->ctx);
 t->ssl=SSL_new(t->ctx);if(!t->ssl)return false;
 SSL_set_tlsext_host_name(t->ssl,hostname.c_str());SSL_set1_host(t->ssl,hostname.c_str());SSL_set_fd(t->ssl,t->fd);
 if(cfg->non_block)fcntl(t->fd,F_SETFL,fcntl(t->fd,F_GETFL,0)|O_NONBLOCK);
 t->started=true;return true;
}
int esp_tls_conn_new_sync(const char*h,int n,int p,const esp_tls_cfg_t*c,esp_tls_t*t){
 return start(h,n,p,c,t)&&SSL_connect(t->ssl)==1?1:-1;
}
int esp_tls_conn_new_async(const char*h,int n,int p,const esp_tls_cfg_t*c,esp_tls_t*t){
 if(!t->started&&!start(h,n,p,c,t))return -1;
 int r=SSL_connect(t->ssl);if(r==1)return 1;int e=SSL_get_error(t->ssl,r);
 return e==SSL_ERROR_WANT_READ||e==SSL_ERROR_WANT_WRITE?0:-1;
}
int esp_tls_get_conn_sockfd(esp_tls_t*t,int*out){*out=t->fd;return t->fd>=0?0:-1;}
void esp_tls_conn_destroy(esp_tls_t*t){if(t->ssl)SSL_free(t->ssl);if(t->ctx)SSL_CTX_free(t->ctx);if(t->fd>=0)close(t->fd);delete t;}
static int outcome(esp_tls_t*t,int r){
 if(r>0)return r;int e=SSL_get_error(t->ssl,r);if(e==SSL_ERROR_ZERO_RETURN)return 0;
 return e==SSL_ERROR_WANT_READ?ESP_TLS_ERR_SSL_WANT_READ:e==SSL_ERROR_WANT_WRITE?ESP_TLS_ERR_SSL_WANT_WRITE:-1;
}
ssize_t esp_tls_conn_read(esp_tls_t*t,void*p,size_t n){return outcome(t,SSL_read(t->ssl,p,std::min<size_t>(n,INT_MAX)));}
ssize_t esp_tls_conn_write(esp_tls_t*t,const void*p,size_t n){return outcome(t,SSL_write(t->ssl,p,std::min<size_t>(n,INT_MAX)));}
''')
    return cpp
