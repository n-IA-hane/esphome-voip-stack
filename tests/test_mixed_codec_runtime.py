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
 RtpAudioPayloads &negotiated_rx_payloads_=accepted;
 AudioFormat &selected_tx_format_=tx,&selected_rx_format_=rx;
 uint8_t &rtp_tx_payload_type_=tx_pt,&rtp_rx_payload_type_=rx_pt;
 std::atomic<bool>rtp_running_{false},rtp_task_terminate_{false},media_active_{true},rtp_task_quiesced_{true};
 std::atomic<uint16_t>rtp_sequence_{0};std::atomic<uint32_t>rtp_timestamp_{0};uint32_t rtp_ssrc_=0;
 std::atomic<bool>rtp_ssrc_latched_{false};
 std::atomic<uint32_t>media_proposal_epoch_{0},latched_rtp_ip_v4_{0},latched_rtp_ssrc_{0},rtp_rx_packets_{0},rtp_rx_bytes_{0};
 std::atomic<uint16_t>latched_rtp_port_{0};
 enum class MediaLifecyclePhase{ACTIVE,CLEANING,IDLE,PREPARED};std::atomic<MediaLifecyclePhase>media_lifecycle_phase_{MediaLifecyclePhase::IDLE};
 Mutex media_lifecycle_mutex_,dialog_mutex_;void *rtp_cleanup_done_=nullptr,*rtp_task_done_=nullptr,*rtp_task_handle_=this;
 std::string call_id_;
 OpusRtpCodec opus_codec_;PcmReceiveConverter pcm_receive_converter_;bool opus_codec_ready_=false;
 void set_audio_formats(const AudioFormatList&,const AudioFormatList&);
 bool prepare_media_path_locked_();bool commit_media_path_locked_();bool bind_udp_(int*,uint16_t,const char*);
 void reset_rtp_latch_();
 void rtp_task_();bool received_audio_format_(uint8_t,AudioFormat*)const;
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


@pytest.mark.parametrize("alternate_rate", [16000, 48000])
def test_real_schema_reports_unsupported_mixed_pcm_geometry(tmp_path, alternate_rate):
    import yaml
    config = yaml.safe_load((ROOT / "tests/minimal_speaker_only.yaml").read_text())
    config["external_components"][0]["source"] = str(ROOT / "esphome/components")
    config["psram"] = {"mode": "octal", "speed": "80MHz"}
    config["voip_stack"]["audio"]["rx_formats"] = [{"codec": "opus", "sample_rate": alternate_rate, "pcm_format": "s16le", "channels": 1, "frame_ms": 10}]
    path = tmp_path / "mixed.yaml"
    path.write_text(yaml.safe_dump(config, sort_keys=False))
    result = subprocess.run([os.environ.get("ESPHOME_PYTHON", sys.executable), "-m", "esphome", "config", str(path)], capture_output=True, text=True, timeout=60)
    if alternate_rate == 48000:
        assert result.returncode == 0, result.stdout + result.stderr
    else:
        assert result.returncode != 0
        assert "mixed PCM/Opus receive formats require the same decoded PCM" in result.stdout + result.stderr
