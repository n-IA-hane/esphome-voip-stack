"""The actual endpoint publisher must fit HA states without losing capabilities."""
import subprocess

from test_refer_dialog_runtime import ROOT, SOURCE, block


def test_full_video_endpoint_with_pbx_packet_time_fits_home_assistant(tmp_path):
    source = (SOURCE / "voip_stack.cpp").read_text()
    code = r'''
#include <cstdio>
#include <cstring>
#include <string>
#include <iostream>
#include "esphome/components/voip_stack/sip_types.h"
using namespace esphome::voip_stack;
#define ESP_LOGW(...) ((void)0)
struct FriendlyName{std::string str()const{return "unused";}};
struct Application{FriendlyName get_friendly_name()const{return {};}}App;
enum class TransportType{TCP,UDP};
struct VoipStack {
 std::string device_name_="Waveshare S3 Touch LCD 1.85C V2",extension_="101";
 uint16_t sip_port_=5060,rtp_port_=40000;TransportType protocol_=TransportType::UDP;
 AudioFormatList tx_audio_formats_{},rx_audio_formats_{};
 std::string local_ip_string_()const{return "192.168.1.192";}
 const char*audio_capability_()const{return "full_duplex";}
 std::string build_endpoint_string_()const;
 static std::string audio_format_token_(const AudioFormat&);
 static std::string audio_rtp_format_token_(const AudioFormat&);
};
'''
    for marker in ["std::string VoipStack::audio_format_token_(", "std::string VoipStack::audio_rtp_format_token_(",
                   "std::string VoipStack::build_endpoint_string_("]:
        code += "\n" + block(source, marker)
    code += r'''
AudioFormat format(unsigned rate,unsigned ms){AudioFormat f;f.sample_rate=rate;f.frame_ms=ms;return f;}
int main(){VoipStack s;s.tx_audio_formats_.count=3;s.rx_audio_formats_.count=6;
 s.tx_audio_formats_.formats[0]=format(16000,16);s.tx_audio_formats_.formats[1]=format(16000,10);s.tx_audio_formats_.formats[2]=format(16000,20);
 for(unsigned i=0;i<6;++i){constexpr unsigned rates[]={48000,32000,16000,16000,16000,16000},ms[]={10,16,10,16,32,20};s.rx_audio_formats_.formats[i]=format(rates[i],ms[i]);}
 std::cout<<s.build_endpoint_string_();}
'''
    cpp = tmp_path / "endpoint.cpp"
    cpp.write_text(code)
    binary = tmp_path / "endpoint"
    build = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-DUSE_ESPHOME_VOIP_STACK_VIDEO_JPEG",
                            "-I", str(ROOT), str(cpp), "-o", str(binary)], capture_output=True, text=True)
    assert build.returncode == 0, build.stderr
    endpoint = subprocess.check_output([binary], text=True)
    assert len(endpoint) <= 255, f"Home Assistant rejects this {len(endpoint)}-character state"
    fields = [part.strip() for part in endpoint.split("|")]
    assert fields[:5] == ["Waveshare S3 Touch LCD 1.85C V2", "192.168.1.192", "5060", "40000", "full_duplex"]
    assert fields[5].replace(" ", "").split(";") == ["16000:s16le:1:16", "16000:s16le:1:10", "16000:s16le:1:20"]
    assert "16000:s16le:1:20" in fields[6] and len(fields[6].split(";")) == 6
    assert fields[7:] == ["sip_udp", "101", "sf=d1", "video=jpeg"]


def test_runtime_extension_rejection_preserves_published_and_persisted_value(tmp_path):
    source = (SOURCE / "voip_stack.cpp").read_text()
    code = r'''
#include <string>
#include <cstdint>
#include <cassert>
#define USE_TEXT
#define ESP_LOGW(...) ((void)0)
static const char* TAG="test";
std::string normalize_endpoint_label(const std::string& value){return value;}
struct Text{std::string state;void publish_state(const std::string&v){state=v;}};
struct VoipStack{
 std::string extension_="101";uint8_t extension_max_characters_=6,extension_max_bytes_=24;
 Text text;Text*extension_text_=&text;unsigned publishes=0,saves=0;
 void request_endpoint_publish_(){++publishes;}
 void schedule_save_routing_settings_(){++saves;}
 void set_extension(const std::string&);
};
'''
    code += block(source, "void VoipStack::set_extension(")
    code += r'''
int main(){VoipStack s;(void)TAG;s.set_extension("1234567");assert(s.extension_=="101"&&s.text.state=="101"&&s.saves==0&&s.publishes==0);
 s.set_extension("😀😀😀😀😀😀");assert(s.extension_=="😀😀😀😀😀😀"&&s.saves==1&&s.publishes==1);
 s.extension_max_bytes_=23;s.set_extension("😀😀😀😀😀😁");assert(s.extension_=="😀😀😀😀😀😀"&&s.saves==1);
 s.set_extension("123456");assert(s.extension_=="123456"&&s.saves==2&&s.publishes==2);}
'''
    cpp = tmp_path / "extension.cpp"
    cpp.write_text(code)
    binary = tmp_path / "extension"
    result=subprocess.run(["g++","-std=c++17","-Wall","-Wextra","-Werror",str(cpp),"-o",str(binary)],capture_output=True,text=True)
    assert result.returncode==0,result.stderr
    subprocess.run([binary],check=True)
