"""A confirmed outbound dialog keeps its terminal BYE owner after audio failure."""
from pathlib import Path
import subprocess

import pytest
from test_incoming_setup_runtime import incoming_source

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize('failure', ['prepare', 'format'])
def test_outgoing_confirmed_failure_sends_bye_once(tmp_path, failure):
    code = incoming_source().replace('unsigned prepares=0,disconnects=0,aborts=0;',
                                    'unsigned prepares=0,disconnects=0,aborts=0,byes=0;')
    code = code.replace('bool start_audio_path(){return true;}',
                        'bool start_audio_path(){++prepares;return prepare_ok;}')
    code = code.replace('Snapshot snapshot()const{return {};}',
                        'Snapshot snapshot()const{return {byes!=0,0};}')
    code = code.replace('bool send_sip_bye_(const std::string&){return true;}',
                        'bool send_sip_bye_(const std::string&){++t.byes;return true;}')
    code += r'''
int main(int argc,char**argv){
 VoipStack phone;phone.call_state_=CallState::CALLING;
 phone.set_call_identity_("outbound","esp","esp","peer","peer");
 SipSignal accepted;accepted.type=SipSignalType::STATUS_200_OK;accepted.call_id="outbound";
 if(std::string(argv[1])=="prepare")phone.t.prepare_ok=false;
 else accepted.selected_tx_format.sample_rate=32000;
 phone.on_sip_signal_received_(accepted);phone.drain();
 assert(phone.t.byes==1&&"200/ACK established the SIP dialog: local audio failure requires BYE");
 assert(phone.t.disconnects==0&&phone.last_terminal_call_id_=="outbound"&&phone.call_state_!=CallState::IDLE&&"pending BYE retains its transport owner and terminal identity");
 phone.on_sip_signal_received_(accepted);phone.drain();
 assert(phone.t.byes==1&&phone.t.disconnects==0);
 phone.t.byes=0;SipSignal done;done.type=SipSignalType::TERMINAL_COMPLETE;
 phone.on_sip_signal_received_(done);phone.drain();
 assert(phone.call_state_==CallState::IDLE&&phone.identity.call_id.empty());
 assert(phone.t.disconnects==0&&"terminal completion must not reset an already closed transport twice");
}
'''
    cpp = tmp_path / 'outgoing.cpp'
    cpp.write_text(code)
    binary = tmp_path / 'outgoing'
    proc = subprocess.run(['g++', '-std=c++17', '-DUSE_ESPHOME_VOIP_STACK_SPEAKER', '-I', str(ROOT), str(cpp), '-o', str(binary)], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    proc = subprocess.run([str(binary), failure], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr


@pytest.fixture(scope='module')
def outgoing_wire_binary(tmp_path_factory):
    from test_incoming_media_failure_runtime import media_failure_source
    from test_refer_wire_runtime import wire_source
    code = wire_source()
    code = code.replace(' uint16_t sip_port_=5060;', ' struct Converter {bool prepare_additional(const RtpAudioPayloads&,const AudioFormat&){assert(false);return false;}}pcm_receive_converter_;\n uint16_t sip_port_=5060;')
    media = media_failure_source().replace('uint32_t esp_random(){return 42;}', '').replace('struct RtpAudioPayloads {};', '')
    app = incoming_source().replace('uint32_t millis(){return 100;}', '')
    app = app.replace('struct Transport {', 'struct AppTransport {').replace('Transport t;Transport*transport_', 'AppTransport t;AppTransport*transport_')
    app = app.replace('bool Transport::send_final_response', 'bool AppTransport::send_final_response')
    app = app.replace('Snapshot snapshot()const{return {};}', 'Snapshot snapshot()const{return {wire_transport->terminal_transaction_pending_locked_(),0};}')
    app = app.replace('bool start_audio_path(){return true;}', 'bool start_audio_path(){++prepares;return media_owner.prepare_media_path_locked_();}')
    app = app.replace('void disconnect(){++disconnects;}', 'void disconnect(){++disconnects;wire_transport->disconnect();}')
    app = app.replace('bool send_sip_bye_(const std::string&){return true;}', 'bool send_sip_bye_(const std::string&id){return wire_transport->send_bye_unlocked_(id);}')
    # The abstraction calls the production reset path only after its owner has
    # finished. Expose that supported primitive through the scheduler adapter.
    code = code.replace(' void stop_audio_path(){media_active_=false;}', ' void stop_audio_path(){media_active_=false;}\n void disconnect(){if(!terminal_transaction_pending_locked_())reset_dialog_();}')
    helpers = (ROOT / 'tests/fixtures/refer_wire_main.cpp').read_text()
    helpers = helpers[:helpers.index('void auth_case(')]
    main = r'''
int main(int argc,char**argv){
 SipTransport s;wire_transport=&s;Peer remote(std::string(argv[1])=="tcp");setup(s,remote);
 s.call_id_.clear();s.media_active_=false;
 int occupied=-1;
 const std::string failure=argv[2];
 if(failure=="converter")media_owner.pcm_receive_converter_.ok=false;
 if(failure=="bind"){
  occupied=socket(AF_INET,SOCK_DGRAM,0);assert(occupied>=0);
  sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  assert(bind(occupied,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
  socklen_t len=sizeof(address);assert(getsockname(occupied,reinterpret_cast<sockaddr*>(&address),&len)==0);
  media_owner.rtp_port_=ntohs(address.sin_port);
 }
 VoipStack phone;phone.call_state_=CallState::CALLING;
 phone.set_call_identity_("outbound","esp","esp","peer","peer");
 assert(s.send_invite_unlocked_("outbound","esp","esp","peer","peer"));
 auto invite=remote.drain();assert(invite.size()==1&&invite[0].rfind("INVITE ",0)==0);
 remote.send_to(s,response(invite[0],200));
 auto ack=remote.drain();assert(ack.size()==1&&ack[0].rfind("ACK ",0)==0);
 assert(s.delivered.size()==1&&s.delivered[0].type==SipSignalType::STATUS_200_OK);
 if(failure=="format")s.delivered[0].selected_tx_format.sample_rate=32000;
 phone.on_sip_signal_received_(s.delivered[0]);phone.drain();
 auto bye=remote.drain();
 assert(bye.size()==1&&bye[0].rfind("BYE ",0)==0&&"confirmed audio failure must send BYE on its actual UDP/TCP dialog");
 assert(!s.pending_bye_.empty()&&s.call_id_=="outbound"&&phone.t.disconnects==0);
 remote.send_to(s,response(invite[0],200));auto replay=remote.drain();
 assert(replay.size()==1&&replay[0]==ack[0]&&"duplicate 200 replays ACK without a second BYE");
 remote.send_to(s,response(bye[0],200));assert(remote.drain().empty());
 assert(s.call_id_.empty()&&!s.terminal_transaction_pending_locked_());
 phone.on_sip_signal_received_(s.delivered.back());phone.drain();
 assert(phone.call_state_==CallState::IDLE&&phone.identity.call_id.empty());
 if(occupied>=0)close(occupied);media_owner.pcm_receive_converter_.ok=true;media_owner.rtp_port_=0;
 phone.set_call_identity_("second","esp","esp","peer","peer");phone.call_state_=CallState::CALLING;
 assert(s.send_invite_unlocked_("second","esp","esp","peer","peer"));
 auto next=remote.drain();assert(next.size()==1);
 remote.send_to(s,response(next[0],200));assert(remote.drain().size()==1);
 phone.on_sip_signal_received_(s.delivered.back());phone.drain();
 assert(phone.call_state_==CallState::IN_CALL&&s.call_id_=="second"&&media_owner.rtp_socket_>=0);
 cleanup(s);
}
'''
    path = tmp_path_factory.mktemp('outgoing-wire')
    cpp = path / 'outgoing.cpp'
    cpp.write_text(code + '\n' + media + '\nSipTransport *wire_transport=nullptr;\n' + app + '\n' + helpers + '\n' + main)
    binary = path / 'outgoing'
    proc = subprocess.run(['g++', '-std=c++17', '-DUSE_ESPHOME_VOIP_STACK_SPEAKER', '-I', str(ROOT), str(cpp), '-o', str(binary)], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    return binary


@pytest.mark.parametrize('transport', ['udp', 'tcp'])
@pytest.mark.parametrize('failure', ['converter', 'bind', 'format'])
def test_confirmed_audio_failure_sends_bye_and_accepts_next_call(outgoing_wire_binary, transport, failure):
    proc = subprocess.run([str(outgoing_wire_binary), transport, failure], capture_output=True, text=True, timeout=5)
    assert proc.returncode == 0, proc.stderr
