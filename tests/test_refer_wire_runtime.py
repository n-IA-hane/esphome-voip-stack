"""Compose production REFER, response dispatcher, cache and socket I/O.

Only the ESP scheduler, hardware/media adapters and initial established call
are fixtures. No response classification, REFER result or replay is injected.
"""
from pathlib import Path
import subprocess

import pytest
from test_refer_dialog_runtime import ROOT, SOURCE, block, refer_fixture_source


def wire_source():
    header = (SOURCE / 'sip_transport.h').read_text()
    source = (SOURCE / 'sip_transport.cpp').read_text()
    requests = (SOURCE / 'sip_request_response.cpp').read_text()
    parser = (SOURCE / 'sip_message.cpp').read_text()
    code = refer_fixture_source()
    code = code.replace('#include <algorithm>', '#include <algorithm>\n#include <cstring>\n#include <cerrno>\n#include <unistd.h>\n#include <fcntl.h>\n#include <sys/socket.h>\n#include <poll.h>\n#include <cstdio>')
    code = code.replace('using SipSignalingString=std::string;', 'struct SipSignalingString:std::string {using std::string::string;using std::string::operator=;std::string str()const{return *this;}};')
    code = code.replace('return std::string(prefix)+"new";', 'static unsigned n=0;return std::string(prefix)+std::to_string(++n);')
    extra = r'''
constexpr uint32_t SIP_T1_MS=500;
constexpr uint16_t SIP_T2_MS=4000;
constexpr size_t MAX_SIP_BODY_BYTES=4096,MAX_SIP_TCP_RX_BUFFER=8192;
uint32_t esp_random(){return 42;}
void delay(unsigned){}
const char*socket_errno_name(int){return "errno";}const char*socket_errno_text(int){return "errno";}
bool socket_errno_is_transient_tx_pressure(int){return false;}
template<class T>void reset_string(T&s){s.clear();}
'''
    for marker in ['static bool is_sip_uri_unreserved(', 'std::string via_branch(', 'std::string message_body(', 'bool sip_content_length(',
                   'std::string reason_text_from_header(', 'bool sip_option_supported(',
                   'std::string sip_uri_user_encode(', 'std::string sip_quoted(',
                   'std::string sip_name_addr(', 'std::string response_via_with_rport(']:
        extra += block(parser, marker) + '\n'
    # Default arguments normally come from sip_message.h.
    extra = extra.replace('const std::string &display_name)', 'const std::string &display_name = "")')
    code = code.replace('struct SipPhoneTransport {', extra + '\nstruct SipPhoneTransport {')
    methods = [
        ('reset_dialog_', source), ('send_sip_', source),
        ('remember_completed_invite_ack_', source), ('remember_udp_transaction_', source),
        ('send_invite_unlocked_', source), ('send_bye_unlocked_', source),
        ('send_cancel_unlocked_', source), ('release_refer_dialog_', source),
        ('pump_udp_retransmits_unlocked_', source),
        ('send_stateless_response_', requests),
    ]
    declarations = {
        'send_stateless_response_': 'bool send_stateless_response_(const std::string&,const sockaddr_in&,uint16_t,const char*,const std::string& = "",bool=false,int=-1);',
    }
    for name, text in methods:
        # Remove the fixture implementation, retaining the production declaration.
        marker = next(line.strip().split('{')[0] for line in code.splitlines() if name+'(' in line and '{' in line and '::' not in line.split('(')[0])
        old = block(code, marker)
        impl_marker = next(line for line in text.splitlines() if ('SipTransport::'+name+'(') in line)
        implementation = block(text, impl_marker)
        declaration = declarations.get(name, implementation[:implementation.index('{')].replace('SipTransport::','').strip()+';')
        code = code.replace(old, declaration, 1) + '\n' + implementation
    more = [
        'bool SipTransport::handle_response_(', 'void SipTransport::handle_sip_datagram_(',
        'void SipTransport::handle_sip_datagram_unlocked_(', 'bool SipTransport::replay_completed_response_(',
        'void SipTransport::remember_completed_response_(', 'bool SipTransport::replay_completed_invite_ack_(',
        'uint16_t SipTransport::acknowledge_completed_invite_(', 'void SipTransport::reset_dialog_media_locked_(',
        'void SipTransport::clear_udp_transactions_(', 'void SipTransport::clear_invite_transaction_(',
        'void SipTransport::clear_bye_transaction_(', 'bool SipTransport::defer_bye_until_uas_ack_locked_(', 'bool SipTransport::terminal_transaction_pending_locked_(',
        'bool SipTransport::send_sip_bytes_(', 'bool SipTransport::send_sip_tcp_bytes_(',
        'bool SipTransport::send_sip_tcp_record_(const char', 'void SipTransport::close_tcp_flow_(',
        'SipTransport::SipTcpFlow *SipTransport::tcp_flow_for_socket_(',
        'void SipTransport::handle_sip_stream_(',
    ]
    # New common ingress/replay helpers are compiled when present. Baseline
    # ingress is extracted verbatim from the original task loop below.
    for marker in ['void SipTransport::handle_sip_udp_datagram_(',
                   'bool SipTransport::send_sip_udp_bytes_(']:
        if marker in source:
            more.append(marker)
    declarations_extra = ''
    for marker in more:
        body = block(source, marker)
        decl = body[:body.index('{')].replace('SipTransport::','').strip()+';'
        declarations_extra += decl+'\n'
        code += '\n'+body
    for marker in ['bool SipTransport::send_invite_error_ack_(',
                   'bool SipTransport::send_invite_transaction_request_(',
                   'std::string SipTransport::format_response_(']:
        body = block(requests, marker)
        declarations_extra += body[:body.index('{')].replace('SipTransport::','').strip()+';\n'
        code += '\n'+body
    if 'void SipTransport::handle_sip_udp_datagram_(' not in source:
        start = source.index('        bool tcp_call_active = false;', source.index('void SipTransport::sip_task_'))
        end = source.index('\n      }', start)
        ingress = source[start:end].replace('continue;', 'return;')
        code += '\nvoid SipTransport::handle_sip_udp_datagram_(char *buf,int n,const sockaddr_in &src){\n'+ingress+'\n}\n'
        declarations_extra += 'void handle_sip_udp_datagram_(char*,int,const sockaddr_in&);\n'
    code = code.replace(' enum class SipEvent{NONE};', ' enum class SipEvent{NONE,RESPONSE};')
    code = code.replace('void mark_sip_event_(SipEvent){}','void mark_sip_event_(SipEvent,uint16_t=0){}')
    code = code.replace('Mutex tcp_send_mutex_,tcp_tx_pending_mutex_;','Mutex tcp_send_mutex_,tcp_tx_pending_mutex_,dialog_mutex_,media_lifecycle_mutex_;')
    code = code.replace(' uint16_t sip_port_=5060;', 'using SipTcpFlow=::SipTcpFlow;\n'+declarations_extra + r'''
 uint16_t sip_port_=5060;int sip_socket_=-1;
 void stop_audio_path(){media_active_=false;}
 bool send_request_(const std::string&m){SipRequestOptions o;return send_request_(m,"",o);}
 std::array<char,8192> sip_rx_scratch_{};
 std::atomic<bool> running_{true},transport_stopping_{false};
 enum class MediaLifecyclePhase {IDLE};std::atomic<MediaLifecyclePhase>media_lifecycle_phase_{MediaLifecyclePhase::IDLE};
 std::atomic<uint16_t>last_sip_status_code_{0};
 DeferredUdpResponse pending_provisional_response_;
 void request_audio_path_stop_locked_(){media_active_=false;}
 void close_tcp_client_from_sip_task_(){close_tcp_flow_(tcp_flow_);}
 void handle_tcp_peer_loss_(SipTcpFlow*f){close_tcp_flow_(*f);}
 static bool sip_method_known_(const std::string&){return false;}
 static std::string sip_failure_reason_(unsigned){return "rejected";}
 bool handle_invite_(const std::string&,const sockaddr_in&){assert(false);return false;}
 bool handle_update_(const std::string&,const sockaddr_in&){assert(false);return false;}
 bool send_response_(uint16_t,const char*){assert(false);return false;}
 bool send_connected_identity_update_(){return false;}
 std::string build_sdp_offer_(){return "v=0\r\nm=audio 40000 RTP/AVP 96\r\na=rtpmap:96 L16/16000/1\r\n";}
 bool learn_remote_rtp_from_sdp_(const std::string&,uint32_t,bool){return true;}
''')
    marker = 'bool remote_audio_expected() const override'
    expected = (block(header, marker).replace(' override', '') if marker in header
                else 'bool remote_audio_expected() const { return true; }')
    code = code.replace('struct SipTransport: SipPhoneTransport {',
                        block(header, 'struct DeferredUdpResponse {') + ';\n' +
                        'struct SipTransport: SipPhoneTransport {\n' + expected + '\n')
    return code


