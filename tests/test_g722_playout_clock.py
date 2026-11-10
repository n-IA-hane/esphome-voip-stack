"""G722 wire clocks remain independent of the configured PCM speaker clock.

The production UDP ingress, jitter, RX worker and DSP run on the host. The
Espressif decoder API adapter uses SpanDSP; FFmpeg supplies and checks the
independent wire/PCM oracle. This does not qualify the ESP codec binary.
"""

import array
import math
import os
from pathlib import Path
import subprocess

import pytest

from test_mixed_codec_runtime import (
    g722_playout_binary as g722_playout_binary,
    mixed_playout_binary as mixed_playout_binary,
)
from test_refer_dialog_runtime import block

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "esphome/components/voip_stack"


@pytest.fixture(scope="module")
def g722_clock_binary(g722_playout_binary):
    folder = g722_playout_binary.parent
    code = g722_playout_binary.with_suffix(".cpp").read_text().split("int main(")[0]
    header = (COMPONENT / "voip_stack.h").read_text()
    code = code.replace(
        "AudioFormat get_current_rx_audio_format_(){return transport_->rx;}",
        "Mutex media_format_mutex_;AudioFormatList rx_audio_formats_;"
        "struct {AudioFormat rx;}current_media_formats_;"
        "std::atomic<uint32_t>current_rx_audio_format_packed_{0};"
        + block(header, "AudioFormat get_current_rx_audio_format_(")
        + block(header, "void set_current_rx_audio_format_("),
    )
    code = code.replace("if(frames.size()==8)", "if(frames.size()==6)")
    refer = block((COMPONENT / "sip_refer.cpp").read_text(), "bool SipTransport::prepare_refer_media_(")
    refer = refer.replace("bool SipTransport::prepare_refer_media_(ReferDialog &other)",
                          "bool prepare_refer_media(SipTransport &self, ReferDialog &other)")
    code += "struct ReferDialog {struct {AudioFormat tx,rx;RtpAudioPayloads rx_payloads;}media;};\n"
    code += refer.replace("this->", "self.")
    code += r'''
#include <fstream>
int main(int argc,char **argv){
 assert(argc==4);const unsigned duration=std::stoul(argv[1]);const std::string mode=argv[2];
 const auto g10=fmt(16000,10,AudioCodec::G722),g20=fmt(16000,20,AudioCodec::G722);
 const auto pcm48=fmt(48000,10,AudioCodec::PCM);
 SipTransport s;s.offer_tx_formats_.count=2;s.offer_tx_formats_.formats[0]=g20;s.offer_tx_formats_.formats[1]=g10;
 s.offer_rx_formats_.count=3;s.offer_rx_formats_.formats[0]=pcm48;
 s.offer_rx_formats_.formats[1]=g20;s.offer_rx_formats_.formats[2]=g10;
 if(mode=="fallback"){
  for(unsigned kind=0;kind<5;++kind){
   auto formats=s.offer_rx_formats_;
   if(kind==0){formats.count=2;formats.formats[0]=g20;formats.formats[1]=g10;}
   if(kind==1)formats.formats[0]=fmt(16000,10,AudioCodec::PCM);
   if(kind==2)formats.formats[0]=fmt(8000,10,AudioCodec::PCM);
   if(kind==3){formats.count=4;formats.formats[3]=fmt(48000,10,AudioCodec::OPUS);}
   if(kind==4)formats.formats[0].pcm_format=PcmFormat::S32LE;
   assert(pcm_playout_format(g20,formats)==g20);
  }
  return 0;
 }
 s.set_audio_formats(s.offer_tx_formats_,s.offer_rx_formats_);
 // Incoming standard peer, static PT9 and no private SDP extensions.
 const std::string offer="v=0\r\nc=IN IP4 127.0.0.1\r\nm=audio 40002 RTP/AVP 9\r\na=ptime:"+
  std::to_string(duration?duration:20)+"\r\na=sendrecv\r\n";
 assert(s.learn_remote_rtp_from_sdp_(offer,0,false));
 const AudioFormat wire=s.rx;assert(wire.codec==AudioCodec::G722&&wire.sample_rate==16000&&s.rx_pt==9);
 s.rtp_port_=0;assert(s.prepare_media_path_locked_());
 if(mode=="refer"){
  ReferDialog next;next.media.tx=s.tx;next.media.rx=g10;
  next.media.rx_payloads.add(9,g10);
  assert(prepare_refer_media(s,next)&&next.media.rx==wire&&s.rx==wire);
  uint8_t input[320]{},output[960]{};
  assert(s.convert_received_pcm(decoded_pcm_format(g10),input,sizeof(input),output,sizeof(output))>0);
  ReferDialog failed;failed.media.tx=fmt(48000,10,AudioCodec::PCM);
  failed.media.rx=g10;failed.media.rx_payloads.add(9,g10);
  assert(!prepare_refer_media(s,failed)&&failed.media.rx==g10&&s.rx==wire);
  assert(s.convert_received_pcm(decoded_pcm_format(g10),input,sizeof(input),output,sizeof(output))==sizeof(output));
  // A failed additional source must roll back its new DSP entries and retain
  // the existing target/history. Both new sources are genuinely configured.
  SipTransport narrow;narrow.tx=s.tx;narrow.rx=g20;narrow.offer_rx_formats_.count=4;
  narrow.offer_rx_formats_.formats[0]=fmt(16000,10,AudioCodec::PCM);
  narrow.offer_rx_formats_.formats[1]=g20;
  const auto pcm8=fmt(8000,10,AudioCodec::PCM);
  narrow.offer_rx_formats_.formats[2]=pcm8;narrow.offer_rx_formats_.formats[3]=pcm48;
  RtpAudioPayloads original;original.add(9,g20);
  assert(narrow.pcm_receive_converter_.prepare(configured_decoded_receive_formats(
   original,narrow.offer_rx_formats_,1200),decoded_pcm_format(g20)));
  ReferDialog incompatible;incompatible.media.tx=narrow.tx;incompatible.media.rx=pcm48;
  incompatible.media.rx_payloads.add(98,pcm8);incompatible.media.rx_payloads.add(99,pcm48);
  assert(!prepare_refer_media(narrow,incompatible)&&incompatible.media.rx==pcm48&&narrow.rx==g20);
  uint8_t low[160]{};assert(narrow.convert_received_pcm(pcm8,low,sizeof(low),output,sizeof(output))==0);
  close(s.rtp_socket_);return 0;
 }
 assert(s.commit_media_path_locked_());
 sockaddr_in destination{};socklen_t length=sizeof(destination);
 assert(getsockname(s.rtp_socket_,reinterpret_cast<sockaddr*>(&destination),&length)==0);
 const int sender=socket(AF_INET,SOCK_DGRAM,0);assert(sender>=0);
 uint8_t storage[16*960]{};RtpJitterBuffer jitter(storage,960,16,1);Speaker speaker;VoipStack app;
 app.speaker_=&speaker;app.rx_jitter_buffer_=&jitter;app.transport_=&s;phone=&app;
 app.rx_audio_formats_=s.offer_rx_formats_;app.set_current_rx_audio_format_(wire);
 app.rx_audio_chunk_alloc_bytes_=pcm_receive_buffer_capacity(s.offer_rx_formats_);
 app.rx_jitter_frame_alloc_bytes_=960;assert(app.rx_audio_chunk_alloc_bytes_==1920);
 assert(app.current_media_formats_.rx==wire&&s.rx==wire);
 // This is the actual sink clock getter used by the speaker/FSM and RX worker.
 assert(app.get_current_rx_audio_format_().sample_rate==48000&&"G722 must retain the configured PCM speaker clock");
 std::ifstream encoded(argv[3],std::ios::binary);std::vector<uint8_t> payload((std::istreambuf_iterator<char>(encoded)),{});
 size_t offset=0;unsigned stamp=0;
 for(unsigned n=0;n<6;++n){
  const unsigned ms=duration?duration:(n%2?20:10),bytes=ms*8;
  assert(offset+bytes<=payload.size());uint8_t packet[172]{};
  packet[0]=0x80;packet[1]=9;packet[3]=n+1;packet[4]=stamp>>24;packet[5]=stamp>>16;packet[6]=stamp>>8;packet[7]=stamp;packet[11]=42;
  memcpy(packet+12,payload.data()+offset,bytes);offset+=bytes;stamp+=ms*8;
  assert(sendto(sender,packet,bytes+12,0,reinterpret_cast<sockaddr*>(&destination),sizeof(destination))==bytes+12);
 }
 bounded_ingress=&s;try{s.rtp_task_();assert(false);}catch(const EndRtp&){}
 assert(s.rtp_rx_packets_==6&&jitter.depth()==6);
 try{app.rx_task_();assert(false);}catch(const EndPlayout&){}
 assert(speaker.frames.size()==6&&s.decodes==6&&s.resets==1);
 std::ofstream output(std::string(argv[3])+".pcm",std::ios::binary);
 size_t samples=0;for(unsigned i=0;i<6;++i){
  const auto &frame=speaker.frames[i];const unsigned ms=duration?duration:(i%2?20:10);
  assert(frame.size()<=ms*48&&frame.size()+32>=ms*48);samples+=frame.size();
  output.write(reinterpret_cast<const char*>(frame.data()),frame.size()*2);
 }
 assert(samples<=stamp*6&&samples+32>=stamp*6&&s.rx==wire&&s.rx_pt==9);
 const auto counters=jitter.counters();assert(!counters.drops&&!counters.missing&&!counters.late);
 close(sender);close(s.rtp_socket_);
}
'''
    cpp = folder / "g722-clock.cpp"
    cpp.write_text(code)
    library = Path(os.environ["ESP_AUDIO_LIBS_SOURCE"])
    sources = ["src/resample/resampler.cpp", "src/resample/art_resampler.cpp", "src/resample/art_biquad.cpp",
               "src/quantization_utils.cpp", "src/memory_utils.cpp", "src/dsp/dsps_dotprod_f32_ansi.c"]
    binary = folder / "g722-clock"
    subprocess.run([
        "g++", "-std=c++17", "-DUSE_ESPHOME_VOIP_STACK_G722", "-I", str(folder), "-I", str(ROOT),
        "-I", str(COMPONENT), "-I", str(ROOT / "tests/fixtures/g722_esp_audio_codec_2_5_0"),
        "-I", str(library / "include"), str(cpp), str(folder / "api.cpp"),
        str(COMPONENT / "g722_rtp_codec.cpp"), *(str(library / item) for item in sources),
        "-lspandsp", "-o", str(binary),
    ], check=True, capture_output=True, text=True)
    return binary


