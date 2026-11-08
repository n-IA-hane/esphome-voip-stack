"""Exercise production preference loading with codec-owned and VoIP-owned volume."""
from pathlib import Path
import subprocess
import pytest
from test_refer_dialog_runtime import block

ROOT = Path(__file__).resolve().parents[1]

@pytest.mark.parametrize('with_number', [None, False, True])
def test_saved_voip_volume_only_applies_to_its_owner(tmp_path, with_number):
    production = block((ROOT / 'esphome/components/voip_stack/voip_settings.cpp').read_text(), 'void VoipStack::load_settings_(')
    source = r'''
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define USE_ESPHOME_VOIP_STACK_SPEAKER
struct StoredSettings {uint8_t version=1,volume_pct=0;int8_t mic_gain_db=0;};
struct Pref {StoredSettings value;bool valid=true;bool load(StoredSettings*out){*out=value;return valid;}};
struct Preferences {Pref pref;template<class T>Pref make_preference(unsigned){return pref;}} preferences;
auto*global_preferences=&preferences;
unsigned fnv1_hash(const char*){return 1;}
float clamp_volume_(float x){return std::fmax(0,std::fmin(1,x));}
float clamp_mic_gain_db_(float x){return std::fmax(-20,std::fmin(20,x));}
float db_to_linear(float x){return std::pow(10,x/20);}
struct Speaker {float volume=.73f;unsigned calls=0;void set_volume(float x){volume=x;++calls;}};
class VoipStack {public:
 static constexpr uint8_t SETTINGS_VERSION=1;
 Pref settings_pref_;bool suppress_save_=false;std::atomic<float>volume_{1};
 void*volume_number_=nullptr;void*mic_gain_number_=nullptr;Speaker*speaker_=nullptr;
 float mic_gain_db_=0;std::atomic<float>mic_gain_{1};
 bool has_microphone_(){return true;}bool ensure_mic_processing_buffer_(){return true;}
 void load_settings_();
};
''' + production + r'''
int main(){
 Speaker sink;
 for(unsigned saved:{0U,25U,100U}){
  preferences.pref.value.volume_pct=saved;
  VoipStack phone;phone.speaker_=&sink;
#ifdef USE_NUMBER
  const bool own=OWN;
  if(own)phone.volume_number_=&phone;
#else
  const bool own=false;
#endif
  auto calls=sink.calls;float old=sink.volume;phone.load_settings_();
  assert(phone.volume_.load()==(own?saved/100.0f:1.0f));
  assert(sink.calls==calls+unsigned(own));assert(sink.volume==(own?saved/100.0f:old));
  assert(!phone.suppress_save_);
 }
}
'''
    cpp=tmp_path/'volume.cpp'
    cpp.write_text(source)
    binary=tmp_path/'volume'
    defines=[] if with_number is None else ['-DUSE_NUMBER',f'-DOWN={int(with_number)}']
    run=subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',*defines,str(cpp),'-o',str(binary)],capture_output=True,text=True)
    assert run.returncode==0,run.stderr
    run=subprocess.run([str(binary)],capture_output=True,text=True)
    assert run.returncode==0,run.stderr
