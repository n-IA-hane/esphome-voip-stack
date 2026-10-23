"""Run production offer generation and SDP parsing without ESP hardware."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def method(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


def sdp_fixture_source():
    base = ROOT / "esphome/components/voip_stack"
    parser = (base / "sip_message.cpp").read_text()
    sdp = (base / "sip_sdp.cpp").read_text()
    source = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <arpa/inet.h>
#include "esphome/components/voip_stack/rtp_audio_payloads.h"
#define USE_ESPHOME_VOIP_STACK_SPEAKER
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
using namespace esphome::voip_stack;
'''
    for signature in ["std::string trim_copy(", "bool parse_decimal_u32(",
                      "bool parse_rtpmap_format(", "bool parse_audio_media_line("]:
        source += method(parser, signature) + "\n"
    from test_refer_dialog_runtime import block
    header = (base / "sip_transport.h").read_text()
    source += "using SipSignalingString = std::string;\nconstexpr size_t kMaxSdpMediaLines = 8;\n"
    source += block(header, "struct RemoteMediaShape {") + ";\n"
    source += r'''
struct RetainedCall { AudioFormat selected_tx_format_; };
struct SipTransport {
 bool refer_context_active_=false;
 RetainedCall *refer_dialog_=nullptr;
 AudioFormatList offer_tx_formats_,offer_rx_formats_,local_offered_tx_formats_;
 RtpAudioPayloads local_offered_rx_payloads_,accepted;
 bool remote_directional_audio_v1_=false,peer_directional_audio_v1_=false,local_offered_directional_audio_v1_=false;
 RemoteMediaShape remote_media_shape_;
 std::atomic<uint32_t> remote_ip_v4_{0},remote_rtp_ip_v4_{0};
 std::atomic<uint16_t> remote_rtp_port_{0};
 uint32_t sdp_session_id_=1,sdp_session_version_=0;
 uint16_t rtp_port_=40000;size_t udp_max_payload_=1200;
 AudioFormat tx,rx;uint8_t tx_pt=0,rx_pt=0;
 void local_ip_for_peer_(uint32_t,std::string *out) const {*out="192.0.2.1";}
 void capture_remote_media_shape_(const std::string&);
 std::string rejected_media_answer_(const std::string&) const;
 void get_media_config_(AudioFormat*a,AudioFormat*b,uint8_t*c,uint8_t*d,RtpAudioPayloads*e)const{*a=tx;*b=rx;*c=tx_pt;*d=rx_pt;*e=accepted;}
 void set_media_config_(const AudioFormat&a,const AudioFormat&b,uint8_t c,uint8_t d,const RtpAudioPayloads*e){tx=a;rx=b;tx_pt=c;rx_pt=d;accepted=*e;}
 std::string wrap_sdp_envelope_(const std::string&,const std::string&,const std::string&,const std::string&,uint8_t,const char * = "sendrecv") const;
 std::string build_sdp_offer_();
 std::string build_sdp_answer_() const;
 bool learn_remote_rtp_from_sdp_(const std::string&,uint32_t,bool=false);
};
'''
    for signature in ["std::string SipTransport::wrap_sdp_envelope_(",
                      "void SipTransport::capture_remote_media_shape_(",
                      "std::string SipTransport::rejected_media_answer_(",
                      "std::string SipTransport::build_sdp_answer_(",
                      "std::string SipTransport::build_sdp_offer_(",
                      "bool SipTransport::learn_remote_rtp_from_sdp_("]:
        source += method(sdp, signature) + "\n"
    # Support immutable pre-fix sources in the red/green witness. Only the
    # declared helper arity changes, never the extracted decision logic.
    if "uint8_t ptime, const char *direction" not in sdp:
        source = source.replace('uint8_t,const char * = "sendrecv") const;', 'uint8_t) const;')
    return source


def test_standard_receive_payload_contract(tmp_path):
    source = sdp_fixture_source()
    source += r'''
AudioFormat fmt(unsigned rate){AudioFormat f;f.sample_rate=rate;f.frame_ms=10;return f;}
int main(){
 SipTransport s;s.offer_tx_formats_.count=1;s.offer_tx_formats_.formats[0]=fmt(16000);
 s.offer_rx_formats_.count=2;s.offer_rx_formats_.formats[0]=fmt(48000);s.offer_rx_formats_.formats[1]=fmt(16000);
 auto offer=s.build_sdp_offer_();assert(offer.find("x-voip-stack")==std::string::npos);
 assert(offer.find("L16/48000/1")!=std::string::npos);
 const std::string base="v=0\r\nc=IN IP4 192.0.2.2\r\nm=audio 40002 RTP/AVP 126 118\r\na=ptime:10\r\n";
 // Deliberately reverse rtpmap order relative to the m-line.
 const std::string maps="a=rtpmap:118 L16/16000/1\r\na=rtpmap:126 L16/48000/1\r\n";
 assert(s.learn_remote_rtp_from_sdp_(base+maps,0));
 assert(s.tx.sample_rate==16000&&s.rx.sample_rate==48000);
 AudioFormat f;assert(s.accepted.find(118,&f)&&f.sample_rate==16000);
 assert(s.accepted.find(126,&f)&&f.sample_rate==48000);assert(!s.accepted.find(99,&f));
 auto prior=s.accepted;
 assert(!s.learn_remote_rtp_from_sdp_(base+maps+"a=rtpmap:118 L16/48000/1\r\n",0));
 assert(s.accepted==prior);
 // Answerer remaps TX payloads. RX still uses our offered payload numbers.
 assert(s.learn_remote_rtp_from_sdp_(base+maps,0,true));
 assert(s.tx_pt==118&&s.rx_pt==96);
 assert(s.accepted.find(97,&f)&&f.sample_rate==16000);
 assert(!s.accepted.find(118,&f));
 assert(!s.learn_remote_rtp_from_sdp_("v=0\r\nc=IN IP4 192.0.2.2\r\nm=audio 40002 RTP/AVP 119\r\na=ptime:10\r\na=rtpmap:119 L16/32000/1\r\n",0,true));
 // Transmit preference follows the m-line despite reversed rtpmap order.
 s.offer_tx_formats_.formats[1]=fmt(48000);s.offer_tx_formats_.count=2;
 assert(s.learn_remote_rtp_from_sdp_(base+maps,0));assert(s.tx.sample_rate==48000);
 s.offer_tx_formats_.count=1;
 // A peer selecting one common codec retains the old 16/16 behavior.
 assert(s.learn_remote_rtp_from_sdp_("v=0\r\nc=IN IP4 192.0.2.2\r\nm=audio 40002 RTP/AVP 118\r\na=ptime:10\r\na=rtpmap:118 L16/16000/1\r\n",0,true));
 assert(s.tx.sample_rate==16000&&s.rx.sample_rate==16000&&s.accepted.count==1);
 // The retained transmitter cannot change rate while its old call is live.
 RetainedCall old{fmt(16000)};s.refer_context_active_=true;s.refer_dialog_=&old;
 auto replacement=s.build_sdp_offer_();
 assert(replacement.find("L16/16000/1")!=std::string::npos);
 assert(replacement.find("L16/48000/1")==std::string::npos);
 assert(s.local_offered_tx_formats_.count==1);
 assert(s.learn_remote_rtp_from_sdp_("v=0\r\nc=IN IP4 192.0.2.2\r\nm=audio 40002 RTP/AVP 96\r\na=ptime:10\r\na=rtpmap:96 L16/16000/1\r\n",0,true));
 assert(!s.learn_remote_rtp_from_sdp_("v=0\r\nc=IN IP4 192.0.2.2\r\nm=audio 40002 RTP/AVP 96\r\na=ptime:10\r\na=rtpmap:96 L16/48000/1\r\n",0,true));
 s.refer_context_active_=false;
 assert(s.build_sdp_offer_().find("L16/48000/1")!=std::string::npos);
}
'''
    cpp = tmp_path / "sdp.cpp"
    cpp.write_text(source)
    binary = tmp_path / "sdp"
    subprocess.run(["g++", "-std=c++17", "-fsanitize=address,undefined", "-I", str(ROOT),
                    str(cpp), "-o", str(binary)], check=True, capture_output=True, text=True)
    subprocess.run([str(binary)], check=True, capture_output=True, text=True)
