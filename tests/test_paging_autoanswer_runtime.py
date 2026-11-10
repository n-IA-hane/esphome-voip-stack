"""Per-call paging authorization uses real parsing and the existing FSM."""

from pathlib import Path
import subprocess

import pytest

from test_incoming_setup_runtime import incoming_source
from test_refer_dialog_runtime import block
from test_sip_registration_transport import registration_transport_source

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "esphome/components/voip_stack"


def test_answer_mode_parser_and_peer_authorization(tmp_path):
    code = tmp_path / "answer_mode.cpp"
    code.write_text(r'''
#include "sip_answer_mode.h"
#include <cassert>
using namespace esphome::voip_stack;
int main(){
 auto mode=[](const std::string&field){return sip_requested_answer_mode("INVITE sip:a@b SIP/2.0\r\n"+field+"\r\n\r\n");};
 assert(!mode("").automatic&&mode("").valid);
 assert(mode("Answer-Mode: Auto").automatic);
 auto required=mode("aNsWeR-mOdE: aUtO;\r\n require");
 assert(required.valid&&required.automatic&&required.required);
 assert(!mode("Answer-Mode: Manual").automatic);
 assert(mode("Answer-Mode: Manual;require").manual);
 auto unknown=mode("Answer-Mode: Future-Mode;require;future=enabled");
 assert(unknown.valid&&!unknown.automatic&&!unknown.manual);
 auto extension=mode("Answer-Mode: Auto;future=\"one;two\";require;another=enabled");
 assert(extension.valid&&extension.automatic&&extension.required);
 assert(!mode("Answer-Mode: Auto\r\nAnswer-Mode: Manual").valid);
 assert(!mode("Answer-Mode: Auto, Manual").valid);
 assert(!mode("Answer-Mode: Auto;require;require").valid);
 assert(!mode("Priv-Answer-Mode: Auto").automatic);
 assert(!sip_requested_answer_mode("INVITE sip:a@b SIP/2.0\r\n\r\nAnswer-Mode: Auto").automatic);
 // A matched account must opt in; direct trust cannot override account policy.
 assert(sip_paging_auto_answer_authorized(true,true,true,false));
 assert(!sip_paging_auto_answer_authorized(true,true,false,true));
 assert(sip_paging_auto_answer_authorized(true,false,false,true));
 assert(!sip_paging_auto_answer_authorized(true,false,false,false));
 assert(!sip_paging_auto_answer_authorized(false,true,true,true));
}
''')
    binary = tmp_path / "answer_mode"
    built = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                            "-fno-pie", "-no-pie", "-I", str(SOURCE), str(code), "-o", str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    ran = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert ran.returncode == 0, ran.stderr


