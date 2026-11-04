"""A negotiated wire rate must not restart a compatible prepared PCM sink."""

from pathlib import Path
import os
import subprocess

import pytest

from test_incoming_setup_runtime import incoming_source
from test_mixed_codec_runtime import mixed_playout_binary as mixed_playout_binary
from test_refer_dialog_runtime import block

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "esphome/components/voip_stack"


@pytest.fixture(scope="module")
def pcm_clock_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("pcm-playout-clock")
    source = incoming_source()
    header = (COMPONENT / "voip_stack.h").read_text()
    fsm = (COMPONENT / "voip_fsm.cpp").read_text()
    source = source.replace(
        "struct CurrentMediaFormats {AudioFormat tx,rx;};",
        "struct CurrentMediaFormats {AudioFormat caller_to_dest,dest_to_caller,tx,rx;};"
        "CurrentMediaFormats current_media_formats_;mutable int media_format_mutex_=0;"
        "std::atomic<uint32_t>current_tx_audio_format_packed_{0},current_rx_audio_format_packed_{0};"
        "std::atomic<size_t>current_tx_audio_frame_bytes_{0};AudioFormat rx_audio_format_;",
    )
    for marker in ("CurrentMediaFormats snapshot_current_media_formats_(", "void set_current_media_formats_("):
        source = source.replace(block(source, marker), block(header, marker))
    source = source.replace("struct VoipStack {", r'''
namespace audio {
struct AudioStreamInfo {
 uint8_t bits,channels;uint32_t rate;
 uint8_t get_bits_per_sample()const{return bits;}
 uint8_t get_channels()const{return channels;}
 uint32_t get_sample_rate()const{return rate;}
};
}
audio::AudioStreamInfo audio_stream_info_from_format(const AudioFormat&f){
 return {audio_format_bits_per_sample(f),f.channels,f.sample_rate};
}
struct Speaker {
 audio::AudioStreamInfo info{16,1,48000};bool running=true,stopped=false;
 unsigned starts=0,stops=0;
 const audio::AudioStreamInfo&get_audio_stream_info(){return info;}
 void set_audio_stream_info(audio::AudioStreamInfo next){info=next;}
 bool is_running(){return running;}bool is_stopped(){return stopped;}
 void stop(){++stops;running=false;}
 void start(){++starts;running=true;stopped=false;}
};
struct VoipStack {
 Speaker*speaker_=nullptr;
 enum class RxPlayoutPhase:uint8_t{STOPPED,PREPARING,WAITING_OUTPUT,READY,ACTIVE};
 RxPlayoutPhase rx_playout_phase_(uint32_t revision)const{
  const uint32_t state=rx_playout_state_;
  return (state>>3)==revision?static_cast<RxPlayoutPhase>(state&7):RxPlayoutPhase::STOPPED;
 }
 void start_speaker_for_current_rx_();
''' + block(header, "AudioFormat get_current_rx_audio_format_("))
    source = source.replace("bool prepare_speaker_for_current_rx_(){return true;}",
                            "bool prepare_speaker_for_current_rx_();")
    source += "\n" + block(fsm, "void VoipStack::start_speaker_for_current_rx_(")
    source += "\n" + block(fsm, "bool VoipStack::prepare_speaker_for_current_rx_(")
    source = '#include <cstdio>\n#include "esphome/components/voip_stack/rtp_audio_payloads.h"\n' + source
    source += r'''
int main(int argc,char**argv){
 assert(argc==2);const std::string mode=argv[1];
 AudioFormat preferred{48000,PcmFormat::S16LE,1,10,AudioCodec::PCM};
 AudioFormat wire{16000,PcmFormat::S16LE,1,10,AudioCodec::PCM};
 bool preserve=true;
 if(mode=="native16")preferred.sample_rate=16000;
 else if(mode=="different-ptime")preferred.frame_ms=20;
 else if(mode=="opus")wire={48000,PcmFormat::S16LE,1,10,AudioCodec::OPUS};
 else if(mode=="declared-20ms"){wire.frame_ms=20;}
 else if(mode=="s32-fallback"){wire.pcm_format=PcmFormat::S32LE;preserve=false;}
 else if(mode=="mixed-pool"||mode=="mixed-s32")preserve=false;
 else assert(mode=="uac48-to16");
 Speaker sink;sink.info={16,preferred.channels,preferred.sample_rate};
 VoipStack phone;phone.speaker_=&sink;phone.rx_audio_format_=preferred;
 phone.rx_audio_formats_.count=2;phone.rx_audio_formats_.formats[0]=preferred;
 phone.rx_audio_formats_.formats[1]=wire;
 if(mode=="mixed-pool"||mode=="mixed-s32"){
  phone.rx_audio_formats_.count=3;auto alternate=wire;
  if(mode=="mixed-pool")alternate.codec=AudioCodec::OPUS;
  else alternate.pcm_format=PcmFormat::S32LE;
  phone.rx_audio_formats_.formats[2]=alternate;
 }
 phone.tx_audio_formats_.count=1;phone.tx_audio_formats_.formats[0]=wire;
 phone.set_current_media_formats_(wire,preferred,wire,preferred);
 phone.rx_audio_revision_=3;phone.rx_playout_state_=(3<<3)|static_cast<unsigned>(VoipStack::RxPlayoutPhase::READY);
 phone.call_state_=CallState::REMOTE_RINGING;
 phone.set_call_identity_("normal-uac","esp","esp","peer","peer");
 SipSignal answer;answer.type=SipSignalType::STATUS_200_OK;answer.call_id="normal-uac";
 answer.selected_tx_format=wire;answer.selected_rx_format=wire;
 phone.on_sip_signal_received_(answer);
 const auto formats=phone.snapshot_current_media_formats_();
 const auto local=phone.get_current_rx_audio_format_();
 fprintf(stderr,"mode=%s wire=%u/%u local=%u/%u stops=%u revision=%u state=%u\n",mode.c_str(),
         formats.rx.sample_rate,formats.rx.frame_ms,local.sample_rate,local.frame_ms,
         sink.stops,unsigned(phone.rx_audio_revision_),unsigned(phone.call_state_.load()));
 assert(formats.rx==wire&&formats.tx==wire&&formats.caller_to_dest==wire&&formats.dest_to_caller==wire);
 assert(local.frame_ms==wire.frame_ms&&local.codec==wire.codec);
 if(preserve){
  assert(!sink.stops&&!sink.starts&&phone.rx_audio_revision_==3&&
         "A compatible local PCM clock must survive the normal UAC answer without sink stop/start");
  assert(local.sample_rate==preferred.sample_rate&&phone.call_state_==CallState::IN_CALL);
 }else{
  assert(local==wire&&sink.stops==1&&phone.call_state_==CallState::REMOTE_RINGING);
  sink.stopped=true;phone.pump_application_media_start_();
  assert(phone.call_state_==CallState::IN_CALL&&sink.info.rate==wire.sample_rate);
 }
}
'''
    cpp = folder / "clock.cpp"
    cpp.write_text(source)
    binary = folder / "clock"
    built = subprocess.run(
        ["g++", "-std=c++17", "-DUSE_ESPHOME_VOIP_STACK_SPEAKER", "-I", str(ROOT),
         str(cpp), "-o", str(binary)], capture_output=True, text=True,
    )
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize("mode", ["uac48-to16", "different-ptime", "native16", "opus",
                                  "declared-20ms", "s32-fallback", "mixed-pool", "mixed-s32"])
