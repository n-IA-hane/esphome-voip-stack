"""Compose actual auto-answer termination with actual media preparation failures."""

from pathlib import Path
import subprocess

import pytest
from test_incoming_setup_runtime import incoming_source
from test_refer_dialog_runtime import block

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "esphome/components/voip_stack"


def media_failure_source():
    transport = (COMPONENT / "sip_transport.cpp").read_text()
    methods = "\n".join(
        block(transport, marker).replace("SipTransport::", "MediaOwner::")
        for marker in (
            "bool SipTransport::prepare_media_path_locked_(",
            "bool SipTransport::bind_udp_(",
        )
    )
    prelude = r"""
#include "esphome/components/voip_stack/sip_types.h"
#include <atomic>
#include <cassert>
#include <cstring>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#define ESP_LOGE(...) ((void)0)
using namespace esphome::voip_stack;
constexpr unsigned kRtpSocketRxBufferBytes=65536;
uint32_t esp_random(){return 42;}
int xSemaphoreTake(int,int){return 1;}
struct RtpAudioPayloads {};
struct MediaOwner {
 enum class MediaLifecyclePhase {IDLE,PREPARED,ACTIVE};
 std::atomic<MediaLifecyclePhase>media_lifecycle_phase_{MediaLifecyclePhase::IDLE};
 std::atomic<bool>rtp_running_{false},rtp_task_terminate_{false},rtp_task_quiesced_{true};
 void *rtp_task_handle_=this;std::atomic<uint16_t>rtp_sequence_{0};std::atomic<uint32_t>rtp_timestamp_{0};uint32_t rtp_ssrc_=0;
 int rtp_socket_=-1,rtp_cleanup_done_=0;uint16_t rtp_port_=0;
 struct Converter {bool ok=true;bool prepare(const RtpAudioPayloads&,const AudioFormat&){return ok;}}pcm_receive_converter_;
 void reset_rtp_latch_(){}
 template<class...T>void get_media_config_(T...){ }
 bool prepare_media_path_locked_();bool bind_udp_(int*,uint16_t,const char*);
 ~MediaOwner(){if(rtp_socket_>=0)close(rtp_socket_);}
};
MediaOwner media_owner;
"""
    return prelude + methods


@pytest.mark.parametrize("failure", ["converter", "bind"])
def test_auto_answer_media_failure_closes_owner_and_can_retry(tmp_path, failure):
    app = incoming_source().replace(
        "bool prepare_media_path(){++prepares;return prepare_ok;}",
        "bool prepare_media_path(){++prepares;return media_owner.prepare_media_path_locked_();}",
    )
    checks = r"""
int main(int argc,char**argv){
 VoipStack phone;int occupied=-1;
 if(std::string(argv[1])=="converter")media_owner.pcm_receive_converter_.ok=false;
 else {
  occupied=socket(AF_INET,SOCK_DGRAM,0);assert(occupied>=0);
  sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_ANY);
  assert(bind(occupied,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
  socklen_t len=sizeof(address);assert(getsockname(occupied,reinterpret_cast<sockaddr*>(&address),&len)==0);
  media_owner.rtp_port_=ntohs(address.sin_port);
 }
 SipSignal invite;invite.type=SipSignalType::INVITE;invite.call_id="failed-media";
 invite.caller_name="caller";invite.caller_route="100";invite.dest_name="esp";invite.dest_route="esp";
 audio_format_list_default(&invite.caller_tx_formats);audio_format_list_default(&invite.caller_rx_formats);
 phone.on_sip_signal_received_(invite);phone.drain();
 assert(last_final_status==500&&"failed preparation must terminate owned incoming setup");
 assert(phone.identity.call_id.empty()&&phone.call_state_==CallState::IDLE);
 assert(phone.t.disconnects==1&&phone.media_stops==1&&media_owner.rtp_socket_==-1);
 assert(media_owner.media_lifecycle_phase_==MediaOwner::MediaLifecyclePhase::IDLE);
 assert(phone.cached_id==invite.call_id);
 if(occupied>=0)close(occupied);media_owner.pcm_receive_converter_.ok=true;media_owner.rtp_port_=0;
 invite.call_id="second-media";phone.on_sip_signal_received_(invite);phone.drain();
 assert(phone.call_state_==CallState::IN_CALL&&media_owner.rtp_socket_>=0);
 assert(media_owner.media_lifecycle_phase_==MediaOwner::MediaLifecyclePhase::PREPARED);
}
"""
    cpp = tmp_path / "incoming_media.cpp"
    cpp.write_text(media_failure_source() + "\n" + app + "\n" + checks)
    exe = tmp_path / "incoming_media"
    result = subprocess.run(
        [
            "g++",
            "-std=c++17",
            "-DUSE_ESPHOME_VOIP_STACK_SPEAKER",
            "-I",
            str(ROOT),
            str(cpp),
            "-o",
            str(exe),
        ],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr
    result = subprocess.run([str(exe), failure], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