@pytest.fixture(scope='module')
def wire_binary(tmp_path_factory):
    path = tmp_path_factory.mktemp('refer-wire')
    cpp = path / 'wire.cpp'
    watchdog = (Path(__file__).parent/'fixtures/refer_watchdog.cpp').read_text()
    handler = (SOURCE/'voip_stack.cpp').read_text()
    watchdog += '\n' + block(handler, 'void VoipStack::handle_call_timeouts_(')
    cpp.write_text(wire_source() + '\n' +
                   (Path(__file__).parent/'fixtures/refer_wire_main.cpp').read_text() +
                   '\n' + watchdog)
    exe = path / 'wire'
    proc = subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-I',str(ROOT),str(cpp),'-o',str(exe)],capture_output=True,text=True)
    assert proc.returncode == 0, proc.stderr
    return exe


@pytest.mark.parametrize('status', [401,407,486,488,503])
@pytest.mark.parametrize('tcp', [0,1])
@pytest.mark.parametrize('ended', [0,1])
@pytest.mark.parametrize('original_tcp', [0,1])
def test_refer_response_dispatch_terminal(wire_binary,status,tcp,ended,original_tcp):
    subprocess.run([str(wire_binary),'auth',str(status),str(tcp),str(ended),str(original_tcp)],check=True,capture_output=True,text=True,timeout=5)


@pytest.mark.parametrize('original_tcp,new_tcp', [(0,0),(0,1),(1,0),(1,1)])
def test_retired_transaction_socket_replay(wire_binary,original_tcp,new_tcp):
    subprocess.run([str(wire_binary),'replay',str(original_tcp),str(new_tcp)],check=True,capture_output=True,text=True,timeout=5)


@pytest.mark.parametrize('original_tcp,target_tcp', [(0,0),(0,1),(1,0),(1,1)])
@pytest.mark.parametrize('outcome', [200,486,408])
def test_refer_ringing_uses_sip_deadline_and_restores_rtp_watchdog(wire_binary,original_tcp,target_tcp,outcome):
    result = subprocess.run([str(wire_binary),'watchdog',str(original_tcp),str(target_tcp),str(outcome)],
                            capture_output=True,text=True,timeout=5)
    assert result.returncode == 0, result.stderr
