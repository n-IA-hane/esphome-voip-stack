"""Real SDP/reoffer/RTP methods keep one active contract through video commits.

Only task scheduling and the video resource adapter are substitutes. Video and
DTMF SDP parsing, response ownership, RTP admission and jitter are production.
"""
import subprocess

import pytest

from test_reinvite_audio_admission_runtime import compile_proposal, proposal_audio_source
from test_refer_dialog_runtime import SOURCE, block


def full_reoffer_source():
    transport = (SOURCE / 'sip_transport.cpp').read_text()
    parser = (SOURCE / 'sip_message.cpp').read_text()
    sdp = (SOURCE / 'sip_sdp.cpp').read_text()
    dtmf = (SOURCE / 'sip_dtmf.cpp').read_text()
    code = proposal_audio_source().rsplit('int main(', 1)[0]
    code = code.replace('int mode=0;', 'uint32_t baseline_epoch=0;int mode=0;')
    code = code.replace('g.mode==2&&value==0', 'g.mode==2&&value==g.baseline_epoch')
    # The UAS owns cached final responses even if their initial socket write
    # fails. Model refusal of publication separately from that retry contract.
    response_declaration = 'bool send_response_(uint16_t,const char*,const std::string & = "",const std::string & = "");'
    assert response_declaration in code
    code = code.replace(response_declaration, r'''
 bool refuse_final_publication=false;
 bool send_response_owned_(uint16_t,const char*,const std::string&,const std::string&);
 bool send_response_(uint16_t status,const char*reason,const std::string&body="",const std::string&app=""){
  if(refuse_final_publication&&status==200)return false;
  return send_response_owned_(status,reason,body,app);
 }
''', 1)
    code = code.replace('bool SipTransport::send_response_(', 'bool SipTransport::send_response_owned_(', 1)
    adapter = r'''
struct VideoChild {
 bool running=false;unsigned starts=0,commits=0,stops=0;
 bool is_running()const{return running;}
 bool negotiation_matches(const VideoCapability&,const VideoCapability&,const VideoCapability&,uint32_t,uint16_t,uint32_t,uint16_t)const{return true;}
 bool can_request_media_direction(bool,bool)const{return true;}
 bool can_request_send_direction(bool)const{return true;}
 bool set_negotiated(const VideoCapability&c,const VideoCapability&,const VideoCapability&,uint32_t,uint16_t,uint32_t,uint16_t,bool,bool){return c.valid();}
 bool start(bool active){assert(!active);running=true;++starts;return true;}
 bool request_media_direction(bool,bool){++commits;return true;}
 bool request_send_direction(bool){++commits;return true;}
 void request_stop(){running=false;++stops;}
};
'''
    extra = ''
    for marker in ['bool parse_video_media_line(', 'bool parse_video_rtpmap(', 'bool parse_rtcp_attribute(']:
        body = block(parser, marker)
        if 'parse_video_media_line' in marker:
            body = body.replace('uint8_t *payload_order,', 'uint8_t *payload_order = nullptr,').replace('size_t *payload_count)', 'size_t *payload_count = nullptr)')
        extra += body + '\n'
    code = code.replace('struct SipPhoneTransport {', extra + adapter + '\nstruct SipPhoneTransport {', 1)
    declarations = r'''
 void get_dtmf_config_(DtmfFormat*,DtmfFormat*,uint32_t* = nullptr)const;
 void set_dtmf_config_(const DtmfFormat&,const DtmfFormat&);
 static void append_dtmf_sdp_(std::string*,std::string*,uint8_t);
 void*dtmf_queue_=nullptr;uint32_t rtp_tx_clock_timestamp_=0,rtp_tx_clock_ms_=0;
 void send_dtmf_packet_(const DtmfSender::Report&,const DtmfFormat&,uint32_t,uint32_t,uint16_t*){assert(false);}
 struct DtmfRequest {uint32_t epoch;uint16_t duration_ms;uint8_t event;};
 VIDEO_TRANSACTION_DECLARATIONS
 PendingVideoDirectionInvite pending_video_direction_invite_{};
 CompletedInviteClientTransaction completed_video_direction_invite_{};
 std::string confirmed_local_sdp_;
 void pump_video_direction_transaction_(){assert(false);}
 bool replay_completed_video_direction_ack_(const std::string&,const sockaddr_in&){assert(false);return false;}
 bool handle_video_direction_response_(const std::string&,const sockaddr_in&,uint16_t,uint32_t){assert(false);return false;}
 bool prepare_video_session_locked_(){assert(false);return false;}
 void emit_video_send_state_(bool,bool){}
 VideoChild child;VideoChild*video_session_=&child;
 void*video_source_=this,*video_sink_=this;
 bool video_offered_=false,video_negotiated_=false,video_send_enabled_=false,video_receive_enabled_=false;
 std::atomic<bool>video_send_requested_{true};
 uint32_t remote_video_ip_v4_=0,remote_video_rtcp_ip_v4_=0;
 uint16_t remote_video_rtp_port_=0,remote_video_rtcp_port_=0,video_rtp_port_=40002;
 uint8_t video_offer_payload_type_=26,video_imageattr_state_=0;
 enum:uint8_t{IMAGEATTR_PEER=1,IMAGEATTR_OFFERED=2,IMAGEATTR_DISABLED=4,IMAGEATTR_FALLBACK=8,IMAGEATTR_ACTIVE=16};
 VideoCapability negotiated_video_capability_;
 static VideoCapability capability(){VideoCapability c;c.encoding="JPEG";c.payload_type=26;c.width=320;c.height=240;c.max_fps=10;return c;}
 VideoCapability local_video_send_capability_()const{return capability();}
 VideoCapability local_video_receive_capability_()const{return capability();}
 VideoCapability local_video_direction_capability_(const VideoCapability&,bool)const{return capability();}
 void emit_video_active_state_(bool){}
 bool learn_remote_video_from_sdp_(const std::string&,uint32_t,bool);
 std::string append_video_sdp_(const std::string&,const std::string&,bool,bool = false)const;
 void reset_video_negotiation_();
 bool apply_video_direction_answer_(const std::string&,uint32_t,bool*);
'''
    declarations = declarations.replace('VIDEO_TRANSACTION_DECLARATIONS', block((SOURCE / 'sip_transport.h').read_text(), 'struct PendingVideoDirectionInvite {') + ';')
    code = code.replace(' uint16_t sip_port_=5060;', declarations + '\n uint16_t sip_port_=5060;', 1)
    for marker, source in [
        ('void SipTransport::get_dtmf_config_(', dtmf),
        ('void SipTransport::set_dtmf_config_(', dtmf),
        ('void SipTransport::append_dtmf_sdp_(', dtmf),
        ('void SipTransport::reset_video_negotiation_(', transport),
        ('std::string SipTransport::append_video_sdp_(', sdp),
        ('bool SipTransport::learn_remote_video_from_sdp_(', sdp),
        ('bool SipTransport::apply_video_direction_answer_(', transport),
    ]:
        code += '\n' + block(source, marker)
    prefix = r'''
#define USE_ESPHOME_VOIP_STACK_VIDEO
#define USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
#define USE_ESPHOME_VOIP_DTMF
#include "esphome/components/voip_stack/video.h"
#include "esphome/components/voip_stack/rtp_dtmf.h"
#include "esphome/components/voip_stack/sdp_imageattr.h"
#include <csignal>
unsigned uxQueueMessagesWaiting(void*){return 0;}
void xQueueReset(void*){}
int xQueueReceive(void*,void*,unsigned){return 0;}
'''
    main = r'''
int main(int argc,char**argv){
 signal(SIGPIPE,SIG_IGN);
 const std::string mode=argv[1];const bool failure=mode=="rollback",send_failure=mode=="owned-send-failure",future=mode=="future-pt",uac=mode=="uac"||mode=="uac-reject",changed_dtmf=mode=="dtmf-change"||send_failure;
 SipTransport t;Peer signaling(failure||send_failure),audio(false);setup(t,signaling);t.media_lifecycle_phase_=SipTransport::MediaLifecyclePhase::ACTIVE;
 AudioFormat format;format.frame_ms=10;RtpAudioPayloads mappings;assert(mappings.add(96,format));
 t.offer_tx_formats_.count=t.offer_rx_formats_.count=t.local_offered_tx_formats_.count=1;
 t.offer_tx_formats_.formats[0]=t.offer_rx_formats_.formats[0]=t.local_offered_tx_formats_.formats[0]=format;
 t.local_offered_rx_payloads_=mappings;t.set_media_config_(format,format,96,96,&mappings);
 t.remote_rtp_ip_v4_=INADDR_LOOPBACK;t.remote_rtp_port_=ntohs(audio.address.sin_port);
 DtmfFormat old_tx{8000,0xffff,101},old_rx{8000,0xffff,uint8_t(uac?120:101)};
 t.set_dtmf_config_(old_tx,old_rx);g.baseline_epoch=t.media_proposal_epoch_.load();t.dtmf_offered_clocks_=1;
 t.rtp_socket_=socket(AF_INET,SOCK_DGRAM,0);assert(t.rtp_socket_>=0);sockaddr_in receive{};receive.sin_family=AF_INET;receive.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 assert(bind(t.rtp_socket_,reinterpret_cast<sockaddr*>(&receive),sizeof(receive))==0);socklen_t length=sizeof(receive);assert(getsockname(t.rtp_socket_,reinterpret_cast<sockaddr*>(&receive),&length)==0);t.rtp_port_=ntohs(receive.sin_port);
 uint8_t storage[16*320]{};RtpJitterBuffer jitter(storage,320,16,0);t.jitter=&jitter;
 std::promise<void>ready;std::thread receiver([&]{{std::lock_guard lock(g.mutex);g.reader=std::this_thread::get_id();}ready.set_value();t.rtp_task_();});ready.get_future().wait();
 auto send_packet=[&](uint16_t seq,uint8_t pt=96){std::vector<uint8_t>p(332);p[0]=0x80;p[1]=pt;p[2]=seq>>8;p[3]=seq;uint32_t ts=seq*160u;for(int i=0;i<4;i++)p[4+i]=ts>>(24-8*i);p[8]=0x12;p[9]=0x34;p[10]=0x56;p[11]=0x78;for(int i=12;i<332;i+=2){p[i]=7;p[i+1]=0xd0;}assert(sendto(audio.fd,p.data(),p.size(),0,reinterpret_cast<sockaddr*>(&receive),sizeof(receive))==ssize_t(p.size()));};
 send_packet(1000);wait_gate([]{return g.emitted.load()==1;});send_packet(1001);wait_gate([]{return g.emitted.load()==2;});
 const auto old_epoch=t.media_proposal_epoch_.load();
 const auto audio_pt=future||mode=="uac-reject"?97:96;const auto dtmf_pt=changed_dtmf||failure?111:101;const auto dtmf_clock=changed_dtmf||failure?16000:8000;
 std::string body="v=0\r\no=peer 1 2 IN IP4 127.0.0.1\r\ns=test\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio "+std::to_string(ntohs(audio.address.sin_port))+" RTP/AVP "+std::to_string(audio_pt)+" "+std::to_string(dtmf_pt)+"\r\na=rtpmap:"+std::to_string(audio_pt)+" L16/16000/1\r\na=ptime:10\r\na=sendrecv\r\na=rtpmap:"+std::to_string(dtmf_pt)+" telephone-event/"+std::to_string(dtmf_clock)+"\r\na=fmtp:"+std::to_string(dtmf_pt)+" 0-15\r\nm=video 55000 RTP/AVP 26\r\na=rtpmap:26 JPEG/90000\r\na=framerate:10\r\na="+(uac?"sendonly":"sendrecv")+"\r\n";
 if(uac){
  t.video_offered_=t.video_negotiated_=t.video_send_enabled_=t.video_receive_enabled_=true;t.video_send_requested_=false;t.child.running=true;
  t.remote_video_ip_v4_=t.remote_video_rtcp_ip_v4_=INADDR_LOOPBACK;t.remote_video_rtp_port_=55000;t.remote_video_rtcp_port_=55001;t.negotiated_video_capability_=t.capability();
  bool send=true;const bool accepted=t.apply_video_direction_answer_(body,INADDR_LOOPBACK,&send);
  assert(accepted==(mode=="uac"));assert(t.media_proposal_epoch_==old_epoch);
  assert(t.dtmf_tx_format_==old_tx&&t.dtmf_rx_format_==old_rx);
  assert(t.child.commits==(mode=="uac"?1u:0u));if(accepted)assert(!send);
  send_packet(1002);wait_gate([]{return g.emitted.load()==3;});
 }else{
  {std::lock_guard lock(g.mutex);g.mode=mode=="changed"?2:1;}
  if(mode=="changed"){send_packet(1002);wait_gate([]{return g.even_read;});}
  t.refuse_final_publication=failure;
  if(send_failure){
   // Direct handler invocation retains the real TCP dispatch scope. Without
   // it, the fixture's default UDP cache legitimately owns the 200 response.
   t.dispatch_socket_=t.tcp_flow_.socket;
   assert(shutdown(t.tcp_flow_.socket,SHUT_WR)==0);
  }
  auto offer=request(signaling,"INVITE",10,"",t.call_id_,t.local_tag_,t.remote_tag_);auto at=offer.find("Content-Length: 0");offer.replace(at,17,"Content-Type: application/sdp\r\nContent-Length: "+std::to_string(body.size()));offer+=body;
  bool returned=false;std::thread proposal([&]{returned=t.handle_reinvite_(offer,signaling.address);});wait_gate([]{return g.staging;});
  AudioFormat observed;assert(t.received_audio_format_(96,&observed)&&observed==format);assert(!t.received_audio_format_(97,&observed));assert(t.media_proposal_epoch_==old_epoch);
  if(future)send_packet(999,97);
  if(mode!="changed"){send_packet(1002);wait_gate([]{return g.emitted.load()==3;});}
  {std::lock_guard lock(g.mutex);g.release_stage=true;}g.changed.notify_all();proposal.join();
  auto replies=signaling.drain();assert(returned==!failure);
  if(failure){assert(replies.empty()&&t.child.starts==1&&t.child.stops==1&&!t.child.running);}
  else if(send_failure){assert(replies.empty()&&t.completed_invite_.awaiting_ack&&t.completed_invite_.response.rfind("SIP/2.0 200",0)==0);}
  else{assert(replies.size()==1);assert(replies[0].rfind(future?"SIP/2.0 488":"SIP/2.0 200",0)==0);if(!future)assert(replies[0].find("m=video 40002")!=std::string::npos);}
  if(changed_dtmf){assert(t.media_proposal_epoch_>old_epoch&&!(t.media_proposal_epoch_.load()&1));assert(t.dtmf_tx_format_.clock==16000&&t.dtmf_tx_format_.payload_type==111);}
  else{assert(t.media_proposal_epoch_==old_epoch);assert(t.dtmf_tx_format_==old_tx&&t.dtmf_rx_format_==old_rx);}
  if(!failure&&!future)assert(t.child.starts==1&&t.child.commits==1&&t.child.running);
  if(future)assert(t.child.starts==0&&!t.child.running);
  if(mode=="changed"){{std::lock_guard lock(g.mutex);g.release_reader=true;}g.changed.notify_all();wait_gate([]{return g.changed_read;});}
 }
 assert(t.selected_tx_format_==format&&t.selected_rx_format_==format&&t.negotiated_rx_payloads_==mappings);
 send_packet(1003);wait_gate([]{return g.last_emitted;});t.rtp_task_terminate_=true;t.rtp_running_=false;t.wake_rtp_task_();receiver.join();
 uint8_t out[320];for(int i=0;i<5;i++)jitter.read(out,sizeof(out));assert(t.rtp_rx_packets_==4&&jitter.counters().missing==0);
 printf("scenario=%s accepted=%u missing=%u epoch=%u\n",mode.c_str(),t.rtp_rx_packets_.load(),jitter.counters().missing,t.media_proposal_epoch_.load());
 close(t.rtp_socket_);close(t.sip_socket_);
}
'''
    return prefix + code + main


@pytest.fixture(scope='module')
def full_reoffer_binary(tmp_path_factory):
    return compile_proposal(tmp_path_factory.mktemp('full-reoffer'), full_reoffer_source())


@pytest.mark.parametrize('scenario', ['odd', 'changed', 'future-pt', 'rollback', 'owned-send-failure', 'dtmf-change', 'uac', 'uac-reject'])
def test_reoffer_commits_only_accepted_controls_and_preserves_old_RTP(full_reoffer_binary, scenario):
    result = subprocess.run([full_reoffer_binary, scenario], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