def test_actual_uac_answer_preserves_local_pcm_clock(pcm_clock_binary, mode):
    run = subprocess.run(
        ["bash", "-c", 'ulimit -c 0; exec "$1" "$2"', "bash", str(pcm_clock_binary), mode],
        capture_output=True, text=True, timeout=5,
    )
    assert run.returncode == 0, run.stderr


@pytest.fixture(scope="module")
def pcm_transport_binary(mixed_playout_binary, tmp_path_factory):
    folder = tmp_path_factory.mktemp("pcm-clock-transport")
    adapter = mixed_playout_binary.parent
    code = (adapter / "mixed.cpp").read_text().split("int main(", 1)[0]
    code = code.replace("return transport_->rx;",
                        "return pcm_playout_format(transport_->rx,transport_->offer_rx_formats_);")
    code = code.replace("frames.size()==8", "frames.size()==20").replace("frames.size()<8", "frames.size()<20")
    code = code.replace("if(++emitted==6)throw EndRtp{};", "if(++emitted==next_emission_limit)throw EndRtp{};")
    code = code.replace(
        "void ulTaskNotifyTake(int,unsigned ticks){if(ticks!=portMAX_DELAY)tick+=ticks;}",
        "void ulTaskNotifyTake(int,unsigned ticks){if(ticks!=portMAX_DELAY){tick+=ticks;if(on_clock)on_clock();}}",
    )
    code = code.replace(
        "void deliver(const TransportAudioFrame&frame){phone->on_audio_received_(frame);}",
        "void deliver(const TransportAudioFrame&frame){"
        "const auto wire=unpack_audio_format(frame.format_packed);"
        "assert(wire.sample_rate==16000&&wire.frame_ms==10&&frame.bytes==320);"
        "assert(frame.timestamp==uint32_t(frame.sequence)*160);"
        "phone->on_audio_received_(frame);}",
    )
    code = "#include <functional>\nunsigned next_emission_limit=6;std::function<void()>on_clock;\n" + code
    code += r'''
int main(int argc,char**argv){
 assert(argc==3);const unsigned local_rate=std::stoul(argv[1]);
 SipTransport s;const auto wire=fmt(16000,10,AudioCodec::PCM),local=fmt(local_rate,10,AudioCodec::PCM);
 s.peer_directional_audio_v1_=std::string(argv[2])=="directional";
 s.offer_tx_formats_.count=1;s.offer_tx_formats_.formats[0]=wire;
 s.offer_rx_formats_.count=local_rate==16000?1:2;s.offer_rx_formats_.formats[0]=local;
 if(s.offer_rx_formats_.count==2)s.offer_rx_formats_.formats[1]=wire;
 s.set_audio_formats(s.offer_tx_formats_,s.offer_rx_formats_);
 const auto offer=s.build_sdp_offer_();
 // Dynamic PT comes from the wire offer, including the stable standard-peer
 // receive capability union. Decode it independently of the production map.
 const auto mapping=offer.find(" L16/16000/1\r\n");assert(mapping!=std::string::npos);
 const auto line=offer.rfind("a=rtpmap:",mapping);assert(line!=std::string::npos);
 const unsigned pt=std::stoul(offer.substr(line+9,mapping-line-9));assert(pt>=96&&pt<120);
 const std::string answer="v=0\r\nc=IN IP4 127.0.0.1\r\nm=audio 40002 RTP/AVP "+std::to_string(pt)+
   "\r\na=rtpmap:"+std::to_string(pt)+" L16/16000/1\r\na=ptime:10\r\na=sendrecv\r\n"+
   (s.peer_directional_audio_v1_?"a=x-voip-stack-flow:"+std::to_string(pt)+" sendrecv\r\n":"");
 assert(s.learn_remote_rtp_from_sdp_(answer,0,true)&&s.accepted.count==1&&s.rx==wire);
 AudioFormat mapped;assert(s.accepted.find(pt,&mapped)&&mapped==wire);
 s.rtp_port_=0;assert(s.prepare_media_path_locked_()&&s.rx==wire);
 assert(s.commit_media_path_locked_());
 sockaddr_in destination{};socklen_t length=sizeof(destination);
 assert(getsockname(s.rtp_socket_,reinterpret_cast<sockaddr*>(&destination),&length)==0);
 const int sender=socket(AF_INET,SOCK_DGRAM,0);assert(sender>=0);
 uint8_t storage[16*2048]{};RtpJitterBuffer jitter(storage,2048,16,1);Speaker speaker;VoipStack app;
 app.speaker_=&speaker;app.rx_jitter_buffer_=&jitter;app.transport_=&s;phone=&app;
 std::vector<int16_t> tone(160);
 for(unsigned i=0;i<160;++i)tone[i]=int16_t(9000*std::sin(2*3.141592653589793*500*i/16000));
 unsigned produced=0;
 auto send_packet=[&](){
  const unsigned seq=++produced;
  uint8_t packet[332]{};packet[0]=0x80;packet[1]=pt;packet[3]=seq;
  const unsigned stamp=seq*160;packet[4]=stamp>>24;packet[5]=stamp>>16;packet[6]=stamp>>8;packet[7]=stamp;packet[11]=42;
  for(unsigned i=0;i<160;++i){packet[12+2*i]=uint16_t(tone[i])>>8;packet[13+2*i]=tone[i];}
  assert(sendto(sender,packet,sizeof(packet),0,reinterpret_cast<sockaddr*>(&destination),sizeof(destination))==332);
 };
 for(unsigned n=0;n<6;++n)send_packet();
 try{s.rtp_task_();assert(false);}catch(const EndRtp&){}
 assert(s.rtp_rx_packets_==6&&jitter.depth()==6&&!jitter.counters().drops);
 on_clock=[&](){
  if(produced==20)return;
  send_packet();next_emission_limit=produced;
  try{s.rtp_task_();assert(false);}catch(const EndRtp&){}
 };
 try{app.rx_task_();assert(false);}catch(const EndPlayout&){}
 on_clock={};
 assert(speaker.frames.size()==20&&s.rtp_rx_packets_==20&&!s.decodes&&s.resets==1&&s.rx==wire);
 assert(!jitter.counters().drops&&!jitter.counters().missing&&!jitter.counters().late);
 std::vector<int16_t> samples;
 for(unsigned i=0;i<speaker.frames.size();++i){
  const auto&frame=speaker.frames[i];
  if(local_rate==16000)assert(frame==tone);
  if(i>0)assert(frame.size()==local_rate/100);
  samples.insert(samples.end(),frame.begin(),frame.end());
 }
 const unsigned expected=20*local_rate/100;
 assert(samples.size()<=expected&&samples.size()+64>=expected);
 double real=0,imag=0,power=0;const unsigned skip=local_rate/100;
 for(unsigned i=skip;i<samples.size();++i){
  const double phase=2*3.141592653589793*500*i/local_rate;
  real+=samples[i]*std::cos(phase);imag+=samples[i]*std::sin(phase);power+=double(samples[i])*samples[i];
 }
 const double count=samples.size()-skip;
 assert(2*(real*real+imag*imag)/(count*power)>.95);
 assert(2*std::sqrt(real*real+imag*imag)/count>8500);
 close(sender);close(s.rtp_socket_);
}
'''
    cpp = folder / "transport.cpp"
    cpp.write_text(code)
    library = Path(os.environ["ESP_AUDIO_LIBS_SOURCE"])
    sources = ["src/resample/resampler.cpp", "src/resample/art_resampler.cpp", "src/resample/art_biquad.cpp",
               "src/quantization_utils.cpp", "src/memory_utils.cpp", "src/dsp/dsps_dotprod_f32_ansi.c"]
    opus = subprocess.run(["pkg-config", "--cflags", "--libs", "opus"], capture_output=True, text=True, check=True)
    binary = folder / "transport"
    built = subprocess.run(
        ["g++", "-std=c++20", "-DUSE_ESPHOME_VOIP_STACK_OPUS", "-fsanitize=address,undefined",
         "-fno-pie", "-no-pie", "-I", str(adapter), "-I", str(ROOT), "-I", str(library / "include"),
         "-I", str(COMPONENT), str(cpp), str(COMPONENT / "opus_rtp_codec.cpp"),
         *(str(library / name) for name in sources), *opus.stdout.split(), "-o", str(binary)],
        capture_output=True, text=True,
    )
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize("local_rate", [16000, 48000])
@pytest.mark.parametrize("peer", ["standard", "directional"])
def test_real_sdp_rtp_pcm_conversion_preserves_wire_and_first_frames(pcm_transport_binary, local_rate, peer):
    run = subprocess.run([str(pcm_transport_binary), str(local_rate), peer], capture_output=True, text=True, timeout=5,
                         env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
    assert run.returncode == 0, run.stderr


def test_actual_refer_reuses_pcm_target_and_retains_mixed_codec_boundary(tmp_path):
    source = r'''
#include <cassert>
#include "esphome/components/voip_stack/pcm_receive_converter.h"
using namespace esphome::voip_stack;
struct ReferDialog {AudioFormat selected_tx_format_,selected_rx_format_;RtpAudioPayloads negotiated_rx_payloads_;};
struct SipTransport {
 AudioFormat tx,rx;AudioFormatList offer_rx_formats_;PcmReceiveConverter pcm_receive_converter_;size_t udp_max_payload_=1200;
 void get_media_config_(AudioFormat*t,AudioFormat*r,void*,void*){*t=tx;*r=rx;}
 bool prepare_refer_media_(ReferDialog&);
};
'''
    source += block((COMPONENT / "sip_refer.cpp").read_text(), "bool SipTransport::prepare_refer_media_(")
    source += r'''
int main(){
 const AudioFormat pcm16{16000,PcmFormat::S16LE,1,10,AudioCodec::PCM};
 auto pcm48=pcm16;pcm48.sample_rate=48000;
 auto pcm32=pcm16;pcm32.sample_rate=32000;
 auto opus16=pcm16;opus16.codec=AudioCodec::OPUS;
 for(bool mixed:{false,true}){
  SipTransport s;s.tx=s.rx=pcm16;s.offer_rx_formats_.count=3;
  s.offer_rx_formats_.formats[0]=pcm48;s.offer_rx_formats_.formats[1]=pcm16;
  s.offer_rx_formats_.formats[2]=mixed?opus16:pcm32;
  const auto target=pcm_playout_format(s.rx,s.offer_rx_formats_);
  assert(target==(mixed?pcm16:pcm48));
  RtpAudioPayloads original;original.add(97,pcm16);
  assert(s.pcm_receive_converter_.prepare(original,decoded_pcm_format(target)));
  ReferDialog next;next.selected_tx_format_=pcm16;next.selected_rx_format_=mixed?pcm16:pcm32;
  next.negotiated_rx_payloads_.add(98,mixed?opus16:pcm32);
  assert(s.prepare_refer_media_(next));
  assert(s.rx==pcm16&&next.selected_rx_format_==pcm16);
  if(!mixed){
   uint8_t input[640]{},output[960]{};
   const auto bytes=s.pcm_receive_converter_.convert(pcm32,input,sizeof(input),output,sizeof(output));
   assert(bytes>800&&bytes<=960);
  }else{
   // Empty PCM fallback maps must use the unchanged 16 kHz decoder target.
   assert(next.negotiated_rx_payloads_.pcm_only().count==0);
   next.selected_rx_format_=opus16;
   assert(!s.prepare_refer_media_(next)&&"Existing REFER codec changes remain unsupported");
  }
 }
}
'''
    cpp = tmp_path / "refer.cpp"
    cpp.write_text(source)
    library = Path(os.environ["ESP_AUDIO_LIBS_SOURCE"])
    sources = ["src/resample/resampler.cpp", "src/resample/art_resampler.cpp", "src/resample/art_biquad.cpp",
               "src/quantization_utils.cpp", "src/memory_utils.cpp", "src/dsp/dsps_dotprod_f32_ansi.c"]
    binary = tmp_path / "refer"
    built = subprocess.run(
        ["g++", "-std=c++20", "-DUSE_ESPHOME_VOIP_STACK_SPEAKER", "-DUSE_ESPHOME_VOIP_STACK_OPUS",
         "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-I", str(ROOT),
         "-I", str(library / "include"), str(cpp), *(str(library / name) for name in sources), "-o", str(binary)],
        capture_output=True, text=True,
    )
    assert built.returncode == 0, built.stderr
    run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5,
                         env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
    assert run.returncode == 0, run.stderr
