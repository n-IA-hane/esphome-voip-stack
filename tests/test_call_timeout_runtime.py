"""Application call deadlines composed with complete SIP units and real sockets.

Only the host clock, hardware/media and publications are adapted. The timeout
policy, decision handler, termination FSM and transport methods are production.
"""
from pathlib import Path
import os
import re
import subprocess

import pytest

from production_sip_source import transport_units
from test_incoming_setup_runtime import incoming_source
from test_refer_dialog_runtime import block

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def call_timeout_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("call-timeout-wire")
    production = Path(os.environ.get('VOIP_TEST_PRODUCTION_ROOT', ROOT))
    component = production / 'esphome/components/voip_stack'
    fixtures = ROOT / 'tests/fixtures'
    for name in ('esphome/core/defines.h', 'esphome/core/helpers.h', 'esphome/core/log.h',
                 'esphome/core/hal.h', 'esphome/components/network/util.h', 'esp_system.h',
                 'esp_heap_caps.h', 'lwip/sockets.h', 'freertos/FreeRTOS.h', 'freertos/task.h',
                 'freertos/portmacro.h', 'freertos/semphr.h'):
        path = folder / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "sip_dialog_host_sdk.h"\n')
    sdk = (fixtures / 'sip_dialog_host_sdk.h').read_text()
    sdk = sdk.replace('inline uint32_t millis() { return 100; }',
                      'extern uint32_t clock_ms; inline uint32_t millis() { return clock_ms; }')
    (folder / 'sip_dialog_host_sdk.h').write_text(sdk)
    owner = (component / 'voip_stack.cpp').read_text()
    header = (component / 'voip_stack.h').read_text()
    app = incoming_source(component)
    app = re.sub(r'\bTransport\b', 'ApplicationTransport', app)
    app = app.replace('uint32_t millis(){return 100;}', '')
    app = app.replace(' static const char*inet_ntoa_r(in_addr a,char*out,size_t n){return inet_ntop(AF_INET,&a,out,n);}', '')
    app = re.sub(r'^#define ESP_LOG\w\(\.\.\.\)\n', '', app, flags=re.M)
    app = app.replace('struct ApplicationTransport {', 'struct ApplicationTransport {\n SipTransport *sip=nullptr;')
    app = app.replace('Snapshot snapshot()const{return {};}',
                      'Snapshot snapshot()const{const auto s=sip->snapshot();return {s.terminal_transaction_pending,s.media_lifecycle_phase};}')
    app = app.replace('void disconnect(){++disconnects;}', 'void disconnect(){++disconnects;sip->disconnect();}')
    app = app.replace('bool is_connected()const{return true;}', 'bool is_connected()const{return sip->is_connected();}')
    app = app.replace('struct VoipStack {', '''struct VoipStack {
     bool has_speaker_()const{return false;}
     void handle_call_timeouts_(uint32_t,uint32_t);
     void fire_unanswered_invite_timeout_();void fire_timeout_decline_();
     std::atomic<uint32_t>media_timeout_rtp_rx_packets_{0},last_peer_audio_ms_{0};
    ''')
    app = app.replace('uint32_t ringing_start_time_=0,calling_start_time_=0;\n static constexpr uint32_t INVITE_NO_RESPONSE_TIMEOUT_MS=5000;',
                      header[header.rfind('  //', 0, header.index('  uint32_t calling_timeout_ms_')):header.index('  // === SIP call state ===')])
    for name in ('set_calling_timeout', 'set_ringing_timeout'):
        app = app.replace('struct VoipStack {', 'struct VoipStack {\n' + block(header, 'void ' + name + '('), 1)
    app = app.replace('struct ApplicationTransport {', '''struct ApplicationTransport {
     uint32_t received_audio_packets()const{return sip->received_audio_packets();}
     uint16_t last_sip_status_code()const{return sip->last_sip_status_code();}
     bool remote_audio_expected()const{return sip->remote_audio_expected();}
    ''')
    app = app.replace('bool send_sip_bye_(const std::string&){return true;}',
                      'bool send_sip_bye_(const std::string&id){return t.sip->send_bye(id);}')
    app = app.replace('bool send_sip_cancel_(const std::string&){return true;}',
                      'bool send_sip_cancel_(const std::string&id){return t.sip->send_cancel(id);}')
    loop = owner[owner.index('  const uint32_t calling_to ='):owner.index('  const CallState state =', owner.index('  const uint32_t calling_to ='))]
    app = app.replace('struct VoipStack {', 'struct VoipStack {\n void timeout_loop(uint32_t now){\n' + loop + '}\n', 1)
    for name in ('handle_call_timeouts_', 'fire_unanswered_invite_timeout_', 'fire_timeout_decline_'):
        app += '\n' + block(owner, 'void VoipStack::' + name + '(')

    app = app.replace('struct VoipStack {', 'struct VoipStack {\n std::atomic<bool>runtime_shutting_down_{false};\n bool can_start_call_()const;\n unsigned became_active=0;\n', 1)
    app = app.replace('void set_in_call_(bool on){if(on)call_state_=CallState::IN_CALL;}',
                      'void set_in_call_(bool on){if(on){++became_active;call_state_=CallState::IN_CALL;}}')
    app += '\n' + block((component / 'voip_fsm.cpp').read_text(), 'bool VoipStack::can_start_call_(')
    fixture = (fixtures / 'call_timeout.cpp').read_text()
    source = fixture.replace('// PRODUCTION_APPLICATION_OWNER', app)
    cpp = folder / 'call_timeout.cpp'
    cpp.write_text(source)
    units = [component / name for name in ('sip_transport.cpp', 'sip_message.cpp', 'sip_request_response.cpp', 'sip_sdp.cpp', 'sip_refer.cpp', 'media_negotiator.cpp')]
    units.extend(sorted(component.glob('sip_dialog_*.cpp')))
    units.extend(unit for unit in transport_units(component) if unit not in units)
    binary = folder / 'call_timeout'
    result = subprocess.run(['g++', '-std=c++17', '-O1', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                             '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie', '-DUSE_ESP32',
                             '-DUSE_ESPHOME_VOIP_SIP_TRANSPORT', '-DUSE_ESPHOME_VOIP_STACK_PCM', '-pthread',
                             '-I', str(folder), '-I', str(fixtures), '-I', str(production), '-I', str(component),
                             str(cpp), *map(str, units), '-o', str(binary)], capture_output=True, text=True)
    (folder / 'build.log').write_text(result.stdout + result.stderr)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("transport", ["udp", "tcp"])
@pytest.mark.parametrize("status", [100, 180])
@pytest.mark.parametrize("scenario", ["default-silent", "default-repeated", "default-late200", "answer-after32"])
def test_outgoing_default_deadline_preserves_sip_completion(call_timeout_binary, transport, status, scenario):
    result = subprocess.run([call_timeout_binary, transport, str(status), scenario],
                            capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.mark.parametrize("transport", ["udp", "tcp"])
@pytest.mark.parametrize("scenario", ["calling-zero", "calling-zero-last", "ringing-zero", "ringing-fallback", "calling-precedence", "incoming-default"])
def test_explicit_timeout_policy_precedence_and_disabled_zero(call_timeout_binary, transport, scenario):
    result = subprocess.run([call_timeout_binary, transport, "180", scenario],
                            capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr
