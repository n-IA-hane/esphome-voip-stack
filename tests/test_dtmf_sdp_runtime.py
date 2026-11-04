"""Actual SDP builder/parser keeps telephone events outside audio formats."""
import subprocess

from test_refer_dialog_runtime import SOURCE, block
from test_sdp_receive_payloads import ROOT, sdp_fixture_source


def test_dtmf_offer_answer_uses_clock_masks_and_directional_payload_numbers(tmp_path):
    source = '#define USE_ESPHOME_VOIP_DTMF\n#include "esphome/components/voip_stack/rtp_dtmf.h"\n' + sdp_fixture_source()
    source = source.replace('struct SipTransport {', '''struct SipTransport {
 DtmfFormat dtmf_tx{},dtmf_rx{};uint8_t dtmf_offered_clocks_=0;
 void get_dtmf_config_(DtmfFormat*a,DtmfFormat*b,uint32_t*c=nullptr)const{if(a)*a=dtmf_tx;if(b)*b=dtmf_rx;if(c)*c=0;}
 void set_dtmf_config_(const DtmfFormat&a,const DtmfFormat&b){dtmf_tx=a;dtmf_rx=b;}
 static void append_dtmf_sdp_(std::string*,std::string*,uint8_t);
''')
    source = source.replace("std::string build_sdp_answer_() const;", "std::string build_sdp_answer_(const DtmfConfig* =nullptr) const;")
    source = source.replace("bool learn_remote_rtp_from_sdp_(const std::string&,uint32_t,bool=false);", "bool learn_remote_rtp_from_sdp_(const std::string&,uint32_t,bool=false,DtmfConfig* =nullptr);")
    source = source.replace('struct SipTransport {', 'struct SipTransport {\n std::string build_sdp_capabilities_(uint32_t) const;')
    source = '#include <array>\n#include <cstdint>\nuint32_t esp_random();\n' + source
    source += block((SOURCE / 'sip_dtmf.cpp').read_text(), 'void SipTransport::append_dtmf_sdp_(')
    source += '\nuint32_t esp_random(){return 123;}\n' + block((SOURCE / 'sip_sdp.cpp').read_text(), 'std::string SipTransport::build_sdp_capabilities_(')
    source += r'''
void configure(SipTransport&s){AudioFormat f;f.frame_ms=10;s.offer_tx_formats_.count=s.offer_rx_formats_.count=1;s.offer_tx_formats_.formats[0]=s.offer_rx_formats_.formats[0]=f;}
std::string session(const char*payloads,const char*maps){return std::string("v=0\r\nc=IN IP4 192.0.2.2\r\nm=audio 50000 RTP/AVP 96 ")+payloads+"\r\na=rtpmap:96 L16/16000/1\r\na=ptime:10\r\n"+maps;}
int main(){
 SipTransport outgoing;configure(outgoing);const auto offer=outgoing.build_sdp_offer_();
 assert(offer.find("a=rtpmap:120 telephone-event/8000")!=std::string::npos);
 assert(offer.find("a=rtpmap:122 telephone-event/16000")!=std::string::npos);
 auto answer=session("105 106","a=rtpmap:105 telephone-event/8000\r\na=rtpmap:106 telephone-event/16000\r\na=fmtp:106 0-11\r\n");
 assert(outgoing.learn_remote_rtp_from_sdp_(answer,0,true));
 assert(outgoing.tx.is_valid()&&outgoing.rx.is_valid()&&outgoing.dtmf_tx.clock==16000&&outgoing.dtmf_tx.payload_type==106);
 assert(outgoing.dtmf_rx.payload_type==122&&outgoing.dtmf_rx.events==0x0FFF);
 const auto clocks=outgoing.dtmf_offered_clocks_;const auto capabilities=outgoing.build_sdp_capabilities_(0);
 assert(capabilities.find("telephone-event")!=std::string::npos&&outgoing.dtmf_offered_clocks_==clocks&&outgoing.dtmf_tx.payload_type==106);
 SipTransport incoming;configure(incoming);
 assert(incoming.learn_remote_rtp_from_sdp_(session("109 111","a=fmtp:111 0-9,11\r\na=rtpmap:109 telephone-event/8000\r\na=rtpmap:111 telephone-event/16000\r\n"),0));
 assert(incoming.dtmf_tx.payload_type==111&&incoming.dtmf_rx.payload_type==111&&incoming.dtmf_tx.events==0x0BFF);
 const auto response=incoming.build_sdp_answer_();assert(response.find("a=rtpmap:111 telephone-event/16000")!=std::string::npos);
 assert(incoming.learn_remote_rtp_from_sdp_(session("109","a=rtpmap:109 telephone-event/8000\r\n"),0));
 assert(incoming.dtmf_tx.clock==8000&&incoming.tx.sample_rate==16000);
 assert(incoming.learn_remote_rtp_from_sdp_(session("109","a=rtpmap:109 telephone-event/9999\r\n"),0));
 assert(incoming.tx.is_valid()&&!incoming.dtmf_tx.valid());
 assert(incoming.learn_remote_rtp_from_sdp_(session("",""),0));assert(!incoming.dtmf_tx.valid());
}
'''
    cpp = tmp_path / 'sdp.cpp'
    cpp.write_text(source)
    binary = tmp_path / 'sdp'
    result = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g',
        '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-I', str(ROOT), str(cpp), '-o', str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr


def test_speculative_dtmf_proposal_has_no_committed_side_effects(tmp_path):
    source = '#define USE_ESPHOME_VOIP_DTMF\n#include "esphome/components/voip_stack/rtp_dtmf.h"\n' + sdp_fixture_source()
    source=source.replace('struct SipTransport {', '''struct SipTransport {
 DtmfConfig committed{{8000,0xffff,109},{8000,0xffff,109}};unsigned commits=0;uint8_t dtmf_offered_clocks_=1;
 void get_dtmf_config_(DtmfFormat*a,DtmfFormat*b,uint32_t*c=nullptr)const{if(a)*a=committed.tx;if(b)*b=committed.rx;if(c)*c=0;}
 void set_dtmf_config_(const DtmfFormat&a,const DtmfFormat&b){committed={a,b};++commits;}
 static void append_dtmf_sdp_(std::string*,std::string*,uint8_t);
''')
    source=source.replace('std::string build_sdp_answer_() const;', 'std::string build_sdp_answer_(const DtmfConfig* =nullptr) const;')
    source=source.replace('bool learn_remote_rtp_from_sdp_(const std::string&,uint32_t,bool=false);',
        'bool learn_remote_rtp_from_sdp_(const std::string&,uint32_t,bool=false,DtmfConfig* =nullptr);')
    source+=block((SOURCE/'sip_dtmf.cpp').read_text(),'void SipTransport::append_dtmf_sdp_(')
    source+=r'''
int main(){
 SipTransport transport;AudioFormat pcm;pcm.frame_ms=10;
 transport.offer_tx_formats_.formats[0]=transport.offer_rx_formats_.formats[0]=pcm;
 const auto old=transport.committed;DtmfConfig proposed;
 const std::string offer="v=0\r\nc=IN IP4 192.0.2.2\r\nm=audio 50000 RTP/AVP 96 111\r\na=rtpmap:96 L16/16000/1\r\na=ptime:10\r\na=rtpmap:111 telephone-event/16000\r\na=fmtp:111 0-11\r\n";
 assert(transport.learn_remote_rtp_from_sdp_(offer,0,false,&proposed));
 assert(transport.committed==old&&transport.commits==0&&proposed.tx.payload_type==111&&proposed.tx.clock==16000);
 const auto answer=transport.build_sdp_answer_(&proposed);
 assert(answer.find("a=rtpmap:111 telephone-event/16000")!=std::string::npos);
 assert(answer.find("a=rtpmap:109 telephone-event")==std::string::npos);
 // Rejected or unsent final response leaves this committed value unchanged.
 assert(transport.committed==old&&transport.commits==0);
 DtmfConfig failed;assert(!transport.learn_remote_rtp_from_sdp_("v=0\r\n",0,false,&failed));
 assert(transport.committed==old&&transport.commits==0);
 transport.set_dtmf_config_(proposed.tx,proposed.rx);assert(transport.commits==1&&transport.committed==proposed);
}
'''
    cpp=tmp_path/'proposal.cpp'
    cpp.write_text(source)
    binary=tmp_path/'proposal'
    built=subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
        '-fno-pie','-no-pie','-I',str(ROOT),str(cpp),'-o',str(binary)],capture_output=True,text=True)
    assert built.returncode==0,built.stderr
    run=subprocess.run([binary],capture_output=True,text=True,timeout=5)
    assert run.returncode==0,run.stderr


