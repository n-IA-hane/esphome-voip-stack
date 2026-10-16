#include <netinet/tcp.h>
// Initial confirmed dialog and media admission are fixtures. All SIP requests,
// responses, dispatch, REFER lifetime and replay below use production methods.
struct Peer {
 bool tcp;int fd=-1,esp=-1;sockaddr_in address{};
 explicit Peer(bool stream,uint32_t ip=INADDR_LOOPBACK):tcp(stream){
  int listener=socket(AF_INET,stream?SOCK_STREAM:SOCK_DGRAM,0);assert(listener>=0);
  address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(ip);
  assert(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
  socklen_t n=sizeof(address);assert(getsockname(listener,reinterpret_cast<sockaddr*>(&address),&n)==0);
  if(stream){assert(listen(listener,1)==0);esp=socket(AF_INET,SOCK_STREAM,0);assert(esp>=0);
   assert(connect(esp,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
   fd=accept(listener,nullptr,nullptr);assert(fd>=0);close(listener);
   int yes=1;assert(setsockopt(esp,IPPROTO_TCP,TCP_NODELAY,&yes,sizeof(yes))==0);assert(setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&yes,sizeof(yes))==0);
   assert(fcntl(esp,F_SETFL,O_NONBLOCK)==0);
  }else fd=listener;
 }
 ~Peer(){if(fd>=0)close(fd);}
 std::string uri()const{return "sip:peer@127.0.0.1:"+std::to_string(ntohs(address.sin_port))+";transport="+(tcp?"tcp":"udp");}
 void send_to(SipTransport&s,const std::string&m){
  if(tcp){assert(send(fd,m.data(),m.size(),MSG_NOSIGNAL)==static_cast<ssize_t>(m.size()));
   pollfd p{esp,POLLIN,0};assert(poll(&p,1,1000)==1);s.handle_sip_stream_(esp,address);
  }else{
   sockaddr_in dst{};socklen_t n=sizeof(dst);assert(getsockname(s.sip_socket_,reinterpret_cast<sockaddr*>(&dst),&n)==0);
   assert(sendto(fd,m.data(),m.size(),0,reinterpret_cast<sockaddr*>(&dst),sizeof(dst))==static_cast<ssize_t>(m.size()));
   char buffer[8192];sockaddr_in src{};n=sizeof(src);int count=recvfrom(s.sip_socket_,buffer,sizeof(buffer)-1,0,reinterpret_cast<sockaddr*>(&src),&n);assert(count>0);
   s.handle_sip_udp_datagram_(buffer,count,src);
  }
 }
 std::vector<std::string> drain(){
  std::string stream;char b[8192];pollfd p{fd,POLLIN,0};
  while(poll(&p,1,10)>0 && (p.revents&POLLIN)){int n=recv(fd,b,sizeof(b),MSG_DONTWAIT);if(n<=0)break;stream.append(b,n);}
  std::vector<std::string> out;
  while(!stream.empty()){auto sep=stream.find("\r\n\r\n");assert(sep!=std::string::npos);size_t len=0;assert(sip_content_length(stream,&len));len+=sep+4;assert(stream.size()>=len);out.push_back(stream.substr(0,len));stream.erase(0,len);}
  return out;
 }
};
void setup(SipTransport&s,Peer&original){
 if(s.sip_socket_<0){s.sip_socket_=socket(AF_INET,SOCK_DGRAM,0);assert(s.sip_socket_>=0);sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);assert(bind(s.sip_socket_,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0);socklen_t n=sizeof(a);getsockname(s.sip_socket_,reinterpret_cast<sockaddr*>(&a),&n);s.sip_port_=ntohs(a.sin_port);}
 s.call_id_="original";s.local_tag_="local";s.remote_tag_="remote";s.cseq_=1;s.remote_dialog_cseq_=1;
 s.local_uri_="<sip:esp@127.0.0.1:"+std::to_string(s.sip_port_)+">";s.remote_uri_="<"+original.uri()+">";
 s.remote_target_uri_=original.uri();s.remote_ip_v4_=INADDR_LOOPBACK;s.remote_sip_port_=ntohs(original.address.sin_port);
 s.remote_sip_tcp_=original.tcp;s.tcp_flow_.socket=original.esp;s.media_active_=true;
}
std::string request(Peer&p,const std::string&method,unsigned seq,const std::string&extra="",const std::string&id="original",const std::string&local="local",const std::string&remote="remote"){
 return method+" sip:esp@127.0.0.1 SIP/2.0\r\nVia: SIP/2.0/"+(p.tcp?"TCP":"UDP")+" 127.0.0.1:"+std::to_string(ntohs(p.address.sin_port))+";branch=z9hG4bK"+method+std::to_string(seq)+"\r\nFrom: <"+p.uri()+">;tag="+remote+"\r\nTo: <sip:esp@127.0.0.1>;tag="+local+"\r\nCall-ID: "+id+"\r\nCSeq: "+std::to_string(seq)+" "+method+"\r\n"+extra+"Content-Length: 0\r\n\r\n";
}
std::string response(const std::string&req,unsigned status){
 std::string to=header_value(req,"To");if(tag_from_header(to).empty())to+=";tag=destination";
 std::string out="SIP/2.0 "+std::to_string(status)+" Test\r\n";
 for(const auto&key:{"Via","From","Call-ID","CSeq"})out+=std::string(key)+": "+header_value(req,key)+"\r\n";
 out+="To: "+to+"\r\n";
 if(status==401)out+="WWW-Authenticate: Digest realm=\"test\", nonce=\"one\", algorithm=MD5\r\n";
 if(status==407)out+="Proxy-Authenticate: Digest realm=\"test\", nonce=\"one\", algorithm=MD5\r\n";
 return out+"Content-Length: 0\r\n\r\n";
}
std::string begin(SipTransport&s,Peer&o,Peer&dest,unsigned sequence=2,bool datagram_body=false){
 auto req=request(o,"REFER",sequence,"Refer-To: <"+dest.uri()+">\r\n");
 if(datagram_body){req.replace(req.find("Content-Length: 0\r\n"),19,"Content-Type: text/plain\r\n");req+="note";}
 o.send_to(s,req);
 auto replies=o.drain();assert(replies.size()==2);assert(replies[0].rfind("SIP/2.0 200",0)==0);assert(message_body(replies[1])=="SIP/2.0 100 Trying\r\n");o.send_to(s,response(replies[1],200));
 assert(s.refer_dialog_);
 if(dest.tcp){auto&flow=s.refer_dialog_->tcp_flow_;flow.socket=dest.esp;flow.connect_requested=false;
  // Deliver the one pending connect record as the socket completion does.
  assert(s.send_sip_tcp_record_(flow.pending.data(),flow.pending.size(),dest.esp));flow.pending.clear();}
 auto sent=dest.drain();assert(sent.size()==1&&sent[0].rfind("INVITE ",0)==0);return sent[0];
}
void cleanup(SipTransport&s){
 int current=s.tcp_flow_.socket, candidate=s.refer_dialog_?s.refer_dialog_->tcp_flow_.socket.load():-1;
 if(s.refer_dialog_)s.release_refer_dialog_();s.close_tcp_flow_(s.tcp_flow_);close(s.sip_socket_);
 assert(!s.refer_dialog_&&s.tcp_flow_.socket==-1&&s.tcp_flow_.connecting_socket==-1&&s.tcp_flow_.pending.empty());
 for(int fd:{current,candidate})if(fd>=0)assert(fcntl(fd,F_GETFD)==-1&&errno==EBADF);
}
void auth_case(unsigned status,bool tcp,bool ended,bool original_tcp){
 SipTransport s;Peer original(original_tcp),dest(tcp);setup(s,original);auto invite=begin(s,original,dest);
 if(ended){original.send_to(s,request(original,"BYE",3));auto bye=original.drain();assert(bye.size()==1&&bye[0].rfind("SIP/2.0 200",0)==0);assert(s.refer_dialog_->owner_invite_ended);}
 auto bad=response(invite,status);auto branch=via_branch(header_value(bad,"Via"));bad.replace(bad.find(branch),branch.size(),"z9hG4bKwrong");dest.send_to(s,bad);assert(original.drain().empty());assert(s.refer_dialog_&&!s.refer_retiring_);
 dest.send_to(s,response(invite,status));auto ack=dest.drain();assert(ack.size()==1&&ack[0].rfind("ACK ",0)==0);
 assert(via_branch(header_value(ack[0],"Via"))==via_branch(header_value(invite,"Via")));assert(cseq_number(header_value(ack[0],"CSeq"))==cseq_number(header_value(invite,"CSeq")));
 auto terminal=original.drain();
 assert(terminal.size()==1 && "challenge must finish REFER immediately");
 assert(message_body(terminal[0])=="SIP/2.0 "+std::to_string(status)+" Transfer Failed\r\n");
 assert(header_value(terminal[0],"Subscription-State").find("terminated")==0);
 assert(header_value(terminal[0],"Event")=="refer;id=2");
 original.send_to(s,response(terminal[0],200));s.pump_refer_();assert(!s.refer_dialog_);
 assert(s.call_id_==(ended?"":"original"));assert(s.media_active_==!ended);
 assert(s.delivered.size()==(ended?1u:0u));if(ended)assert(s.delivered[0].type==SipSignalType::BYE);
 s.pump_refer_();assert(s.delivered.size()==(ended?1u:0u));assert(original.drain().empty());
 if(tcp)assert(fcntl(dest.esp,F_GETFD)==-1&&errno==EBADF);
 else {dest.send_to(s,response(invite,status));auto replay=dest.drain();assert(replay.size()==1&&replay[0]==ack[0]);assert(original.drain().empty());assert(s.delivered.size()==(ended?1u:0u));}
 if(ended)setup(s,original);
 Peer retry(tcp);begin(s,original,retry,4);assert(s.refer_dialog_);cleanup(s);
}
void replay_case(bool old_tcp,bool new_tcp){
 SipTransport s;Peer original(old_tcp),dest(new_tcp);setup(s,original);auto refer=request(original,"REFER",2,"Refer-To: <"+dest.uri()+">\r\n");if(!old_tcp){refer.replace(refer.find("Content-Length: 0\r\n"),19,"Content-Type: text/plain\r\n");refer+="note";}
 auto invite=begin(s,original,dest,2,!old_tcp);
 auto bye=request(original,"BYE",3);original.send_to(s,bye);auto lost=original.drain();assert(lost.size()==1&&lost[0].rfind("SIP/2.0 200",0)==0); // Deliberately discard this response.
 dest.send_to(s,response(invite,200));auto ack=dest.drain();assert(ack.size()==1&&ack[0].rfind("ACK ",0)==0);
 auto final=original.drain();assert(final.size()==1&&message_body(final[0])=="SIP/2.0 200 OK\r\n");original.send_to(s,response(final[0],200));s.pump_refer_();assert(!s.refer_dialog_);
 const auto id=s.call_id_,tag=s.local_tag_;const auto socket=s.tcp_flow_.socket.load();const auto events=s.delivered.size();
 if(!old_tcp){
  for(auto req:{bye,refer}){original.send_to(s,req);auto replay=original.drain();assert(replay.size()==1 && "retired UDP transaction must replay over UDP");assert(replay[0].rfind("SIP/2.0 200",0)==0);assert(header_value(replay[0],"CSeq")==header_value(req,"CSeq"));assert(dest.drain().empty());}
  std::vector<std::string> invalids;
  auto invalid=bye;invalid.replace(invalid.find("z9hG4bKBYE3"),10,"z9hG4bKbad");invalids.push_back(invalid);
  invalid=bye;invalid.replace(invalid.find("CSeq: 3"),7,"CSeq: 9");invalids.push_back(invalid);
  invalid=bye;invalid.replace(invalid.find("Call-ID: original"),17,"Call-ID: foreign");invalids.push_back(invalid);
  invalid=bye;invalid.replace(invalid.find("Content-Length: 0"),17,"Content-Length: 9");invalids.push_back(invalid);
  for(const auto&bad:invalids){original.send_to(s,bad);auto rejected=original.drain();for(const auto&r:rejected)assert(r.rfind("SIP/2.0 200",0)!=0);assert(dest.drain().empty());}
  Peer stranger(false,0x7f000002);stranger.send_to(s,bye);
  for(const auto&r:stranger.drain())assert(r.rfind("SIP/2.0 200",0)!=0);
  assert(dest.drain().empty());
  assert(s.call_id_==id&&s.local_tag_==tag&&s.remote_sip_tcp_==new_tcp&&s.tcp_flow_.socket==socket&&s.delivered.size()==events);
  clock_ms+=SIP_TRANSACTION_TIMEOUT_MS+1;s.pump_refer_();for(const auto&entry:s.completed_refers_)assert(!entry);
  original.send_to(s,bye);for(const auto&r:original.drain())assert(r.rfind("SIP/2.0 200",0)!=0);
 }
 assert(s.call_id_==id&&s.remote_sip_tcp_==new_tcp&&s.tcp_flow_.socket==socket);
 dest.send_to(s,request(dest,"OPTIONS",7,"",id,tag,"destination"));auto options=dest.drain();assert(options.size()==1&&options[0].rfind("SIP/2.0 200",0)==0);
 dest.send_to(s,request(dest,"BYE",8,"",id,tag,"destination"));auto ended=dest.drain();assert(ended.size()==1&&ended[0].rfind("SIP/2.0 200",0)==0);assert(s.call_id_.empty()&&!s.media_active_);
 assert(s.send_invite_unlocked_("next-call","esp","ESP","target","Target"));
 auto next=dest.drain();assert(next.size()==1&&next[0].rfind("INVITE ",0)==0);
 dest.send_to(s,response(next[0],200));auto next_ack=dest.drain();assert(next_ack.size()==1&&next_ack[0].rfind("ACK ",0)==0);
 assert(s.call_id_=="next-call"&&s.media_active_);
 dest.send_to(s,request(dest,"BYE",2,"",s.call_id_,s.local_tag_,"destination"));
 auto final_bye=dest.drain();assert(final_bye.size()==1&&final_bye[0].rfind("SIP/2.0 200",0)==0);assert(s.call_id_.empty()&&!s.media_active_);
 cleanup(s);
}
int main(int argc,char**argv){assert(argc>=4);if(std::string(argv[1])=="auth")auth_case(std::atoi(argv[2]),std::atoi(argv[3]),std::atoi(argv[4]),std::atoi(argv[5]));else replay_case(std::atoi(argv[2]),std::atoi(argv[3]));}
