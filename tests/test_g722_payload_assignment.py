"""Static G722 serialization and actual stock FreeSWITCH SDP ingestion."""
from pathlib import Path
import re
import subprocess

import pytest

from test_sdp_receive_payloads import ROOT, method, sdp_fixture_source


@pytest.fixture(scope="module")
def static_g722_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("g722-static-payload")
    code = '#include <array>\n#include <fstream>\n#include <iostream>\n' + sdp_fixture_source()
    code = code.replace('std::string build_sdp_offer_();',
        'std::string build_sdp_offer_();std::string build_sdp_capabilities_(uint32_t) const;')
    code += "\nuint32_t esp_random(){return 1;}\n"
    code += method((ROOT / "esphome/components/voip_stack/sip_sdp.cpp").read_text(),
                   "std::string SipTransport::build_sdp_capabilities_(")
    code += r'''
AudioFormat format(unsigned rate,unsigned ms,AudioCodec codec=AudioCodec::PCM){
 AudioFormat f;f.sample_rate=rate;f.frame_ms=ms;f.codec=codec;return f;
}
int main(int argc,char **argv){
 assert(argc>=2);const std::string mode=argv[1];SipTransport s;
 const auto g20=format(16000,20,AudioCodec::G722),g10=format(16000,10,AudioCodec::G722);
 s.offer_tx_formats_.count=s.offer_rx_formats_.count=4;
 s.offer_tx_formats_.formats[0]=s.offer_rx_formats_.formats[0]=format(16000,20);
 s.offer_tx_formats_.formats[1]=s.offer_rx_formats_.formats[1]=format(16000,10);
 s.offer_tx_formats_.formats[2]=s.offer_rx_formats_.formats[2]=g20;
 s.offer_tx_formats_.formats[3]=s.offer_rx_formats_.formats[3]=g10;
 if(mode=="d1")s.peer_directional_audio_v1_=true;
 if(mode=="sendonly")s.offer_rx_formats_.count=0;
 if(mode=="recvonly")s.offer_tx_formats_.count=0;
 if(mode=="capabilities"){std::cout<<s.build_sdp_capabilities_(0);return 0;}
 if(mode=="standard"||mode=="d1"||mode=="sendonly"||mode=="recvonly"){
  const auto offer=s.build_sdp_offer_();assert(!offer.empty());std::cout<<offer;return 0;
 }
 assert(argc==3);
 // Actual Native SDP accepted by stock FreeSWITCH1.10.12 after only PT98->9;
 // addresses are documentation addresses, media/codec fields remain intact.
 std::ifstream file(argv[2]);std::string body((std::istreambuf_iterator<char>(file)),{});assert(!body.empty());
 std::string wire;for(char c:body){if(c=='\n')wire+='\r';wire+=c;}
 const bool dynamic=mode.find("dynamic")!=std::string::npos,answer=mode.rfind("answer",0)==0||mode=="stock-answer";
 if(dynamic){auto at=wire.find(" 9 120");assert(at!=std::string::npos);wire.replace(at,6," 98 120");at=wire.find("rtpmap:9 ");assert(at!=std::string::npos);wire.replace(at,9,"rtpmap:98 ");}
 s.offer_tx_formats_.count=s.offer_rx_formats_.count=2;
 s.offer_tx_formats_.formats[0]=s.offer_rx_formats_.formats[0]=g20;
 s.offer_tx_formats_.formats[1]=s.offer_rx_formats_.formats[1]=g10;
 if(answer){const auto own=s.build_sdp_offer_();assert(own.find("a=rtpmap:9 G722/8000/1\r\n")!=std::string::npos);}
 assert(s.learn_remote_rtp_from_sdp_(wire,0,answer));
 assert(s.tx==g10&&s.rx==g20&&s.tx_pt==(dynamic?98:9)&&s.rx_pt==(answer?9:dynamic?98:9));
 assert(s.accepted.count==1&&s.accepted.entries[0].payload_type==s.rx_pt);
 assert(audio_format_rtp_clock_rate(s.tx)==8000&&s.tx.sample_rate==16000);
 const auto response=s.build_sdp_answer_();assert(response.find("a=rtpmap:"+std::to_string(s.tx_pt)+" G722/8000/1")!=std::string::npos);
}
'''
    cpp = folder / "static.cpp"
    cpp.write_text(code)
    binary = folder / "static"
    built = subprocess.run(["g++", "-std=c++17", "-fno-pie", "-no-pie", "-fsanitize=address,undefined",
                            "-I", str(ROOT), str(cpp), "-o", str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize("mode", ["standard", "d1", "sendonly", "recvonly", "capabilities"])
def test_g722_uses_one_static_payload_across_duration_aliases(static_g722_binary, mode):
    result = subprocess.run([static_g722_binary, mode], capture_output=True, text=True, check=True)
    mappings = re.findall(r"^a=rtpmap:(\d+) ([^\r\n]+)", result.stdout, re.MULTILINE)
    assert len(mappings) == len(dict(mappings)) == 2
    assert dict(mappings)["9"] == "G722/8000/1"
    audio = next(line.split()[3:] for line in result.stdout.splitlines() if line.startswith("m=audio "))
    assert len(audio) == len(set(audio)) == 2 and "9" in audio
    assert sum(value == "G722/8000/1" for _, value in mappings) == 1
    assert ("a=x-voip-stack-flow:9 sendrecv" in result.stdout) == (mode in {"d1", "capabilities"})
    if mode == "capabilities":
        for direction in ("tx", "rx"):
            assert f"a=x-voip-stack-audio-{direction}:L16/16000/1/20,L16/16000/1/10,G722/8000/1/20,G722/8000/1/10" in result.stdout


@pytest.mark.parametrize("mode", ["incoming-static", "incoming-dynamic", "answer-static", "answer-dynamic", "stock-answer"])
def test_actual_stock_witness_preserves_static_and_dynamic_received_mapping(static_g722_binary, mode):
    witness = Path(__file__).parent / "fixtures" / ("freeswitch_vanilla_g722_answer.sdp"
        if mode == "stock-answer" else "freeswitch_vanilla_native_static9.sdp")
    result = subprocess.run([static_g722_binary, mode, witness], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
