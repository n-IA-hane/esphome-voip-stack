"""The real microphone callback must filter interleaved channels independently."""

from pathlib import Path
import re
import subprocess

import pytest
from test_refer_dialog_runtime import block

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "esphome/components/voip_stack"


@pytest.mark.parametrize("channels", [1, 2])
def test_dc_filter_preserves_channel_identity_across_callback_boundaries(tmp_path, channels):
    source = (COMPONENT / "voip_audio.cpp").read_text()
    header = (COMPONENT / "voip_stack.h").read_text()
    fields = "\n".join(line for line in header.splitlines() if re.search(r"DcBlockerState|dc_channel_phase_", line))
    fsm = (COMPONENT / "voip_fsm.cpp").read_text()
    reset = block(fsm, "void VoipStack::set_in_call_(")
    reset = reset[reset.index("#ifdef USE_ESPHOME_VOIP_STACK_MIC", reset.index("start_speaker_for_current_rx_")):]
    reset = reset[reset.index("\n") + 1:reset.index("#endif")]
    code = r'''
#include "esphome/components/voip_stack/audio_core_audio_utils.h"
#include "esphome/components/voip_stack/sip_types.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <vector>
using namespace esphome;using namespace esphome::voip_stack;
struct VoipStack {
 void *mic_buffer_=this;int16_t scratch[511]{};
 std::atomic<int16_t*>mic_converted_{scratch};std::atomic<float>mic_gain_{1};
 bool dc_offset_removal_=true;AudioFormat tx;
 FIELDS
 std::vector<int16_t> output;
 bool is_tx_stream_ready_()const{return true;}
 AudioFormat get_current_tx_audio_format_()const{return tx;}
 size_t mic_processing_samples_()const{return 511;}
 bool write_mic_buffer_(const uint8_t*p,size_t n){auto*s=reinterpret_cast<const int16_t*>(p);output.insert(output.end(),s,s+n/2);return true;}
 void on_microphone_data_(const uint8_t*,size_t);
 void reset_call(){RESET
 }
};
'''.replace("FIELDS", fields).replace("RESET", reset)
    code += block(source, "void VoipStack::on_microphone_data_(")
    code += r'''
int main(int argc,char**argv){
 const unsigned channels=std::stoi(argv[1]);VoipStack phone;phone.tx.channels=channels;
 for(unsigned call=0;call<2;++call){
  phone.reset_call();phone.output.clear();DcBlockerState expected[2];
  std::vector<int16_t> input(20000),reference;
  for(size_t i=0;i<input.size();++i){
   const unsigned ch=i%channels;input[i]=ch?0:1000+int(i%7);
   reference.push_back(expected[ch].process(input[i]));
  }
  // An odd number of samples splits a stereo frame across callbacks.
  size_t offset=0;for(size_t chunk:{1U,101U,513U,5001U,14384U}){
   phone.on_microphone_data_(reinterpret_cast<uint8_t*>(input.data()+offset),chunk*2);offset+=chunk;
  }
  assert(offset==input.size());
  assert(phone.output==reference&&"DC filtering must never share history between microphone channels");
 }
}
'''
    path = tmp_path / "dc.cpp"
    path.write_text(code)
    binary = tmp_path / "dc"
    result = subprocess.run(["g++", "-std=c++17", "-DUSE_ESPHOME_VOIP_STACK_MIC", "-I", str(ROOT), str(path), "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([str(binary), str(channels)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
