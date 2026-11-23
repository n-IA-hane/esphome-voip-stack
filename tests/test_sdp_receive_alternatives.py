"""Standard negotiated mappings retain independently bounded receive alternatives."""

from production_sip_source import read_production_source
import json
import subprocess

import pytest

from test_sdp_receive_payloads import ROOT, method, sdp_fixture_source


# Historical frame14 with anonymized addresses. Unknown optional attributes
# remain in the raw witness; current negotiation does not interpret them.
HA_OFFER = (ROOT / "tests/fixtures/ha_pcm_alternatives_captured.sdp").read_text().replace("\n", "\r\n")


@pytest.fixture(scope="module")
def receive_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("standard-receive")
    source = sdp_fixture_source()
    source = "#include <iostream>\n#include <vector>\n" + source
    source = source.replace("struct SipTransport {", """struct SipTransport {
 int media_config_lock_=0;
 bool received_audio_format_(uint8_t,AudioFormat*,const uint8_t* =nullptr,size_t=0)const;
""", 1)
    source += "\n#define portENTER_CRITICAL(p) ((void)0)\n#define portEXIT_CRITICAL(p) ((void)0)\n"
    transport = read_production_source(ROOT / "esphome/components/voip_stack/sip_transport.cpp")
    source += method(transport, "bool SipTransport::received_audio_format_(")
    source += '\nconst std::string peer_offer=' + json.dumps(HA_OFFER) + ';\n'
    source += r'''
AudioFormat pcm(unsigned rate,unsigned ms){AudioFormat f;f.sample_rate=rate;f.frame_ms=ms;return f;}
void configure(SipTransport &s){
 s.offer_tx_formats_.count=5;
 s.offer_tx_formats_.formats[0]=pcm(16000,20);s.offer_tx_formats_.formats[1]=pcm(16000,10);
 auto g722=pcm(16000,20);g722.codec=AudioCodec::G722;
 s.offer_tx_formats_.formats[2]=g722;g722.frame_ms=10;s.offer_tx_formats_.formats[3]=g722;
 s.offer_tx_formats_.formats[4]=pcm(48000,10);
 s.offer_rx_formats_.count=6;
 s.offer_rx_formats_.formats[0]=pcm(48000,10);
 s.offer_rx_formats_.formats[1]=pcm(16000,20);s.offer_rx_formats_.formats[2]=pcm(16000,10);
 g722.frame_ms=20;s.offer_rx_formats_.formats[3]=g722;g722.frame_ms=10;s.offer_rx_formats_.formats[4]=g722;
 s.offer_rx_formats_.formats[5]=pcm(48000,20);
}
size_t occurrences(const std::string &text,const std::string &needle){
 size_t n=0,at=0;while((at=text.find(needle,at))!=std::string::npos){++n;at+=needle.size();}return n;
}
int main(int argc,char**argv){
 assert(argc==2);const std::string mode=argv[1];SipTransport s;configure(s);
 auto peer=peer_offer;
 if(mode=="unconfigured")s.offer_rx_formats_.count=1;
 assert(s.learn_remote_rtp_from_sdp_(peer,0xc0000214));
 assert(s.tx==(mode=="unconfigured"?pcm(48000,10):pcm(16000,20)));
 assert(s.tx_pt==(mode=="unconfigured"?100:96));
 assert(s.rx==pcm(48000,10)&&s.rx_pt==100);
 const auto answer=s.build_sdp_answer_();std::cout<<answer<<std::flush;
 assert(answer.find("a=ptime:10\r\n")!=std::string::npos);
 assert(answer.find("a=maxptime:"+(mode=="unconfigured"?std::string("10"):std::string("20"))+"\r\n")!=std::string::npos);
 assert(answer.find("a=x-voip-stack-")==std::string::npos);
 AudioFormat f;
 if(mode=="unconfigured"){
  assert(!s.accepted.find(96,&f));
  assert(occurrences(answer,"a=rtpmap:100 ")==1);
  return 0;
 }
 assert(occurrences(answer,"a=rtpmap:96 ")==1);
 assert(s.accepted.find(96,&f)&&f==pcm(16000,10));
 assert(s.accepted.find(100,&f)&&f==pcm(48000,10));
 assert(s.accepted.find(9,&f)); // Every mutually declared standard mapping is accepted.
 if(mode=="admission"){
  std::vector<uint8_t>packet(2037,0x42);
  assert(s.received_audio_format_(96,&f,packet.data(),320)&&f==pcm(16000,10));
  assert(s.received_audio_format_(96,&f,packet.data(),640)&&f==pcm(16000,20));
  for(unsigned time:{1U,5U,16U,20U}){
   assert(s.received_audio_format_(96,&f,packet.data(),32*time)&&f==pcm(16000,time));
   assert(unpack_audio_format(pack_audio_format(f))==f);
   assert(s.received_audio_format_(100,&f,packet.data(),96*time)&&f==pcm(48000,time));
   assert(unpack_audio_format(pack_audio_format(f))==f);
  }
  assert(!s.received_audio_format_(96,&f,packet.data(),672)); // Exceeds the advertised 20 ms maximum.
  assert(!s.received_audio_format_(96,&f,packet.data(),321));
  assert(s.received_audio_format_(100,&f,packet.data(),960)&&f==pcm(48000,10));
  assert(s.received_audio_format_(100,&f,packet.data(),1920)&&f==pcm(48000,20));
  assert(!s.received_audio_format_(100,&f,packet.data(),2016));
  assert(!s.received_audio_format_(100,&f,packet.data(),2037));
  assert(!s.received_audio_format_(126,&f,packet.data(),320));
 }
 const auto configured=configured_decoded_receive_formats(s.accepted,s.offer_rx_formats_,RTP_AUDIO_RECEIVE_PAYLOAD_BYTES);
 assert(audio_format_list_contains(configured,pcm(16000,10)));
 assert(audio_format_list_contains(configured,pcm(16000,20)));
 assert(audio_format_list_contains(configured,pcm(48000,20)));
 assert(!audio_format_list_contains(configured,pcm(16000,16)));
}
'''
    cpp = folder / "standard_receive.cpp"
    cpp.write_text(source)
    binary = folder / "standard_receive"
    built = subprocess.run(["g++", "-std=c++17", "-I", str(ROOT), str(cpp), "-o", str(binary)],
                           capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize("mode", ["offer", "admission", "unconfigured"])
def test_standard_receive_alternatives_and_independent_packet_capacity(receive_binary, mode):
    run = subprocess.run(["bash", "-c", 'ulimit -c 0; exec "$1" "$2"', "bash",
                          str(receive_binary), mode], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr
