"""Exercise the production adapter with ESPHome's real portable resampler.

Set ESP_AUDIO_LIBS_SOURCE to the pinned esp-audio-libs source directory used by
the firmware build. No substitute DSP implementation is used by this test.
"""
import os
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("opus_enabled", [False, True])
def test_real_pcm_converter_preserves_tone_and_resets(tmp_path, opus_enabled):
    location = os.environ.get("ESP_AUDIO_LIBS_SOURCE")
    if not location:
        pytest.fail(
            "ESP_AUDIO_LIBS_SOURCE is required for the native DSP test; "
            "follow the native-test setup in README.md"
        )
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
unsigned allocations=0;
void *operator new(std::size_t size, const std::nothrow_t&) noexcept {
  ++allocations;
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
      const auto prepared_allocations=allocations;
      fail_allocation=true;
      assert(converter.prepare(map,output)&&"An identical prepared PCM target must reuse its converters");
      converter.reset();
      fail_allocation=false;
      assert(allocations==prepared_allocations);
      std::vector<int16_t> source(input.nominal_frame_samples()), sink(480), captured;
      for (unsigned packet=0;packet<100;++packet) {
        if (packet==50) {
          RtpAudioPayloads extra;
          extra.add(98, format(rate==32000 ? 24000 : 32000));
          RtpAudioPayloads uncached;
          uncached.add(99, format(8000));
          fail_allocation=true;
          assert(!converter.prepare_additional(uncached,output));
          if(call!=0){
            const auto cached_allocations=allocations;
            assert(converter.prepare_additional(extra,output));
            assert(allocations==cached_allocations);
          }
          fail_allocation=false;
          assert(converter.prepare_additional(extra,output));
          assert(!converter.prepare_additional(extra,format(16000)));
        }
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
    built = subprocess.run([
        # Match the firmware SDK's C++20 signed PCM quantization semantics.
        "g++", "-std=c++20", "-DUSE_ESPHOME_VOIP_STACK_SPEAKER",
        *(["-DUSE_ESPHOME_VOIP_STACK_OPUS"] if opus_enabled else []),
        "-fsanitize=address,undefined", "-g", "-I", str(ROOT),
        "-I", str(library / "include"), str(cpp),
        *(str(library / source) for source in sources), "-o", str(binary),
    ], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    run = subprocess.run(["bash", "-c", 'ulimit -c 0; exec "$1"', "bash", str(binary)],
                         capture_output=True, text=True,
                         env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
    assert run.returncode == 0, run.stderr


def test_variable_packetization_preserves_one_resampler_history(tmp_path):
    library = Path(os.environ["ESP_AUDIO_LIBS_SOURCE"])
    cpp = tmp_path / "variable.cpp"
    cpp.write_text(r'''
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
#include "esphome/components/voip_stack/pcm_receive_converter.h"
using namespace esphome::voip_stack;
unsigned allocations=0;
void *operator new(std::size_t size,const std::nothrow_t&) noexcept {
 ++allocations;try{return ::operator new(size);}catch(...){return nullptr;}
}
int main(){
 AudioFormat ten{16000,PcmFormat::S16LE,1,10,AudioCodec::PCM},twenty=ten;twenty.frame_ms=20;
 AudioFormat target=ten;target.sample_rate=48000;
 AudioFormatList variants;variants.count=2;variants.formats[0]=ten;variants.formats[1]=twenty;
 PcmReceiveConverter converter;assert(converter.prepare(variants,target));
 const unsigned contexts=allocations;
 // Independent official DSP owner, one stream with runtime input/output counts.
 esp_audio_libs::resampler::Resampler reference(320,960);
 esp_audio_libs::resampler::ResamplerConfiguration configuration{16000,48000,16,16,1,false,false,16,16};
 assert(reference.initialize(configuration));
 std::vector<int16_t> observed,expected;unsigned position=0;
 for(unsigned packet=0;packet<48;++packet){
  const auto source_format=packet%3==1?twenty:ten;
  std::vector<int16_t> source(source_format.nominal_frame_samples()),actual(960),golden(960);
  for(unsigned i=0;i<source.size();++i)source[i]=int16_t(10000*std::sin(2*3.141592653589793*660*(position+i)/16000));
  position+=source.size();
  const auto produced=converter.convert(source_format,reinterpret_cast<const uint8_t*>(source.data()),source.size()*2,
                                       reinterpret_cast<uint8_t*>(actual.data()),actual.size()*2);
  const auto result=reference.resample(reinterpret_cast<const uint8_t*>(source.data()),reinterpret_cast<uint8_t*>(golden.data()),
                                       source.size(),source.size()*3,0.0f);
  assert(result.frames_used==source.size());
  observed.insert(observed.end(),actual.begin(),actual.begin()+produced/2);
  expected.insert(expected.end(),golden.begin(),golden.begin()+result.frames_generated);
 }
 fprintf(stderr,"contexts=%u actual_samples=%zu reference_samples=%zu\n",contexts,observed.size(),expected.size());
 assert(observed==expected&&"10/20/10 packet boundaries must preserve one continuous DSP history");
 assert(contexts==1&&"Packet duration must not duplicate the resampler for one PCM geometry");
 assert(allocations==contexts&&"No context allocation is permitted while consuming packets");
}
''')
    sources = ["src/resample/resampler.cpp", "src/resample/art_resampler.cpp", "src/resample/art_biquad.cpp",
               "src/quantization_utils.cpp", "src/memory_utils.cpp", "src/dsp/dsps_dotprod_f32_ansi.c"]
    binary = tmp_path / "variable"
    built = subprocess.run(["g++", "-std=c++20", "-DUSE_ESPHOME_VOIP_STACK_SPEAKER", "-I", str(ROOT),
                            "-I", str(library / "include"), str(cpp), *(str(library / name) for name in sources),
                            "-o", str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    result = subprocess.run([binary], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