@pytest.mark.parametrize("global_auto_answer", [False, True])
def test_actual_fsm_manual_required_defers_without_changing_global_setting(tmp_path, global_auto_answer):
    code = tmp_path / "manual_required.cpp"
    code.write_text('#include "esphome/components/voip_stack/sip_answer_mode.h"\n' + incoming_source() + r'''
int main(int argc,char**argv){
 assert(argc==2);VoipStack phone;phone.auto_answer_=argv[1][0]=='1';
 const bool previous=phone.auto_answer_;
 auto mode=sip_requested_answer_mode("INVITE sip:a@b SIP/2.0\r\nAnswer-Mode: Manual;require\r\n\r\n");
 assert(mode.valid&&mode.manual&&mode.required);
 SipSignal invite;invite.type=SipSignalType::INVITE;invite.call_id="manual-required";
 invite.caller_name="HA";invite.caller_route="ha";invite.dest_name="esp";invite.dest_route="esp";
 invite.paging_auto_answer=mode.automatic;invite.paging_manual_answer=mode.manual;
 audio_format_list_default(&invite.caller_tx_formats);audio_format_list_default(&invite.caller_rx_formats);
 phone.on_sip_signal_received_(invite);phone.drain();
 assert(phone.auto_answer_==previous&&phone.call_state_==CallState::RINGING);
}
''')
    binary = tmp_path / "manual_required"
    built = subprocess.run(["g++", "-std=c++17", "-DUSE_ESPHOME_VOIP_PAGING", "-I", str(ROOT),
        str(code), "-o", str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    ran = subprocess.run([binary, "1" if global_auto_answer else "0"], capture_output=True, text=True, timeout=5)
    assert ran.returncode == 0, ran.stderr


@pytest.mark.parametrize("authorized", [False, True])
def test_actual_fsm_honors_only_this_call_without_changing_global_setting(tmp_path, authorized):
    code = tmp_path / "paging_fsm.cpp"
    code.write_text(incoming_source() + r'''
int main(int argc,char**argv){
 assert(argc==2);
 VoipStack phone;phone.auto_answer_=false;
 SipSignal invite;invite.type=SipSignalType::INVITE;
 invite.call_id="paging-generation";invite.caller_name="HA";invite.caller_route="ha";
 invite.dest_name="esp";invite.dest_route="esp";invite.paging_auto_answer=argv[1][0]=='1';
 audio_format_list_default(&invite.caller_tx_formats);audio_format_list_default(&invite.caller_rx_formats);
 phone.on_sip_signal_received_(invite);phone.drain();
 assert(!phone.auto_answer_);
 assert(phone.call_state_==(invite.paging_auto_answer?CallState::IN_CALL:CallState::RINGING));
}
''')
    binary = tmp_path / "paging_fsm"
    built = subprocess.run(["g++", "-std=c++17", "-DUSE_ESPHOME_VOIP_PAGING", "-I", str(ROOT),
                            str(code), "-o", str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    ran = subprocess.run([binary, "1" if authorized else "0"], capture_output=True, text=True, timeout=5)
    assert ran.returncode == 0, ran.stderr


@pytest.fixture(scope="module")
def paging_wire_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("paging-wire")
    defines = folder / "esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    code = '#include "sip_answer_mode.h"\nusing namespace esphome::voip_stack;\n' + registration_transport_source(full_invite=True)
    code = code.replace("struct SipTransport: SipPhoneTransport {", "struct SipTransport: SipPhoneTransport {\n"
                        "std::vector<uint32_t>paging_auto_answer_peers_;void set_paging_auto_answer_peers(const std::vector<std::string>&);\n")
    code += "\n" + block((SOURCE / "sip_transport.cpp").read_text(), "void SipTransport::set_paging_auto_answer_peers(")
    peers = (ROOT / "tests/fixtures/refer_wire_main.cpp").read_text().split("std::string begin(")[0]
    code += "\n" + peers
    registration = (ROOT / "tests/fixtures/registration_transport.cpp").read_text()
    code += "\n" + registration[:registration.index("void start_account_call(")]
    code += "\n" + (ROOT / "tests/fixtures/multiaccount_global_busy.cpp").read_text()
    code += r'''
int main(int argc,char**argv){
 assert(argc==5);Peer peer(std::string(argv[1])=="tcp");SipTransport s;bind_account(s,peer);
 s.call_id_.clear();s.local_tag_.clear();s.remote_tag_.clear();s.media_active_=false;
 const bool direct=std::string(argv[2])=="direct",allowed=std::string(argv[3])=="allowed";
 const std::string mode=argv[4];
 s.accept_direct_calls_=true;
 s.registration_.account(0).config.allow_paging_auto_answer=allowed;
 if(direct&&allowed)s.set_paging_auto_answer_peers({"127.0.0.1","invalid","127.0.0.1"});
 assert(s.paging_auto_answer_peers_.size()==(direct&&allowed?1:0));
 auto invite=initial_invite(peer,direct?"unknown":"office","paging-call");
 const std::string header=mode=="manual-required"?"Manual;require;future=enabled":
  mode=="unknown"?"Future-Mode;require":mode=="extensions"?"Auto;future=\"one;two\"":
  mode=="auto-required"?"Auto;require":"Auto";
 invite.insert(invite.find("Content-Length:"),"Answer-Mode: "+header+"\r\n");
 if(mode=="auto-required"&&!allowed){
  peer.send_to(s,invite);expect_response(peer,403,"paging-call");assert(s.delivered.empty());
  for(auto&flow:s.registration_flows_)s.close_tcp_flow_(flow);
  s.close_tcp_flow_(s.tcp_flow_);s.close_tcp_flow_(s.inbound_tcp_flow_);close(s.sip_socket_);return 0;
 }
 peer.send_to(s,invite);expect_response(peer,100,"paging-call");
 assert(s.delivered.size()==1&&s.delivered[0].paging_auto_answer==(allowed&&mode!="manual-required"&&mode!="unknown"));
 assert(s.delivered[0].paging_manual_answer==(mode=="manual-required"));
 assert(s.active_account_==(direct?-1:0));
 // A second line, even with an authorized auto-answer request, stays busy.
 auto competing=initial_invite(peer,"office","competing-page");
 competing.insert(competing.find("Content-Length:"),"Answer-Mode: Auto\r\n");
 const ActiveSnapshot owned(s);peer.send_to(s,competing);expect_response(peer,486,"competing-page");owned.unchanged(s);
 assert(s.delivered.size()==1);
 for(auto&flow:s.registration_flows_)s.close_tcp_flow_(flow);
 s.close_tcp_flow_(s.tcp_flow_);s.close_tcp_flow_(s.inbound_tcp_flow_);close(s.sip_socket_);
}
'''
    cpp = folder / "paging.cpp"
    cpp.write_text(code)
    binary = folder / "paging"
    built = subprocess.run(["g++", "-std=c++17", "-g", "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                            "-DUSE_ESPHOME_VOIP_PAGING", "-DUSE_ESPHOME_VOIP_REGISTRATION", "-I", str(folder), "-I", str(ROOT), "-I", str(SOURCE),
                            str(cpp), str(SOURCE / "sip_registration.cpp"), "-lcrypto", "-o", str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize("transport", ["udp", "tcp"])
@pytest.mark.parametrize("route", ["account", "direct"])
@pytest.mark.parametrize("allowed", ["allowed", "denied"])
@pytest.mark.parametrize("mode", ["auto", "manual-required", "unknown", "extensions", "auto-required"])
def test_real_wire_admission_authorizes_per_account_or_explicit_peer_and_never_bypasses_busy(paging_wire_binary, transport, route, allowed, mode):
    ran = subprocess.run([paging_wire_binary, transport, route, allowed, mode], capture_output=True, text=True, timeout=8)
    assert ran.returncode == 0, ran.stderr
