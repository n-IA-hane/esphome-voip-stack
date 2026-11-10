"""D1 retains configured PCM receive alternatives from the captured HA offer."""
import json
import subprocess

import pytest

from test_sdp_receive_payloads import ROOT, method, sdp_fixture_source


# HA offer from wire frame 14. Only the origin identifier and IP addresses
# are anonymized; payload IDs, roles, packet times and video are unchanged.
HA_OFFER = """v=0
o=- 1 0 IN IP4 192.0.2.20
s=VoIP Stack
c=IN IP4 192.0.2.20
t=0 0
m=audio 40004 RTP/AVP 96 100 9 101 105 98 8 0 97
a=rtpmap:96 L16/16000/1
a=x-voip-stack-flow:96 sendrecv
a=rtpmap:100 L16/48000/1
a=x-voip-stack-flow:100 sendrecv
a=rtpmap:9 G722/8000/1
a=x-voip-stack-flow:9 sendrecv
a=rtpmap:101 L16/32000/1
a=x-voip-stack-flow:101 sendrecv
a=rtpmap:105 L16/8000/1
a=x-voip-stack-flow:105 sendrecv
a=rtpmap:98 OPUS/48000/2
a=fmtp:98 stereo=0;sprop-stereo=0;maxaveragebitrate=28000
a=x-voip-stack-flow:98 sendrecv
a=rtpmap:8 PCMA/8000/1
a=x-voip-stack-flow:8 sendrecv
a=rtpmap:0 PCMU/8000/1
a=x-voip-stack-flow:0 sendrecv
a=rtpmap:97 telephone-event/8000
a=fmtp:97 0-15
a=x-voip-stack-independent-ptime:1
a=ptime:20
a=maxptime:32
a=sendrecv
m=video 40008 RTP/AVP 26
a=rtpmap:26 JPEG/90000
a=imageattr:26 send * recv *
a=framerate:10
a=rtcp:40009
a=recvonly
""".replace("\n", "\r\n")