def test_audio_payload_conflict_is_rejected_before_receive_pt_remapping(tmp_path):
    source='#define USE_ESPHOME_VOIP_DTMF\n#include "esphome/components/voip_stack/rtp_dtmf.h"\n'+sdp_fixture_source()
    source=source.replace('struct SipTransport {','''struct SipTransport {
 DtmfFormat dtmf_tx{},dtmf_rx{};uint8_t dtmf_offered_clocks_=0;
 void get_dtmf_config_(DtmfFormat*a,DtmfFormat*b,uint32_t*c=nullptr)const{if(a)*a=dtmf_tx;if(b)*b=dtmf_rx;if(c)*c=0;}
 void set_dtmf_config_(const DtmfFormat&a,const DtmfFormat&b){dtmf_tx=a;dtmf_rx=b;}
 static void append_dtmf_sdp_(std::string*,std::string*,uint8_t);
''')
    source=source.replace('std::string build_sdp_answer_() const;','std::string build_sdp_answer_(const DtmfConfig* =nullptr) const;')
    source=source.replace('bool learn_remote_rtp_from_sdp_(const std::string&,uint32_t,bool=false);',
        'bool learn_remote_rtp_from_sdp_(const std::string&,uint32_t,bool=false,DtmfConfig* =nullptr);')
    source+=block((SOURCE/'sip_dtmf.cpp').read_text(),'void SipTransport::append_dtmf_sdp_(')
    source+=r'''
int main(){
 for(bool answering:{false,true}){
  SipTransport transport;AudioFormat pcm;pcm.frame_ms=10;
  transport.offer_tx_formats_.count=answering?0:1;
  transport.offer_tx_formats_.formats[0]=transport.offer_rx_formats_.formats[0]=pcm;
  const auto offered=transport.build_sdp_offer_();assert(!offered.empty());
  const std::string pt=answering?"120":"96";
  const auto conflicting="v=0\r\nc=IN IP4 192.0.2.2\r\nm=audio 50000 RTP/AVP "+pt+
   "\r\na=rtpmap:"+pt+" L16/16000/1\r\na=rtpmap:"+pt+" telephone-event/16000\r\na=ptime:10\r\n"+(answering?"a=sendonly\r\n":"");
  assert(transport.learn_remote_rtp_from_sdp_(conflicting,0,answering));
  assert(!transport.dtmf_tx.valid()&&!transport.dtmf_rx.valid()&&"Conflicting audio/event PT is invalid even when the audio receive PT is remapped");
 }
}
'''
    cpp=tmp_path/'conflict.cpp'
    cpp.write_text(source)
    binary=tmp_path/'conflict'
    built=subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
        '-fno-pie','-no-pie','-I',str(ROOT),str(cpp),'-o',str(binary)],capture_output=True,text=True)
    assert built.returncode==0,built.stderr
    run=subprocess.run([binary],capture_output=True,text=True,timeout=5)
    assert run.returncode==0,run.stderr
