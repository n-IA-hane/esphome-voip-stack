"""Existing re-INVITE owner commits DTMF only after successful final publication."""
import subprocess
import pytest
from test_refer_dialog_runtime import SOURCE, ROOT, block
from test_sdp_receive_payloads import sdp_fixture_source


@pytest.fixture(scope='module')
def reinvite_binary(tmp_path_factory):
    folder=tmp_path_factory.mktemp('dtmf-reinvite')
    source='#define USE_ESPHOME_VOIP_DTMF\n#include <mutex>\n#include "esphome/components/voip_stack/rtp_dtmf.h"\n'+sdp_fixture_source()
    source='#include \"esphome/components/voip_stack/sip_header_name.h\"\n#include \"esphome/components/voip_stack/sip_record.h\"\nconstexpr unsigned MAX_SIP_BODY_BYTES=4096;\n'+source
    source=source.replace('AudioFormat selected_tx_format_; };',
        'AudioFormat selected_tx_format_; bool owner_invite_ended=false; };')
    extra=r'''
 using LockGuard=std::lock_guard<std::mutex>;
 mutable std::mutex media_config_lock_,media_lifecycle_mutex_;
 std::atomic<uint32_t>media_proposal_epoch_{0},dtmf_generation_{8};
 std::atomic<bool>media_active_{true};bool refer_retiring_=false;
 struct{bool awaiting_ack=false;}completed_invite_;
 enum class MediaLifecyclePhase{IDLE,ACTIVE,CLEANING,SHUTTING_DOWN};
 std::atomic<MediaLifecyclePhase>media_lifecycle_phase_{MediaLifecyclePhase::ACTIVE};
 std::string local_tag_="local",remote_tag_="remote",last_invite_via_,last_invite_from_,last_invite_to_,last_invite_cseq_,last_invite_response_,remote_target_uri_;
 uint32_t last_invite_cseq_number_=0,remote_dialog_cseq_=1,last_invite_peer_ip_v4_=0;uint16_t last_invite_peer_port_=0;
 DtmfFormat dtmf_tx_format_{8000,0xFFFF,109},dtmf_rx_format_{8000,0xFFFF,109};uint8_t dtmf_offered_clocks_=1;
 DtmfConfig observed_at_final;bool fail_send=false;unsigned status=0,latch_resets=0;
 void get_dtmf_config_(DtmfFormat*,DtmfFormat*,uint32_t* =nullptr)const;
 void set_dtmf_config_(const DtmfFormat&,const DtmfFormat&);
 static void append_dtmf_sdp_(std::string*,std::string*,uint8_t);
 bool send_stateless_response_(const std::string&,const sockaddr_in&,uint16_t s,const char*,const std::string& ="",bool=false,int=-1){status=s;return true;}
 bool send_response_(uint16_t s,const char*,const std::string&){status=s;get_dtmf_config_(&observed_at_final.tx,&observed_at_final.rx);return !fail_send;}
 void reset_rtp_latch_(){++latch_resets;}
 bool handle_reinvite_(const std::string&,const sockaddr_in&);
'''
    source=source.replace('struct SipTransport {','struct SipTransport {'+extra)
    source=source.replace('std::string build_sdp_answer_() const;', 'std::string build_sdp_answer_(const DtmfConfig* =nullptr) const;')
    source=source.replace('bool learn_remote_rtp_from_sdp_(const std::string&,uint32_t,bool=false);',
        'bool learn_remote_rtp_from_sdp_(const std::string&,uint32_t,bool=false,DtmfConfig* =nullptr);')
    # Dependencies retain their production contracts; only host locking and
    # final-response transport completion are controlled independently.
    source+='#define portENTER_CRITICAL(p) (p)->lock()\n#define portEXIT_CRITICAL(p) (p)->unlock()\n'
    parser=(SOURCE/'sip_message.cpp').read_text()
    helpers=''
    for marker in ['std::string header_value(', 'std::string header_values(', 'std::string tag_from_header(',
                   'uint32_t cseq_number(', 'std::string message_body(', 'std::string name_addr_uri(', 'std::string strip_angle_uri(']:
        helpers+=block(parser,marker)+'\n'
    transport=(SOURCE/'sip_transport.cpp').read_text()
    helpers+='uint32_t esp_random(){return 123;}\n'+block(transport,'class ScopedMediaProposal {')+';\n'
    source=source.replace('struct RetainedCall {',helpers+'\nstruct RetainedCall {')
    for marker in ['void SipTransport::get_dtmf_config_(', 'void SipTransport::set_dtmf_config_(', 'void SipTransport::append_dtmf_sdp_(']:
        source+=block((SOURCE/'sip_dtmf.cpp').read_text(),marker)+'\n'
    source+=block(transport,'bool SipTransport::handle_reinvite_(')
    source+=r'''
int main(int argc,char**argv){
 assert(argc==2);const std::string mode=argv[1];SipTransport transport;
 transport.remote_ip_v4_=0xc0000202;AudioFormat pcm;pcm.frame_ms=10;
 transport.offer_tx_formats_.formats[0]=transport.offer_rx_formats_.formats[0]=pcm;
 const std::string audio="v=0\r\nc=IN IP4 192.0.2.2\r\nm=audio 50000 RTP/AVP 96 ";
 const std::string common="\r\na=rtpmap:96 L16/16000/1\r\na=ptime:10\r\n";
 const std::string initial=audio+"109"+common+"a=rtpmap:109 telephone-event/8000\r\n";
 assert(transport.learn_remote_rtp_from_sdp_(initial,0));
 const DtmfConfig old{transport.dtmf_tx_format_,transport.dtmf_rx_format_};const auto generation=transport.dtmf_generation_.load();
 std::string proposal=mode=="unchanged"?initial:audio+"111"+common+"a=rtpmap:111 telephone-event/16000\r\na=fmtp:111 0-11\r\n";
 if(mode=="rejected-audio")proposal.replace(proposal.find("L16/16000"),9,"L16/48000");
 transport.fail_send=mode=="send-failure";
 const auto request="INVITE sip:esp@192.0.2.1 SIP/2.0\r\nFrom: <sip:peer@192.0.2.2>;tag=remote\r\nTo: <sip:esp@192.0.2.1>;tag=local\r\nCSeq: 2 INVITE\r\nVia: SIP/2.0/UDP 192.0.2.2;branch=z9hG4bK-test\r\nContent-Type: application/sdp\r\nContent-Length: "+std::to_string(proposal.size())+"\r\n\r\n"+proposal;
 sockaddr_in peer{};peer.sin_addr.s_addr=htonl(0xc0000202);peer.sin_port=htons(5060);
 const auto result=transport.handle_reinvite_(request,peer);
 assert(transport.media_proposal_epoch_==2&&transport.tx.sample_rate==16000&&transport.rx.sample_rate==16000);
 if(mode=="accepted"){
  assert(result&&transport.status==200&&transport.observed_at_final==old);
  assert(transport.dtmf_generation_==generation+1&&transport.dtmf_tx_format_.payload_type==111);
 }else{
  assert((DtmfConfig{transport.dtmf_tx_format_,transport.dtmf_rx_format_}==old)&&transport.dtmf_generation_==generation);
  if(mode=="unchanged")assert(result&&transport.status==200&&transport.observed_at_final==old);
  if(mode=="send-failure")assert(!result&&transport.status==200&&transport.observed_at_final==old);
  if(mode=="rejected-audio")assert(result&&transport.status==488);
 }
 assert(transport.media_active_);
}
'''
    cpp=folder/'reinvite.cpp'
    cpp.write_text(source)
    binary=folder/'reinvite'
    built=subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
        '-fsanitize=address,undefined','-fno-pie','-no-pie','-pthread','-I',str(ROOT),str(cpp),'-o',str(binary)],capture_output=True,text=True)
    assert built.returncode==0,built.stderr
    return binary


@pytest.mark.parametrize('mode',['unchanged','accepted','send-failure','rejected-audio'])
def test_final_response_owns_negotiated_dtmf_commit(reinvite_binary,mode):
    run=subprocess.run([str(reinvite_binary),mode],capture_output=True,text=True,timeout=5)
    assert run.returncode==0,run.stdout+run.stderr
