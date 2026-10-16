"""Exercise production REFER ownership and notifications without an ESP scheduler."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'esphome/components/voip_stack'


def block(source, marker):
    start = source.index(marker)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def refer_fixture_source():
    header = (SOURCE / 'sip_transport.h').read_text()
    parser = (SOURCE / 'sip_message.cpp').read_text()
    refer = (SOURCE / 'sip_refer.cpp').read_text()
    production = refer[refer.index('SipTransport::ReferScope::ReferScope'):refer.index('}  // namespace voip_stack')]
    declarations = '\n'.join(block(header, 'struct ' + name + ' {') + ';' for name in (
        'UdpTransaction', 'CompletedServerTransaction', 'CompletedInviteClientTransaction', 'SipIdentity', 'RemoteMediaShape', 'SipTcpFlow'))
    context = block(header, 'struct ReferDialog {')
    fields = context[context.index('{') + 1:-1]
    fields = fields.replace('bool remote_sip_tcp_{false};', 'std::atomic<bool> remote_sip_tcp_{false};')
    fields = fields.replace('bool outgoing_invite_pending_{false}, cancel_requested_{false};',
                            'std::atomic<bool> outgoing_invite_pending_{false}, cancel_requested_{false};')
    for typ, name in [('uint32_t', 'remote_ip_v4_'), ('uint16_t', 'remote_sip_port_'),
                      ('uint32_t', 'remote_rtp_ip_v4_'), ('uint16_t', 'remote_rtp_port_')]:
        fields = fields.replace(typ + ' ' + name, 'std::atomic<' + typ + '> ' + name)
    code = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include <arpa/inet.h>
#include "esphome/components/voip_stack/sip_types.h"
#include "esphome/components/voip_stack/rtp_audio_payloads.h"
using namespace esphome::voip_stack;
using SipSignalingString=std::string;
static constexpr int kMaxSdpMediaLines=8;
static constexpr uint32_t SIP_TRANSACTION_TIMEOUT_MS=32000;
static uint32_t clock_ms=100;
uint32_t millis(){return clock_ms;}
const char* inet_ntoa_r(in_addr value,char *out,size_t n){return inet_ntop(AF_INET,&value,out,n);}
std::string make_token(const char *prefix){return std::string(prefix)+"new";}
template<class T> struct RAMAllocator {
 T* allocate(size_t n){return static_cast<T*>(std::malloc(n*sizeof(T)));}
 void deallocate(T *p,size_t){std::free(p);}
};
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define TAG "test"
std::string sip_header_token(const std::string &v,size_t=160){return v;}
std::string sip_route_id(const std::string &v,size_t){return v;}
'''
    for signature in ['std::string trim_copy(', 'static uint8_t hex_value(', 'std::string sip_uri_user_decode(',
                      'std::string header_value(', 'std::string header_values(', 'bool parse_decimal_u32(', 'bool time_reached(',
                      'std::string name_addr_uri(', 'std::string strip_angle_uri(', 'bool sip_uri_ipv4_target(', 'std::string sip_user_from_header(',
                      'std::string cseq_method(', 'uint32_t cseq_number(', 'std::string tag_from_header(']:
        code += block(parser, signature) + '\n'
    code += 'std::string sip_display_name_from_header(const std::string&,size_t){return "Room";}\n'
    code += declarations + r'''
struct SipPhoneTransport {
 std::vector<SipSignal> delivered;
 void emit_sip_signal_(SipSignal s){delivered.push_back(std::move(s));}
};
struct SipTransport: SipPhoneTransport {
''' + context + ';\n' + block(header, 'struct CompletedRefer {') + ';\n' + block(header, 'class ReferScope {') + ';\n' + fields + r'''
 struct ReferDeleter {template<class T>void operator()(T*p)const{if(p){p->~T();RAMAllocator<T>().deallocate(p,1);}}};
 std::array<std::unique_ptr<CompletedRefer,ReferDeleter>,4> completed_refers_;
 struct SipRequestOptions {
  uint32_t cseq_number=0;std::string cseq_method,branch_override,extra_headers;
  bool remember_transaction=true,remember_invite_ack=true;
  std::string *formatted_request=nullptr;const char *content_type="application/sdp";
 };
 std::unique_ptr<ReferDialog,ReferDeleter> refer_dialog_;
 bool refer_context_active_=false,refer_retiring_=false,refer_signal_pending_=false;
 SipSignalType refer_signal_type_=SipSignalType::PROTOCOL_ERROR;uint16_t refer_status_=0;
 uint32_t refer_cseq_=0,refer_expires_=0;
 std::string logical_call_id_,logical_wire_id_,refer_wire_id_;
 std::atomic<bool> media_active_{true};
 struct Mutex{};struct LockGuard{explicit LockGuard(Mutex&) {}};
 Mutex tcp_send_mutex_,tcp_tx_pending_mutex_;
 std::atomic<uint32_t> media_proposal_epoch_{0};
 bool fail_invite=false;
 struct Wire {std::string method,id,headers,body;};std::vector<Wire> wire;
 uint16_t response=0;unsigned bye_count=0,cancel_count=0;
 bool handle_refer_(const std::string&,const sockaddr_in&);
 void notify_refer_(uint32_t,uint16_t,const char*);
 bool end_refer_invite_usage_(bool);
 void cancel_refer_();void finish_failed_refer_(uint16_t,const char*);
 void open_media_session_(){if(!refer_context_active_)media_active_=true;}
 void close_media_session_(){if(!refer_context_active_)media_active_=false;}
 void reset_dialog_(){call_id_.clear();outgoing_invite_pending_=false;if(!refer_context_active_)media_active_=false;}
 void emit_sip_signal_(SipSignal);
 bool prepare_refer_media_(ReferDialog&);
 void release_refer_dialog_(){refer_dialog_.reset();}
 void pump_refer_();void swap_dialog_(ReferDialog&);void swap_negotiation_(ReferDialog&);
 bool reject_if_stale_dialog_(const std::string&,const sockaddr_in&,const char*);
 bool send_stateless_response_(const std::string&,const sockaddr_in&,uint16_t s,const char*,const std::string& = "",bool remember=false){
  response=s;if(remember){completed_control_.call_id=call_id_;completed_control_.response="accepted";completed_control_.udp=!remote_sip_tcp_.load();}return true;}
 uint16_t sip_port_=5060;
 enum class SipEvent{NONE};
 static SipEvent sip_event_from_method_(const std::string&){return SipEvent::NONE;}
 void mark_sip_event_(SipEvent){}
 void local_ip_for_peer_(uint32_t,std::string*out){*out="192.0.2.10";}
 bool send_sip_(const std::string&m,uint32_t,uint16_t){const auto pos=m.find("\r\n\r\n");
  wire.push_back({m.substr(0,m.find(' ')),header_value(m,"Call-ID"),m.substr(0,pos),m.substr(pos+4)});return true;}
 void remember_completed_invite_ack_(const std::string&,uint32_t,uint16_t){}
 void remember_udp_transaction_(const std::string&,const std::string&,uint32_t,uint16_t){}
 bool send_request_(const std::string&,const std::string&,const SipRequestOptions&);
 bool send_invite_unlocked_(const std::string&id,const std::string&r,const std::string&n,const std::string&d,const std::string&dn){
  assert(refer_context_active_);call_id_=id;caller_={r,n};destination_={d,dn};outgoing_invite_pending_=true;return !fail_invite;}
 bool send_bye_unlocked_(const std::string&id){assert(refer_context_active_);wire.push_back({"BYE",id,"",""});++bye_count;return true;}
 bool send_cancel_unlocked_(const std::string&){assert(refer_context_active_);++cancel_count;return true;}
 void wake_sip_task_(){} void reset_rtp_latch_(){} void pump_udp_retransmits_unlocked_(){}
 void get_media_config_(AudioFormat*t,AudioFormat*r,uint8_t*tp,uint8_t*rp,RtpAudioPayloads*p=nullptr){
  if(t)*t=selected_tx_format_;if(r)*r=selected_rx_format_;if(tp)*tp=rtp_tx_payload_type_;if(rp)*rp=rtp_rx_payload_type_;if(p)*p=negotiated_rx_payloads_;}
 void set_media_config_(const AudioFormat&t,const AudioFormat&r,uint8_t tp,uint8_t rp,const RtpAudioPayloads*p){
  selected_tx_format_=t;selected_rx_format_=r;rtp_tx_payload_type_=tp;rtp_rx_payload_type_=rp;negotiated_rx_payloads_=*p;}
};
''' + block((SOURCE/'sip_transport.cpp').read_text(), 'bool SipTransport::reject_if_stale_dialog_(') + '\n' + block((SOURCE/'sip_request_response.cpp').read_text(), 'bool SipTransport::send_request_(const std::string &method, const std::string &body,') + '\n' + production
    return code


def test_refer_keeps_one_media_owner_and_reports_actual_outcome(tmp_path):
    code = refer_fixture_source()
    code += r'''
std::string request(const std::string&target="sip:room@192.0.2.20:5060",const std::string&tag="remote"){
 return "REFER sip:room@192.0.2.10 SIP/2.0\r\nCall-ID: original\r\nFrom: <sip:pbx@192.0.2.1>;tag="+tag+
 "\r\nTo: <sip:room@192.0.2.10>;tag=local\r\nCSeq: 2 REFER\r\nRefer-To: <"+target+">\r\nContent-Length: 0\r\n\r\n";
}
void setup(SipTransport&s){s.call_id_="original";s.local_tag_="local";s.remote_tag_="remote";
 s.remote_ip_v4_=0xc0000201;s.remote_sip_port_=5060;s.remote_rtp_ip_v4_=0xc0000201;s.remote_rtp_port_=40000;
 s.local_uri_="<sip:room@192.0.2.10>";s.remote_uri_="<sip:pbx@192.0.2.1>";}
int main(){
 sockaddr_in peer{};peer.sin_addr.s_addr=htonl(0xc0000201);peer.sin_port=htons(5060);
 for(bool original_tcp:{false,true}) for(bool target_tcp:{false,true}) for(bool accepted:{false,true}) {
  SipTransport s;setup(s);s.remote_sip_tcp_=original_tcp;s.tcp_flow_.socket=original_tcp?41:-1;
  const std::string uri="sip:room@192.0.2.20:5060;transport="+std::string(target_tcp?"tcp":"udp")+";opaque=kept";
  assert(s.handle_refer_(request(uri),peer));
  assert(s.call_id_=="original" && s.remote_sip_tcp_==original_tcp);
  assert(s.tcp_flow_.socket==(original_tcp?41:-1));
  assert(s.refer_dialog_->target_uri==uri && s.refer_dialog_->remote_sip_tcp_==target_tcp);
  assert(s.refer_dialog_->tcp_flow_.connect_requested==target_tcp);
  {SipTransport::ReferScope scope(s);
   assert(s.remote_sip_tcp_==target_tcp);s.tcp_flow_.socket=target_tcp?42:-1;
   s.tcp_flow_.connect_requested=false;s.outgoing_invite_pending_=false;
   SipSignal r;r.type=accepted?SipSignalType::STATUS_200_OK:SipSignalType::FINAL_RESPONSE;r.status_code=accepted?200:486;s.emit_sip_signal_(r);
  }
  s.pump_refer_();
  assert(s.remote_sip_tcp_==(accepted?target_tcp:original_tcp));
  assert(s.tcp_flow_.socket==(accepted?(target_tcp?42:-1):(original_tcp?41:-1)));
  assert(s.call_id_==(accepted?"refer-new":"original"));
 }
 for(const auto &uri:{"sips:room@192.0.2.20","sip:room@192.0.2.20;transport=tls","sip:room@192.0.2.20;method=SUBSCRIBE"}) {
  SipTransport s;setup(s);assert(!s.handle_refer_(request(uri),peer));assert(s.response==416 && s.call_id_=="original");
 }
 for(bool accepted:{false,true}) {
  SipTransport s;setup(s);assert(s.handle_refer_(request(),peer));
  assert(s.end_refer_invite_usage_(true));assert(!s.media_active_&&s.call_id_=="original"&&s.delivered.empty());
  {SipTransport::ReferScope scope(s);s.outgoing_invite_pending_=false;
   if(!accepted)s.call_id_.clear();
   SipSignal r;r.type=accepted?SipSignalType::STATUS_200_OK:SipSignalType::FINAL_RESPONSE;r.status_code=accepted?200:486;s.emit_sip_signal_(r);
  }
  s.pump_refer_();assert(s.cancel_count==0&&s.bye_count==0);
  if(accepted){assert(s.call_id_=="refer-new"&&s.media_active_&&s.delivered.back().type==SipSignalType::CONNECTED_IDENTITY);}
  else{assert(s.call_id_.empty()&&!s.media_active_&&s.delivered.back().type==SipSignalType::BYE);}
 }
 {SipTransport s;setup(s);assert(s.handle_refer_(request(),peer));s.cancel_refer_();
  assert(s.cancel_count==1&&s.refer_retiring_&&s.wire.back().body=="SIP/2.0 487 Request Terminated\r\n");}
 {SipTransport s;setup(s);s.remote_sip_tcp_=true;
  for(unsigned attempt=0;attempt<6;++attempt){
   auto retry=request("sip:room@192.0.2.20;transport=tcp");
   const std::string sequence="CSeq: "+std::to_string(attempt+2);
   retry.replace(retry.find("CSeq: 2"),7,sequence);
   assert(s.handle_refer_(retry,peer));s.refer_dialog_->accepted_refer.call_id="original";
   s.refer_dialog_->accepted_refer.udp=false;
   {SipTransport::ReferScope scope(s);s.call_id_.clear();s.outgoing_invite_pending_=false;
    SipSignal r;r.type=SipSignalType::FINAL_RESPONSE;r.status_code=486;s.emit_sip_signal_(r);}
   s.pump_refer_();assert(!s.refer_dialog_);assert(s.call_id_=="original");
  }
 }
 for(bool success:{false,true}){
  SipTransport s;setup(s);assert(s.handle_refer_(request(),peer));
  assert(s.response==200 && s.call_id_=="original" && s.media_active_);
  assert(s.remote_rtp_port_==40000 && s.media_proposal_epoch_==2);
  assert(s.wire.back().id=="original" && s.wire.back().body=="SIP/2.0 100 Trying\r\n");
  assert(s.wire.back().headers.find("Event: refer;id=2")!=std::string::npos);
  assert(s.wire.back().headers.find("Content-Type: message/sipfrag;version=2.0")!=std::string::npos);
  assert(s.wire.back().headers.find("Content-Length: 20")!=std::string::npos);
  {SipTransport::ReferScope scope(s);assert(s.call_id_=="refer-new");
   s.outgoing_invite_pending_=false;s.remote_rtp_port_=42000;
   SipSignal result;result.type=success?SipSignalType::STATUS_200_OK:SipSignalType::FINAL_RESPONSE;
   result.status_code=success?200:486;s.emit_sip_signal_(result);}
  assert(s.call_id_=="original" && s.remote_rtp_port_==40000 && s.delivered.empty());
  s.pump_refer_();assert(s.media_active_ && s.media_proposal_epoch_%2==0);
  if(success){assert(s.call_id_=="refer-new" && s.logical_call_id_=="original");
   assert(s.remote_rtp_port_==42000 && s.bye_count==1);
   assert(s.delivered.size()==1 && s.delivered[0].call_id=="original");
   assert(s.wire.back().method=="BYE" && s.wire.back().id=="original");
  }else{assert(s.call_id_=="original" && s.remote_rtp_port_==40000 && s.bye_count==0);
   assert(s.wire.back().body=="SIP/2.0 486 Transfer Failed\r\n");}
 }
 {SipTransport s;setup(s);assert(!s.handle_refer_(request("sip:room@192.0.2.20","wrong"),peer));assert(s.response==481 && !s.refer_dialog_);}
 {SipTransport s;setup(s);assert(s.handle_refer_(request(),peer));
  {SipTransport::ReferScope scope(s);s.call_id_.clear();s.outgoing_invite_pending_=false;
   SipSignal r;r.type=SipSignalType::FINAL_RESPONSE;r.status_code=486;s.emit_sip_signal_(r);}
  s.pump_refer_();assert(!s.refer_dialog_ && s.completed_refers_[0]);
  auto retry=request();retry.replace(retry.find("CSeq: 2"),7,"CSeq: 3");
  assert(s.handle_refer_(retry,peer));assert(s.call_id_=="original");}
 {SipTransport s;setup(s);s.fail_invite=true;assert(s.handle_refer_(request(),peer));
  assert(s.call_id_=="original" && s.wire.back().body=="SIP/2.0 503 Service Unavailable\r\n");}
 {SipTransport s;setup(s);assert(s.handle_refer_(request(),peer));
  {SipTransport::ReferScope scope(s);s.outgoing_invite_pending_=false;SipSignal result;
   result.type=SipSignalType::STATUS_200_OK;result.status_code=200;s.emit_sip_signal_(result);}
  s.call_id_="unrelated-next-call";s.pump_refer_();assert(s.call_id_=="unrelated-next-call");
  assert(s.bye_count==1 && s.delivered.empty());}
 {SipTransport s;setup(s);assert(s.handle_refer_(request(),peer));s.call_id_.clear();s.pump_refer_();
  assert(s.cancel_count==1 && s.delivered.empty());clock_ms=s.refer_expires_;s.pump_refer_();assert(!s.refer_dialog_);}
}
'''
    cpp = tmp_path / 'refer.cpp'
    cpp.write_text(code)
    binary = tmp_path / 'refer'
    subprocess.run(['g++','-std=c++17','-fsanitize=address,undefined','-I',str(ROOT),str(cpp),'-o',str(binary)],check=True,capture_output=True,text=True)
    subprocess.run([str(binary)],check=True,capture_output=True,text=True)
