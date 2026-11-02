"""Real registered TCP traffic is framed per record, not per kernel backlog."""

from pathlib import Path
import subprocess

import pytest

from test_refer_dialog_runtime import SOURCE, block
from test_sip_registration_transport import registration_transport_source


@pytest.fixture(scope="module")
def tcp_burst_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("tcp-stream-burst")
    defines = folder / "esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    code = "#include <chrono>\n" + registration_transport_source(True)
    code += "\n" + block((SOURCE / "sip_transport.cpp").read_text(), "void SipTransport::accept_tcp_client_(")
    # Observe the existing buffer and receive work, without replacing dispatch.
    original = "struct SipSignalingString:std::string {using std::string::string;using std::string::operator=;std::string str()const{return *this;}};"
    assert original in code
    code = code.replace(original, r'''
size_t rx_peak=0,rx_capacity=0,rx_growths=0,rx_read_bytes=0;
struct SipSignalingString:std::string {
 using std::string::string;using std::string::operator=;
 char &operator[](size_t)=delete;const char &operator[](size_t)const=delete;
 std::string str()const{return *this;}
 std::string &append(const char *p,size_t n){
  const auto before=capacity();std::string::append(p,n);
  rx_peak=std::max(rx_peak,size());rx_capacity=std::max(rx_capacity,capacity());
  if(capacity()!=before)++rx_growths;return *this;
 }
};
''')
    stream = block((SOURCE / "sip_transport.cpp").read_text(), "void SipTransport::handle_sip_stream_(")
    assert stream in code
    code = code.replace(stream, stream.replace("if (n > 0) {", "if (n > 0) { rx_read_bytes += static_cast<size_t>(n);"))
    peers = (Path(__file__).parent / "fixtures/refer_wire_main.cpp").read_text()
    account = (Path(__file__).parent / "fixtures/registration_transport.cpp").read_text()
    admission = (Path(__file__).parent / "test_tcp_control_admission_runtime.py").read_text()
    notify = admission[admission.index("std::string notification("):admission.index("void expect_status(")]
    code += "\n" + peers[:peers.index("std::string begin(")]
    code += "\n" + account[:account.index("std::string account_refer(")] + "\n" + notify
    code += r'''
void write_all(Peer &pbx,const std::string &message){
 size_t sent=0;while(sent<message.size()){
  const auto n=send(pbx.fd,message.data()+sent,message.size()-sent,MSG_NOSIGNAL);
  assert(n>0);sent+=n;
 }
 pollfd p{pbx.esp,POLLIN,0};assert(poll(&p,1,1000)==1);
}
void pump(SipTransport &s,Peer &pbx){
 for(unsigned tick=0;tick<64;++tick){
  if(!s.tcp_flow_for_socket_(pbx.esp))return;
  pollfd p{pbx.esp,POLLIN,0};if(poll(&p,1,0)<=0)return;
  rx_read_bytes=0;const auto begin=std::chrono::steady_clock::now();
  s.handle_sip_stream_(pbx.esp,pbx.address);
  const auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-begin).count();
  fprintf(stderr,"tick_read_bytes=%zu peak_rx=%zu capacity=%zu growths=%zu host_tick_us=%lld\n",
          rx_read_bytes,rx_peak,rx_capacity,rx_growths,static_cast<long long>(us));
  assert(rx_read_bytes<=MAX_SIP_TCP_RX_BUFFER&&"TCP receive work must yield at the existing byte budget");
 }
 assert(false&&"stream failed to yield/drain within bounded ticks");
}
int main(int argc,char **argv){
 assert(argc==2);const std::string scenario=argv[1];SipTransport s;Peer pbx(true);bind_account(s,pbx);
 SipMwi::Hooks hooks;hooks.context=&s;hooks.send=SipTransport::registration_send_view_;
 hooks.reply=SipTransport::registration_mwi_reply_;hooks.random=esp_random;hooks.hash=SipTransport::registration_hash_;
 hooks.local_ip=[](void*,uint32_t){return std::string("127.0.0.1");};hooks.local_port=s.sip_port_;
 s.mwi_.set_hooks(hooks);s.mwi_.update_registered_account(0,s.registration_.account(0),clock_ms);
 s.mwi_.tick(clock_ms);auto outgoing=pbx.drain();assert(outgoing.size()==1);const auto subscribe=outgoing[0];
 auto notify=notification(subscribe,1,false);
 rx_peak=rx_capacity=rx_growths=0;
 if(scenario=="burst" || scenario=="burst-aux"){
  Peer auxiliary(true);Peer &notifier=scenario=="burst-aux"?auxiliary:pbx;
  if(scenario=="burst-aux"){
   s.call_id_="registered-call";s.media_active_=true;s.active_account_=0;
   s.accept_tcp_client_(auxiliary.esp,auxiliary.address);
   assert(s.inbound_tcp_flow_.socket==auxiliary.esp);
  }
  std::string burst;
  for(unsigned sequence=1;sequence<=8;++sequence){
   auto message=notification(subscribe,sequence,false);
   message.insert(message.find("Content-Length:"),"X-Padding: "+std::string(1400,'x')+"\r\n");
   assert(message.size()<MAX_SIP_TCP_RX_BUFFER);burst+=response(subscribe,200)+message;
  }
  assert(burst.size()>2*MAX_SIP_TCP_RX_BUFFER);write_all(notifier,burst);pump(s,notifier);
  const auto replies=notifier.drain();
  assert(replies.size()==8&&"coalesced valid NOTIFY records must each receive 200");
  for(const auto &reply:replies)assert(reply.rfind("SIP/2.0 200",0)==0);
  assert(s.registration_flows_[0].socket==pbx.esp&&s.registration_.is_registered("office"));
  assert(fcntl(pbx.esp,F_GETFD)>=0);
  if(scenario=="burst-aux"){
   assert(s.call_id_=="registered-call"&&s.media_active_);
   assert(s.inbound_tcp_flow_.socket<0&&"drained auxiliary control flow must be released");
  }else{
   char byte;assert(recv(pbx.fd,&byte,1,MSG_PEEK|MSG_DONTWAIT)<0&&(errno==EAGAIN||errno==EWOULDBLOCK));
  }
  assert(s.mwi_.state(0).available&&s.mwi_.state(0).new_messages==2);
  assert(rx_peak<=MAX_SIP_TCP_RX_BUFFER);
 }else if(scenario=="leading-crlf"){
  pbx.send_to(s,response(subscribe,200));
  unsigned replies=0;
  for(const auto &part:std::vector<std::string>{"\r","\n",notify.substr(0,7),notify.substr(7),
                                               "\r\n\r","\n"+notification(subscribe,2,false)}){
   write_all(pbx,part);pump(s,pbx);
   assert(s.registration_flows_[0].socket==pbx.esp&&s.registration_.is_registered("office"));
   for(const auto &reply:pbx.drain()){assert(reply.rfind("SIP/2.0 200",0)==0);++replies;}
  }
  assert(replies==2&&"single/double split CRLF must be ignored before SIP records");
 }else if(scenario=="partial-body-tokens"){
  pbx.send_to(s,response(subscribe,200));
  const auto sep=notify.find("\r\n\r\n");
  const std::string body="Messages-Waiting: yes\r\nVoice-Message: 2/1 (0/0)\r\nX-Note: SIP/2.0 200 NOTIFY INVITE\r\n";
  notify.erase(notify.find("Content-Length:"));notify+="Content-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
  size_t begin=0;
  for(const auto end:{size_t(7),sep+3,notify.size()-5,notify.size()}){
   write_all(pbx,notify.substr(begin,end-begin));pump(s,pbx);begin=end;
   if(end<notify.size()){assert(pbx.drain().empty());assert(s.registration_flows_[0].socket==pbx.esp);}
  }
  const auto replies=pbx.drain();assert(replies.size()==1&&replies[0].rfind("SIP/2.0 200",0)==0);
  assert(s.mwi_.state(0).available);
 }else{
  if(scenario=="oversized-body")notify.replace(notify.find("Content-Length:"),notify.size(),"Content-Length: 4097\r\n\r\n");
  else if(scenario=="ambiguous-length")notify.insert(notify.find("Content-Length:"),"Content-Length: 0\r\n");
  else{assert(scenario=="oversized-header");notify.insert(notify.find("Content-Length:"),"X-Padding: "+std::string(MAX_SIP_TCP_RX_BUFFER,'x')+"\r\n");}
  write_all(pbx,notify);pump(s,pbx);assert(s.registration_flows_[0].socket<0);
  assert(fcntl(pbx.esp,F_GETFD)<0&&errno==EBADF);
 }
 for(auto &flow:s.registration_flows_)s.close_tcp_flow_(flow);
 s.close_tcp_flow_(s.tcp_flow_);s.close_tcp_flow_(s.inbound_tcp_flow_);close(s.sip_socket_);
}
'''
    cpp = folder / "burst.cpp"
    cpp.write_text(code)
    binary = folder / "burst"
    result = subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra", "-g", "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer", "-fno-pie", "-no-pie", "-DUSE_ESPHOME_VOIP_REGISTRATION",
        "-DUSE_ESPHOME_VOIP_MWI", "-I", str(folder), "-I", str(SOURCE.parents[2]),
        "-I", str(SOURCE), str(cpp), str(SOURCE / "sip_registration.cpp"),
        str(SOURCE / "sip_mwi.cpp"), "-lcrypto", "-o", str(binary),
    ], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("scenario", ["burst", "burst-aux", "leading-crlf", "partial-body-tokens", "oversized-body", "ambiguous-length", "oversized-header"])
def test_registered_tcp_stream_is_framed_incrementally(tcp_burst_binary, scenario):
    result = subprocess.run([tcp_burst_binary, scenario], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
