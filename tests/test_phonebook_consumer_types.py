"""Run actual call/snapshot consumers with the real optional Phonebook types."""
import json
import subprocess

import pytest

from test_correlated_call_behavior import (
    consumer_root, correlated_fixture_source, write_consumer_includes,
)

MODES = ['off', 'signaling-only', 'directory', 'directory-psram']

TYPE_ASSERTIONS = r'''
#include <type_traits>
using namespace esphome::voip_stack;
#if defined(USE_ESPHOME_VOIP_DIRECTORY) && defined(USE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM)
static_assert(std::is_same_v<ContactText,SipSignalingString>);
#else
static_assert(std::is_same_v<ContactText,std::string>);
#endif
#ifdef USE_ESPHOME_VOIP_DIRECTORY
static_assert(std::is_same_v<decltype(Phonebook{}.current_name()),std::string_view>);
#else
static_assert(std::is_same_v<decltype(Phonebook{}.current_name()),const std::string&>);
#endif
'''


def run_consumer(tmp_path, mode, body, *, snapshot=False, video=False):
    write_consumer_includes(tmp_path)
    source = tmp_path / 'consumer.cpp'
    source.write_text(correlated_fixture_source(snapshot=snapshot) + TYPE_ASSERTIONS + body)
    flags = []
    if mode.startswith('directory'):
        flags += ['-DUSE_ESPHOME_VOIP_DIRECTORY', '-DUSE_ESPHOME_VOIP_CONTACT_METADATA']
    if mode in {'signaling-only', 'directory-psram'}:
        flags.append('-DUSE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM')
    if video:
        flags.append('-DUSE_ESPHOME_VOIP_STACK_VIDEO')
    binary = tmp_path / 'consumer'
    component = consumer_root() / 'esphome/components/voip_stack'
    built = subprocess.run(['g++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-DUSE_ESP32',
                            *flags, '-I', str(tmp_path), '-I', str(consumer_root()),
                            '-I', str(component), str(source), '-o', str(binary)],
                           capture_output=True, text=True, timeout=45)
    (tmp_path / 'compile.stderr').write_text(built.stderr)
    assert built.returncode == 0, built.stderr
    executed = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    (tmp_path / 'receipt.json').write_text(json.dumps({
        'mode': mode, 'video': video, 'snapshot': snapshot,
        'consumer_source_root': str(consumer_root()), 'exit': executed.returncode,
        'stdout': executed.stdout, 'stderr': executed.stderr,
    }, indent=2) + '\n')
    assert executed.returncode == 0, executed.stdout + executed.stderr


@pytest.mark.parametrize('mode', MODES)
def test_actual_ha_call_selection_fallback_and_missing_peer(tmp_path, mode):
    run_consumer(tmp_path, mode, r'''
int main(){
 VoipStack phone;
 // Execute the public correlated HA route, retaining its exact target and ID.
 phone.call_via_ha("Desk","ha-case-sensitive@controller");
 assert(phone.transport.originations==1&&phone.invites==1);
 assert(phone.transport.ip=="192.0.2.10");
 assert(phone.identity.call_id=="ha-case-sensitive@controller");
 assert(phone.identity.caller_route=="phone"&&phone.identity.dest_route=="Desk");
 assert(phone.phonebook_.current_name()=="HA"&&phone.pending_dialplan_target_.empty());
 phone.stop();
 // The existing direct attempt can retry through HA without losing its peer.
 phone.call("Desk");const auto direct=phone.identity;
 assert(phone.transport.ip=="192.0.2.20"&&direct.dest_name=="Desk");
 phone.start_call_attempt_(true,direct.call_id);
 assert(phone.transport.originations==3&&phone.invites==3);
 assert(phone.transport.ip=="192.0.2.10"&&phone.identity.call_id==direct.call_id);
 assert(phone.identity.dest_name=="Desk"&&phone.identity.dest_route=="Desk");
 // Real removal must not silently reuse the previously dialled HA address.
 assert(phone.phonebook_.remove_one("HA"));
 phone.start_call_attempt_(true,direct.call_id);
 assert(phone.transport.originations==3&&phone.invites==3&&phone.terminations==1);
 assert(phone.termination_reason==CallEndReason::TRANSPORT_UNREACHABLE);
 assert(phone.terminated_identity.call_id==direct.call_id);
 assert(phone.terminated_identity.dest_name=="Desk"&&phone.call_state_==CallState::IDLE);
 const unsigned failed=phone.call_failed_trigger_.calls;
 phone.call_via_ha("Desk","missing-ha@controller");
 assert(phone.call_failed_trigger_.calls==failed+1&&phone.transport.originations==3);
 assert(phone.pending_dialplan_target_.empty());
 // A later manual direct call still uses the actual retained contact.
 phone.call("Desk");assert(phone.transport.originations==4&&phone.invites==4);
 assert(phone.transport.ip=="192.0.2.20"&&phone.identity.dest_route=="Desk");
}
''')


