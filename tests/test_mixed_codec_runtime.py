"""Mixed codec offers retain every negotiated same-geometry receive mapping."""

from pathlib import Path
import os
import subprocess
import sys

import pytest
from test_sdp_receive_payloads import sdp_fixture_source
from test_refer_dialog_runtime import block

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("opus_first", [False, True])
def test_mixed_answer_keeps_both_advertised_receive_payloads(tmp_path, opus_first):
    code = "#define USE_ESPHOME_VOIP_STACK_OPUS\n" + sdp_fixture_source()
    code += r'''
AudioFormat fmt(AudioCodec codec){AudioFormat f;f.frame_ms=20;f.codec=codec;return f;}
int main(int argc,char**argv){
 const bool opus_first=std::string(argv[1])=="opus";
 SipTransport s;s.offer_tx_formats_.count=s.offer_rx_formats_.count=2;
 const auto first=fmt(opus_first?AudioCodec::OPUS:AudioCodec::PCM);
 const auto second=fmt(opus_first?AudioCodec::PCM:AudioCodec::OPUS);
 s.offer_tx_formats_.formats[0]=s.offer_rx_formats_.formats[0]=first;
 s.offer_tx_formats_.formats[1]=s.offer_rx_formats_.formats[1]=second;
 for(bool incoming:{false,true}){
  const auto offer=s.build_sdp_offer_();assert(s.local_offered_rx_payloads_.count==2);
  const std::string maps=std::string("a=rtpmap:96 ")+(opus_first?"opus/48000/2":"L16/16000/1")+
    "\r\na=rtpmap:97 "+(opus_first?"L16/16000/1":"opus/48000/2")+"\r\n";
  const std::string answer="v=0\r\nc=IN IP4 192.0.2.2\r\nm=audio 40002 RTP/AVP 96 97\r\na=ptime:20\r\n"+maps+"a=sendrecv\r\n";
  assert(s.learn_remote_rtp_from_sdp_(answer,0,!incoming));
  AudioFormat f;assert(s.accepted.find(96,&f)&&f.codec==first.codec);
  assert(s.accepted.find(97,&f)&&f.codec==second.codec&&"A negotiated fallback codec must remain receivable");
  assert(s.accepted.count==2&&!s.accepted.find(98,&f));
  if(incoming){auto local=s.build_sdp_answer_();assert(local.find("a=rtpmap:97 ")!=std::string::npos);}
 }
}
'''
    path = tmp_path / "mixed.cpp"
    path.write_text(code)
    binary = tmp_path / "mixed"
    result = subprocess.run(["g++", "-std=c++17", "-I", str(ROOT), str(path), "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([str(binary), "opus" if opus_first else "pcm"], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


@pytest.fixture(scope="module")
def mixed_playout_binary(tmp_path_factory):
    library = Path(os.environ["ESP_AUDIO_LIBS_SOURCE"])
    component = ROOT / "esphome/components/voip_stack"
    path = tmp_path_factory.mktemp("mixed-playout")
    core = path / "esphome/core"
    core.mkdir(parents=True)
    (core / "defines.h").write_text("#pragma once\n")
    (core / "helpers.h").write_text("#pragma once\nnamespace esphome {struct Mutex {};struct LockGuard {explicit LockGuard(Mutex&) {}};}\n")
    (core / "log.h").write_text("#pragma once\n#define ESP_LOGE(...) ((void)0)\n#define ESP_LOGI ESP_LOGE\n#define ESP_LOGW ESP_LOGE\n")
    fixture = (ROOT / "tests/fixtures/mixed_codec_playout.cpp").read_text()
    before, after = fixture.split("// TRANSPORT_FIELDS")
    code = "#define USE_ESPHOME_VOIP_STACK_OPUS\n#define USE_ESPHOME_VOIP_STACK_SPEAKER\n" + before
    sdp = sdp_fixture_source()
    fields = r'''
 int media_config_lock_=0,rtp_socket_=-1;
 unsigned emitted=0,decodes=0,plc=0,resets=0;
 std::atomic<bool>rtp_running_{false},rtp_task_terminate_{false},media_active_{true},rtp_task_quiesced_{true};
 std::atomic<uint16_t>rtp_sequence_{0};std::atomic<uint32_t>rtp_timestamp_{0};uint32_t rtp_ssrc_=0;
 std::atomic<bool>rtp_ssrc_latched_{false};
 std::atomic<uint32_t>media_proposal_epoch_{0},latched_rtp_ip_v4_{0},latched_rtp_ssrc_{0},rtp_rx_packets_{0},rtp_rx_bytes_{0},rtp_rx_datagrams_{0};
 std::atomic<uint16_t>latched_rtp_port_{0};
 enum class MediaLifecyclePhase{ACTIVE,CLEANING,IDLE,PREPARED};std::atomic<MediaLifecyclePhase>media_lifecycle_phase_{MediaLifecyclePhase::IDLE};
 Mutex media_lifecycle_mutex_,dialog_mutex_;void *rtp_cleanup_done_=nullptr,*rtp_task_done_=nullptr,*rtp_task_handle_=this;
 std::string call_id_;
 OpusRtpCodec opus_codec_;PcmReceiveConverter pcm_receive_converter_;bool opus_codec_ready_=false;
 void set_audio_formats(const AudioFormatList&,const AudioFormatList&);
 bool prepare_media_path_locked_();bool commit_media_path_locked_();bool bind_udp_(int*,uint16_t,const char*);
 void reset_rtp_latch_();
 void rtp_task_();bool received_audio_format_(uint8_t,AudioFormat*,const uint8_t* =nullptr,size_t=0)const;
 void reset_audio_decoder();
 size_t decode_audio_payload(const uint8_t*,size_t,const AudioFormat&,uint8_t*,size_t);
 size_t convert_received_pcm(const AudioFormat&,const uint8_t*,size_t,uint8_t*,size_t);
 void emit_audio_frame_(const uint8_t*p,size_t n,uint16_t seq,uint32_t ts,bool changed,uint32_t f){
  deliver({p,n,seq,ts,true,changed,f});if(++emitted==6)throw EndRtp{};
 }
 void close_media_session_(){media_active_=false;}
 void finish_audio_path_stop_(){assert(false);}
 void emit_media_quiesced_(){}
 bool send_bye_unlocked_(const std::string&){assert(false);return false;}
 void reset_dialog_(){assert(false);}
 void emit_sip_signal_(SipSignal){assert(false);}
'''
    code += sdp.replace("struct SipTransport {", "struct SipTransport {" + fields) + after
    transport = (component / "sip_transport.cpp").read_text()
    parser = (component / "sip_message.cpp").read_text()
    audio = (component / "voip_audio.cpp").read_text()
    fsm = (component / "voip_fsm.cpp").read_text()
    methods = block(parser, "size_t rtp_payload_to_pcm(") + "\n"
    old_getter = block(code, "void get_media_config_(")
    getter = block(transport, "void SipTransport::get_media_config_(")
    code = code.replace(old_getter, getter[:getter.index("{")].replace("SipTransport::", "").strip() + ";")
    methods += getter + "\n"
    for marker in ("bool SipTransport::received_audio_format_(", "void SipTransport::set_audio_formats(", "bool SipTransport::prepare_media_path_locked_(", "bool SipTransport::commit_media_path_locked_(", "bool SipTransport::bind_udp_(", "void SipTransport::reset_rtp_latch_(", "void SipTransport::rtp_task_(", "void SipTransport::reset_audio_decoder(", "size_t SipTransport::decode_audio_payload(", "size_t SipTransport::convert_received_pcm("):
        body = block(transport, marker)
        if "reset_audio_decoder" in marker:
            body = body.replace("{", "{++resets;", 1)
        if "decode_audio_payload" in marker:
            body = body.replace("{", "{++decodes;if(payload==nullptr)++plc;", 1)
        methods += body + "\n"
    for marker in ("void VoipStack::on_audio_received_(",):
        methods += block(fsm, marker) + "\n"
    for marker in ("void VoipStack::enqueue_rx_frame_(", "void VoipStack::play_rx_frame_(", "void VoipStack::play_silence_frame_(", "void VoipStack::rx_task_("):
        methods += block(audio, marker) + "\n"
    code = code.replace("// PRODUCTION_METHODS", methods)
    cpp = path / "mixed.cpp"
    cpp.write_text(code)
    sources = ["src/resample/resampler.cpp", "src/resample/art_resampler.cpp", "src/resample/art_biquad.cpp", "src/quantization_utils.cpp", "src/memory_utils.cpp", "src/dsp/dsps_dotprod_f32_ansi.c"]
    opus = subprocess.run(["pkg-config", "--cflags", "--libs", "opus"], capture_output=True, text=True)
    assert opus.returncode == 0, "Install libopus-dev and pkg-config for the real codec regression"
    binary = path / "mixed"
    result = subprocess.run(["g++", "-std=c++17", "-DUSE_ESPHOME_VOIP_STACK_OPUS", "-I", str(path), "-I", str(ROOT), "-I", str(library / "include"), "-I", str(component), str(cpp), str(component / "opus_rtp_codec.cpp"), *(str(library / source) for source in sources), *opus.stdout.split(), "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("opus_first", [False, True])
@pytest.mark.parametrize("rate,frame", [(8000, 20), (16000, 20), (24000, 20), (48000, 10)])
def test_actual_rtp_and_playout_switch_pcm_opus_and_conceal_the_last_codec(mixed_playout_binary, opus_first, rate, frame):
    result = subprocess.run([str(mixed_playout_binary), "opus" if opus_first else "pcm", str(rate), str(frame)], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr


@pytest.fixture(scope="module")
def g722_playout_binary(mixed_playout_binary, tmp_path_factory):
    """Reuse the hardware adapters, retaining every extracted production method."""
    folder = tmp_path_factory.mktemp("g722-playout")
    code = mixed_playout_binary.with_suffix(".cpp").read_text()
    code = code[:code.index("int main(")]
    code = code.replace("#define USE_ESPHOME_VOIP_STACK_OPUS", "#define USE_ESPHOME_VOIP_STACK_G722")
    code = code.replace("#define USE_ESPHOME_VOIP_STACK_G722\n", "", 1)
    transport = (ROOT / "esphome/components/voip_stack/sip_transport.cpp").read_text()
    getter_declaration = block(transport, "void SipTransport::get_media_config_(").split("{")[0].replace("SipTransport::", "").strip() + ";"
    assert getter_declaration in code
    code = code.replace(getter_declaration, getter_declaration.replace("RtpAudioPayloads *rx_payloads)", "RtpAudioPayloads *rx_payloads = nullptr)"), 1)
    code = code.replace('#include "esphome/components/voip_stack/opus_rtp_codec.h"',
                        '#include "esphome/components/voip_stack/g722_rtp_codec.h"')
    code = code.replace("OpusRtpCodec opus_codec_;PcmReceiveConverter pcm_receive_converter_;bool opus_codec_ready_=false;",
                        "G722RtpCodec g722_codec_;PcmReceiveConverter pcm_receive_converter_;bool g722_codec_ready_=false;"
                        "Mutex rtp_socket_mutex_;std::atomic<uint32_t>rtp_tx_packets_{0},rtp_tx_bytes_{0};"
                        "void send_audio_frame(const uint8_t*,size_t);")
    # Bound the genuine ingress loop after the independently sent datagrams,
    # including when a failing admission drops every packet. Poll still uses
    # the real socket; this avoids treating a process timeout as the oracle.
    poll_adapter = r'''
SipTransport *bounded_ingress=nullptr;unsigned bounded_datagrams=6;
int bounded_select(int nfds,fd_set*r,fd_set*w,fd_set*e,timeval*timeout){
 if(bounded_ingress&&bounded_ingress->rtp_rx_datagrams_>=bounded_datagrams)throw EndRtp{};
 timespec duration{0,5000000};if(timeout){duration.tv_sec=timeout->tv_sec;duration.tv_nsec=timeout->tv_usec*1000;}
 return pselect(nfds,r,w,e,&duration,nullptr);
}
#define select bounded_select
'''
    code = code.replace("void SipTransport::rtp_task_() {", poll_adapter + "\nvoid SipTransport::rtp_task_() {", 1)
    component = ROOT / "esphome/components/voip_stack"
    code += block((component / "sip_message.cpp").read_text(), "size_t pcm_to_rtp_payload(") + "\n"
    code += block((component / "sip_transport.cpp").read_text(), "void SipTransport::send_audio_frame(") + "\n"
    code += r'''
#include <spandsp.h>
int main(int argc,char **argv){
 assert(argc==2);const bool mixed=std::string(argv[1]).rfind("mixed",0)==0;const bool asymmetric=std::string(argv[1])=="asymmetric";const bool variable=std::string(argv[1])=="variable";SipTransport s;
 const auto g722=fmt(16000,20,AudioCodec::G722),pcm48=fmt(48000,10,AudioCodec::PCM);
 s.offer_tx_formats_.count=2;s.offer_tx_formats_.formats[0]=g722;s.offer_tx_formats_.formats[1]=fmt(16000,10,AudioCodec::G722);
 s.offer_rx_formats_.count=mixed?2:1;
 s.offer_rx_formats_.formats[0]=mixed?pcm48:g722;
 if(mixed)s.offer_rx_formats_.formats[1]=g722;
 if(variable){s.offer_rx_formats_.count=2;s.offer_rx_formats_.formats[1]=fmt(16000,10,AudioCodec::G722);}
 if(std::string(argv[1])=="mixed-undeclared")s.offer_rx_formats_.formats[1]=fmt(16000,10,AudioCodec::G722);
 if(std::string(argv[1])=="mixed"||std::string(argv[1])=="mixed-times"){
  s.offer_rx_formats_.count=3;s.offer_rx_formats_.formats[2]=fmt(16000,10,AudioCodec::G722);
 }
 s.set_audio_formats(s.offer_tx_formats_,s.offer_rx_formats_);assert(s.g722_codec_ready_);
 if(mixed){s.offer_tx_formats_.count=3;s.offer_tx_formats_.formats[2]=pcm48;} // Ordinary sendrecv advertises only bidirectional mappings.
 const auto offer=s.build_sdp_offer_();
 if(std::string(argv[1])=="mixed-incompatible"){
  // The original48k10+G72220-only receive union cannot honor one maxptime.
  // Reject before any RTP admission, rather than inventing G72210 support.
  assert(offer.empty()&&s.rtp_socket_==-1&&!s.rtp_running_);
  assert(s.rtp_rx_packets_==0&&s.rtp_tx_packets_==0&&s.emitted==0&&s.decodes==0);
  return 0;
 }
 assert(!offer.empty());
 const std::string answer=std::string("v=0\r\nc=IN IP4 127.0.0.1\r\nm=audio 40002 RTP/AVP ")+
  (mixed?"96 97\r\na=rtpmap:96 L16/48000/1\r\na=rtpmap:97 G722/8000/1\r\n":"9\r\n")+
  "a=ptime:"+std::string(asymmetric?"10":"20")+"\r\na=sendrecv\r\n";
 // The pure incoming peer uses static PT9 without any rtpmap attribute.
 assert(s.learn_remote_rtp_from_sdp_(answer,0,mixed));
 assert(s.tx==fmt(16000,asymmetric?10:20,AudioCodec::G722)&&s.rx==(mixed?pcm48:g722)&&s.accepted.count==(mixed?2:1));
 assert(audio_format_rtp_clock_rate(s.tx)==8000);
 if(mixed){AudioFormat received;assert(s.accepted.find(9,&received)&&received.codec==AudioCodec::G722&&!s.accepted.find(97,nullptr));}
 assert(pcm_receive_buffer_capacity(s.offer_rx_formats_)==(std::string(argv[1])=="mixed-undeclared"?960U:mixed?1920U:640U));
 assert(s.g722_codec_.configure(&g722,nullptr,1200));
 assert(!s.prepare_media_path_locked_()&&s.rtp_socket_==-1&&"Every accepted fallback decoder must be ready before admission");
 s.set_audio_formats(s.offer_tx_formats_,s.offer_rx_formats_);
 s.rtp_port_=0;assert(s.prepare_media_path_locked_());assert(s.commit_media_path_locked_());
 sockaddr_in destination{};socklen_t length=sizeof(destination);
 assert(getsockname(s.rtp_socket_,reinterpret_cast<sockaddr*>(&destination),&length)==0);
 const int sender=socket(AF_INET,SOCK_DGRAM,0);assert(sender>=0);
 sockaddr_in peer{};peer.sin_family=AF_INET;peer.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 assert(bind(sender,reinterpret_cast<sockaddr*>(&peer),sizeof(peer))==0);
 length=sizeof(peer);assert(getsockname(sender,reinterpret_cast<sockaddr*>(&peer),&length)==0);
 s.remote_rtp_port_=ntohs(peer.sin_port);s.remote_rtp_ip_v4_=INADDR_LOOPBACK;
 timeval limit{1,0};assert(setsockopt(sender,SOL_SOCKET,SO_RCVTIMEO,&limit,sizeof(limit))==0);
 std::vector<int16_t> low(320),high(480);
 for(size_t i=0;i<low.size();++i)low[i]=int16_t(9000*std::sin(2*3.141592653589793*440*i/16000));
 for(size_t i=0;i<high.size();++i)high[i]=int16_t(9000*std::sin(2*3.141592653589793*440*i/48000));
 uint32_t prior=0;
 for(unsigned n=0;n<2;++n){
  s.send_audio_frame(reinterpret_cast<const uint8_t*>(low.data()),s.tx.nominal_frame_bytes());
  uint8_t packet[512];const auto bytes=recv(sender,packet,sizeof(packet),0);assert(bytes==(asymmetric?92:172));
  assert((packet[1]&127)==s.tx_pt);
  const uint32_t ts=uint32_t(packet[4])<<24|uint32_t(packet[5])<<16|uint32_t(packet[6])<<8|packet[7];
  if(n)assert(ts-prior==(asymmetric?80U:160U)&&"G722 timestamps use8k, not the16k PCM rate");prior=ts;
 }
 assert(s.rtp_tx_packets_==2);
 uint8_t storage[16*2048]{};RtpJitterBuffer jitter(storage,2048,16,1);Speaker speaker;VoipStack app;
 app.speaker_=&speaker;app.rx_jitter_buffer_=&jitter;app.transport_=&s;phone=&app;
 app.rx_audio_chunk_alloc_bytes_=std::string(argv[1])=="mixed-undeclared"?960:mixed?1920:640;app.rx_jitter_frame_alloc_bytes_=mixed?960:640;
 auto *encoder=g722_encode_init(nullptr,64000,0);assert(encoder);
 auto *reference=g722_decode_init(nullptr,64000,0);assert(reference);
 std::vector<std::vector<int16_t>> expected_frames;unsigned sample_position=0,wire_timestamp=0;
 for(unsigned n=0;n<6;++n){
  const unsigned seq=variable?n+1:std::vector<unsigned>{1,2,4,5,7,8}[n];
  const bool encoded=!mixed||seq==4||seq==5;std::vector<uint8_t> packet(12+960);
  packet[0]=0x80;packet[1]=mixed?(encoded?9:96):9;packet[2]=seq>>8;packet[3]=seq;packet[11]=42;
  const unsigned duration=variable?(n%3==1?20:10):20;
  const unsigned ts=variable?wire_timestamp:seq*(encoded?160:480);wire_timestamp+=8*duration;packet[4]=ts>>24;packet[5]=ts>>16;packet[6]=ts>>8;packet[7]=ts;
  size_t bytes=0;
  if(encoded){
   std::vector<int16_t> input(duration*16);
   for(unsigned i=0;i<input.size();++i)input[i]=variable?int16_t(9000*std::sin(2*3.141592653589793*440*(sample_position+i)/16000)):low[i];
   sample_position+=input.size();bytes=g722_encode(encoder,packet.data()+12,input.data(),input.size());assert(bytes==duration*8);
   if(variable){std::vector<int16_t> expected(input.size());assert(g722_decode(reference,expected.data(),packet.data()+12,bytes)==int(input.size()));expected_frames.push_back(expected);}
  }
  else{bytes=high.size()*2;for(size_t i=0;i<high.size();++i){packet[12+2*i]=uint16_t(high[i])>>8;packet[13+2*i]=high[i];}}
  assert(sendto(sender,packet.data(),12+bytes,0,reinterpret_cast<sockaddr*>(&destination),sizeof(destination))==ssize_t(12+bytes));
 }
 bounded_ingress=&s;try{s.rtp_task_();assert(false);}catch(const EndRtp&){}
 if(std::string(argv[1])=="mixed-undeclared"){
  assert(s.rtp_rx_datagrams_==6&&s.rtp_rx_packets_==4&&jitter.depth()==4);
  assert(g722_decode_free(reference)==0&&g722_encode_free(encoder)==0);close(sender);close(s.rtp_socket_);return 0;
 }
 assert(s.rtp_rx_packets_==6&&jitter.depth()==6);
 try{app.rx_task_();assert(false);}catch(const EndPlayout&){}
 assert(speaker.frames.size()==8);
 if(variable){
  for(unsigned i=0;i<6;++i)assert(speaker.frames[i]==expected_frames[i]&&"One G722 decoder history must survive10/20/10 on the same PT");
  assert(s.resets==1&&"Duration changes must not reset decoder or resampler state");
  assert(g722_decode_free(reference)==0&&g722_encode_free(encoder)==0);close(sender);close(s.rtp_socket_);return 0;
 }
 if(mixed){
  for(size_t i:{0U,1U,6U,7U})assert(speaker.frames[i]==high);
  assert(speaker.frames[2].size()==480&&speaker.frames[5].size()==960);
  for(size_t i:{3U,4U}){assert(speaker.frames[i].size()>900&&speaker.frames[i].size()<=960);}
  assert(s.decodes==2);
 }else{for(const auto&frame:speaker.frames)assert(frame.size()==320);assert(s.decodes==6);}
 for(size_t i:{2U,5U})for(auto value:speaker.frames[i])assert(value==0);
 double power=0;for(size_t i:{3U,4U})for(auto value:speaker.frames[i])power+=double(value)*value;
 assert(power>1000000&&"Decoded G722 and optional resampling must reach the real playout owner");
 assert(g722_decode_free(reference)==0&&g722_encode_free(encoder)==0);close(sender);close(s.rtp_socket_);
}
'''
    (folder / "esphome/core").mkdir(parents=True)
    for name in ("defines.h", "helpers.h", "log.h"):
        (folder / "esphome/core" / name).write_bytes((mixed_playout_binary.parent / "esphome/core" / name).read_bytes())
    fixture = ROOT / "tests/fixtures/g722_esp_audio_codec_2_5_0"
    api = folder / "api.cpp"
    api.write_text((fixture / "runtime.cpp").read_text().split("static void contracts()")[0])
    cpp = folder / "g722.cpp"
    cpp.write_text(code)
    library = Path(os.environ["ESP_AUDIO_LIBS_SOURCE"])
    sources = ["src/resample/resampler.cpp", "src/resample/art_resampler.cpp", "src/resample/art_biquad.cpp", "src/quantization_utils.cpp", "src/memory_utils.cpp", "src/dsp/dsps_dotprod_f32_ansi.c"]
    binary = folder / "g722"
    result = subprocess.run(["g++", "-std=c++17", "-DUSE_ESPHOME_VOIP_STACK_G722", "-I", str(folder), "-I", str(ROOT), "-I", str(component), "-I", str(fixture), "-I", str(library / "include"), str(cpp), str(api), str(component / "g722_rtp_codec.cpp"), *(str(library / source) for source in sources), "-lspandsp", "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("mode", ["g722", "mixed", "asymmetric", "mixed-times", "mixed-undeclared", "mixed-incompatible", "variable"])
def test_g722_real_rtp_clock_decode_and_mixed_pcm_playout(g722_playout_binary, mode):
    result = subprocess.run([g722_playout_binary, mode], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr


@pytest.fixture(scope="module")
def pcm_packet_times_binary(g722_playout_binary):
    folder = g722_playout_binary.parent
    code = g722_playout_binary.with_suffix(".cpp").read_text().split("int main(")[0]
    code += r'''
int main(int argc,char**argv){
 assert(argc==2);const std::string mode=argv[1];const bool reject=mode!="valid";
 SipTransport s;const auto ten=fmt(16000,10,AudioCodec::PCM),twenty=fmt(16000,20,AudioCodec::PCM);
 s.offer_tx_formats_.count=s.offer_rx_formats_.count=2;
 s.offer_tx_formats_.formats[0]=twenty;s.offer_tx_formats_.formats[1]=ten;
 s.offer_rx_formats_.formats[0]=ten;s.offer_rx_formats_.formats[1]=twenty;
 if(mode=="undeclared")s.offer_rx_formats_.count=1;
 if(mode=="unsafe")s.udp_max_payload_=576;
 s.set_audio_formats(s.offer_tx_formats_,s.offer_rx_formats_);
 const auto offer=s.build_sdp_offer_();assert(offer.find("a=ptime:10\r\n")!=std::string::npos);
 const std::string answer=std::string("v=0\r\nc=IN IP4 127.0.0.1\r\nm=audio 40002 RTP/AVP 96\r\na=rtpmap:96 L16/16000/1\r\na=ptime:")+(mode=="unsafe"?"10":"20")+"\r\na=sendrecv\r\n";
 assert(s.learn_remote_rtp_from_sdp_(answer,0,true));assert(s.tx==(mode=="unsafe"?ten:twenty)&&s.rx==ten);
 s.rtp_port_=0;assert(s.prepare_media_path_locked_());assert(s.commit_media_path_locked_());
 sockaddr_in destination{};socklen_t length=sizeof(destination);
 assert(getsockname(s.rtp_socket_,reinterpret_cast<sockaddr*>(&destination),&length)==0);
 const int sender=socket(AF_INET,SOCK_DGRAM,0);assert(sender>=0);
 uint8_t storage[16*640]{};RtpJitterBuffer jitter(storage,640,16,1);Speaker speaker;VoipStack app;
 app.speaker_=&speaker;app.rx_jitter_buffer_=&jitter;app.transport_=&s;phone=&app;
 app.rx_audio_chunk_alloc_bytes_=app.rx_jitter_frame_alloc_bytes_=640;
 std::vector<int16_t> authored(320);for(unsigned i=0;i<320;++i)authored[i]=int16_t(i*101-16000);
 for(unsigned seq:{1U,2U,4U,5U,7U,8U}){
  uint8_t packet[652]{};packet[0]=0x80;packet[1]=mode=="unknown"?99:s.rx_pt;packet[2]=seq>>8;packet[3]=seq;packet[11]=42;
  const unsigned ts=seq*320;packet[4]=ts>>24;packet[5]=ts>>16;packet[6]=ts>>8;packet[7]=ts;
  for(unsigned i=0;i<320;++i){packet[12+2*i]=uint16_t(authored[i])>>8;packet[13+2*i]=authored[i];}
  const size_t length=sizeof(packet)-(mode=="malformed"?2:0);
  assert(sendto(sender,packet,length,0,reinterpret_cast<sockaddr*>(&destination),sizeof(destination))==ssize_t(length));
 }
 bounded_ingress=&s;try{s.rtp_task_();assert(false);}catch(const EndRtp&){}
 if(reject){assert(s.rtp_rx_datagrams_==6&&s.rtp_rx_packets_==0&&jitter.depth()==0);close(sender);close(s.rtp_socket_);return 0;}
 assert(s.rtp_rx_datagrams_==6&&s.rtp_rx_packets_==6&&jitter.depth()==6);
 try{app.rx_task_();assert(false);}catch(const EndPlayout&){}
 for(size_t i:{0U,1U,3U,4U,6U,7U})assert(speaker.frames[i]==authored);
 for(size_t i:{2U,5U}){assert(speaker.frames[i].size()==320);for(auto value:speaker.frames[i])assert(value==0);}
 close(sender);close(s.rtp_socket_);
}
'''
    cpp = folder / "pcm-ptime.cpp"
    cpp.write_text(code)
    component = ROOT / "esphome/components/voip_stack"
    fixture = ROOT / "tests/fixtures/g722_esp_audio_codec_2_5_0"
    library = Path(os.environ["ESP_AUDIO_LIBS_SOURCE"])
    sources = ["src/resample/resampler.cpp", "src/resample/art_resampler.cpp", "src/resample/art_biquad.cpp", "src/quantization_utils.cpp", "src/memory_utils.cpp", "src/dsp/dsps_dotprod_f32_ansi.c"]
    binary = folder / "pcm-ptime"
    result = subprocess.run(["g++", "-std=c++17", "-DUSE_ESPHOME_VOIP_STACK_G722", "-I", str(folder), "-I", str(ROOT), "-I", str(component), "-I", str(fixture), "-I", str(library / "include"), str(cpp), str(folder / "api.cpp"), str(component / "g722_rtp_codec.cpp"), *(str(library / source) for source in sources), "-lspandsp", "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("mode", ["valid", "undeclared", "unsafe", "unknown", "malformed"])
def test_declared_pcm_packet_duration_is_admitted_independently_of_preferred_ptime(pcm_packet_times_binary, mode):
    result = subprocess.run([pcm_packet_times_binary, mode], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("alternate_rate", [16000, 48000])
def test_real_schema_reports_unsupported_mixed_pcm_geometry(tmp_path, alternate_rate):
    import yaml
    config = yaml.safe_load((ROOT / "tests/minimal_speaker_only.yaml").read_text())
    config["external_components"][0]["source"] = str(ROOT / "esphome/components")
    config["psram"] = {"mode": "octal", "speed": "80MHz"}
    config["voip_stack"]["audio"]["codecs"].append({"codec": "opus", "rx": [{"sample_rate": alternate_rate, "frame_ms": 10}]})
    path = tmp_path / "mixed.yaml"
    path.write_text(yaml.safe_dump(config, sort_keys=False))
    result = subprocess.run([os.environ.get("ESPHOME_PYTHON", sys.executable), "-m", "esphome", "config", str(path)], capture_output=True, text=True, timeout=60)
    if alternate_rate == 48000:
        assert result.returncode == 0, result.stdout + result.stderr
    else:
        assert result.returncode != 0
        assert "mixed PCM/Opus receive formats require the same decoded PCM" in result.stdout + result.stderr
