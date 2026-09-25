"""Execute transport-owned connect failure/retry and peer-loss transaction cleanup."""

from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_tcp_connect_retry_and_stream_transaction_ownership(tmp_path):
    source = (ROOT / "esphome/components/voip_stack/sip_transport.cpp").read_text()
    start = source.index(
        "  auto close_connecting =", source.index("void SipTransport::sip_task_")
    )
    end = source.index("  while (this->running_", start)
    connect = source[start:end]
    start = source.index("void SipTransport::handle_tcp_peer_loss_()")
    lost = source[start : source.index("\n}", start) + 2]
    cpp = tmp_path / "tcp.cpp"
    cpp.write_text(
        r"""
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <set>
#include <netinet/in.h>
#include <arpa/inet.h>
#define USE_ESPHOME_VOIP_STACK_VIDEO
#define ESP_LOGI(tag,...) do{(void)tag;if(false)std::printf(__VA_ARGS__);}while(false)
#define ESP_LOGW(tag,...) ESP_LOGI(tag,__VA_ARGS__)
constexpr const char*TAG="test";
const char*socket_errno_name(int){return "error";}const char*socket_errno_text(int){return "error";}
void inet_ntoa_r(in_addr a,char*p,size_t n){inet_ntop(AF_INET,&a,p,n);}
std::set<int> fds;int close(int fd){assert(fds.erase(fd)==1);return 0;}
struct Mutex{};struct LockGuard{explicit LockGuard(Mutex&) {}};
void reset_string(std::string&s){s.clear();}
struct Pending {std::string text;std::string str()const{return text;}void clear(){text.clear();}};
void reset_string(Pending&p){p.clear();}
struct Transaction{bool owned=true,udp=false;bool empty()const{return !owned;}void clear(){owned=false;}};
class SipTransport {
 public:
 Mutex dialog_mutex_,tcp_tx_pending_mutex_,tcp_send_mutex_;
 Pending tcp_tx_pending_;std::string call_id_,sip_tcp_rx_buffer_;
 std::atomic<bool> tcp_connect_requested_{false},sip_tcp_client_close_requested_{false},remote_sip_tcp_{true},outgoing_invite_pending_{false},media_active_{false};
 std::atomic<int> sip_tcp_client_socket_{-1};std::atomic<uint32_t> sip_tcp_client_ip_v4_{0};
 Transaction completed_invite_,completed_control_,completed_invite_client_,completed_video_direction_invite_;
 bool terminate_after_invite_ack_=true;unsigned disconnected=0;std::string sent;
 bool terminal_transaction_pending_locked_()const{return terminate_after_invite_ack_;}
 void reset_dialog_(){call_id_.clear();media_active_=false;outgoing_invite_pending_=false;terminate_after_invite_ack_=false;}
 void emit_connection_change_(bool value){assert(!value);++disconnected;}
 void close_tcp_client_from_sip_task_(){int fd=sip_tcp_client_socket_.exchange(-1);if(fd>=0)close(fd);sip_tcp_rx_buffer_.clear();}
 bool send_sip_tcp_record_(const std::string&m,int fd){assert(fds.count(fd));sent=m;return true;}
 void handle_tcp_peer_loss_();
 void connect_failure_and_retry(){
  int connecting_fd=11;fds.insert(11);uint32_t connect_deadline_ms=2000,connecting_ip_v4=0x7f000001;uint16_t connecting_port=5060;
"""
        + connect
        + r"""
  const auto before=disconnected;
  call_id_="failed";tcp_tx_pending_.text="INVITE OLD";tcp_connect_requested_=true;
  fail_tcp_connect(111);
  assert(connecting_fd==-1&&connect_deadline_ms==0&&connecting_ip_v4==0&&connecting_port==0&&fds.empty());
  assert(call_id_.empty()&&tcp_tx_pending_.str().empty()&&!tcp_connect_requested_&&disconnected==before+1);
  connecting_fd=12;fds.insert(12);connecting_ip_v4=0x7f000001;connecting_port=5060;
  call_id_="next";tcp_tx_pending_.text="INVITE NEXT";tcp_connect_requested_=true;
  promote_tcp_connect();
  assert(connecting_fd==-1&&sip_tcp_client_socket_==12&&fds.size()==1);
  assert(sent=="INVITE NEXT"&&tcp_tx_pending_.str().empty()&&!tcp_connect_requested_);
  assert(call_id_=="next");close_tcp_client_from_sip_task_();assert(fds.empty());
 }
};
"""
        + lost
        + r"""
int main(){
 SipTransport t;t.connect_failure_and_retry();
 for(bool udp_cache:{false,true}){
  SipTransport s;s.call_id_="active";s.media_active_=true;s.sip_tcp_client_socket_=13;fds.insert(13);
  s.completed_invite_.udp=s.completed_control_.udp=s.completed_invite_client_.udp=s.completed_video_direction_invite_.udp=udp_cache;
  s.handle_tcp_peer_loss_();
  assert(fds.empty()&&s.sip_tcp_client_socket_==-1&&s.call_id_.empty()&&!s.media_active_&&!s.remote_sip_tcp_);
  assert(s.disconnected==1&&!s.terminate_after_invite_ack_);
  assert(s.completed_invite_.owned==udp_cache&&s.completed_control_.owned==udp_cache&&s.completed_invite_client_.owned==udp_cache&&s.completed_video_direction_invite_.owned==udp_cache);
  s.handle_tcp_peer_loss_();assert(s.disconnected==1&&fds.empty());
  s.connect_failure_and_retry();
 }
}
"""
    )
    exe = tmp_path / "tcp"
    subprocess.run(
        ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)],
        check=True,
        capture_output=True,
        text=True,
    )
    subprocess.run([str(exe)], check=True, capture_output=True, text=True)
