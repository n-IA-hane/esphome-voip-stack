#include "sip_mwi.h"
#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <iostream>
#include <openssl/evp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace esphome::voip_stack;
std::string hash_value(const std::string &algorithm, const std::string &input) {
  const EVP_MD *md = algorithm == "MD5" ? EVP_md5() : EVP_sha256();
  unsigned char bytes[EVP_MAX_MD_SIZE];
  unsigned size = 0;
  assert(EVP_Digest(input.data(), input.size(), bytes, &size, md, nullptr) ==
         1);
  std::string result;
  for (unsigned n = 0; n < size; ++n) {
    char hex[3];
    std::snprintf(hex, sizeof(hex), "%02x", bytes[n]);
    result += hex;
  }
  return result;
}
std::string field(const std::string &text, const std::string &key) {
  const auto at = text.find("\r\n" + key + ": ");
  if (at == std::string::npos)
    return "";
  const auto start = at + key.size() + 4;
  return text.substr(start, text.find("\r\n", start) - start);
}
uint32_t random_value() {
  static uint32_t n = 123;
  return ++n;
}
struct Peer {
  bool tcp;
  int client{-1}, remote{-1};
  uint16_t port{0};
  explicit Peer(bool stream, bool incoming = false) : tcp(stream) {
    int listener = socket(AF_INET, tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
    assert(listener >= 0);
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(listener, reinterpret_cast<sockaddr *>(&dst), sizeof(dst)) ==
           0);
    socklen_t size = sizeof(dst);
    assert(getsockname(listener, reinterpret_cast<sockaddr *>(&dst), &size) ==
           0);
    port = ntohs(dst.sin_port);
    if (tcp && incoming) {
      assert(listen(listener, 1) == 0);
      remote = socket(AF_INET, SOCK_STREAM, 0);
      assert(connect(remote, reinterpret_cast<sockaddr *>(&dst), sizeof(dst)) ==
             0);
      client = accept(listener, nullptr, nullptr);
      close(listener);
      assert(client >= 0 && remote >= 0);
      size = sizeof(dst);
      assert(getsockname(remote, reinterpret_cast<sockaddr *>(&dst), &size) ==
             0);
      port = ntohs(dst.sin_port);
      timeval timeout{1, 0};
      setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      setsockopt(remote, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      return;
    }
    if (tcp)
      assert(listen(listener, 1) == 0);
    client = socket(AF_INET, tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
    assert(client >= 0);
    assert(connect(client, reinterpret_cast<sockaddr *>(&dst), sizeof(dst)) ==
           0);
    remote = tcp ? accept(listener, nullptr, nullptr) : listener;
    if (tcp)
      close(listener);
    assert(remote >= 0);
    timeval timeout{1, 0};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(remote, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  }
  ~Peer() {
    close(client);
    close(remote);
  }
  std::string receive() {
    char buf[8192];
    int n = recv(remote, buf, sizeof(buf), 0);
    assert(n > 0);
    return {buf, size_t(n)};
  }
  std::string deliver(const std::string &text) {
    sockaddr_in dst{};
    socklen_t len = sizeof(dst);
    assert(getsockname(client, reinterpret_cast<sockaddr *>(&dst), &len) == 0);
    const auto n = tcp ? send(remote, text.data(), text.size(), 0)
                       : sendto(remote, text.data(), text.size(), 0,
                                reinterpret_cast<sockaddr *>(&dst), len);
    assert(n == ssize_t(text.size()));
    char buf[8192];
    int got = recv(client, buf, sizeof(buf), 0);
    assert(got > 0);
    return {buf, size_t(got)};
  }
  bool quiet() {
    char c;
    return recv(remote, &c, 1, MSG_DONTWAIT) < 0 &&
           (errno == EAGAIN || errno == EWOULDBLOCK);
  }
};
std::string response(const std::string &request, unsigned status,
                     const std::string &extra = "",
                     const std::string &remote_tag = "server") {
  std::string r = "SIP/2.0 " + std::to_string(status) + " Test\r\n";
  for (const char *name : {"Via", "From", "To", "Call-ID", "CSeq"}) {
    auto value = field(request, name);
    if (std::string(name) == "To" && value.find(";tag=") == std::string::npos)
      value += ";tag=" + remote_tag;
    r += std::string(name) + ": " + value + "\r\n";
  }
  return r + extra + "Content-Length: 0\r\n\r\n";
}
std::string
notify(const std::string &request, unsigned cseq = 1,
       const std::string &body =
           "Messages-Waiting: yes\r\nVoice-Message: 2/4 (1/2)\r\n",
       const std::string &substate = "active;expires=60",
       const std::string &type = "application/simple-message-summary",
       const std::string &server_tag = "server",
       const std::string &extra = "") {
  return "NOTIFY " +
         field(request, "Contact")
             .substr(1, field(request, "Contact").size() - 2) +
         " SIP/2.0\r\nVia: SIP/2.0/UDP 127.0.0.1;branch=z9hG4bKnotify" +
         std::to_string(cseq) +
         ";rport\r\nFrom: <sip:mailbox@example.test>;tag=" + server_tag +
         "\r\nTo: " + field(request, "From") +
         "\r\nCall-ID: " + field(request, "Call-ID") +
         "\r\nCSeq: " + std::to_string(cseq) +
         " NOTIFY\r\nEvent: message-summary\r\nSubscription-State: " +
         substate + "\r\nContact: <sip:mailbox@127.0.0.1>\r\n" + extra +
         "Content-Type: " + type +
         "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" +
         body;
}
struct Harness {
  Peer first, second;
  SipMwi client;
  SipMwi::Hooks hooks;
  std::vector<MessageWaitingState> updates;
  unsigned replies = 0;
  Peer *reply_peer{nullptr};
  SipAccountConfig config;
  explicit Harness(bool tcp) : first(tcp), second(tcp) {
    hooks.context = this;
    hooks.local_port = 5060;
    hooks.random = random_value;
    hooks.hash = hash_value;
    hooks.local_ip = [](void *, uint32_t) { return std::string("127.0.0.1"); };
    hooks.send = [](void *ctx, size_t index, std::string_view msg,
                    uint32_t ip, uint16_t port, bool stream) {
      auto &h = *static_cast<Harness *>(ctx);
      auto &p = index == 0 ? h.first : h.second;
      assert(ip == INADDR_LOOPBACK && port == p.port && stream == p.tcp);
      return send(p.client, msg.data(), msg.size(), 0) == ssize_t(msg.size());
    };
    hooks.reply = [](void *ctx, const std::string &msg, uint32_t, uint16_t port,
                     bool, int) {
      auto &h = *static_cast<Harness *>(ctx);
      auto &p = h.reply_peer != nullptr
                    ? *h.reply_peer
                    : (port == h.second.port ? h.second : h.first);
      h.replies++;
      return send(p.client, msg.data(), msg.size(), 0) == ssize_t(msg.size());
    };
#ifdef USE_ESPHOME_VOIP_MWI
    hooks.notify = [](void *ctx, const MessageWaitingState &s) {
      static_cast<Harness *>(ctx)->updates.push_back(s);
    };
#endif
    client.set_hooks(hooks);
    config.id = "desk";
    config.registrar = "127.0.0.1";
    config.domain = "example.test";
    config.username = "alice";
    config.password = "test-password";
    config.tcp = tcp;
    config.port = first.port;
  }
  std::string start(uint32_t now = 0, bool boot_enabled = true) {
    config.enabled = boot_enabled;
    client.update_account(0, config, INADDR_LOOPBACK, first.port,
                          "sip:alice@127.0.0.1:5060", true, true, now);
    client.tick(now);
    return first.receive();
  }
  void answer(const std::string &r, uint32_t now = 1) {
    assert(client.handle_response(first.deliver(r), INADDR_LOOPBACK, first.port,
                                  first.tcp, 0, now));
  }
  std::string notification(const std::string &n, uint32_t now = 2,
                           uint32_t ip = INADDR_LOOPBACK, uint16_t port = 0,
                           int flow = 0) {
    assert(client.handle_notify(first.deliver(n), ip, port ? port : first.port,
                                first.tcp, flow, now));
    return first.receive();
  }
  std::string active() {
    auto sub = start();
    answer(response(sub, 200, "Expires: 60\r\n"));
    assert(notification(notify(sub)).rfind("SIP/2.0 200", 0) == 0);
    return sub;
  }
};
std::string xml(unsigned version,const char *state,const char *content) {
 return "<dialog-info xmlns='urn:ietf:params:xml:ns:dialog-info' version='"+
  std::to_string(version)+"' state='"+state+"' entity='sip:101@example.test'>"+content+"</dialog-info>";
}
std::string dialog_notify(const std::string &request,unsigned seq,const std::string &body,
                          const std::string &state="active;expires=60") {
 auto wire=notify(request,seq,body,state);
 auto at=wire.find("Event: message-summary");assert(at!=std::string::npos);
 wire.replace(at,22,"Event: dialog");
 at=wire.find("application/simple-message-summary");assert(at!=std::string::npos);
 wire.replace(at,34,"application/dialog-info+xml");
 return wire;
}
std::vector<BlfStateInfo> blf_updates;

int main(int argc,char**argv){
 assert(argc==2);const std::string mode=argv[1];Harness h(false);
 SipRegistration owner;assert(owner.set_account(h.config,0));auto&account=owner.account(0);
 account.bound=true;account.target_ip=INADDR_LOOPBACK;account.target_port=h.first.port;account.contact="sip:alice@127.0.0.1:5060";
 assert(h.client.configure_blf(1));assert(h.client.set_blf_watch(0,0,account,"target","sip:101@example.test",0));h.client.tick(0);
 const auto first=h.first.receive();h.answer(response(first,200,"Expires: 60\r\n"),1);
 const auto initial=xml(0,"full",mode=="version"?"<dialog id='call'><state>confirmed</state></dialog>":"");
 assert(h.notification(dialog_notify(first,1,initial),2).rfind("SIP/2.0 200",0)==0);
 if(mode=="version"){
  assert(h.notification(dialog_notify(first,2,xml(2,"partial","")),3).rfind("SIP/2.0 200",0)==0);
  assert(!h.client.blf_state(0)->available);h.client.tick(4);const auto resync=h.first.receive();
  assert(field(resync,"Call-ID")==field(first,"Call-ID"));h.answer(response(resync,200,"Expires: 60\r\n"),5);
  assert(h.notification(dialog_notify(resync,3,xml(1,"full","")),6).rfind("SIP/2.0 200",0)==0);
  const auto&s=*h.client.blf_state(0);std::cerr<<"old-full-after-gap available="<<s.available<<" state="<<unsigned(s.state)<<" version="<<s.version<<"\n";
  assert(!s.available&&s.state==BlfState::UNKNOWN&&"same-dialog old full must not regress after gap");
 }else if(mode=="reply-failure"){
  const auto saved=h.hooks;
  h.hooks.reply=[](void*,const std::string&,uint32_t,uint16_t,bool,int){return false;};
  h.client.set_hooks(h.hooks);
  const auto rejected=dialog_notify(first,2,xml(1,"full","<dialog id='rejected'><state>confirmed</state></dialog>"));
  assert(h.client.handle_notify(h.first.deliver(rejected),INADDR_LOOPBACK,h.first.port,false,0,3));
  assert(h.first.quiet()&&h.client.blf_state(0)->available&&h.client.blf_state(0)->state==BlfState::IDLE&&h.client.blf_state(0)->version==0);
  h.client.set_hooks(saved);
  assert(h.notification(dialog_notify(first,3,xml(1,"partial","")),4).rfind("SIP/2.0 200",0)==0);
  assert(h.client.blf_state(0)->available&&h.client.blf_state(0)->state==BlfState::IDLE&&h.client.blf_state(0)->active_dialogs==0);
 }else{
  const auto rejected=dialog_notify(first,2,xml(1,"full","<dialog id='rejected'><state>confirmed</state></dialog>"),"active;expires=bad");
  assert(h.notification(rejected,3).rfind("SIP/2.0 400",0)==0);
  assert(h.client.blf_state(0)->available&&h.client.blf_state(0)->state==BlfState::IDLE);
  assert(h.notification(dialog_notify(first,3,xml(2,"partial","")),4).rfind("SIP/2.0 200",0)==0);
  const auto&s=*h.client.blf_state(0);std::cerr<<"rejected-body-following-partial available="<<s.available<<" state="<<unsigned(s.state)<<" version="<<s.version<<"\n";
  assert(!s.available&&s.state==BlfState::UNKNOWN&&s.active_dialogs==0&&"400 body must never enter accepted dialog table");
  h.client.tick(5);const auto resync=h.first.receive();h.answer(response(resync,200,"Expires: 60\r\n"),6);
  assert(h.notification(dialog_notify(resync,4,xml(3,"full","")),7).rfind("SIP/2.0 200",0)==0);
  assert(h.client.blf_state(0)->available&&h.client.blf_state(0)->state==BlfState::IDLE);
 }
}
