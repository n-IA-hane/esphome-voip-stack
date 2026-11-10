"""SDP writers receive format facts instead of parsing their own output."""
import subprocess

import pytest

from test_refer_dialog_runtime import ROOT, SOURCE, block
from test_sdp_receive_payloads import sdp_fixture_source


@pytest.fixture(scope='module')
def typed_writer(tmp_path_factory):
    code = sdp_fixture_source()
    mapping = block(code, 'bool parse_rtpmap_format(')
    implementation = mapping.replace('parse_rtpmap_format(', 'parse_mapping_control(', 1)
    probe = r'''
unsigned parsed_mappings=0;
bool parse_rtpmap_format(const std::string&line,AudioFormat*format,uint8_t*pt){
 ++parsed_mappings;return parse_mapping_control(line,format,pt);
}
'''
    code = code.replace(mapping, implementation + '\n' + probe)
    code = '#include <array>\n#include <cstdint>\nuint32_t esp_random(){return 7;}\n' + code
    code = code.replace('struct SipTransport {', 'struct SipTransport {std::string build_sdp_capabilities_(uint32_t)const;')
    code += '\n' + block((SOURCE / 'sip_sdp.cpp').read_text(), 'std::string SipTransport::build_sdp_capabilities_(')
    code += r'''
int main(int argc,char**argv){
 assert(argc==2);const std::string mode=argv[1];SipTransport s;AudioFormat pcm;pcm.sample_rate=16000;pcm.frame_ms=10;
 s.offer_tx_formats_.formats[0]=s.offer_rx_formats_.formats[0]=pcm;
 const std::string incoming="v=0\r\nc=IN IP4 192.0.2.2\r\nm=audio 40004 RTP/AVP 96\r\na=rtpmap:96 L16/16000/1\r\na=ptime:10\r\n";
 assert(s.learn_remote_rtp_from_sdp_(incoming,0xc0000202)&&parsed_mappings>0);
 parsed_mappings=0;
 const auto output=mode=="offer"?s.build_sdp_offer_():mode=="answer"?s.build_sdp_answer_():s.build_sdp_capabilities_(0);
 assert(!output.empty()&&output.find("a=maxptime:10\r\n")!=std::string::npos);
 assert(parsed_mappings==0&&"An SDP writer must not parse its serialized RTP map strings");
}
'''
    path = tmp_path_factory.mktemp('typed-sdp')
    cpp = path / 'writer.cpp'
    cpp.write_text(code)
    binary = path / 'writer'
    built = subprocess.run(['g++', '-std=c++17', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                            '-I', str(ROOT), str(cpp), '-o', str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize('kind', ['offer', 'answer', 'options'])
def test_serialization_never_parses_generated_mapping_lines(typed_writer, kind):
    run = subprocess.run([str(typed_writer), kind], capture_output=True, text=True, timeout=5)
    assert run.returncode == 0, run.stdout + run.stderr