@pytest.mark.parametrize('mode', MODES)
@pytest.mark.parametrize('video', [False, True], ids=['audio', 'video'])
def test_actual_snapshot_selected_contact_identity_and_empty_book(tmp_path, mode, video):
    run_consumer(tmp_path, mode, r'''
int main(){
 VoipStack phone;
 phone.call("Desk");
 phone.transport.value.rtp_tx_packets=123;phone.transport.value.rtp_rx_packets=456;
 phone.transport.value.last_sip_status_code=200;phone.transport.value.last_sip_event="answer;ok";
 phone.transport.value.selected_tx_format.sample_rate=16000;
 phone.transport.value.selected_tx_format.frame_ms=10;
 phone.transport.value.selected_rx_format.sample_rate=48000;
 phone.transport.value.selected_rx_format.frame_ms=10;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
 phone.transport.value.video_tx_packets=11;phone.transport.value.video_rx_packets=22;
 phone.transport.value.video_tx_access_units=3;phone.transport.value.video_rx_access_units=4;
 phone.transport.value.media_lifecycle_phase=2;
#endif
 auto text=phone.build_sip_snapshot_string_();
 assert(text.find("st=calling; id=phone-100-42")!=std::string::npos);
 assert(text.find("dir=out; from=Phone; to=Desk; ct=Desk; tr=udp; sc=200")!=std::string::npos);
 assert(text.find("tx=16k/10; rx=48k/10; pt=123; pr=456")!=std::string::npos);
 assert(text.find("tqd=1; tqdrop=2; rqd=3; rqdrop=4")!=std::string::npos);
 assert(text.find("ev=answer ok")!=std::string::npos);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
 assert(text.find("vt=11; vr=22; vat=3; var=4; mlp=2")!=std::string::npos);
#endif
 assert(phone.phonebook_.select("HA"));text=phone.build_sip_snapshot_string_();
 assert(text.find("to=Desk; ct=HA;")!=std::string::npos);
 phone.phonebook_.clear();text=phone.build_sip_snapshot_string_();
 assert(text.find("to=Desk; ct=;")!=std::string::npos);
 // Empty current selection cannot erase the owned terminal identity.
 phone.stop();phone.last_terminal_call_id_="old-call";phone.last_reason_="local_hangup";
 phone.last_terminal_caller_name_="Phone";phone.last_terminal_dest_name_="Desk";
 phone.last_terminal_direction_="outgoing";
 phone.last_terminal_tx_audio_format_.sample_rate=16000;
 phone.last_terminal_tx_audio_format_.frame_ms=20;
 phone.last_terminal_rx_audio_format_.sample_rate=16000;
 phone.last_terminal_rx_audio_format_.frame_ms=20;
 text=phone.build_sip_snapshot_string_();
 assert(text.find("st=idle; id=old-call; dir=out; from=Phone; to=Desk; ct=;")!=std::string::npos);
 assert(text.find("tx=16k/20; rx=16k/20")!=std::string::npos);
}
''', snapshot=True, video=video)
