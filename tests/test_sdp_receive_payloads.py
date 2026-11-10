"""Run production offer generation and SDP parsing without ESP hardware."""
from pathlib import Path
import json
import re
import subprocess

import pytest

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
#define USE_ESPHOME_VOIP_STACK_PCM
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
using namespace esphome::voip_stack;
'''
    for signature in ["std::string trim_copy(", "bool parse_decimal_u32(",
                      "bool parse_rtpmap_format(", "bool parse_audio_media_line("]:
        source += method(parser, signature) + "\n"
    from test_refer_dialog_runtime import block, negotiated_fixture_source
    header = (base / "sip_transport.h").read_text()
    source += "using SipSignalingString = std::string;\nconstexpr size_t kMaxSdpMediaLines = 8;\n"
    source += block(header, "struct RemoteMediaShape {") + ";\n"
    source += r'''
struct RetainedCall { struct{AudioFormat tx;}media;AudioFormat selected_tx_format_; };
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
 AudioFormat &selected_tx_format_=tx,&selected_rx_format_=rx;
 uint8_t &rtp_tx_payload_type_=tx_pt,&rtp_rx_payload_type_=rx_pt;
 RtpAudioPayloads &negotiated_rx_payloads_=accepted;
 void local_ip_for_peer_(uint32_t,std::string *out) const {*out="192.0.2.1";}
 void capture_remote_media_shape_(const std::string&);
 std::string rejected_media_answer_(const std::string&) const;
 void get_media_config_(AudioFormat*a,AudioFormat*b,uint8_t*c,uint8_t*d,RtpAudioPayloads*e=nullptr)const{if(a)*a=tx;if(b)*b=rx;if(c)*c=tx_pt;if(d)*d=rx_pt;if(e)*e=accepted;}
 void set_media_config_(const AudioFormat&a,const AudioFormat&b,uint8_t c,uint8_t d,const RtpAudioPayloads*e){tx=a;rx=b;tx_pt=c;rx_pt=d;accepted=*e;}
 std::string wrap_sdp_envelope_(const std::string&,const std::string&,const std::string&,const std::string&,const AudioSdpFacts&,uint8_t,const char * = "sendrecv") const;
 std::string build_sdp_offer_();
 std::string build_sdp_answer_() const;
 bool learn_remote_rtp_from_sdp_(const std::string&,uint32_t,bool=false);
};
'''
    if "struct AudioSdpDirection {" in header:
        # Preserve the production conditional DTMF arity and final hold output.
        direction = block(header, "struct AudioSdpDirection {") + ";\n"
        source = source.replace("struct SipTransport {", "struct SipTransport {\n" + direction + r'''
 std::atomic<bool> audio_send_enabled_{true},audio_receive_enabled_{true},audio_receive_reset_pending_{false};
 std::atomic<bool> rtp_source_reacquire_pending_{false};
''')
        source = source.replace("struct SipTransport {", "struct SipTransport {\n" +
                                block(header, "bool audio_receive_paused() const override").replace(" override", ""))
        for old, marker, end in (
            ("std::string build_sdp_answer_() const;", "  std::string build_sdp_answer_(", ") const;"),
            ("bool learn_remote_rtp_from_sdp_(const std::string&,uint32_t,bool=false);",
             "  bool learn_remote_rtp_from_sdp_(", ");"),
        ):
            start = header.index(marker)
            declaration = header[start:header.index(end, start) + len(end)]
            source = source.replace(old, declaration.strip())
        source = source.replace(
            "{tx=a;rx=b;tx_pt=c;rx_pt=d;accepted=*e;}",
            "{tx=a;rx=b;tx_pt=c;rx_pt=d;accepted=*e;"
            "audio_send_enabled_=a.is_valid();audio_receive_enabled_=b.is_valid();}",
        )
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
    return negotiated_fixture_source(source, sdp=True)


def test_standard_receive_payload_contract(tmp_path):
    source = sdp_fixture_source()
    source += r'''
