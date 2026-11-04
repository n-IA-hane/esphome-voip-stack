"""Exact parking offer reaches the production SDP parsers and re-INVITE owner."""
import json
from pathlib import Path
import subprocess

import pytest

from test_refer_dialog_runtime import SOURCE, ROOT, block
from test_sdp_receive_payloads import sdp_fixture_source


def source():
    owner = (SOURCE/'sip_transport.cpp').read_text()
    parser = (SOURCE/'sip_message.cpp').read_text()
    sdp = (SOURCE/'sip_sdp.cpp').read_text()
    header = (SOURCE/'sip_transport.h').read_text()
    code = '''
#define USE_ESPHOME_VOIP_STACK_VIDEO
#define USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
#define USE_ESPHOME_VOIP_DTMF
#include <mutex>
#include <cstring>
#include <cctype>
#include <iostream>
#include "esphome/components/voip_stack/video.h"
#include "esphome/components/voip_stack/sdp_imageattr.h"
#include "esphome/components/voip_stack/rtp_dtmf.h"
#include "esphome/components/voip_stack/sip_header_name.h"
constexpr unsigned MAX_SIP_BODY_BYTES=4096;
'''+sdp_fixture_source()
    code = code.replace('\"192.0.2.1\"', '\"192.168.1.192\"')
    prototype = 'bool parse_video_media_line(const std::string&,uint16_t*,bool[128],uint8_t* = nullptr,size_t* = nullptr);\n'
    helpers = prototype+'\n'.join(block(parser, marker) for marker in [
        'std::string header_value(', 'std::string header_values(', 'std::string tag_from_header(',
        'uint32_t cseq_number(', 'std::string message_body(', 'std::string name_addr_uri(',
        'std::string strip_angle_uri(', 'bool parse_video_media_line(',
        'bool parse_video_rtpmap(', 'bool parse_rtcp_attribute('])
    helpers += '\nuint32_t esp_random(){return 123;}\n'+block(owner,'class ScopedMediaProposal {')+';\n'
    # Only the transport boundary and dormant hardware child are controlled.
    # All SDP choices, transaction admission, rollback and response status use
    # extracted production methods, with JPEG and DTMF enabled together.
    helpers += r'''
struct Child {
 bool running=true,send=false,receive=false,fail_retarget=false;unsigned calls=0;
 uint32_t ip=0xc0a801c8,rtcp_ip=0xc0a801c8;uint16_t port=40040,rtcp_port=40041;
 bool is_running()const{return running;}
 bool can_retarget_inactive_remote()const{return running&&!send&&!receive;}
 bool retarget_inactive_remote(uint32_t i,uint16_t p,uint32_t ri,uint16_t rp){
  assert(can_retarget_inactive_remote());++calls;if(fail_retarget)return false;ip=i;port=p;rtcp_ip=ri;rtcp_port=rp;return true;
 }
 template<class...T>bool negotiation_matches(T&&...){++calls;return true;}
 template<class...T>bool set_negotiated(T&&...){++calls;return true;}
 bool start(bool){++calls;running=true;return true;}
 bool can_request_media_direction(bool,bool){++calls;return true;}
 bool request_media_direction(bool,bool){++calls;return true;}
 void request_stop(){++calls;running=false;}
};
'''
    code = code.replace('struct RetainedCall {',helpers+'\nstruct RetainedCall {')
    code = code.replace('AudioFormat selected_tx_format_; };', 'AudioFormat selected_tx_format_; bool owner_invite_ended=false; };')
    enum_start = header.index('  enum : uint8_t { IMAGEATTR_PEER')
    enums = header[enum_start:header.index('#endif',enum_start)]
    extra = r'''
 using LockGuard=std::lock_guard<std::mutex>;
 std::mutex media_lifecycle_mutex_;
 std::atomic<uint32_t>media_proposal_epoch_{0};std::atomic<bool>media_active_{true};
 bool refer_retiring_=false,terminate_after_invite_ack_=false,fail_send=false;
 struct{bool awaiting_ack=false;}completed_invite_;struct{bool pending()const{return false;}}pending_video_direction_invite_;
 enum class MediaLifecyclePhase{IDLE,ACTIVE,CLEANING,SHUTTING_DOWN};
 std::atomic<MediaLifecyclePhase>media_lifecycle_phase_{MediaLifecyclePhase::ACTIVE};
 std::string local_tag_="local",remote_tag_="remote",last_invite_via_="old-via",last_invite_from_="old-from",last_invite_to_="old-to",last_invite_cseq_="1 INVITE",last_invite_response_="old-response",remote_target_uri_="sip:old-target",call_id_="owned-call";
 uint32_t last_invite_cseq_number_=1,remote_dialog_cseq_=1,last_invite_peer_ip_v4_=0xc0a801c8;uint16_t last_invite_peer_port_=5060;
 bool video_offered_=false,video_negotiated_=false,video_send_enabled_=false,video_receive_enabled_=false;
 void *video_source_=nullptr,*video_sink_=this;VideoCapability video_rx_,negotiated_video_capability_;
 Child child;Child*video_session_=&child;
 std::atomic<bool>video_send_requested_{false};uint8_t video_offer_payload_type_=26;
 uint16_t video_rtp_port_=40002,remote_video_rtp_port_=0,remote_video_rtcp_port_=0;
 uint32_t remote_video_ip_v4_=0,remote_video_rtcp_ip_v4_=0;
 std::string confirmed_local_sdp_;
 DtmfFormat dtmf_tx{},dtmf_rx{};uint8_t dtmf_offered_clocks_=0;
 void get_dtmf_config_(DtmfFormat*a,DtmfFormat*b,uint32_t*c=nullptr)const{if(a)*a=dtmf_tx;if(b)*b=dtmf_rx;if(c)*c=0;}
 void set_dtmf_config_(const DtmfFormat&a,const DtmfFormat&b){dtmf_tx=a;dtmf_rx=b;}
 static void append_dtmf_sdp_(std::string*,std::string*,uint8_t);
 unsigned status=0,finals=0,latch_resets=0,active_emits=0,stops=0,signals=0;std::string reason,answer;
 bool send_stateless_response_(const std::string&,const sockaddr_in&,uint16_t s,const char*,const std::string&r="",bool=false,int=-1){status=s;reason=r;return true;}
 bool send_response_(uint16_t s,const char*,const std::string&body){++finals;status=s;answer=body;return !fail_send;}
 void reset_rtp_latch_(){++latch_resets;}
 void emit_video_active_state_(bool value){active_emits+=value;}
 void emit_video_send_state_(bool value,bool){assert(!value);}
 void request_audio_path_stop_locked_(){++stops;}
 void emit_sip_signal_(SipSignal signal){assert(signal.status_code==500&&signal.terminal_transaction_pending);++signals;}
 VideoCapability local_video_send_capability_()const{return {};}
 VideoCapability local_video_receive_capability_()const{return video_rx_;}
 VideoCapability local_video_direction_capability_(const VideoCapability&,bool)const;
 void reset_video_negotiation_();
 bool learn_remote_video_from_sdp_(const std::string&,uint32_t,bool);
 std::string append_video_sdp_(const std::string&,const std::string&,bool,bool=false)const;
 bool handle_reinvite_(const std::string&,const sockaddr_in&);
'''+enums
    code = code.replace('struct SipTransport {','struct SipTransport {'+extra)
    code = code.replace('std::string build_sdp_answer_() const;', 'std::string build_sdp_answer_(const DtmfConfig* =nullptr) const;')
    code = code.replace('bool learn_remote_rtp_from_sdp_(const std::string&,uint32_t,bool=false);',
        'bool learn_remote_rtp_from_sdp_(const std::string&,uint32_t,bool=false,DtmfConfig* =nullptr);')
    for text, marker in [(owner,'void SipTransport::reset_video_negotiation_()'),
                         (owner,'VideoCapability SipTransport::local_video_direction_capability_('),
                         (sdp,'bool SipTransport::learn_remote_video_from_sdp_('),
                         (sdp,'std::string SipTransport::append_video_sdp_('),
                         ((SOURCE/'sip_dtmf.cpp').read_text(),'void SipTransport::append_dtmf_sdp_('),
                         (owner,'bool SipTransport::handle_reinvite_(')]:
        code += '\n'+block(text,marker)
    data = json.loads((Path(__file__).parent/'fixtures/park_inactive_video_offer.json').read_text())
    for key in ('initial_offer','initial_answer','park_offer'):
        code += '\nconst std::string '+key+'='+json.dumps(data[key])+';\n'
    return code+r'''
int main(int argc,char**argv){
 assert(argc==2);const std::string mode=argv[1];SipTransport t;
 t.video_rx_.encoding="JPEG";t.video_rx_.payload_type=26;t.video_rx_.width=320;t.video_rx_.height=240;t.video_rx_.max_fps=10;
 // Exact FULL10 generated format lists, including non-selected alternatives.
 AudioFormat f;f.sample_rate=16000;t.offer_tx_formats_.count=3;
 f.frame_ms=16;t.offer_tx_formats_.formats[0]=f;f.frame_ms=10;t.offer_tx_formats_.formats[1]=f;
 f.frame_ms=20;t.offer_tx_formats_.formats[2]=f;t.offer_rx_formats_.count=6;
 f.sample_rate=48000;f.frame_ms=10;t.offer_rx_formats_.formats[0]=f;
 f.sample_rate=32000;f.frame_ms=16;t.offer_rx_formats_.formats[1]=f;
 f.sample_rate=16000;f.frame_ms=10;t.offer_rx_formats_.formats[2]=f;
 f.frame_ms=16;t.offer_rx_formats_.formats[3]=f;f.frame_ms=32;t.offer_rx_formats_.formats[4]=f;
 f.frame_ms=20;t.offer_rx_formats_.formats[5]=f;t.peer_directional_audio_v1_=true;
 const auto built=t.build_sdp_offer_();
 assert(built.substr(built.find("m=audio"))==initial_offer.substr(initial_offer.find("m=audio")));
 auto initial=initial_answer;
 if(mode=="active-retarget")initial.replace(initial.find("a=inactive"),10,"a=sendonly");
 assert(t.learn_remote_rtp_from_sdp_(initial,0xc0a801c8,true));
 assert(t.tx.sample_rate==16000&&t.rx.sample_rate==48000&&t.tx_pt==97&&t.rx_pt==96&&t.accepted.count==1);
 assert(t.video_negotiated_&&!t.video_send_enabled_&&t.remote_video_rtp_port_==40040);
 t.child.send=t.video_send_enabled_;t.child.receive=t.video_receive_enabled_;
 const auto old_rx=t.accepted;const auto old_tx=t.tx;const auto old_format=t.negotiated_video_capability_;
 const auto old_dtmf=DtmfConfig{t.dtmf_tx,t.dtmf_rx};const auto old_direction=t.video_receive_enabled_;
 auto proposal=park_offer;
 if(mode=="old-ports"){
  proposal.replace(proposal.find("video 40042"),11,"video 40040");
  proposal.replace(proposal.find("rtcp:40043"),10,"rtcp:40041");
 }
 if(mode=="inactive-to-active")proposal.replace(proposal.find("a=inactive"),10,"a=sendonly");
 if(mode=="no-rtcp-attribute")proposal.erase(proposal.find("a=rtcp:40043"),14);
 if(mode=="format-change")proposal+="a=framerate:5\r\n";
 t.child.fail_retarget=mode=="commit-failure";
 t.fail_send=mode=="send-failure";t.remote_ip_v4_=0xc0a801c8;
 const std::string request="INVITE sip:esp@192.168.1.192 SIP/2.0\r\nFrom: <sip:ha@192.168.1.200>;tag=remote\r\nTo: <sip:esp@192.168.1.192>;tag=local\r\nCSeq: 2 INVITE\r\nVia: SIP/2.0/UDP 192.168.1.200;branch=z9hG4bK-owned\r\nContact: <sip:new-target@192.168.1.200>\r\nContent-Type: application/sdp\r\nContent-Length: "+std::to_string(proposal.size())+"\r\n\r\n"+proposal;
 sockaddr_in peer{};peer.sin_addr.s_addr=htonl(0xc0a801c8);peer.sin_port=htons(5060);
 const bool result=t.handle_reinvite_(request,peer);
 std::cerr<<"status="<<t.status<<" reason="<<t.reason<<" final_attempts="<<t.finals<<" child_calls="<<t.child.calls<<" remote_video="<<t.remote_video_rtp_port_<<"\n";
 assert(t.tx==old_tx&&t.accepted==old_rx&&t.media_active_&&t.media_proposal_epoch_==2);
 assert((DtmfConfig{t.dtmf_tx,t.dtmf_rx}==old_dtmf)&&t.negotiated_video_capability_.encoding==old_format.encoding);
 const bool reject=mode=="active-retarget"||mode=="inactive-to-active"||mode=="format-change";
 if(reject){
  assert(result&&t.status==488&&t.reason=="video_renegotiation_unsupported"&&t.finals==0);
 }else if(mode=="commit-failure"){
  assert(result&&t.status==200&&t.finals==1&&t.terminate_after_invite_ack_&&t.stops==1&&t.signals==1);
  assert(t.remote_video_rtp_port_==40040&&t.remote_video_rtcp_port_==40041&&!t.video_receive_enabled_);
  assert(t.remote_dialog_cseq_==2&&t.last_invite_cseq_number_==2);
 }else if(mode=="send-failure"){
  assert(!result&&t.status==200&&t.finals==1);
 }else{
  assert(result&&t.status==200&&t.finals==1);
  assert(t.remote_video_rtp_port_==(mode=="old-ports"?40040:40042));
  assert(t.remote_video_rtcp_port_==(mode=="old-ports"?40041:40043));
  assert(t.remote_dialog_cseq_==2&&t.last_invite_cseq_number_==2&&t.remote_target_uri_=="sip:new-target@192.168.1.200");
  assert(!t.video_send_enabled_&&!t.video_receive_enabled_&&t.active_emits==0);
  assert(t.answer.find("a=inactive")!=std::string::npos);
 }
 if(reject||mode=="send-failure"){
  assert(t.remote_video_rtp_port_==40040&&t.remote_video_rtcp_port_==40041&&t.video_receive_enabled_==old_direction);
  assert(t.remote_dialog_cseq_==1&&t.last_invite_cseq_number_==1&&t.remote_target_uri_=="sip:old-target");
 }
 const bool moved=mode=="new-ports"||mode=="no-rtcp-attribute";
 assert(t.child.calls==((moved||mode=="commit-failure")?1U:0U));
 assert(t.child.port==(moved?40042:40040)&&t.child.rtcp_port==(moved?40043:40041));
}
'''


@pytest.fixture(scope='module')
def reinvite(tmp_path_factory):
    folder=tmp_path_factory.mktemp('inactive-video-reinvite')
    stub=folder/'esphome/core/defines.h'
    stub.parent.mkdir(parents=True)
    stub.write_text('#pragma once\n')
    cpp=folder/'reinvite.cpp'
    cpp.write_text(source())
    binary=folder/'reinvite'
    built=subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
        '-fsanitize=address,undefined','-fno-pie','-no-pie','-pthread','-I',str(folder),'-I',str(ROOT),str(cpp),'-o',str(binary)],
        capture_output=True,text=True)
    assert built.returncode==0,built.stderr
    return binary


@pytest.mark.parametrize('scenario',['old-ports','new-ports','send-failure','active-retarget','inactive-to-active','no-rtcp-attribute','format-change','commit-failure'])
def test_inactive_video_reoffer_uses_existing_dialog_without_activating_media(reinvite,scenario):
    result=subprocess.run([str(reinvite),scenario],capture_output=True,text=True,timeout=5)
    assert result.returncode==0,result.stdout+result.stderr