@pytest.mark.parametrize("frame_ms", [10, 20, 0])
def test_g722_udp_decode_keeps_pcm_sink_and_independent_signal(g722_clock_binary, tmp_path, frame_ms):
    pcm = tmp_path / "authored.pcm"
    samples = array.array("h", (int(10000 * math.sin(2 * math.pi * 440 * i / 16000)
                                  + 3500 * math.sin(2 * math.pi * 2700 * i / 16000)) for i in range(3200)))
    pcm.write_bytes(samples.tobytes())
    wire = tmp_path / "ffmpeg.g722"
    subprocess.run(["ffmpeg", "-v", "error", "-f", "s16le", "-ar", "16000", "-ac", "1", "-i", str(pcm),
                    "-c:a", "adpcm_g722", "-f", "g722", str(wire)], check=True, capture_output=True)
    run = subprocess.run([str(g722_clock_binary), str(frame_ms), "udp", str(wire)], capture_output=True, text=True, timeout=5)
    assert run.returncode == 0, run.stderr
    reference = tmp_path / "ffmpeg-48k.pcm"
    subprocess.run(["ffmpeg", "-v", "error", "-f", "g722", "-i", str(wire), "-ar", "48000", "-ac", "1",
                    "-f", "s16le", str(reference)], check=True, capture_output=True)
    actual = array.array("h")
    actual.frombytes(Path(str(wire) + ".pcm").read_bytes())
    expected = array.array("h")
    expected.frombytes(reference.read_bytes())
    # Different resampler filters may have different startup delay. Compare
    # steady waveform at one fixed clock, without inventing samples or gaps.
    window = actual[960:]
    correlations = []
    for shift in range(-96, 97):
        other = expected[960 + shift:960 + shift + len(window)]
        correlations.append(sum(a * b for a, b in zip(window, other)) /
                            math.sqrt(sum(a * a for a in window) * sum(b * b for b in other)))
    assert max(correlations) > 0.98
    expected_count = 6 * frame_ms * 48 if frame_ms else 90 * 48
    assert expected_count - 32 <= len(actual) <= expected_count


@pytest.mark.parametrize("mode", ["fallback", "refer"])
def test_g722_clock_fallback_and_actual_refer_rollback(g722_clock_binary, mode):
    run = subprocess.run([str(g722_clock_binary), "20", mode, "unused"], capture_output=True, text=True, timeout=5)
    assert run.returncode == 0, run.stderr
