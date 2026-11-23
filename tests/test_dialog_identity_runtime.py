"""Production INVITE ownership checks through UDP/TCP parser and socket replies."""

from production_sip_source import read_production_source
from production_sip_source import transport_units
from pathlib import Path
import subprocess

import pytest
from test_refer_dialog_runtime import block
from test_refer_wire_runtime import wire_source

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / 'esphome/components/voip_stack'


@pytest.fixture(scope='module')
def dialog_identity_binary(tmp_path_factory):
    path = tmp_path_factory.mktemp('dialog-identity')
    code = wire_source()
    code = code.replace(' uint16_t sip_port_=5060;', ' AudioFormatList offer_tx_formats_,offer_rx_formats_;\n uint16_t sip_port_=5060;')
    code = code.replace('const RtpAudioPayloads*p){', 'const RtpAudioPayloads*p=nullptr){').replace('negotiated_rx_payloads_=*p;', 'if(p)negotiated_rx_payloads_=*p;')
    code = code.replace('const std::string&,uint32_t,bool){return true;}', 'const std::string&,uint32_t,bool=false){return true;}')
    source = read_production_source(COMPONENT / 'sip_transport.cpp')
    responses = (COMPONENT / 'sip_request_response.cpp').read_text()
    code = code.replace('bool handle_invite_(const std::string&,const sockaddr_in&){assert(false);return false;}',
                        'bool handle_invite_(const std::string&,const sockaddr_in&);')
    code = code.replace('bool send_response_(uint16_t,const char*){assert(false);return false;}',
                        'bool send_response_(uint16_t,const char*,const std::string & = "",const std::string & = "");\n'
                        'void defer_udp_provisional_response_(const std::string&,uint32_t,uint16_t){assert(false);}\n'
                        'unsigned reinvites=0;bool handle_reinvite_(const std::string&,const sockaddr_in&){++reinvites;return true;}')
    code = code.replace('enum class MediaLifecyclePhase {IDLE}', 'enum class MediaLifecyclePhase {IDLE,CLEANING,SHUTTING_DOWN}')
    for marker in ['bool SipTransport::handle_invite_(', 'bool SipTransport::send_response_(']:
        code += '\n' + block(source if 'handle_invite' in marker else responses, marker)
    helpers = (ROOT / 'tests/fixtures/refer_wire_main.cpp').read_text()
    helpers = helpers[:helpers.index('void auth_case(')]
    main = r'''
int main(int argc,char**argv){
 SipTransport s;Peer original(std::string(argv[1])=="tcp");setup(s,original);
 s.last_invite_cseq_number_=1;
 const auto id=s.call_id_,local=s.local_tag_,remote=s.remote_tag_,uri=s.remote_uri_;
 const auto socket=s.tcp_flow_.socket.load();
 for(bool active:{true,false}){
  s.media_active_=active;
  auto bad=request(original,"INVITE",2,"","original","foreign-local","foreign-remote");
  original.send_to(s,bad);auto replies=original.drain();
  assert(replies.size()==1&&replies[0].rfind("SIP/2.0 481",0)==0&&"foreign dialog tags must receive 481 before metadata mutation");
  assert(s.call_id_==id&&s.local_tag_==local&&s.remote_tag_==remote&&s.remote_uri_==uri);
  assert(s.tcp_flow_.socket==socket&&s.delivered.empty());
 }
 s.media_active_=true;
 original.send_to(s,request(original,"BYE",3));auto ended=original.drain();
 assert(ended.size()==1&&ended[0].rfind("SIP/2.0 200",0)==0);
 assert(s.call_id_.empty()&&!s.media_active_&&s.delivered.size()==1);
 cleanup(s);
}
'''
    cpp = path / 'identity.cpp'
    cpp.write_text(code + '\n' + helpers + '\n' + main)
    binary = path / 'identity'
    proc = subprocess.run(['g++', '-std=c++17', '-I', str(ROOT), str(cpp), '-o', str(binary)], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    return binary


@pytest.mark.parametrize('transport', ['udp', 'tcp'])
def test_foreign_invite_tags_preserve_dialog_and_legitimate_bye(dialog_identity_binary, transport):
    proc = subprocess.run([str(dialog_identity_binary), transport], capture_output=True, text=True, timeout=5)
    assert proc.returncode == 0, proc.stderr


@pytest.fixture(scope="module")
def native_dialog_owner(tmp_path_factory):
    """Link whole production translation units to the real POSIX UDP socket."""
    folder = tmp_path_factory.mktemp("native-dialog-owner")
    component = ROOT / "esphome/components/voip_stack"
    fixtures = ROOT / "tests/fixtures"
    for header_name in (
        "esphome/core/defines.h", "esphome/core/helpers.h", "esphome/core/log.h",
        "esphome/core/hal.h", "esphome/components/network/util.h", "esp_system.h",
        "esp_heap_caps.h", "lwip/sockets.h", "freertos/FreeRTOS.h",
        "freertos/task.h", "freertos/portmacro.h", "freertos/semphr.h",
    ):
        header = folder / header_name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "sip_dialog_host_sdk.h"\n')
    units = [component / name for name in ("sip_transport.cpp", "sip_message.cpp", "sip_request_response.cpp",
                                           "sip_sdp.cpp", "sip_refer.cpp", "media_negotiator.cpp")]
    units.extend(sorted(component.glob("sip_dialog_*.cpp")))
    units.extend(unit for unit in transport_units(component) if unit not in units)
    binary = folder / "dialog"
    subprocess.run(
        ["g++", "-std=c++17", "-O1", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
         "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie", "-DUSE_ESP32",
         "-DUSE_ESPHOME_VOIP_SIP_TRANSPORT", "-DUSE_ESPHOME_VOIP_STACK_PCM",
         "-I", str(folder), "-I", str(fixtures), "-I", str(ROOT), "-I", str(component),
         str(fixtures / "sip_dialog_owner.cpp"), *map(str, units), "-o", str(binary)],
        check=True,
    )
    return binary


@pytest.mark.parametrize("mode", ["decline", "success", "crossed", "incompatible", "incoming",
                                  "identity-name", "identity-bare", "identity-number"])
def test_native_dialog_owner_ack_terminal_and_reuse(native_dialog_owner, mode):
    subprocess.run([native_dialog_owner, mode], check=True, timeout=5)
