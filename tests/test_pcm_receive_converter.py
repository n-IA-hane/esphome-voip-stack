"""Exercise the production adapter with ESPHome's real portable resampler.

Set ESP_AUDIO_LIBS_SOURCE to the pinned esp-audio-libs source directory used by
the firmware build. No substitute DSP implementation is used by this test.
"""
import os
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_real_pcm_converter_preserves_tone_and_resets(tmp_path):
    location = os.environ.get("ESP_AUDIO_LIBS_SOURCE")
    if not location:
        pytest.skip("ESP_AUDIO_LIBS_SOURCE is required for the native DSP test")
    library = Path(location)
    cpp = tmp_path / "converter.cpp"
    cpp.write_text(r'''
#include <cassert>
#include <cmath>
#include <complex>
#include <vector>
#include "esphome/components/voip_stack/pcm_receive_converter.h"
using namespace esphome::voip_stack;
bool fail_allocation=false;
void *operator new(std::size_t size, const std::nothrow_t&) noexcept {
  if(fail_allocation) return nullptr;
  try { return ::operator new(size); } catch (...) { return nullptr; }
}

AudioFormat format(unsigned rate) { AudioFormat f; f.sample_rate=rate; f.frame_ms=10; return f; }
int main() {
  for (unsigned rate : {16000,24000,32000,44100}) {
    PcmReceiveConverter converter;
    const auto input=format(rate), output=format(48000);
    RtpAudioPayloads map;
    assert(map.add(96,output)); assert(map.add(97,input));
    assert(map.add(97,input)); assert(!map.add(97,output));
    AudioFormat found; assert(map.find(97,&found) && found==input); assert(!map.find(99,&found));
    fail_allocation=true; assert(!converter.prepare(map,output));
    fail_allocation=false;
    for (unsigned call=0;call<3;++call) {
      assert(converter.prepare(map,output));
      std::vector<int16_t> source(input.nominal_frame_samples()), sink(480), captured;
      for (unsigned packet=0;packet<100;++packet) {
        for (unsigned i=0;i<source.size();++i)
          source[i]=static_cast<int16_t>(8000*std::sin(2*M_PI*660*(packet*source.size()+i)/rate));
        size_t bytes=converter.convert(input,reinterpret_cast<uint8_t*>(source.data()),source.size()*2,
                                       reinterpret_cast<uint8_t*>(sink.data()),sink.size()*2);
        assert(bytes>800 && bytes<=960 && bytes%2==0);
        captured.insert(captured.end(),sink.begin(),sink.begin()+bytes/2);
      }
      assert(captured.size()>=48000-64 && captured.size()<=48000);
      std::complex<double> tone{}; double power=0;
      for (size_t i=480;i<captured.size();++i) {
        double phase=2*M_PI*660*i/48000;
        tone += double(captured[i])*std::complex<double>(std::cos(phase),std::sin(phase));
        power += double(captured[i])*captured[i];
      }
      const double count=captured.size()-480;
      const double amplitude=2*std::abs(tone)/count;
      assert(amplitude>7900 && amplitude<8100);
      assert(2*std::norm(tone)/(count*power)>.99);
      converter.reset();
      std::fill(source.begin(),source.end(),0);
      size_t bytes=converter.convert(input,reinterpret_cast<uint8_t*>(source.data()),source.size()*2,
                                     reinterpret_cast<uint8_t*>(sink.data()),sink.size()*2);
      for(size_t i=0;i<bytes/2;++i) assert(sink[i]==0);
      assert(!converter.convert(input,reinterpret_cast<uint8_t*>(source.data()),source.size()*2-1,
                                reinterpret_cast<uint8_t*>(sink.data()),sink.size()*2));
    }
  }
}
''')
    sources = ["src/resample/resampler.cpp", "src/resample/art_resampler.cpp",
               "src/resample/art_biquad.cpp", "src/quantization_utils.cpp",
               "src/memory_utils.cpp", "src/dsp/dsps_dotprod_f32_ansi.c"]
    binary = tmp_path / "converter"
    subprocess.run([
        "g++", "-std=c++17", "-DUSE_ESPHOME_VOIP_STACK_SPEAKER",
        "-fsanitize=address,undefined", "-g", "-I", str(ROOT),
        "-I", str(library / "include"), str(cpp),
        *(str(library / source) for source in sources), "-o", str(binary),
    ], check=True, capture_output=True, text=True)
    subprocess.run([str(binary)], check=True, capture_output=True, text=True)
