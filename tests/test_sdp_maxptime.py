"""Exercise published receive bounds and actual transmit selection."""

import subprocess

import pytest

from test_sdp_receive_payloads import ROOT, method, sdp_fixture_source


@pytest.fixture(scope="module")
def maxptime_binary(tmp_path_factory):
    source = sdp_fixture_source()
    source = source.replace("struct SipTransport {", "struct SipTransport {\n std::string build_sdp_capabilities_(uint32_t) const;")
    source = "#include <array>\nunsigned esp_random(){return 7;}\n" + source
    source += method((ROOT / "esphome/components/voip_stack/sip_sdp.cpp").read_text(),
                     "std::string SipTransport::build_sdp_capabilities_(")
    source += r'''
#include <iostream>
AudioFormat pcm(unsigned rate,unsigned ms){AudioFormat f;f.sample_rate=rate;f.frame_ms=ms;return f;}
void defaults(SipTransport&s){
 s.offer_tx_formats_.count=2;s.offer_tx_formats_.formats[0]=pcm(16000,20);s.offer_tx_formats_.formats[1]=pcm(16000,10);
 s.offer_rx_formats_.count=3;s.offer_rx_formats_.formats[0]=pcm(48000,10);s.offer_rx_formats_.formats[1]=pcm(16000,20);s.offer_rx_formats_.formats[2]=pcm(16000,10);
}
int main(int argc,char**argv){
 assert(argc>=3);SipTransport s;defaults(s);const std::string mode=argv[1];
 if(mode=="emit"){
  const std::string which=argv[2];
  if(which=="pcm32-only"||which=="pcm32-ordinary"){
   s.offer_tx_formats_.count=s.offer_rx_formats_.count=1;
   s.offer_tx_formats_.formats[0]=s.offer_rx_formats_.formats[0]=pcm(32000,16);
   if(which=="pcm32-ordinary"){
    // A48k receive-only local alternative is omitted in ordinary sendrecv.
    s.offer_rx_formats_.count=2;s.offer_rx_formats_.formats[1]=pcm(48000,10);
   }
   std::cout<<s.build_sdp_offer_();
  }else if(which=="historical32-impossible"){
   s.peer_directional_audio_v1_=true;
   s.offer_rx_formats_.formats[2]=pcm(32000,16);
   std::cout<<s.build_sdp_offer_();
  }else if(which=="packed24"){
   s.offer_tx_formats_.count=s.offer_rx_formats_.count=1;
   auto format=pcm(16000,20);format.pcm_format=PcmFormat::S24LE_IN_S32;
   s.offer_tx_formats_.formats[0]=s.offer_rx_formats_.formats[0]=format;
   std::cout<<s.build_sdp_offer_();
  }else if(which=="d1-limited-hint"||which=="d1-impossible"||which=="standard-without48"||which=="options-impossible"||which=="answer-impossible"){
   s.offer_rx_formats_.formats[0]=pcm(16000,20);s.offer_rx_formats_.formats[1]=pcm(16000,10);s.offer_rx_formats_.formats[2]=pcm(48000,10);
   if(which!="d1-limited-hint"){s.offer_rx_formats_.count=2;s.offer_rx_formats_.formats[1]=pcm(48000,10);}
   s.peer_directional_audio_v1_=which!="standard-without48";
   if(which=="options-impossible")std::cout<<s.build_sdp_capabilities_(0);
   else if(which=="answer-impossible"){
    s.tx=s.rx=pcm(16000,20);s.tx_pt=s.rx_pt=96;s.accepted.add(96,s.rx);s.accepted.add(97,pcm(48000,10));s.remote_directional_audio_v1_=true;
    std::cout<<s.build_sdp_answer_();
   }else std::cout<<s.build_sdp_offer_();
  }else if(which=="standard")std::cout<<s.build_sdp_offer_();
  else if(which=="d1"){s.peer_directional_audio_v1_=true;std::cout<<s.build_sdp_offer_();}
  else if(which=="options"||which=="options-no-hint"){
   if(which=="options-no-hint"){s.offer_rx_formats_.formats[2]=pcm(16000,20);}
   if(which=="options-no-hint")s.offer_rx_formats_.count=2;
   std::cout<<s.build_sdp_capabilities_(0);
  }else if(which=="sendonly"){
   s.offer_rx_formats_.count=0;std::cout<<s.build_sdp_offer_();
  }else{
   s.tx=pcm(16000,20);s.rx=pcm(which=="answer-mixed"?48000:16000,which=="answer-mixed"?10:20);
   s.tx_pt=97;s.rx_pt=96;s.accepted.add(96,s.rx);s.remote_directional_audio_v1_=true;
   std::cout<<s.build_sdp_answer_();
  }
  assert(pcm(48000,20).nominal_rtp_payload_bytes()==1920);
  assert(audio_format_rtp_encoding(pcm(48000,20),1200)==nullptr);return 0;
 }
 assert(mode=="select"&&argc==6);const bool answer=std::string(argv[2])=="answer";
 const std::string maximum=argv[3],hint=argv[4];const unsigned expected=std::stoul(argv[5]);
 if(answer)s.build_sdp_offer_();
 s.tx=pcm(16000,10);s.rx=pcm(48000,10);s.tx_pt=s.rx_pt=112;s.accepted.add(112,s.rx);
 const auto prior=s.accepted;const auto prior_tx=s.tx,prior_rx=s.rx;
 s.remote_rtp_ip_v4_=0xc0000209;s.remote_rtp_port_=45000;
 std::string body="v=0\r\nc=IN IP4 192.0.2.2\r\nm=audio 40002 RTP/AVP 96\r\na=rtpmap:96 L16/16000/1\r\n";
 if(maximum!="absent")body+="a=maxptime:"+maximum+"\r\n";
 if(hint!="absent")body+="a=ptime:"+hint+"\r\n";
 body+="a=sendrecv\r\n";
 const bool ok=s.learn_remote_rtp_from_sdp_(body,0xc0000202,answer);
 assert(ok==(expected!=0));
 if(ok){assert(s.tx==pcm(16000,expected));assert(s.rx==pcm(16000,answer?10:20));}
 else{assert(s.tx==prior_tx&&s.rx==prior_rx&&s.accepted==prior&&s.tx_pt==112&&s.rx_pt==112);assert(s.remote_rtp_ip_v4_==0xc0000209&&s.remote_rtp_port_==45000);}
}
'''
    folder = tmp_path_factory.mktemp("sdp-maxptime")
    cpp = folder / "maxptime.cpp"
    cpp.write_text(source)
    binary = folder / "maxptime"
    result = subprocess.run(["g++", "-std=c++17", "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                             "-I", str(ROOT), str(cpp), "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("mode,expected", [
    ("standard", 20), ("d1", 10), ("answer-mixed", 10), ("answer-only16", 20),
    ("options", 10), ("options-no-hint", None), ("sendonly", None),
])
def test_maxptime_bounds_only_published_receive_mappings(maxptime_binary, mode, expected):
    result = subprocess.run([maxptime_binary, "emit", mode], capture_output=True, text=True, check=True)
    bounds = [line for line in result.stdout.splitlines() if line.startswith("a=maxptime:")]
    assert bounds == ([] if expected is None else [f"a=maxptime:{expected}"])
    if mode == "d1":
        assert "L16/48000/1" in result.stdout and "a=ptime:10" in result.stdout
    if mode == "standard":
        assert "L16/48000/1" not in result.stdout and "a=ptime:20" in result.stdout
    if mode == "options-no-hint":
        # Without16k10 this union has no valid16k packet time under max10.
        assert not result.stdout


@pytest.mark.parametrize("role", ["offer", "answer"])
@pytest.mark.parametrize("maximum,hint,expected", [
    ("10", "absent", 10), ("15", "absent", 10), ("20", "absent", 20),
    ("5", "absent", 0), ("10.5", "absent", 10), ("0.5", "absent", 0),
    ("20", "10", 10), ("20", "16", 20), ("10", "20", 10),
    ("absent", "absent", 20), ("absent", "10", 10),
    ("0", "absent", 0), ("bad", "absent", 0), ("10.", "absent", 0),
])
def test_remote_receive_bound_filters_only_configured_tx(maxptime_binary, role, maximum, hint, expected):
    result = subprocess.run([maxptime_binary, "select", role, maximum, hint, str(expected)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("mode", [
    "packed24", "d1-limited-hint", "d1-impossible", "standard-without48", "options-impossible", "answer-impossible",
    "historical32-impossible", "pcm32-only", "pcm32-ordinary",
])
def test_receive_bounds_use_wire_mapping_and_declared_durations(maxptime_binary, mode):
    result = subprocess.run([maxptime_binary, "emit", mode], capture_output=True, text=True, check=True)
    body = result.stdout
    if mode.endswith("impossible"):
        assert not body, "An offered receive mapping has no declared duration below the media bound"
    elif mode.startswith("pcm32-"):
        assert "L16/32000/1\n" in body
        assert "a=ptime:16\n" in body and "a=maxptime:16\n" in body
        assert "L16/48000/1" not in body
    elif mode == "d1-limited-hint":
        assert "a=maxptime:10\n" in body
        assert "a=ptime:20\n" not in body
    else:
        assert "a=ptime:20\n" in body and "a=maxptime:20\n" in body
        assert ("L24/16000/1" in body) is (mode == "packed24")
        assert "L16/48000/1" not in body
