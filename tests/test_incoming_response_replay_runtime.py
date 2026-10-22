"""The final local-setup error uses the existing SIP transaction cache and sockets."""

from pathlib import Path
import subprocess

import pytest
from test_refer_dialog_runtime import block
from test_refer_wire_runtime import wire_source

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "esphome/components/voip_stack"


@pytest.fixture(scope="module")
def replay_binary(tmp_path_factory):
    path = tmp_path_factory.mktemp("incoming-replay")
    code = wire_source()
    requests = (COMPONENT / "sip_request_response.cpp").read_text()
    source = (COMPONENT / "sip_transport.cpp").read_text()
    old = "bool send_response_(uint16_t,const char*){assert(false);return false;}"
    assert old in code
    code = code.replace(
        old,
        'bool send_response_(uint16_t,const char*,const std::string&="",const std::string&="");\n'
        "bool send_final_response(const std::string&,uint16_t,const std::string&);\n"
        "void defer_udp_provisional_response_(const std::string&,uint32_t,uint16_t){assert(false);}",
    )
    # Token separation is significant before a default reference argument.
    code = code.replace("std::string&=", "std::string & =")
    code += "\n" + block(requests, "bool SipTransport::send_response_(")
    code += "\n" + block(source, "bool SipTransport::send_final_response(")
    # TCP enters the INVITE handler before consulting this cache. Keep its
    # production validation/replay prefix; this focused fixture must never
    # negotiate a new INVITE or invent another replay implementation.
    invite = block(source, "bool SipTransport::handle_invite_(")
    invite = invite[
        : invite.index("  if (this->terminal_transaction_pending_locked_())")
    ]
    invite += '  assert(false && "unexpected new INVITE in replay-only fixture"); return false;\n}'
    code = code.replace(
        "bool handle_invite_(const std::string&,const sockaddr_in&){assert(false);return false;}",
        "bool handle_invite_(const std::string&,const sockaddr_in&);",
    )
    code += "\n" + invite
    helpers = (ROOT / "tests/fixtures/refer_wire_main.cpp").read_text()
    helpers = helpers[: helpers.index("void auth_case(")]
    main = r"""
int main(int argc,char **argv){
 SipTransport s;Peer original(std::string(argv[1])=="tcp");setup(s,original);
 s.media_active_=false;const auto invite=request(original,"INVITE",1);
 s.last_invite_via_=header_value(invite,"Via");s.last_invite_from_=header_value(invite,"From");
 s.last_invite_to_=header_value(invite,"To");s.last_invite_cseq_=header_value(invite,"CSeq");
 s.last_invite_cseq_number_=1;s.last_invite_peer_ip_v4_=INADDR_LOOPBACK;
 s.last_invite_peer_port_=ntohs(original.address.sin_port);
 assert(s.send_final_response("original",500,"media_setup_failed"));
 auto failed=original.drain();assert(failed.size()==1&&failed[0].rfind("SIP/2.0 500",0)==0);
 assert(s.call_id_.empty()&&!s.media_active_);
 assert(s.completed_invite_.method=="INVITE"&&s.completed_invite_.response==failed[0]);
 original.send_to(s,invite);auto replay=original.drain();assert(replay.size()==1&&replay[0]==failed[0]);
 assert(s.delivered.empty()&&"a retransmission must not create a new phone operation");
 auto ack=invite;ack.replace(0,6,"ACK");
 auto cseq=ack.find("CSeq: 1 INVITE");assert(cseq!=std::string::npos);ack.replace(cseq,14,"CSeq: 1 ACK");
 original.send_to(s,ack);assert(!s.completed_invite_.awaiting_ack);
 assert(!s.terminal_transaction_pending_locked_()&&s.call_id_.empty());
 auto next=request(original,"INVITE",2,"","second");
 assert(!s.replay_completed_response_(next,original.address,"INVITE",!original.tcp));
 cleanup(s);
}
"""
    cpp = path / "replay.cpp"
    cpp.write_text(code + "\n" + helpers + "\n" + main)
    exe = path / "replay"
    result = subprocess.run(
        ["g++", "-std=c++17", "-I", str(ROOT), str(cpp), "-o", str(exe)],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr
    return exe


@pytest.mark.parametrize("transport", ["udp", "tcp"])
def test_incoming_error_replays_final_response(replay_binary, transport):
    result = subprocess.run(
        [str(replay_binary), transport], capture_output=True, text=True
    )
    assert result.returncode == 0, result.stderr
