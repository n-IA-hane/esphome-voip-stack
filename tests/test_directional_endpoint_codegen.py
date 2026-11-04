"""Real ESPHome codegen does not invent the missing audio endpoint."""
import os
from pathlib import Path
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize('role', ['microphone', 'speaker'])
@pytest.mark.parametrize('legacy_absent_format', [False, True])
def test_absent_audio_direction_is_not_generated(tmp_path, role, legacy_absent_format):
    path = ROOT / 'tests' / ('minimal_no_text.yaml' if role == 'microphone' else 'minimal_speaker_only.yaml')
    config = path.read_text().replace('../esphome/components', str(ROOT / 'esphome/components'))
    if not legacy_absent_format:
        import yaml
        data = yaml.safe_load(config)
        data['voip_stack']['audio']['codecs'][0].pop('rx' if role == 'microphone' else 'tx', None)
        config = yaml.safe_dump(data, sort_keys=False)
    fixture = tmp_path / 'phone.yaml'
    fixture.write_text(config)
    proc = subprocess.run([os.environ.get('ESPHOME_PYTHON', sys.executable), '-m', 'esphome', 'compile', str(fixture), '--only-generate'], capture_output=True, text=True, timeout=60)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    code = next(tmp_path.rglob('main.cpp')).read_text()
    present, absent = ('tx', 'rx') if role == 'microphone' else ('rx', 'tx')
    assert f'set_{present}_audio_format(48000' in code
    assert f'set_{absent}_audio_format(' not in code
    assert f'add_supported_{absent}_audio_format(' not in code


def test_endpoint_serializer_always_publishes_independent_ptime_support(tmp_path):
    from test_refer_dialog_runtime import block

    source = (ROOT / 'esphome/components/voip_stack/voip_stack.cpp').read_text()
    header = (ROOT / 'esphome/components/voip_stack/voip_stack.h').read_text()
    transport_enum = block(header, 'enum class TransportType') + ';\n'
    methods = '\n'.join(block(source, marker) for marker in (
        'std::string VoipStack::audio_format_token_(',
        'std::string VoipStack::audio_rtp_format_token_(',
        'std::string VoipStack::build_endpoint_string_(',
    ))
    cpp = tmp_path / 'endpoint.cpp'
    cpp.write_text(r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include "esphome/components/voip_stack/sip_types.h"
#define ESP_LOGW(...)
namespace esphome::voip_stack {
''' + transport_enum + r'''
struct FriendlyName {std::string str() const {return "fallback";}};
struct Application {FriendlyName get_friendly_name() const {return {};}} App;
struct VoipStack {
 std::string device_name_="Demo",extension_="100";
 unsigned sip_port_=5060,rtp_port_=40000;
 TransportType protocol_=TransportType::UDP;
 AudioFormatList tx_audio_formats_,rx_audio_formats_;
 const char *audio_capability_() const {return "both";}
 std::string local_ip_string_() const {return "192.0.2.10";}
 static std::string audio_format_token_(const AudioFormat&);
 static std::string audio_rtp_format_token_(const AudioFormat&);
 std::string build_endpoint_string_() const;
};
''' + methods + r'''
}
int main(){
 using namespace esphome::voip_stack;
 VoipStack s;AudioFormat pcm;pcm.sample_rate=16000;pcm.frame_ms=20;
 s.tx_audio_formats_.count=s.rx_audio_formats_.count=1;
 s.tx_audio_formats_.formats[0]=s.rx_audio_formats_.formats[0]=pcm;
 for(bool g722:{false,true}){
  s.tx_audio_formats_.formats[0].codec=g722?AudioCodec::G722:AudioCodec::PCM;
  auto endpoint=s.build_endpoint_string_();assert(!endpoint.empty());
  assert(endpoint.find(" | sf=d1 | ip=1")!=std::string::npos);
  if(g722)assert(endpoint.find(" | at=g/8/1/20")!=std::string::npos);
  else assert(endpoint.find("16000:s16le:1:20")!=std::string::npos);
 }
}
''')
    exe = tmp_path / 'endpoint'
    built = subprocess.run(['g++', '-std=c++17', '-I', str(ROOT), str(cpp), '-o', str(exe)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    run = subprocess.run([exe], capture_output=True, text=True, timeout=5)
    assert run.returncode == 0, run.stderr
