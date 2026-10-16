"""Run production bounded TCP flow helpers on real localhost sockets."""

from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def block(source, marker):
    start = source.index(marker)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def test_tcp_connect_retry_and_stream_transaction_ownership(tmp_path):
    base = ROOT / "esphome/components/voip_stack"
    source = (base / "sip_transport.cpp").read_text()
    header = (base / "sip_transport.h").read_text()
    cpp = tmp_path / "tcp.cpp"
    prelude = r'''
#include <atomic>
#include <array>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cassert>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <string>
#include <utility>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#define USE_ESPHOME_VOIP_STACK_VIDEO
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(tag,...) do{(void)tag;if(false)std::printf(__VA_ARGS__);}while(false)
constexpr const char*TAG="test";
const char*socket_errno_name(int){return "error";}const char*socket_errno_text(int){return "error";}
void inet_ntoa_r(in_addr a,char*p,size_t n){inet_ntop(AF_INET,&a,p,n);}
constexpr size_t MAX_SIP_BODY_BYTES=4096,MAX_SIP_TCP_RX_BUFFER=8192;
std::string trim_copy(const std::string&s){auto a=s.find_first_not_of(" \t");return a==std::string::npos?"":s.substr(a,s.find_last_not_of(" \t")-a+1);}
uint32_t millis(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
struct Mutex{};struct LockGuard{explicit LockGuard(Mutex&) {}};
struct SipSignalingString:std::string{using std::string::string;using std::string::operator=;std::string str()const{return *this;}};
template<class T>void reset_string(T&s){s.clear();}
struct Transaction{bool owned=true,udp=false;bool empty()const{return !owned;}void clear(){owned=false;}};
enum class SipSignalType{PROTOCOL_ERROR};
struct SipSignal{SipSignalType type;uint16_t status_code=0;};
'''
    prelude += block((base / "sip_message.cpp").read_text(), "bool sip_content_length(") + "\n"
    prelude += block(header, "struct SipTcpFlow {") + ";\n"
    prelude += r'''
struct ReferState{SipTcpFlow tcp_flow_;std::string call_id="replacement";bool tcp=true;};
class SipTransport {
 public:
 using SipTcpFlow=::SipTcpFlow;
 std::array<char,1024> sip_rx_scratch_{};
 std::vector<std::string> received;
 bool swap_on_receive=false;
 void handle_sip_stream_(int,const sockaddr_in&);
 void handle_sip_datagram_(const char*data,size_t n,const sockaddr_in&){
  received.emplace_back(data,n);
  if(swap_on_receive&&received.size()==1)tcp_flow_.swap(refer_dialog_->tcp_flow_);
 }
 Mutex dialog_mutex_,tcp_tx_pending_mutex_,tcp_send_mutex_;
 SipTcpFlow tcp_flow_;std::unique_ptr<ReferState> refer_dialog_;
 std::string call_id_;bool refer_context_active_=false;
 std::atomic<bool> remote_sip_tcp_{true},outgoing_invite_pending_{false},media_active_{false};
 Transaction completed_invite_,completed_control_,completed_invite_client_,completed_video_direction_invite_;
 bool terminate_after_invite_ack_=true;unsigned disconnected=0,candidate_failures=0;
 class ReferScope{
  SipTransport&t;
  void swap(){t.tcp_flow_.swap(t.refer_dialog_->tcp_flow_);t.call_id_.swap(t.refer_dialog_->call_id);t.refer_dialog_->tcp=t.remote_sip_tcp_.exchange(t.refer_dialog_->tcp);}
 public:
  explicit ReferScope(SipTransport&x):t(x){swap();t.refer_context_active_=true;}
  ~ReferScope(){swap();t.refer_context_active_=false;}
 };
 bool terminal_transaction_pending_locked_()const{return terminate_after_invite_ack_;}
 void reset_dialog_(){call_id_.clear();if(!refer_context_active_)media_active_=false;outgoing_invite_pending_=false;terminate_after_invite_ack_=false;}
 void emit_connection_change_(bool value){assert(!value);++disconnected;}
 void emit_sip_signal_(SipSignal s){assert(refer_context_active_&&s.status_code==503);++candidate_failures;}
 bool end_refer_invite_usage_(bool){return false;}
 void pump_refer_(){}
 void wake_sip_task_(){}
 bool send_sip_tcp_record_(const std::string&m,int fd){return send(fd,m.data(),m.size(),MSG_NOSIGNAL)==static_cast<ssize_t>(m.size());}
 void request_tcp_client_close_();void close_tcp_client_from_sip_task_();
 bool close_tcp_dialog_();void close_tcp_flow_(SipTcpFlow&);
 void handle_tcp_peer_loss_(SipTcpFlow* = nullptr);
 SipTcpFlow*tcp_flow_for_socket_(int);
 void release_refer_dialog_();
 void fail_tcp_flow_(SipTcpFlow&,int);void promote_tcp_flow_(SipTcpFlow&);void prepare_tcp_flow_(SipTcpFlow&);
};
'''
    for marker in (
        "void SipTransport::handle_sip_stream_(",
        "void SipTransport::request_tcp_client_close_()",
        "void SipTransport::close_tcp_flow_(",
        "void SipTransport::close_tcp_client_from_sip_task_()",
        "bool SipTransport::close_tcp_dialog_()",
        "void SipTransport::handle_tcp_peer_loss_(",
        "SipTransport::SipTcpFlow *SipTransport::tcp_flow_for_socket_(",
        "void SipTransport::release_refer_dialog_()",
        "void SipTransport::fail_tcp_flow_(",
        "void SipTransport::promote_tcp_flow_(",
        "void SipTransport::prepare_tcp_flow_(",
    ):
        prelude += block(source, marker) + "\n"
    cpp.write_text(prelude + r'''
int listener(uint16_t &port,bool accept_connections=true){
 int fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);
 sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 assert(bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0);socklen_t n=sizeof(a);
 assert(getsockname(fd,reinterpret_cast<sockaddr*>(&a),&n)==0);port=ntohs(a.sin_port);
 if(accept_connections)assert(listen(fd,4)==0);return fd;
}
void connect_flow(SipTransport&t,SipTcpFlow&f,uint16_t port,const char*body){
 f.target_ip=INADDR_LOOPBACK;f.target_port=port;f.pending=body;f.connect_requested=true;t.prepare_tcp_flow_(f);
 if(f.connecting_socket<0)return;
 fd_set writable;FD_ZERO(&writable);FD_SET(f.connecting_socket,&writable);timeval timeout{1,0};
 assert(select(f.connecting_socket+1,nullptr,&writable,nullptr,&timeout)==1);
 int err=0;socklen_t n=sizeof(err);assert(getsockopt(f.connecting_socket,SOL_SOCKET,SO_ERROR,&err,&n)==0);
 if(err)t.fail_tcp_flow_(f,err);else t.promote_tcp_flow_(f);
}
void receive(int fd,const char*expected){char bytes[64];auto n=recv(fd,bytes,sizeof(bytes),0);assert(n>0&&std::string(bytes,n)==expected);}
int main(){
 uint16_t port;int server=listener(port);
 SipTransport t;t.call_id_="original";t.media_active_=true;
 connect_flow(t,t.tcp_flow_,port,"PRIMARY");int first=accept(server,nullptr,nullptr);assert(first>=0);receive(first,"PRIMARY");
 int original=t.tcp_flow_.socket;assert(original>=0&&t.tcp_flow_.pending.empty());
 t.refer_dialog_=std::make_unique<ReferState>();
 connect_flow(t,t.refer_dialog_->tcp_flow_,port,"REPLACEMENT");int second=accept(server,nullptr,nullptr);assert(second>=0);receive(second,"REPLACEMENT");
 int replacement=t.refer_dialog_->tcp_flow_.socket;
 assert(replacement>=0&&replacement!=original&&t.tcp_flow_.socket==original);
 assert(t.tcp_flow_for_socket_(original)==&t.tcp_flow_);
 assert(t.tcp_flow_for_socket_(replacement)==&t.refer_dialog_->tcp_flow_);
 t.handle_tcp_peer_loss_(&t.refer_dialog_->tcp_flow_);
 assert(t.call_id_=="original"&&t.media_active_&&t.tcp_flow_.socket==original&&t.disconnected==0&&t.candidate_failures==1);
 assert(fcntl(replacement,F_GETFD)==-1&&errno==EBADF);close(second);t.release_refer_dialog_();
 // A refusal on the new flow must not close the original connection either.
 uint16_t refused_port;int refused=listener(refused_port,false);
 t.refer_dialog_=std::make_unique<ReferState>();connect_flow(t,t.refer_dialog_->tcp_flow_,refused_port,"NEVER SENT");
 assert(t.candidate_failures==2&&t.disconnected==0&&t.call_id_=="original"&&t.tcp_flow_.socket==original);
 assert(t.refer_dialog_->tcp_flow_.pending.empty()&&t.refer_dialog_->tcp_flow_.connecting_socket==-1);
 close(refused);t.release_refer_dialog_();
 // Transport close must not erase UDP transaction caches.
 for(bool udp:{false,true}){
  t.completed_invite_.owned=t.completed_control_.owned=t.completed_invite_client_.owned=t.completed_video_direction_invite_.owned=true;
  t.completed_invite_.udp=t.completed_control_.udp=t.completed_invite_client_.udp=t.completed_video_direction_invite_.udp=udp;
  t.remote_sip_tcp_=true;t.call_id_="active";t.media_active_=true;
  t.handle_tcp_peer_loss_();
  assert(t.call_id_.empty()&&!t.media_active_&&!t.remote_sip_tcp_&&!t.terminate_after_invite_ack_);
  assert(t.completed_invite_.owned==udp&&t.completed_control_.owned==udp&&t.completed_invite_client_.owned==udp&&t.completed_video_direction_invite_.owned==udp);
  auto count=t.disconnected;t.handle_tcp_peer_loss_();assert(t.disconnected==count);
 }
 assert(fcntl(original,F_GETFD)==-1&&errno==EBADF);close(first);
 // Retry owns a new flow and consumes only its own pending record.
 t.remote_sip_tcp_=true;t.call_id_="retry";connect_flow(t,t.tcp_flow_,port,"RETRY");
 int retry=accept(server,nullptr,nullptr);assert(retry>=0);receive(retry,"RETRY");
 assert(t.call_id_=="retry"&&t.tcp_flow_.pending.empty());
 t.request_tcp_client_close_();t.prepare_tcp_flow_(t.tcp_flow_);assert(t.tcp_flow_.socket==-1);

 close(retry);
 // Complete records preceding FIN survive closure, including an owner swap.
 for(bool swap_owner:{false,true}){
  SipTransport framed;framed.call_id_="framed";framed.swap_on_receive=swap_owner;
  if(swap_owner)framed.refer_dialog_=std::make_unique<ReferState>();
  connect_flow(framed,framed.tcp_flow_,port,"FRAME");
  int peer=accept(server,nullptr,nullptr);assert(peer>=0);receive(peer,"FRAME");
  std::string one="SIP/2.0 200 OK\r\nContent-Length: 0\r\n\r\n";
  std::string two="NOTIFY sip:a@localhost SIP/2.0\r\nContent-Length: 4\r\n\r\ndone";
  std::string bytes=one+two+"incomplete";
  assert(send(peer,bytes.data(),bytes.size(),0)==static_cast<ssize_t>(bytes.size()));
  assert(shutdown(peer,SHUT_WR)==0);
  int fd=framed.tcp_flow_.socket;
  // Blocking receive makes the buffered-data-plus-EOF witness deterministic.
  assert(fcntl(fd,F_SETFL,fcntl(fd,F_GETFL)&~O_NONBLOCK)==0);
  sockaddr_in source{};source.sin_family=AF_INET;source.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  framed.handle_sip_stream_(fd,source);
  assert(framed.received==std::vector<std::string>({one,two}));
  assert(framed.tcp_flow_for_socket_(fd)==nullptr);
  close(peer);
 }
 close(server);
 // Losing an idle TCP stream cannot terminate an active UDP call.
 t.remote_sip_tcp_=false;t.call_id_="udp";t.media_active_=true;auto count=t.disconnected;
 t.handle_tcp_peer_loss_();assert(t.call_id_=="udp"&&t.media_active_&&t.disconnected==count);
}
''')
    exe = tmp_path / "tcp"
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-I", str(ROOT), str(cpp), "-o", str(exe)], check=True, capture_output=True, text=True)
    subprocess.run([str(exe)], check=True, capture_output=True, text=True, timeout=5)
