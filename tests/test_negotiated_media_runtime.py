"""Parsing and answer preparation leave the retained media owner untouched."""
import subprocess

import pytest

from test_refer_dialog_runtime import ROOT, refer_fixture_source
from test_sdp_version_runtime import version_source


@pytest.mark.parametrize('video', [False, True], ids=['audio-only', 'video-enabled'])
def test_complete_media_proposal_is_pure_until_explicit_commit(tmp_path, video):
    source = version_source(video).split('\nint main(){', 1)[0] + r'''
int main(){
 SipTransport t;AudioFormat pcm;pcm.sample_rate=16000;pcm.frame_ms=10;
 t.offer_tx_formats_.formats[0]=t.offer_rx_formats_.formats[0]=pcm;
 t.remote_ip_v4_=INADDR_LOOPBACK;t.sdp_session_id_=123;t.sdp_session_version_=7;
 std::string initial="v=0\r\nc=IN IP4 127.0.0.1\r\nm=audio 40004 RTP/AVP 96 109\r\na=rtpmap:96 L16/16000/1\r\na=rtpmap:109 telephone-event/8000\r\na=ptime:10\r\na=sendrecv\r\n";
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
 t.video_rx_.encoding="JPEG";t.video_rx_.payload_type=26;t.video_rx_.width=320;t.video_rx_.height=240;t.video_rx_.max_fps=10;
 initial+="m=video 40008 RTP/AVP 26\r\na=sendonly\r\n";
#endif
 assert(t.learn_remote_rtp_from_sdp_(initial,INADDR_LOOPBACK));
 t.audio_receive_reset_pending_=true;t.rtp_source_reacquire_pending_=true;
 const auto retained=t.negotiated_media_();const auto generation=t.dtmf_generation_.load();
 const auto original=t.build_sdp_answer_();
 auto held=initial;held.replace(held.find("a=sendrecv"),10,"a=inactive");
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
 held.replace(held.find("a=sendonly"),10,"a=inactive");
#endif
 auto invalid=held;invalid.replace(invalid.find("L16/16000"),9,"L16/48000");
 SipTransport::NegotiatedMedia proposal;
 assert(!t.parse_remote_media_(invalid,INADDR_LOOPBACK,false,retained,&proposal,true));
 assert(t.build_sdp_answer_()==original&&t.sdp_session_version_==7&&t.dtmf_generation_==generation);
 assert(t.audio_receive_reset_pending_&&t.rtp_source_reacquire_pending_);
 assert(t.parse_remote_media_(held,INADDR_LOOPBACK,false,retained,&proposal,true));
 assert(!proposal.send&&!proposal.receive&&proposal.tx==retained.tx&&proposal.rx==retained.rx);
 assert(proposal.rx_payloads==retained.rx_payloads&&proposal.dtmf==retained.dtmf);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
 assert(proposal.video_negotiated&&!proposal.video_send&&!proposal.video_receive);
 assert(proposal.video_ip==retained.video_ip&&proposal.video_port==retained.video_port);
 assert(proposal.video_rtcp_ip==retained.video_rtcp_ip&&proposal.video_rtcp_port==retained.video_rtcp_port);
#endif
 const auto answer=t.build_negotiated_sdp_answer_(proposal,8);
 assert(answer.find("o=- 123 8 IN IP4 ")!=std::string::npos&&answer.find("a=inactive\r\n")!=std::string::npos);
 assert(t.build_sdp_answer_()==original&&t.sdp_session_version_==7&&t.audio_send_enabled_&&t.audio_receive_enabled_);
 // A notification consumed after the snapshot must remain consumed when a
 // signaling proposal is committed or exchanged by REFER.
 t.audio_receive_reset_pending_=false;t.rtp_source_reacquire_pending_=false;
 t.commit_negotiated_media_(proposal);
 assert(!t.audio_send_enabled_&&!t.audio_receive_enabled_&&t.tx==retained.tx&&t.rx==retained.rx);
 assert(t.accepted==retained.rx_payloads&&t.remote_rtp_port_==retained.audio_port);
 assert(!t.audio_receive_reset_pending_&&!t.rtp_source_reacquire_pending_&&t.dtmf_generation_==generation);
 auto legacy=held;legacy.replace(legacy.find("c=IN IP4 127.0.0.1"),18,"c=IN IP4 0.0.0.0");
 assert(t.parse_remote_media_(legacy,INADDR_LOOPBACK,false,retained,&proposal,true));
 assert(proposal.zero_connection);
 t.commit_negotiated_media_(proposal);
 const auto committed=t.negotiated_media_();
 assert(committed.zero_connection&&committed.audio_ip==retained.audio_ip);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
 assert(committed.video_zero_connection&&committed.video_ip==retained.video_ip);
#endif
}
'''
    core = tmp_path / 'esphome/core'
    core.mkdir(parents=True)
    (core / 'defines.h').write_text('#pragma once\n')
    (core / 'helpers.h').write_text('#pragma once\n#include <mutex>\nnamespace esphome {using Mutex=std::mutex;using LockGuard=std::lock_guard<Mutex>;}\n')
    cpp = tmp_path / 'proposal.cpp'
    cpp.write_text(source)
    binary = tmp_path / 'proposal'
    built = subprocess.run(['g++', '-std=c++17', '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-pthread',
                            '-I', str(tmp_path), '-I', str(ROOT), str(cpp), '-o', str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
    assert run.returncode == 0, run.stdout + run.stderr


def test_refer_media_scope_exchanges_owned_storage_without_allocating(tmp_path):
    code = refer_fixture_source() + r'''
#include <new>
static bool measuring=false;static unsigned allocations=0;
void*operator new(size_t size){if(measuring)++allocations;auto*p=std::malloc(size);if(!p)throw std::bad_alloc();return p;}
void operator delete(void*p)noexcept{std::free(p);}
void operator delete(void*p,size_t)noexcept{std::free(p);}
int main(){
 SipTransport s;s.call_id_="established-original";
 s.refer_dialog_.reset(new SipTransport::ReferDialog);
 const std::string original="m=audio 40000 RTP/AVP 96 97 98 99 100 101";
 const std::string candidate="m=audio 42000 RTP/AVP 96 97 98 99 100 101";
 assert(s.remote_media_shape_.append(original));assert(s.refer_dialog_->media.shape.append(candidate));
 measuring=true;
 for(unsigned n=0;n<10;++n){SipTransport::ReferScope scope(s);}
 measuring=false;
 assert(allocations==0&&"Temporary REFER signaling must exchange existing owned storage without copying it");
 assert(s.remote_media_shape_.line(0)==original&&s.refer_dialog_->media.shape.line(0)==candidate);
}
'''
    cpp = tmp_path / 'exchange.cpp'
    cpp.write_text(code)
    binary = tmp_path / 'exchange'
    built = subprocess.run(['g++', '-std=c++17', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                            '-I', str(ROOT), str(cpp), '-o', str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
    assert run.returncode == 0, run.stdout + run.stderr
