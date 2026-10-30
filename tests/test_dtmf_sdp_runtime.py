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
    source += block((SOURCE / 'sip_dtmf.cpp').read_text(), 'void SipTransport::append_dtmf_sdp_(')
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