@pytest.fixture(scope="module")
def d1_receive_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("d1-receive")
    source = sdp_fixture_source()
    source = "#include <iostream>\n#include <vector>\n" + source
    source = source.replace("struct SipTransport {", """struct SipTransport {
 int media_config_lock_=0;
 bool received_audio_format_(uint8_t,AudioFormat*,const uint8_t* =nullptr,size_t=0)const;
""", 1)
    source += "\n#define portENTER_CRITICAL(p) ((void)0)\n#define portEXIT_CRITICAL(p) ((void)0)\n"
    transport = (ROOT / "esphome/components/voip_stack/sip_transport.cpp").read_text()
    source += method(transport, "bool SipTransport::received_audio_format_(")
    source += '\nconst std::string captured_offer=' + json.dumps(HA_OFFER) + ';\n'
    source += r'''
AudioFormat pcm(unsigned rate,unsigned ms){AudioFormat f;f.sample_rate=rate;f.frame_ms=ms;return f;}
void configure(SipTransport &s){
 s.offer_tx_formats_.count=4;
 s.offer_tx_formats_.formats[0]=pcm(16000,20);s.offer_tx_formats_.formats[1]=pcm(16000,10);
 auto g722=pcm(16000,20);g722.codec=AudioCodec::G722;
 s.offer_tx_formats_.formats[2]=g722;g722.frame_ms=10;s.offer_tx_formats_.formats[3]=g722;
 s.offer_rx_formats_.count=5;
 s.offer_rx_formats_.formats[0]=pcm(48000,10);
 s.offer_rx_formats_.formats[1]=pcm(16000,20);s.offer_rx_formats_.formats[2]=pcm(16000,10);
 g722.frame_ms=20;s.offer_rx_formats_.formats[3]=g722;g722.frame_ms=10;s.offer_rx_formats_.formats[4]=g722;
}
size_t occurrences(const std::string &text,const std::string &needle){
 size_t n=0,at=0;while((at=text.find(needle,at))!=std::string::npos){++n;at+=needle.size();}return n;
}
int main(int argc,char**argv){
 assert(argc==2);const std::string mode=argv[1];SipTransport s;configure(s);
 auto peer=captured_offer;
 if(mode=="tx-only"){
  const auto at=peer.find("a=x-voip-stack-flow:96 sendrecv");
  peer.replace(at,std::string("a=x-voip-stack-flow:96 sendrecv").size(),"a=x-voip-stack-flow:96 recv");
 }
 if(mode=="unconfigured")s.offer_rx_formats_.count=1;
 assert(s.learn_remote_rtp_from_sdp_(peer,0xc0000214));
 assert(s.remote_directional_audio_v1_);
 assert(s.tx==pcm(16000,20)&&s.tx_pt==96);
 assert(s.rx==pcm(48000,10)&&s.rx_pt==100);
 if(mode=="builder"){
  // Isolate answer serialization from the parser's receive-admission decision.
  assert(s.accepted.add(96,pcm(16000,20)));
 }
 const auto answer=s.build_sdp_answer_();std::cout<<answer<<std::flush;
 assert(answer.find("a=ptime:10\r\n")!=std::string::npos);
 assert(answer.find("a=maxptime:10\r\n")!=std::string::npos);
 assert(answer.find("a=x-voip-stack-flow:100 recv\r\n")!=std::string::npos);
 assert(occurrences(answer,"a=rtpmap:96 ")==1);
 AudioFormat f;
 if(mode=="tx-only"||mode=="unconfigured"){
  assert(!s.accepted.find(96,&f));
  assert(!s.received_audio_format_(96,&f));
  assert(answer.find("a=x-voip-stack-flow:96 send\r\n")!=std::string::npos);
  return 0;
 }
 if(mode=="builder"){
  assert(answer.find("a=x-voip-stack-flow:96 sendrecv\r\n")!=std::string::npos);
  return 0;
 }
 if(mode=="admission"){
  std::vector<uint8_t> packet(1920,0x42);
  assert(s.received_audio_format_(96,&f,packet.data(),320)&&f==pcm(16000,10));
  assert(s.received_audio_format_(96,&f,packet.data(),640)&&f==pcm(16000,20));
  assert(!s.received_audio_format_(96,&f,packet.data(),512)); // 16 ms is not configured.
  assert(!s.received_audio_format_(96,&f,packet.data(),321));
  assert(s.received_audio_format_(100,&f,packet.data(),960)&&f==pcm(48000,10));
  assert(!s.received_audio_format_(100,&f,packet.data(),1920)); // 48 kHz/20 ms exceeds 1200.
  assert(!s.received_audio_format_(126,&f,packet.data(),320));
  return 0;
 }
 assert(mode=="captured-offer");
 assert(s.accepted.find(96,&f)&&f==pcm(16000,20));
 assert(s.accepted.find(100,&f)&&f==pcm(48000,10));
 assert(!s.accepted.find(9,&f)); // No expansion of D1 G722 alternatives.
 assert(answer.find("a=x-voip-stack-flow:96 sendrecv\r\n")!=std::string::npos);
 const auto configured=configured_decoded_receive_formats(s.accepted,s.offer_rx_formats_,1200);
 assert(audio_format_list_contains(configured,pcm(16000,10)));
 assert(audio_format_list_contains(configured,pcm(16000,20)));
 assert(!audio_format_list_contains(configured,pcm(16000,16)));
}
'''
    cpp = folder / "d1_receive.cpp"
    cpp.write_text(source)
    binary = folder / "d1_receive"
    built = subprocess.run(["g++", "-std=c++17", "-I", str(ROOT), str(cpp), "-o", str(binary)],
                           capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize("mode", ["captured-offer", "builder", "admission", "tx-only", "unconfigured"])
def test_d1_configured_pcm_receive_alternatives(d1_receive_binary, mode):
    run = subprocess.run(["bash", "-c", 'ulimit -c 0; exec "$1" "$2"', "bash",
                          str(d1_receive_binary), mode], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr
