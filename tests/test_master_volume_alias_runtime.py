"""A Native Master alias observes the Audio owner, including zero and stale caches."""
import os
from pathlib import Path
import subprocess

import pytest

from test_refer_dialog_runtime import ROOT, SOURCE, block


def alias_declarations():
    header = (SOURCE / 'voip_stack.h').read_text()
    if 'struct MasterVolumeControl {' not in header:
        return ('template<typename Master>void set_master_volume_control(Master*){}\n'
                'bool has_master_volume_control_()const{return false;}\n' +
                block(header, 'float get_volume() const'))
    binding = ('#ifdef USE_ESPHOME_VOIP_STACK_MASTER_NUMBER_ALIAS\n' + (block(header, 'void set_master_volume_number(')
               if 'void set_master_volume_number(' in header else 'void set_master_volume_number(number::Number*){}') + '\n#endif\n')
    return (block(header, 'struct MasterVolumeControl {') + ';MasterVolumeControl master_volume_control_;\n' + binding +
            '#ifdef USE_NUMBER\n' + block(header, 'void register_volume_number(') + '\n#endif\n' +
            block(header, 'template<typename Master>') + '\n' +
            block(header, 'bool has_master_volume_control_() const') + '\n' +
            block(header, 'float get_volume() const') + '\n' +
            (block(header, 'float get_volume_control_percent() const') if 'float get_volume_control_percent() const' in header
             else 'float get_volume_control_percent()const{return this->get_volume()*100;}'))


@pytest.fixture(scope='module')
def master_alias(tmp_path_factory):
    settings = (SOURCE / 'voip_settings.cpp').read_text()
    header = (SOURCE / 'voip_stack.h').read_text()
    code = r'''
#define USE_NUMBER
#define USE_SPEAKER
#define USE_ESPHOME_VOIP_STACK_SPEAKER
#define USE_ESPHOME_VOIP_STACK_MASTER_ALIAS
#define USE_ESPHOME_VOIP_STACK_MASTER_NUMBER_ALIAS
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <functional>
#include <vector>
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
struct StoredSettings {uint8_t version=1,volume_pct=0;int8_t mic_gain_db=0;};
struct Pref {StoredSettings value;bool load(StoredSettings*out){*out=value;return true;}};
float audio_saved=0;unsigned audio_saves=0;
struct ESPPreferenceObject {bool load(float*out){*out=audio_saved;return true;}void save(const float*value){audio_saved=*value;++audio_saves;}};
struct Preferences {Pref pref;template<class T>auto make_preference(unsigned){if constexpr(std::is_same<T,float>::value)return ESPPreferenceObject{};else return pref;}}preferences;
auto*global_preferences=&preferences;unsigned fnv1_hash(const char*){return 1;}
float clamp_volume_(float value){return std::isfinite(value)?std::fmax(0,std::fmin(1,value)):0;}
float clamp_mic_gain_db_(float value){return value;}float db_to_linear(float value){return std::pow(10,value/20);}
namespace number {
struct NumberCall;
struct Number {float state=0;bool present=false;unsigned updates=0;
 struct Traits {float min=0,max=100,step=1;}traits;
 std::vector<std::function<void(float)>>callbacks;
 template<class F>void add_on_state_callback(F&&cb){callbacks.emplace_back(std::forward<F>(cb));}
 bool has_state()const{return present;}void publish_state(float value){state=value;present=true;++updates;for(auto&cb:callbacks)cb(value);}
 unsigned get_object_id_hash()const{return 2;}NumberCall make_call();
 virtual void control(float)=0;};
struct NumberCall {Number*parent;float value=0;NumberCall&set_value(float v){value=v;return *this;}
 void perform(){assert(std::isfinite(value)&&value>=0&&value<=100);parent->control(value);}};
NumberCall Number::make_call(){return {this};}
}
template<class T>struct Parented {T*parent_=nullptr;void set_parent(T*p){parent_=p;}};
struct Number:number::Number {void control(float)override{}};
struct Speaker {float volume=.73f;bool muted=true;unsigned calls=0;float get_volume()const{return volume;}void set_volume(float value){volume=value;++calls;}};
struct AudioMaster {float value=.37f;bool curved=true;unsigned calls=0;float get_master_volume()const{return value;}
 void set_master_volume(float requested){++calls;value=curved?requested*.5f:requested;}};
struct VoipStack {
 static constexpr uint8_t SETTINGS_VERSION=1;
 Pref settings_pref_;bool suppress_save_=false;std::atomic<float>volume_{1};
 number::Number*volume_number_=nullptr;void*mic_gain_number_=nullptr;Speaker*speaker_=nullptr;
 float mic_gain_db_=0;std::atomic<float>mic_gain_{1};unsigned saves=0;
 bool has_microphone_(){return true;}bool ensure_mic_processing_buffer_(){return true;}
 void schedule_save_settings_(){++saves;}
 void set_volume(float);void load_settings_();void sync_master_volume_();
@ALIAS@
};
'''.replace('@ALIAS@', alias_declarations())
    code += '\n' + block(settings, 'void VoipStack::set_volume(')
    code += '\n' + block(settings, 'void VoipStack::load_settings_(')
    code += '\n' + (block(settings, 'void VoipStack::sync_master_volume_(') if 'void VoipStack::sync_master_volume_(' in settings
                     else 'void VoipStack::sync_master_volume_(){}')
    code += '\n' + block(header, 'class VoipStackVolume :') + ';\n'
    audio_source = os.environ.get('ESP_AUDIO_STACK_SOURCE')
    if audio_source:
        audio_header = (Path(audio_source) / 'esphome/components/esp_audio_stack/number.h').read_text()
        code += r'''
#define HAS_AUDIO_CANONICAL_CONTROL
struct CanonicalAudioMaster : AudioMaster {
 float level=1;
 float get_master_volume_level()const{return level;}
 void set_master_volume(float value){level=value;AudioMaster::set_master_volume(value);}
};
struct Component {virtual void setup(){}virtual void dump_config(){}};
template<typename...Args>void log_config(Args&&...){}
namespace esphome {
using Component=::Component;
namespace number {using Number=::number::Number;}
namespace speaker {using Speaker=::Speaker;}
namespace esp_audio_stack {using ESPAudioStack=::CanonicalAudioMaster;
''' + block(audio_header, 'class MasterVolumeNumber final') + ';\n}}\n'
    code += r'''
#include <string>
int main(int argc,char**argv){
 assert(argc==2);const std::string mode=argv[1];VoipStack phone;Speaker speaker;AudioMaster master;Number number;
 phone.speaker_=&speaker;phone.volume_number_=&number;
 if(mode=="logical-no-number"){
  struct SoftwareMaster {
   float level=1,gain=1;unsigned calls=0;
   float get_master_volume()const{return gain;}
   float get_master_volume_level()const{return level;}
   void set_master_volume(float value){++calls;level=value;gain=value==0?0:std::pow(10.0f,(-49.0f+49.0f*value)/20.0f);}
  } software;
  phone.set_master_volume_control(&software);
  VoipStackVolume alias;alias.set_parent(&phone);phone.volume_number_=&alias;
  alias.control(50);
  assert(std::fabs(phone.get_volume()-.0595662f)<.000001f&&alias.state==50);
  const auto publishes=alias.updates;phone.sync_master_volume_();assert(alias.updates==publishes);
  alias.control(0);assert(phone.get_volume()==0&&alias.state==0);
  alias.control(100);assert(phone.get_volume()==1&&alias.state==100);
  assert(software.calls==3&&phone.saves==0&&speaker.calls==0);return 0;
 }
 if(mode=="legacy"){
  phone.load_settings_();assert(phone.volume_==0&&speaker.volume==0&&speaker.calls==1&&speaker.muted);
  phone.set_volume(.6f);assert(phone.get_volume()==.6f&&speaker.volume==.6f&&phone.saves==1&&speaker.muted);return 0;
 }
 phone.set_master_volume_control(&master);
#ifdef HAS_AUDIO_CANONICAL_CONTROL
 if(mode=="canonical-idle"){
  CanonicalAudioMaster master;master.curved=false;phone.set_master_volume_control(&master);
  esphome::esp_audio_stack::MasterVolumeNumber canonical;canonical.set_parent(&master);canonical.set_speaker(&speaker);
  audio_saved=20;canonical.setup();assert(canonical.state==20);
  phone.volume_number_=nullptr;phone.set_master_volume_number(&canonical);
  VoipStackVolume alias;alias.set_parent(&phone);alias.traits={7,93,7};
  phone.register_volume_number(&alias);assert(alias.has_state()&&alias.state==20);
  const auto initial=alias.updates;
  // No loop(), manual sync or wakeup: the canonical Number is the event owner.
  canonical.make_call().set_value(80).perform();
  assert(master.value==.8f&&alias.state==80&&alias.updates==initial+1);
  canonical.publish_state(80);assert(alias.updates==initial+1);
  alias.control(40);assert(canonical.state==40&&alias.state==40&&alias.updates==initial+2);
  assert(alias.traits.min==7&&alias.traits.max==93&&alias.traits.step==7);
  phone.register_volume_number(nullptr);canonical.publish_state(30);
  phone.register_volume_number(&alias);assert(alias.state==30);
  assert(phone.volume_==1&&phone.saves==0&&speaker.calls==0);return 0;
 }
 if(mode=="canonical"){
  CanonicalAudioMaster master;phone.set_master_volume_control(&master);
  master.curved=false;master.value=0;audio_saved=0;
  esphome::esp_audio_stack::MasterVolumeNumber canonical;canonical.set_parent(&master);canonical.set_speaker(&speaker);canonical.setup();
  VoipStackVolume alias;alias.set_parent(&phone);phone.volume_number_=&alias;phone.set_master_volume_number(&canonical);
  phone.sync_master_volume_();assert(alias.has_state()&&alias.state==0&&canonical.has_state()&&canonical.state==0);
  const auto initial_audio=canonical.updates,initial_native=alias.updates;
  phone.set_volume(.01f);phone.sync_master_volume_();
  assert(master.value==.01f&&canonical.state==1&&audio_saved==1&&audio_saves==1&&alias.state==1);
  assert(canonical.updates==initial_audio+1&&alias.updates==initial_native+1&&speaker.calls==0&&speaker.muted&&phone.saves==0);
  phone.sync_master_volume_();phone.sync_master_volume_();assert(canonical.updates==initial_audio+1&&alias.updates==initial_native+1&&audio_saves==1);
  canonical.make_call().set_value(40).perform();assert(phone.get_volume()==.4f&&audio_saved==40);
  phone.sync_master_volume_();assert(alias.state==40&&canonical.state==40&&audio_saves==2);
  const auto updates=alias.updates;alias.control(1);assert(alias.updates==updates+1&&canonical.state==1&&audio_saved==1&&audio_saves==3);
  phone.sync_master_volume_();assert(alias.updates==updates+1);
  phone.set_volume(0);phone.sync_master_volume_();assert(master.value==0&&canonical.state==0&&alias.state==0&&audio_saved==0&&audio_saves==4);
  master.curved=true;canonical.make_call().set_value(50).perform();phone.sync_master_volume_();
  assert(master.value==.25f&&phone.get_volume()==.25f&&canonical.state==50&&alias.state==50&&audio_saved==50&&audio_saves==5);
  alias.control(1);assert(canonical.state==1&&alias.state==1&&audio_saved==1&&audio_saves==6);
  const auto stable=alias.updates;phone.sync_master_volume_();assert(alias.updates==stable);
  assert(phone.volume_==1&&phone.saves==0&&speaker.calls==0&&speaker.muted);return 0;
 }
#endif
 if(mode=="getter"){
  phone.volume_=0;assert(phone.get_volume()==master.value);
  master.value=0;assert(phone.get_volume()==0);master.value=.8f;assert(phone.get_volume()==.8f&&phone.volume_==0);
 }else if(mode=="zero-sync"){
  master.value=0;phone.sync_master_volume_();assert(number.has_state()&&number.state==0&&number.updates==1);
  phone.sync_master_volume_();assert(number.updates==1);
  master.value=.4f;phone.sync_master_volume_();assert(number.state==40&&number.updates==2);
 }else if(mode=="setter"){
  phone.volume_=.9f;phone.set_volume(.6f);
  assert(master.value==.3f&&master.calls==1&&phone.get_volume()==.3f&&phone.volume_==.9f);
  assert(speaker.calls==0&&speaker.volume==.73f&&speaker.muted&&phone.saves==0);
  phone.set_volume(std::numeric_limits<float>::quiet_NaN());assert(master.value==0&&phone.get_volume()==0&&phone.saves==0);
 }else if(mode=="number"){
  VoipStackVolume alias;alias.set_parent(&phone);phone.volume_number_=&alias;alias.control(60);
  assert(alias.has_state()&&alias.state==phone.get_volume()*100&&std::fabs(alias.state-30)<.001f&&speaker.calls==0);
  assert(alias.updates==1);phone.sync_master_volume_();assert(alias.updates==1);
 }else{
  phone.load_settings_();assert(phone.get_volume()==master.value&&phone.volume_==1&&speaker.calls==0&&!phone.suppress_save_);
  preferences.pref.value.volume_pct=100;phone.load_settings_();assert(master.calls==0&&speaker.calls==0);
 }
}
'''
    path = tmp_path_factory.mktemp('master-alias')
    cpp = path / 'master.cpp'
    cpp.write_text(code)
    binary = path / 'master'
    built = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-fno-pie', '-no-pie', str(cpp), '-o', str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize('mode', ['getter', 'zero-sync', 'setter', 'number', 'restore', 'legacy', 'logical-no-number',
                                pytest.param('canonical-idle', marks=pytest.mark.skipif(not os.environ.get('ESP_AUDIO_STACK_SOURCE'),
                                             reason='Actual Audio control gate requires ESP_AUDIO_STACK_SOURCE')),
                                pytest.param('canonical', marks=pytest.mark.skipif(not os.environ.get('ESP_AUDIO_STACK_SOURCE'),
                                             reason='Actual Audio control gate requires ESP_AUDIO_STACK_SOURCE'))])
