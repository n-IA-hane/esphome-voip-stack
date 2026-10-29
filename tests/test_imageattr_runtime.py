"""RFC6236 JPEG preferences use directional limits and bounded legacy fallback."""
from pathlib import Path
import subprocess

import pytest

from test_refer_dialog_runtime import block

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "esphome/components/voip_stack"


def compile_run(tmp_path, code, *args):
    stub = tmp_path / "esphome/core/defines.h"
    stub.parent.mkdir(parents=True, exist_ok=True)
    stub.write_text("#pragma once\n")
    source = tmp_path / "imageattr.cpp"
    source.write_text(code)
    binary = tmp_path / "imageattr"
    result = subprocess.run(["g++", "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror",
                             "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                             "-I", str(tmp_path), "-I", str(ROOT), str(source), "-o", str(binary)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([str(binary), *args], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("attribute,valid,send,recv", [
    ("26 send * recv *", True, True, True),
    ("* recv [x=[8:8:320],y=[8:8:240],sar=1.0,q=1.0]", True, True, True),
    ("26 send [x=240,y=176]", True, True, True),
    ("26 send [x=640,y=480]", True, False, True),
    ("26 recv [x=320,y=240]", True, True, True),
    ("26 recv [x=240,y=240]", True, True, False),
    ("26 send [x=[9:8:240],y=240]", True, False, True),
    ("26 send [x=[8,16,240],y=[8:240],sar=[0.9-1.1]]", True, True, True),
    ("26 send [x=240,y=240,foo=bar]", True, True, True),
    ("26 send [x=240,y=240,foo=[one,[two,three]]]", True, True, True),
    ("26 send [x=240,y=240,sar=1.2]", True, False, True),
    ("26 send [x=240,y=240,sar=[0.9,1.0,1.2]]", True, True, True),
    ("26 send [x=240,y=240,par=[1.0-1.5]]", True, False, True),
    ("26 send [x=0,y=240]", False, False, False),
    ("26 send [x=240,y=240", False, False, False),
    ("26 send [x=240,y=240,q=1.1]", False, False, False),
    ("26 send [x=[8:0:240],y=240]", False, False, False),
    ("26 send [x=[240:8],y=240]", False, False, False),
    ("26 send [x=240,y=240,sar=1.0,sar=1.0]", False, False, False),
    ("26 send * send *", False, False, False),
    ("26 send * recv * trailing", False, False, False),
    ("97 send * recv *", False, False, False),
    ("26 send [x=999999999999,y=240]", False, False, False),
])
def test_bounded_parser(tmp_path, attribute, valid, send, recv):
    code = r'''
#include <cassert>
#include "esphome/components/voip_stack/sdp_imageattr.h"
using namespace esphome::voip_stack;
int main(int argc,char**argv){assert(argc==2);auto value=ImageAttributeParser::parse(argv[1],240,240,320,240);
'''
    code += f'assert(value.valid=={str(valid).lower()});'
    if valid:
        code += f'assert(value.send_compatible=={str(send).lower()}&&value.recv_compatible=={str(recv).lower()});'
    code += "}\n"
    compile_run(tmp_path, code, attribute)