AudioFormat fmt(unsigned rate){AudioFormat f;f.sample_rate=rate;f.frame_ms=10;return f;}
int main(){
 SipTransport s;s.offer_tx_formats_.count=1;s.offer_tx_formats_.formats[0]=fmt(16000);
 s.offer_rx_formats_.count=2;s.offer_rx_formats_.formats[0]=fmt(48000);s.offer_rx_formats_.formats[1]=fmt(16000);
 auto offer=s.build_sdp_offer_();assert(offer.find("a=x-voip-stack-flow:")==std::string::npos);
 assert(offer.find("a=x-voip-stack-independent-ptime:1\r\n")!=std::string::npos);
 assert(offer.find("L16/48000/1")==std::string::npos);
 assert(offer.find("L16/16000/1")!=std::string::npos);
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
 assert(s.rx.sample_rate==16000&&s.accepted.find(96,&f)&&f.sample_rate==16000);
 assert(!s.accepted.find(97,&f));
 assert(!s.accepted.find(118,&f));
 assert(!s.learn_remote_rtp_from_sdp_("v=0\r\nc=IN IP4 192.0.2.2\r\nm=audio 40002 RTP/AVP 119\r\na=ptime:10\r\na=rtpmap:119 L16/32000/1\r\n",0,true));
 // Transmit preference follows the explicit local format order.
 s.offer_tx_formats_.formats[1]=fmt(48000);s.offer_tx_formats_.count=2;
 assert(s.learn_remote_rtp_from_sdp_(base+maps,0));assert(s.tx.sample_rate==16000);
 s.offer_tx_formats_.count=1;
 // A peer selecting one common codec retains the old 16/16 behavior.
 assert(s.learn_remote_rtp_from_sdp_("v=0\r\nc=IN IP4 192.0.2.2\r\nm=audio 40002 RTP/AVP 118\r\na=ptime:10\r\na=rtpmap:118 L16/16000/1\r\n",0,true));
 assert(s.tx.sample_rate==16000&&s.rx.sample_rate==16000&&s.accepted.count==1);
 // D1 may advertise RX-only alternatives; REFER still retains its transmitter.
 s.peer_directional_audio_v1_=true;
 assert(s.build_sdp_offer_().find("L16/48000/1")!=std::string::npos);
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


@pytest.fixture(scope="module")
def answer_ptime_binary(tmp_path_factory):
    source = sdp_fixture_source() + r'''
AudioFormat pcm(unsigned rate,unsigned ptime){AudioFormat f;f.sample_rate=rate;f.frame_ms=ptime;return f;}
int main(int argc,char **argv){
 assert(argc==2);const std::string scenario=argv[1];SipTransport s;
 s.offer_tx_formats_.count=s.offer_rx_formats_.count=2;
 s.offer_tx_formats_.formats[0]=s.offer_rx_formats_.formats[0]=pcm(16000,10);
 s.offer_tx_formats_.formats[1]=s.offer_rx_formats_.formats[1]=pcm(16000,20);
 unsigned rate=16000,ptime=20;bool expected=false,configured=true;
 if(scenario=="same-time"){ptime=10;expected=true;}
 else if(scenario=="supported-time")expected=true;
 else if(scenario=="unsupported-time"){ptime=32;configured=false;expected=true;}
 else if(scenario=="unoffered-mapping"){
  rate=32000;
  s.udp_max_payload_=1400; // 32 kHz/20 ms is 1280 bytes, within this configured LAN budget.
 }else{
  assert(scenario=="unsafe-payload");rate=48000;configured=false;expected=true;
  s.offer_tx_formats_.count=s.offer_rx_formats_.count=4;
  s.offer_tx_formats_.formats[2]=s.offer_rx_formats_.formats[2]=pcm(rate,10);
  s.offer_tx_formats_.formats[3]=s.offer_rx_formats_.formats[3]=pcm(rate,20);
  assert(pcm(rate,20).nominal_rtp_payload_bytes()>s.udp_max_payload_);
 }
 const auto offer=s.build_sdp_offer_();assert(!offer.empty());
 assert(offer.find("a=ptime:10\r\n")!=std::string::npos);
 assert(offer.find("L16/32000/1")==std::string::npos);
 const uint8_t pt=scenario=="unsafe-payload"?97:96;
 assert(offer.find("a=rtpmap:"+std::to_string(pt)+" L16/"+
                   std::to_string(scenario=="unsafe-payload"?48000:16000)+"/1\r\n")!=std::string::npos);
 for(uint8_t i=0;i<s.local_offered_tx_formats_.count;++i)assert(s.local_offered_tx_formats_.formats[i].frame_ms==10);
 for(uint8_t i=0;i<s.local_offered_rx_payloads_.count;++i)assert(s.local_offered_rx_payloads_.entries[i].format.frame_ms==10);
 const auto snapshots=s.local_offered_rx_payloads_;
 if(scenario=="unoffered-mapping"){
  // Later capability changes cannot authorize a mapping absent from this offer.
  s.offer_tx_formats_.count=s.offer_rx_formats_.count=3;
  s.offer_tx_formats_.formats[2]=s.offer_rx_formats_.formats[2]=pcm(rate,20);
 }
 AudioFormat match;const auto candidate=pcm(rate,ptime);
 assert(audio_format_list_match_udp_safe(s.offer_tx_formats_,candidate,&match,s.udp_max_payload_)==configured);
 assert(audio_format_list_match_udp_safe(s.offer_rx_formats_,candidate,&match,s.udp_max_payload_)==configured);
 // Rejections must preserve an existing media configuration and endpoint.
 s.tx=s.rx=pcm(16000,10);s.tx_pt=s.rx_pt=110;
 assert(s.accepted.add(110,s.rx));
 s.remote_rtp_ip_v4_=ntohl(inet_addr("192.0.2.9"));s.remote_rtp_port_=45000;
 const auto prior_tx=s.tx,prior_rx=s.rx;const auto prior_maps=s.accepted;
 const auto prior_ip=s.remote_rtp_ip_v4_.load();const auto prior_port=s.remote_rtp_port_.load();
 const std::string answer="v=0\r\no=- 2 2 IN IP4 192.0.2.2\r\ns=-\r\nc=IN IP4 192.0.2.2\r\nt=0 0\r\n"
  "m=audio 40002 RTP/AVP "+std::to_string(pt)+"\r\na=rtpmap:"+std::to_string(pt)+
  " L16/"+std::to_string(rate)+"/1\r\na=ptime:"+std::to_string(ptime)+"\r\na=sendrecv\r\n";
 const bool accepted=s.learn_remote_rtp_from_sdp_(answer,ntohl(inet_addr("192.0.2.2")),true);
 assert(accepted==expected);
 assert(s.local_offered_rx_payloads_==snapshots);
 if(accepted){
  // A hint cannot authorize undeclared or oversized packets. Prefer the
  // configured safe10ms alternative when the requested duration is unusable.
  const auto expected_tx=pcm(rate,configured?ptime:10);
  assert(s.tx==expected_tx&&s.tx.nominal_rtp_payload_bytes()<=s.udp_max_payload_);
  assert(s.rx==pcm(rate,10)&&s.tx_pt==pt&&s.rx_pt==pt);
  assert(s.accepted.count==1&&s.accepted.find(pt,&match)&&match==pcm(rate,10));
  assert(s.remote_rtp_ip_v4_==ntohl(inet_addr("192.0.2.2"))&&s.remote_rtp_port_==40002);
 }else{
  assert(s.tx==prior_tx&&s.rx==prior_rx&&s.tx_pt==110&&s.rx_pt==110&&s.accepted==prior_maps);
  assert(s.remote_rtp_ip_v4_==prior_ip&&s.remote_rtp_port_==prior_port);
 }
}
'''
    folder = tmp_path_factory.mktemp("answer-ptime")
    cpp = folder / "answer.cpp"
    cpp.write_text(source)
    binary = folder / "answer"
    built = subprocess.run(
        ["g++", "-std=c++17", "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
         "-I", str(ROOT), str(cpp), "-o", str(binary)],
        capture_output=True, text=True,
    )
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize("scenario", [
    "same-time", "supported-time", "unsupported-time", "unoffered-mapping", "unsafe-payload",
])
def test_answer_packet_time_keeps_offered_mapping_and_config_budget(answer_ptime_binary, scenario):
    result = subprocess.run([answer_ptime_binary, scenario], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


def test_independent_ptime_capability_keeps_only_legacy_d1_coupled(tmp_path):
    source = sdp_fixture_source() + r'''
AudioFormat pcm(unsigned time){AudioFormat f;f.frame_ms=time;return f;}
int main(){
 for(const std::string mode:{"ordinary","legacy","new","wrong-version","session","video"}){
  SipTransport s;s.offer_tx_formats_.count=s.offer_rx_formats_.count=2;
  s.offer_tx_formats_.formats[0]=s.offer_rx_formats_.formats[0]=pcm(20);
  s.offer_tx_formats_.formats[1]=s.offer_rx_formats_.formats[1]=pcm(10);
  const auto standalone=s.build_sdp_offer_();
  assert(standalone.find("a=x-voip-stack-independent-ptime:1\r\n")!=std::string::npos);
  std::string peer="v=0\r\nc=IN IP4 192.0.2.2\r\n";
  if(mode=="session")peer+="a=x-voip-stack-independent-ptime:1\r\n";
  peer+="m=audio 40002 RTP/AVP 96\r\na=rtpmap:96 L16/16000/1\r\na=ptime:10\r\n";
  if(mode!="ordinary")peer+="a=x-voip-stack-flow:96 sendrecv\r\n";
  if(mode=="new")peer+="a=x-voip-stack-independent-ptime:1\r\n";
  if(mode=="wrong-version")peer+="a=x-voip-stack-independent-ptime:0\r\n";
  if(mode=="video")peer+="m=video 40004 RTP/AVP 97\r\na=x-voip-stack-independent-ptime:1\r\n";
  assert(s.learn_remote_rtp_from_sdp_(peer,0));
  const unsigned receive_time=mode=="ordinary"||mode=="new"?20:10;
  assert(s.tx==pcm(10)&&s.rx==pcm(receive_time));
  const auto answer=s.build_sdp_answer_();
  assert(answer.find("a=ptime:"+std::to_string(receive_time)+"\r\n")!=std::string::npos);
  assert(answer.find("a=x-voip-stack-independent-ptime:1\r\n")!=std::string::npos);
 }
}
'''
    cpp = tmp_path / "compat.cpp"
    cpp.write_text(source)
    binary = tmp_path / "compat"
    subprocess.run(["g++", "-std=c++17", "-I", str(ROOT), str(cpp), "-o", str(binary)], check=True)
    subprocess.run([binary], check=True, capture_output=True, text=True)


@pytest.mark.parametrize("peer_ptime", [10, 20])
def test_actual_ha_bridge_receiver_hint_controls_only_native_tx(tmp_path, peer_ptime):
    # Captured HA bridge offer, with addresses and origin identifier anonymized.
    peer = (ROOT / "tests/fixtures/ha_independent_bridge_offer.sdp").read_text()
    peer = peer.replace("a=ptime:10\n", f"a=ptime:{peer_ptime}\n").replace("\n", "\r\n")
    source = sdp_fixture_source() + "\nint main(){const std::string peer=" + json.dumps(peer) + r''';
 SipTransport s;AudioFormat f;f.sample_rate=16000;f.frame_ms=20;
 s.offer_tx_formats_.count=4;s.offer_tx_formats_.formats[0]=f;f.frame_ms=10;s.offer_tx_formats_.formats[1]=f;
 f.codec=AudioCodec::G722;f.frame_ms=20;s.offer_tx_formats_.formats[2]=f;f.frame_ms=10;s.offer_tx_formats_.formats[3]=f;
 s.offer_rx_formats_.count=5;f.codec=AudioCodec::PCM;f.sample_rate=48000;f.frame_ms=10;s.offer_rx_formats_.formats[0]=f;
 f.sample_rate=16000;f.frame_ms=20;s.offer_rx_formats_.formats[1]=f;f.frame_ms=10;s.offer_rx_formats_.formats[2]=f;
 f.codec=AudioCodec::G722;f.frame_ms=20;s.offer_rx_formats_.formats[3]=f;f.frame_ms=10;s.offer_rx_formats_.formats[4]=f;
 assert(s.learn_remote_rtp_from_sdp_(peer,0xc0000201));
 assert(s.tx.codec==AudioCodec::PCM&&s.tx.sample_rate==16000&&s.tx.frame_ms==PEER_PTIME&&s.tx_pt==96);
 assert(s.rx.codec==AudioCodec::PCM&&s.rx.sample_rate==48000&&s.rx.frame_ms==10&&s.rx_pt==100);
 assert(s.build_sdp_answer_().find("a=ptime:10\r\n")!=std::string::npos);
}
'''.replace("PEER_PTIME", str(peer_ptime))
    cpp = tmp_path / "bridge.cpp"
    cpp.write_text(source)
    binary = tmp_path / "bridge"
    subprocess.run(["g++", "-std=c++17", "-I", str(ROOT), str(cpp), "-o", str(binary)], check=True)
    subprocess.run([binary], check=True, capture_output=True, text=True)


@pytest.fixture(scope="module")
def offer_modes_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("offer-modes")
    cpp = folder / "offer.cpp"
    cpp.write_text(sdp_fixture_source() + r'''
#include <iostream>
int main(int argc,char**argv){
 assert(argc==2);const std::string mode=argv[1];SipTransport s;AudioFormat f;
 f.sample_rate=16000;f.frame_ms=20;
 s.offer_tx_formats_.count=4;s.offer_tx_formats_.formats[0]=f;f.frame_ms=10;s.offer_tx_formats_.formats[1]=f;
 f.codec=AudioCodec::G722;f.frame_ms=20;s.offer_tx_formats_.formats[2]=f;f.frame_ms=10;s.offer_tx_formats_.formats[3]=f;
 s.offer_rx_formats_.count=5;f.codec=AudioCodec::PCM;f.sample_rate=48000;f.frame_ms=10;s.offer_rx_formats_.formats[0]=f;
 f.sample_rate=16000;f.frame_ms=20;s.offer_rx_formats_.formats[1]=f;f.frame_ms=10;s.offer_rx_formats_.formats[2]=f;
 f.codec=AudioCodec::G722;f.frame_ms=20;s.offer_rx_formats_.formats[3]=f;f.frame_ms=10;s.offer_rx_formats_.formats[4]=f;
 if(mode=="d1")s.peer_directional_audio_v1_=true;
 if(mode=="sendonly")s.offer_rx_formats_.count=0;
 if(mode=="recvonly")s.offer_tx_formats_.count=0;
 if(mode=="asymmetric"){
  s.offer_tx_formats_.count=s.offer_rx_formats_.count=1;
  f.codec=AudioCodec::PCM;f.sample_rate=16000;f.frame_ms=20;s.offer_tx_formats_.formats[0]=f;
  f.frame_ms=10;s.offer_rx_formats_.formats[0]=f;
 }
 const auto offer=s.build_sdp_offer_();assert(!offer.empty());
 if(mode=="asymmetric"){
  assert(s.local_offered_tx_formats_.count==1&&s.local_offered_tx_formats_.formats[0].frame_ms==20);
  assert(s.local_offered_rx_payloads_.count==1&&s.local_offered_rx_payloads_.entries[0].format.frame_ms==10);
 }
 std::cout<<offer;
}
''')
    binary = folder / "offer"
    subprocess.run(["g++", "-std=c++17", "-I", str(ROOT), str(cpp), "-o", str(binary)], check=True)
    return binary


@pytest.mark.parametrize("mode,wide,ptime,direction", [
    ("standard", False, 20, "sendrecv"), ("d1", True, 10, "sendrecv"),
    ("sendonly", False, 20, "sendonly"), ("recvonly", True, 10, "recvonly"),
    ("asymmetric", False, 10, "sendrecv"),
])
def test_offer_modes_share_only_declared_rtp_mappings(offer_modes_binary, mode, wide, ptime, direction):
    result = subprocess.run([offer_modes_binary, mode], capture_output=True, text=True, check=True)
    mappings = dict(re.findall(r"^a=rtpmap:(\d+) ([^\r\n]+)", result.stdout, re.MULTILINE))
    expected = {"L16/16000/1"} if mode == "asymmetric" else {"L16/16000/1", "G722/8000/1"}
    if wide:
        expected.add("L16/48000/1")
    assert set(mappings.values()) == expected
    assert f"a=ptime:{ptime}\n" in result.stdout and f"a={direction}\n" in result.stdout
    flows = dict(re.findall(r"^a=x-voip-stack-flow:(\d+) ([^\r\n]+)", result.stdout, re.MULTILINE))
    assert bool(flows) is (mode == "d1")
    if mode == "d1":
        assert flows[next(pt for pt, encoding in mappings.items() if encoding == "L16/48000/1")] == "recv"
