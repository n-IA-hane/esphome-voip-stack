"""A Native Master alias observes the Audio owner, including zero and stale caches."""
import subprocess

import pytest

from test_refer_dialog_runtime import ROOT, SOURCE, block


def alias_declarations():
    header = (SOURCE / 'voip_stack.h').read_text()
    if 'struct MasterVolumeControl {' not in header:
        return ('template<typename Master>void set_master_volume_control(Master*){}\n'
                'bool has_master_volume_control_()const{return false;}\n' +
                block(header, 'float get_volume() const'))
    return (block(header, 'struct MasterVolumeControl {') + ';MasterVolumeControl master_volume_control_;\n' +
            block(header, 'template<typename Master>') + '\n' +
            block(header, 'bool has_master_volume_control_() const') + '\n' +
            block(header, 'float get_volume() const'))


@pytest.fixture(scope='module')
def master_alias(tmp_path_factory):
    settings = (SOURCE / 'voip_settings.cpp').read_text()
    header = (SOURCE / 'voip_stack.h').read_text()
    code = r'''
#define USE_NUMBER
#define USE_ESPHOME_VOIP_STACK_SPEAKER
#define USE_ESPHOME_VOIP_STACK_MASTER_ALIAS
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
struct StoredSettings {uint8_t version=1,volume_pct=0;int8_t mic_gain_db=0;};
struct Pref {StoredSettings value;bool load(StoredSettings*out){*out=value;return true;}};
struct Preferences {Pref pref;template<class T>Pref make_preference(unsigned){return pref;}}preferences;
auto*global_preferences=&preferences;unsigned fnv1_hash(const char*){return 1;}
float clamp_volume_(float value){return std::isfinite(value)?std::fmax(0,std::fmin(1,value)):0;}
float clamp_mic_gain_db_(float value){return value;}float db_to_linear(float value){return std::pow(10,value/20);}
namespace number {
struct Number {float state=0;bool present=false;unsigned updates=0;
 bool has_state()const{return present;}void publish_state(float value){state=value;present=true;++updates;}
 virtual void control(float)=0;};
}
template<class T>struct Parented {T*parent_=nullptr;void set_parent(T*p){parent_=p;}};
struct Number:number::Number {void control(float)override{}};
struct Speaker {float volume=.73f;bool muted=true;unsigned calls=0;void set_volume(float value){volume=value;++calls;}};
struct AudioMaster {float value=.37f;unsigned calls=0;float get_master_volume()const{return value;}
 void set_master_volume(float requested){++calls;value=requested*.5f;}};
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
    code += r'''
#include <string>
int main(int argc,char**argv){
 assert(argc==2);const std::string mode=argv[1];VoipStack phone;Speaker speaker;AudioMaster master;Number number;
 phone.speaker_=&speaker;phone.volume_number_=&number;
 if(mode=="legacy"){
  phone.load_settings_();assert(phone.volume_==0&&speaker.volume==0&&speaker.calls==1&&speaker.muted);
  phone.set_volume(.6f);assert(phone.get_volume()==.6f&&speaker.volume==.6f&&phone.saves==1&&speaker.muted);return 0;
 }
 phone.set_master_volume_control(&master);
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


@pytest.mark.parametrize('mode', ['getter', 'zero-sync', 'setter', 'number', 'restore', 'legacy'])
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