def sdp_source():
    sdp = (COMPONENT / "sip_sdp.cpp").read_text()
    message = (COMPONENT / "sip_message.cpp").read_text()
    owner = (COMPONENT / "sip_transport.cpp").read_text()
    header = (COMPONENT / "sip_transport.h").read_text()
    enum_start = header.index("  enum : uint8_t { IMAGEATTR_PEER")
    enum_end = header.index("#endif", enum_start)
    code = r'''
#define USE_ESPHOME_VOIP_STACK_VIDEO
#define USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <cctype>
#include <string>
#include <arpa/inet.h>
#include "esphome/components/voip_stack/video.h"
#include "esphome/components/voip_stack/sdp_imageattr.h"
using namespace esphome::voip_stack;
'''
    for signature in ["std::string trim_copy(", "bool parse_decimal_u32(",
                      "bool parse_video_media_line(", "bool parse_video_rtpmap(", "bool parse_rtcp_attribute("]:
        code += block(message, signature) + "\n"
    code += r'''
struct SipTransport {
 bool refer_context_active_=false,video_offered_=false,video_negotiated_=false,video_send_enabled_=false,video_receive_enabled_=false;
 void *video_source_=nullptr,*video_sink_=this;VideoCapability tx,rx,negotiated_video_capability_;
 std::atomic<bool>video_send_requested_{false};
 uint8_t video_offer_payload_type_=26;uint16_t video_rtp_port_=40002;
 uint32_t remote_video_ip_v4_=0,remote_video_rtcp_ip_v4_=0;uint16_t remote_video_rtp_port_=0,remote_video_rtcp_port_=0;
 std::string confirmed_local_sdp_;uint32_t sdp_session_id_=1;
'''
    code += header[enum_start:enum_end]
    code += r'''
 SipTransport(){rx.encoding="JPEG";rx.payload_type=26;rx.width=240;rx.height=240;rx.max_fps=5;
  tx=rx;tx.width=320;tx.height=240;}
 VideoCapability local_video_send_capability_()const{return video_source_?tx:VideoCapability{};}
 VideoCapability local_video_receive_capability_()const{return video_sink_?rx:VideoCapability{};}
 void reset_video_negotiation_();
 bool learn_remote_video_from_sdp_(const std::string&,uint32_t,bool);
 std::string append_video_sdp_(const std::string&,const std::string&,bool)const;
 std::string build_video_direction_offer_(bool,uint32_t)const;
};
'''
    for source, signature in [(owner, "void SipTransport::reset_video_negotiation_()"),
                              (sdp, "bool SipTransport::learn_remote_video_from_sdp_("),
                              (sdp, "std::string SipTransport::append_video_sdp_("),
                              (owner, "std::string SipTransport::build_video_direction_offer_(")]:
        code += block(source, signature) + "\n"
    return code


@pytest.mark.parametrize("scenario", range(9))
def test_production_sdp_directional_preferences_and_legacy(tmp_path, scenario):
    code = sdp_source() + r'''
int main(int argc,char**argv){assert(argc==2);unsigned scenario=std::atoi(argv[1]);SipTransport call;
 const std::string offer="v=0\r\no=- 1 1 IN IP4 192.0.2.2\r\nc=IN IP4 192.0.2.2\r\nm=video 42002 RTP/AVP 26\r\na=rtpmap:26 JPEG/90000\r\na=sendonly\r\n";
 const std::string own=call.append_video_sdp_("","192.0.2.1",false);
 assert(own.find("a=imageattr:26 recv [x=[8:8:240],y=[8:8:240],sar=1.0,q=1.0]")!=std::string::npos);
 if(scenario==0){
  assert(call.learn_remote_video_from_sdp_(offer,0,false));assert(call.video_negotiated_&&call.video_receive_enabled_);
  assert(call.append_video_sdp_("","192.0.2.1",true).find("imageattr")==std::string::npos);
 }else if(scenario==1){
  assert(call.learn_remote_video_from_sdp_(offer+"a=imageattr:26 send * recv *\r\n",0,false));
  assert(call.append_video_sdp_("","192.0.2.1",true).find("recv [x=[8:8:240]")!=std::string::npos);
 }else if(scenario==2){
  assert(call.learn_remote_video_from_sdp_(offer+"a=imageattr:26 send [x=oops,y=240]\r\n",0,false));
  assert(call.video_negotiated_);assert(call.append_video_sdp_("","192.0.2.1",true).find("imageattr")==std::string::npos);
 }else if(scenario==3){
  call.video_imageattr_state_=call.IMAGEATTR_OFFERED;
  assert(call.learn_remote_video_from_sdp_(offer+"a=imageattr:26 send [x=640,y=480]\r\n",0,true));
  assert(call.video_imageattr_state_&call.IMAGEATTR_FALLBACK);assert(call.video_imageattr_state_&call.IMAGEATTR_DISABLED);
  call.confirmed_local_sdp_="v=0\r\no=- 1 1 IN IP4 192.0.2.1\r\nm=audio 40000 RTP/AVP 96\r\na=rtpmap:96 L16/16000/1\r\n"+own;
  const auto retry=call.build_video_direction_offer_(false,2);
  assert(retry.find("imageattr")==std::string::npos&&retry.find("m=audio 40000 RTP/AVP 96")!=std::string::npos);
  call.video_imageattr_state_&=~call.IMAGEATTR_FALLBACK;
  assert(call.learn_remote_video_from_sdp_(offer+"a=imageattr:26 send [x=640,y=480]\r\n",0,true));
  assert(!(call.video_imageattr_state_&call.IMAGEATTR_FALLBACK));
 }else if(scenario==4){
  call.video_imageattr_state_=call.IMAGEATTR_OFFERED;
  assert(call.learn_remote_video_from_sdp_(offer,0,true));assert(!(call.video_imageattr_state_&call.IMAGEATTR_FALLBACK));
 }else if(scenario==5){
  call.video_source_=&call;call.video_send_requested_=true;
  const auto both=call.append_video_sdp_("","192.0.2.1",false);
  assert(both.find("send [x=320,y=240,sar=1.0]")!=std::string::npos);
  assert(both.find("recv [x=[8:8:240]")!=std::string::npos);
 }else if(scenario==6||scenario==7){
  call.video_imageattr_state_=call.IMAGEATTR_OFFERED;
  const std::string send="a=imageattr:26 send [x=640,y=480]\r\n",recv="a=imageattr:26 recv *\r\n";
  assert(call.learn_remote_video_from_sdp_(offer+(scenario==6?send+recv:recv+send),0,true));
  assert(call.video_imageattr_state_&call.IMAGEATTR_FALLBACK);
 }else if(scenario==8){
  call.video_imageattr_state_=call.IMAGEATTR_OFFERED;
  assert(call.learn_remote_video_from_sdp_(offer+"a=imageattr:* send [x=640,y=480] recv *\r\na=imageattr:26 recv *\r\n",0,true));
  assert(call.video_imageattr_state_&call.IMAGEATTR_FALLBACK);
 }
}
'''
    compile_run(tmp_path, code, str(scenario))