def test_master_alias_has_one_effective_owner(master_alias, mode):
    run = subprocess.run([str(master_alias), mode], capture_output=True, text=True, timeout=5)
    assert run.returncode == 0, run.stdout + run.stderr


def test_stale_zero_cache_cannot_keep_real_rx_playout_muted(tmp_path):
    from test_rx_playout_owner import owner_source
    code = owner_source().split('\nint main(', 1)[0]
    code = '#define USE_ESPHOME_VOIP_STACK_MASTER_ALIAS\n' + code
    if 'float get_volume()const' in code:
        code = code.replace(block(code, 'float get_volume()const'), '', 1)
    code = code.replace('struct VoipStack {', 'struct VoipStack {\n' + alias_declarations())
    code = code.replace('struct Speaker {', 'struct Speaker {\n std::vector<uint8_t>output;')
    code = code.replace('size_t play(const uint8_t*,size_t bytes,uint32_t){if(!is_running())return 0;accepted+=bytes;return bytes;}',
                        'size_t play(const uint8_t*p,size_t bytes,uint32_t){if(!is_running())return 0;accepted+=bytes;output.insert(output.end(),p,p+bytes);return bytes;}')
    code += r'''
struct AudioMaster {float value=0;float get_master_volume()const{return value;}void set_master_volume(float v){value=v;}};
int main(){
 VoipStack phone;Speaker speaker;AudioMaster master;speaker.state=Speaker::RUNNING;phone.speaker_=&speaker;
 phone.volume_=0;phone.set_master_volume_control(&master);phone.call_state_=CallState::IN_CALL;
 uint8_t scratch[960]{},samples[960];for(unsigned n=0;n<960;++n)samples[n]=uint8_t(n*7);
 phone.rx_audio_chunk_=scratch;phone.rx_audio_chunk_alloc_bytes_=sizeof(scratch);
 phone.rx_playout_state_=tag(phone.rx_audio_revision_,Phase::ACTIVE);
 phone.play_rx_frame_(samples,sizeof(samples),SilenceReason::NONE,10,phone.rx_audio_revision_);
 assert(speaker.output==std::vector<uint8_t>(960,0));speaker.output.clear();
 master.value=.4f;phone.play_rx_frame_(samples,sizeof(samples),SilenceReason::NONE,10,phone.rx_audio_revision_);
 assert(speaker.output==std::vector<uint8_t>(samples,samples+960)&&phone.volume_==0);
}
'''
    core = tmp_path / 'esphome/core'
    core.mkdir(parents=True)
    (core / 'helpers.h').write_text('namespace esphome {struct Mutex {};struct LockGuard {explicit LockGuard(Mutex&){}};}\n')
    cpp = tmp_path / 'playout.cpp'
    cpp.write_text(code)
    binary = tmp_path / 'playout'
    built = subprocess.run(['g++', '-std=c++17', '-DUSE_ESP32', '-DUSE_ESPHOME_VOIP_STACK_SPEAKER',
                            '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-I', str(tmp_path), '-I', str(ROOT),
                            str(cpp), '-o', str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
    assert run.returncode == 0, run.stdout + run.stderr
