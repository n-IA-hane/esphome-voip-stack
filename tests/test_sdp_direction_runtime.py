"""Unidirectional hardware must advertise and negotiate only real audio paths."""
from pathlib import Path
import subprocess

import pytest
from test_sdp_receive_payloads import sdp_fixture_source

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope='module')
def direction_binary(tmp_path_factory):
    path = tmp_path_factory.mktemp('sdp-direction')
    code = sdp_fixture_source()
    code += r'''
AudioFormat fmt(unsigned rate){AudioFormat f;f.sample_rate=rate;f.frame_ms=10;return f;}
int main(int argc,char**argv){
 const std::string scenario=argv[1];SipTransport s;
 s.offer_tx_formats_.formats[0]=fmt(48000);s.offer_rx_formats_.formats[0]=fmt(48000);
 if(scenario=="mic")s.offer_rx_formats_.count=0;
 if(scenario=="speaker")s.offer_tx_formats_.count=0;
 auto offer=s.build_sdp_offer_();
 const std::string expected=scenario=="mic"?"sendonly":scenario=="speaker"?"recvonly":"sendrecv";
 assert(!offer.empty()&&"absent hardware must not impose a fictitious ptime");
 assert(offer.find("a="+expected+"\r\n")!=std::string::npos&&"offer direction follows physical audio endpoints");
 const std::string base="v=0\r\nc=IN IP4 192.0.2.2\r\nm=audio 40002 RTP/AVP 96\r\na=rtpmap:96 L16/48000/1\r\na=ptime:10\r\n";
 for(std::string direction:{"sendrecv","sendonly","recvonly"}){
  const bool compatible=(scenario!="mic"||direction!="sendonly")&&(scenario!="speaker"||direction!="recvonly");
  bool accepted=s.learn_remote_rtp_from_sdp_(base+"a="+direction+"\r\n",0);
  assert(accepted==compatible&&"only intersecting SDP directions require codec/ptime agreement");
  if(accepted){
   const bool send=scenario!="speaker"&&direction!="sendonly";
   const bool receive=scenario!="mic"&&direction!="recvonly";
   assert(s.tx.is_valid()==send&&s.rx.is_valid()==receive&&"negotiated absent directions have no synthetic PCM format");
   assert((s.accepted.count!=0)==receive);
   auto answer=s.build_sdp_answer_();
   const std::string answered=send?(receive?"sendrecv":"sendonly"):"recvonly";
   assert(answer.find("a="+answered+"\r\n")!=std::string::npos);
   assert(answer.find("a=ptime:10\r\n")!=std::string::npos);
  }
 }
}
'''
    cpp = path / 'direction.cpp'
    cpp.write_text(code)
    binary = path / 'direction'
    proc = subprocess.run(['g++', '-std=c++17', '-I', str(ROOT), str(cpp), '-o', str(binary)], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    return binary


@pytest.mark.parametrize('role', ['mic', 'speaker', 'duplex'])
def test_offer_and_negotiation_follow_real_audio_directions(direction_binary, role):
    proc = subprocess.run([str(direction_binary), role], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr


@pytest.mark.parametrize('role', ['mic', 'speaker'])
def test_incoming_fsm_accepts_only_its_negotiated_direction(tmp_path, role):
    from test_incoming_setup_runtime import incoming_source
    code = incoming_source() + r'''
int main(int argc,char**argv){
 VoipStack phone;
 const bool mic=std::string(argv[1])=="mic";
 if(mic)phone.rx_audio_formats_.count=0;else phone.tx_audio_formats_.count=0;
 SipSignal invite;invite.type=SipSignalType::INVITE;invite.call_id="one-way";
 invite.caller_name="caller";invite.caller_route="100";invite.dest_name="esp";invite.dest_route="esp";
 if(mic)invite.caller_tx_formats.count=0;else invite.caller_rx_formats.count=0;
 phone.on_sip_signal_received_(invite);phone.drain();
 assert(phone.call_state_==CallState::IN_CALL&&phone.identity.call_id=="one-way");
 assert(last_final_status==0&&phone.t.disconnects==0);
}
'''
    cpp = tmp_path / 'oneway.cpp'
    cpp.write_text(code)
    binary = tmp_path / 'oneway'
    proc = subprocess.run(['g++', '-std=c++17', '-I', str(ROOT), str(cpp), '-o', str(binary)], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    proc = subprocess.run([str(binary), role], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr


def test_capture_request_follows_negotiated_direction_without_double_stop(tmp_path):
    from test_wifi_media_guard import active_function
    source = r'''
#include "esphome/components/voip_stack/sip_types.h"
#include <atomic>
#include <cassert>
using namespace esphome::voip_stack;
struct Mic {
 unsigned listeners=3,starts=0,stops=0;
 void start(){++listeners;++starts;}
 void stop(){assert(listeners>0);--listeners;++stops;}
};
struct VoipStack {
 std::atomic<bool>audio_devices_active_{false},microphone_requested_{false};
 Mic *microphone_=nullptr,*microphone_source_=nullptr;
 AudioFormat tx;
 AudioFormat get_current_tx_audio_format_()const{return tx;}
 void notify_audio_tasks_(){}
 void reset_peer_audio_watchdog_(bool){}
 void set_audio_devices_active_(bool);
};
'''+active_function()+r'''
int main(){
 for(bool source:{false,true}){
  VoipStack phone;Mic mic;
  if(source)phone.microphone_source_=&mic;else phone.microphone_=&mic;
  phone.set_audio_devices_active_(true);phone.set_audio_devices_active_(true);
  assert(mic.listeners==4&&mic.starts==1&&mic.stops==0);
  phone.tx=disabled_audio_format();phone.set_audio_devices_active_(true);
  assert(mic.listeners==3&&mic.starts==1&&mic.stops==1);
  phone.set_audio_devices_active_(false);phone.set_audio_devices_active_(false);
  assert(mic.listeners==3&&mic.stops==1);
  phone.tx=DEFAULT_AUDIO_FORMAT;phone.set_audio_devices_active_(true);
  assert(mic.listeners==4&&mic.starts==2);
  phone.set_audio_devices_active_(false);
  assert(mic.listeners==3&&mic.stops==2);
 }
}
'''
    cpp = tmp_path / 'capture.cpp'
    cpp.write_text(source)
    binary = tmp_path / 'capture'
    proc = subprocess.run(['g++', '-std=c++17', '-DUSE_ESPHOME_VOIP_STACK_MIC', '-I', str(ROOT), str(cpp), '-o', str(binary)], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    proc = subprocess.run([str(binary)], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr


@pytest.fixture(scope='module')
def directional_wire_binary(tmp_path_factory):
    from test_refer_dialog_runtime import block
    from test_refer_wire_runtime import wire_source
    base = ROOT / 'esphome/components/voip_stack'
    parser = (base / 'sip_message.cpp').read_text()
    sdp = (base / 'sip_sdp.cpp').read_text()
    transport = (base / 'sip_transport.cpp').read_text()
    code = wire_source()
    extra = ''
    for marker in ['bool parse_decimal_u32(', 'bool parse_rtpmap_format(', 'bool parse_audio_media_line(']:
        if marker not in code:
            extra += block(parser, marker) + '\n'
    code = code.replace('struct SipPhoneTransport {', extra + '\nstruct SipPhoneTransport {')
    code = code.replace(' uint16_t sip_port_=5060;', ' AudioFormatList offer_tx_formats_,offer_rx_formats_;int media_config_lock_=0;uint16_t rtp_port_=40000;size_t udp_max_payload_=1200;\n uint16_t sip_port_=5060;')
    code = '#define portENTER_CRITICAL(...) ((void)0)\n#define portEXIT_CRITICAL(...) ((void)0)\n' + code
    code = code.replace('void local_ip_for_peer_(uint32_t,std::string*out){', 'void local_ip_for_peer_(uint32_t,std::string*out)const{')
    methods = [
        ('std::string build_sdp_offer_(){', 'std::string SipTransport::build_sdp_offer_(', sdp),
        ('bool learn_remote_rtp_from_sdp_(', 'bool SipTransport::learn_remote_rtp_from_sdp_(', sdp),
        ('void set_media_config_(', 'void SipTransport::set_media_config_(', transport),
    ]
    for old_marker, new_marker, source in methods:
        body = block(source, new_marker)
        declaration = body[:body.index('{')].replace('SipTransport::', '').strip()
        if 'learn_remote' in new_marker:
            declaration = declaration.replace('bool remote_is_answer, bool direction_only)', 'bool remote_is_answer = false, bool direction_only = false)')
        if 'set_media_config' in new_marker:
            declaration = declaration.replace('const RtpAudioPayloads *rx_payloads)', 'const RtpAudioPayloads *rx_payloads = nullptr)')
        code = code.replace(block(code, old_marker), declaration + ';', 1)
        code += '\n' + body
    declarations = ''
    for marker in ['std::string SipTransport::wrap_sdp_envelope_(', 'void SipTransport::capture_remote_media_shape_(']:
        body = block(sdp, marker)
        declarations += body[:body.index('{')].replace('SipTransport::', '').strip() + ';\n'
        code += '\n' + body
    code = code.replace(' uint16_t sip_port_=5060;', declarations + '\n uint16_t sip_port_=5060;', 1)
    helpers = (ROOT / 'tests/fixtures/refer_wire_main.cpp').read_text()
    helpers = helpers[:helpers.index('void auth_case(')]
    main = r'''
std::string answer_sdp(const std::string &req,const std::string &body){
 auto answer=response(req,200);auto length=answer.find("Content-Length: 0");assert(length!=std::string::npos);
 answer.replace(length,17,"Content-Type: application/sdp\r\nContent-Length: "+std::to_string(body.size()));
 return answer+body;
}
AudioFormat fmt(unsigned rate){AudioFormat f;f.sample_rate=rate;f.frame_ms=10;return f;}
int main(int argc,char**argv){
 SipTransport s;Peer remote(std::string(argv[1])=="tcp");setup(s,remote);s.call_id_.clear();s.media_active_=false;
 const std::string role=argv[2];
 s.offer_tx_formats_.formats[0]=fmt(48000);s.offer_rx_formats_.formats[0]=fmt(48000);
 if(role=="mic")s.offer_rx_formats_.count=0;if(role=="speaker")s.offer_tx_formats_.count=0;
 const std::string local=role=="mic"?"sendonly":role=="speaker"?"recvonly":"sendrecv";
 const std::string peer=role=="mic"?"recvonly":role=="speaker"?"sendonly":"sendrecv";
 for(unsigned cycle=0;cycle<2;++cycle){
  assert(s.send_invite_unlocked_("one-way-"+std::to_string(cycle),"esp","esp","peer","peer"));
  auto invite=remote.drain();assert(invite.size()==1&&message_body(invite[0]).find("a="+local+"\r\n")!=std::string::npos);
  const std::string body="v=0\r\nc=IN IP4 127.0.0.1\r\nm=audio 40002 RTP/AVP 96\r\na=rtpmap:96 L16/48000/1\r\na=ptime:10\r\na="+peer+"\r\n";
  remote.send_to(s,answer_sdp(invite[0],body));auto ack=remote.drain();
  assert(ack.size()==1&&ack[0].rfind("ACK ",0)==0);
  assert(s.delivered.back().type==SipSignalType::STATUS_200_OK);
  assert(s.selected_tx_format_.is_valid()==(role!="speaker"));
  assert(s.selected_rx_format_.is_valid()==(role!="mic"));
  assert(s.remote_audio_expected()==(role!="mic"));
  assert(s.send_bye_unlocked_(s.call_id_));auto bye=remote.drain();assert(bye.size()==1&&bye[0].rfind("BYE ",0)==0);
  remote.send_to(s,response(bye[0],200));assert(remote.drain().empty());
  assert(s.call_id_.empty()&&!s.media_active_&&!s.terminal_transaction_pending_locked_());
 }
 cleanup(s);
}
'''
    path = tmp_path_factory.mktemp('direction-wire')
    cpp = path / 'direction.cpp'
    cpp.write_text(code + '\n' + helpers + '\n' + main)
    binary = path / 'direction'
    proc = subprocess.run(['g++', '-std=c++17', '-I', str(ROOT), str(cpp), '-o', str(binary)], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    return binary


@pytest.mark.parametrize('transport', ['udp', 'tcp'])
@pytest.mark.parametrize('role', ['mic', 'speaker', 'duplex'])
def test_directional_sdp_socket_dialog_and_redial(directional_wire_binary, transport, role):
    proc = subprocess.run([str(directional_wire_binary), transport, role], capture_output=True, text=True, timeout=5)
    assert proc.returncode == 0, proc.stderr