def test_fallback_runs_once_in_the_existing_sip_pump_and_keeps_audio(tmp_path):
    source = (COMPONENT / "sip_transport.cpp").read_text()
    code = r'''
#define USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ignore_log(__VA_ARGS__)
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <string>
constexpr const char *TAG="test";template<class...Args>void ignore_log(Args&&...){}
constexpr uint16_t SIP_T2_MS=4000;uint32_t millis(){return 0;}
bool time_reached(uint32_t now,uint32_t target){return int32_t(now-target)>=0;}
enum class SipSignalType{PROTOCOL_ERROR};
struct SipSignal{SipSignalType type;uint16_t status_code;std::string call_id,reason;bool terminal_transaction_pending;};
struct Transaction{bool udp=false,completed=false;uint32_t next_ms=1,ip_v4=0;uint16_t port=0,interval_ms=500,retries=0;std::string request;};
struct Pending{bool used=false,waiting_retry=false,target_send=false,previous_send=false;uint32_t retry_at_ms=1,response_deadline_ms=1000;Transaction transaction;
 bool pending()const{return used;}void clear(){used=false;}};
struct SipTransport {
 enum:uint8_t{IMAGEATTR_DISABLED=4,IMAGEATTR_FALLBACK=8,IMAGEATTR_ACTIVE=16};
 uint8_t video_imageattr_state_=IMAGEATTR_DISABLED|IMAGEATTR_FALLBACK;
 Pending pending_video_direction_invite_;
 std::atomic<bool>media_active_{true},outgoing_invite_pending_{false},transport_stopping_{false},video_send_requested_{false};
 bool video_negotiated_=true,video_send_enabled_=false;struct{bool awaiting_ack=false;}completed_invite_;
 std::string pending_bye_,call_id_="established";unsigned sent=0,bye=0;bool send_ok=true;
 bool send_video_direction_reinvite_unlocked_(bool enabled,bool retry=false,bool fallback=false){
  assert(!enabled&&!retry&&fallback);++sent;pending_video_direction_invite_.used=send_ok;return send_ok;}
 void emit_video_send_state_(bool,bool){}bool send_bye_unlocked_(const std::string&){++bye;return true;}
 void reset_dialog_(){assert(false);}void emit_sip_signal_(SipSignal){assert(false);}
 bool send_sip_(const std::string&,uint32_t,uint16_t){assert(false);return false;}
 void pump_video_direction_transaction_();
};
''' + block(source, "void SipTransport::pump_video_direction_transaction_()") + r'''
int main(){
 SipTransport call;call.completed_invite_.awaiting_ack=true;call.pump_video_direction_transaction_();assert(call.sent==0);
 call.completed_invite_.awaiting_ack=false;call.pump_video_direction_transaction_();assert(call.sent==1&&call.media_active_&&call.bye==0);
 for(unsigned n=0;n<20;++n){call.pump_video_direction_transaction_();}
 assert(call.sent==1&&call.bye==0);
 call.pending_video_direction_invite_.clear();call.pump_video_direction_transaction_();assert(call.sent==1&&call.media_active_);
 SipTransport failed;failed.send_ok=false;failed.pump_video_direction_transaction_();failed.pump_video_direction_transaction_();
 assert(failed.sent==1&&failed.media_active_&&failed.bye==0);
}
'''
    compile_run(tmp_path, code)
