"""Typed writers stay pure; offer orchestration snapshots the actual sent SDP."""
import subprocess

import pytest

from test_refer_dialog_runtime import ROOT, SOURCE, block
from test_sdp_receive_payloads import sdp_fixture_source


@pytest.fixture(scope='module')
def typed_writer(tmp_path_factory):
    code = '#define USE_ESPHOME_VOIP_DTMF\n#include "esphome/components/voip_stack/rtp_dtmf.h"\n' + sdp_fixture_source()
    code = code.replace('struct SipTransport {', r'''struct SipTransport {
 DtmfFormat dtmf_tx{},dtmf_rx{};uint8_t dtmf_offered_clocks_=0;
 void get_dtmf_config_(DtmfFormat*a,DtmfFormat*b,uint32_t*c=nullptr)const{if(a)*a=dtmf_tx;if(b)*b=dtmf_rx;if(c)*c=0;}
 void set_dtmf_config_(const DtmfFormat&a,const DtmfFormat&b){dtmf_tx=a;dtmf_rx=b;}
 static void append_dtmf_sdp_(std::string*,std::string*,uint8_t);
''')
    code = code.replace("std::string build_sdp_answer_() const;", "std::string build_sdp_answer_(const DtmfConfig* =nullptr) const;")
    code = code.replace("bool learn_remote_rtp_from_sdp_(const std::string&,uint32_t,bool=false);", "bool learn_remote_rtp_from_sdp_(const std::string&,uint32_t,bool=false,DtmfConfig* =nullptr);")
    code += block((SOURCE / 'sip_dtmf.cpp').read_text(), 'void SipTransport::append_dtmf_sdp_(')
    mapping = block(code, 'bool esphome::voip_stack::parse_rtpmap_format(')
    counted = mapping.replace('{', '{ ++parsed_mappings; if(inside_typed_writer) ++writer_parsed_mappings;', 1)
    counters = r'''
unsigned parsed_mappings=0,writer_parsed_mappings=0,typed_writer_calls=0;
bool inside_typed_writer=false;
struct TypedWriterScope {
 bool previous;
 TypedWriterScope():previous(inside_typed_writer){inside_typed_writer=true;++typed_writer_calls;}
 ~TypedWriterScope(){inside_typed_writer=previous;}
};
'''
    code = code.replace(mapping, counters + counted)
    writer = block(code, 'std::string SipTransport::wrap_sdp_envelope_version_(')
    code = code.replace(writer, writer.replace('{', '{ TypedWriterScope scope;', 1))
    code = '#include <array>\n#include <cstdint>\nuint32_t esp_random(){return 7;}\n' + code
    code = code.replace('struct SipTransport {', 'struct SipTransport {std::string build_sdp_capabilities_(uint32_t)const;')
    code += '\n' + block((SOURCE / 'sip_sdp.cpp').read_text(), 'std::string SipTransport::build_sdp_capabilities_(')
    code += r'''
int main(int argc,char**argv){
 assert(argc==2);const std::string mode=argv[1];SipTransport s;AudioFormat pcm;pcm.sample_rate=16000;pcm.frame_ms=10;
 s.offer_tx_formats_.formats[0]=s.offer_rx_formats_.formats[0]=pcm;
 const std::string incoming="v=0\r\nc=IN IP4 192.0.2.2\r\nm=audio 40004 RTP/AVP 96\r\na=rtpmap:96 L16/16000/1\r\na=ptime:10\r\n";
 assert(s.learn_remote_rtp_from_sdp_(incoming,0xc0000202)&&parsed_mappings>0);
 parsed_mappings=writer_parsed_mappings=typed_writer_calls=0;
 const auto output=mode=="offer"?s.build_sdp_offer_():mode=="answer"?s.build_sdp_answer_():s.build_sdp_capabilities_(0);
 assert(!output.empty()&&output.find("a=maxptime:10\r\n")!=std::string::npos);
 assert(typed_writer_calls>0);
 assert(writer_parsed_mappings==0&&"A typed SDP writer must not parse its serialized RTP map strings");
 if(mode!="offer"){
  assert(parsed_mappings==0&&"Answer and capability writers must not parse their serialized RTP maps");
 }else{
  // build_sdp_offer_ also captures its exact serialized offer. The
  // directional media contract requires that snapshot to reflect the actual SDP,
  // including its receive geometry and telephone-event payload meanings.
  AudioFormat receive;
  assert(s.local_offered_rx_payloads_.count==1&&s.local_offered_rx_payloads_.find(96,&receive)&&receive==pcm);
  assert(output.find("a=rtpmap:96 L16/16000/1\r\n")!=std::string::npos);
  for(const auto &event:{DtmfFormat{8000,0xffff,120},DtmfFormat{16000,0xffff,122}}){
   assert(s.local_offered_dtmf_.find_payload(event.payload_type)==event);
   const auto pt=std::to_string(event.payload_type);
   assert(output.find("a=rtpmap:"+pt+" telephone-event/"+std::to_string(event.clock)+"\r\n")!=std::string::npos);
   assert(output.find("a=fmtp:"+pt+" 0-15\r\n")!=std::string::npos);
  }
  assert(s.local_offered_dtmf_.clocks()==s.dtmf_offered_clocks_);
 }
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
