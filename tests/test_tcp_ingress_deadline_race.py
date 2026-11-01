"""An observed idle timeout cannot retire a newly owned TCP call generation."""

import re
import subprocess

from test_refer_dialog_runtime import SOURCE
from test_tcp_incoming_account_policy import incoming_account_source


def test_idle_expiry_rechecks_the_owned_deadline_after_a_new_originate(tmp_path):
    source = (SOURCE / "sip_transport.cpp").read_text()
    begin = source.index("    for (auto *flow : {&this->tcp_flow_, &this->inbound_tcp_flow_})",
                         source.index("const int ready = ::select"))
    expiry = source[begin:source.index("    bool connect_expired", begin)]
    # Deterministically schedule the main-context originate after the SIP
    # worker observes expiry, immediately before its actual loss owner runs.
    expiry, inserted = re.subn(
        r"(this->handle_tcp_peer_loss_\(flow(?:, [^)]+)?\);)",
        r"{if(on_expiry_read)on_expiry_read();\1}", expiry, count=1,
    )
    assert inserted == 1
    code = '#include <functional>\n' + incoming_account_source(False)
    code = code.replace("struct SipTransport: SipPhoneTransport {",
                        "struct SipTransport: SipPhoneTransport {\n"
                        "std::function<void()> on_expiry_read;\nvoid expire_inbound_(){\n" + expiry + "}\n", 1)
    code += r'''
int main(){
 SipTransport s;Peer peer(true);setup(s,peer);
 peer.send_to(s,request(peer,"BYE",2));auto ended=peer.drain();
 assert(ended.size()==1&&ended[0].rfind("SIP/2.0 200",0)==0&&s.call_id_.empty());
 const uint32_t deadline=s.tcp_flow_.ingress_deadline;assert(deadline!=0);
 clock_ms=deadline;bool scheduled=false;
 s.on_expiry_read=[&]{
  if(scheduled)return;scheduled=true;++clock_ms;
  assert(s.send_invite_unlocked_("replacement-call","esp","ESP","peer","Peer"));
 };
 s.expire_inbound_();
 assert(scheduled&&s.call_id_=="replacement-call"&&s.outgoing_invite_pending_);
 assert(s.connection_losses==0&&fcntl(peer.esp,F_GETFD)>=0);
 auto invite=peer.drain();assert(invite.size()==1&&invite[0].rfind("INVITE ",0)==0);
 peer.send_to(s,response(invite[0],486));auto ack=peer.drain();
 assert(ack.size()==1&&ack[0].rfind("ACK ",0)==0);
 assert(s.call_id_.empty()&&!s.outgoing_invite_pending_);
 s.close_tcp_flow_(s.tcp_flow_);s.close_tcp_flow_(s.inbound_tcp_flow_);close(s.sip_socket_);
}
'''
    defines = tmp_path / "esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    cpp = tmp_path / "expiry.cpp"
    cpp.write_text(code)
    binary = tmp_path / "expiry"
    built = subprocess.run(["g++", "-std=c++17", "-g", "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                            "-I", str(tmp_path), "-I", str(SOURCE.parents[2]), "-I", str(SOURCE),
                            str(cpp), "-o", str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=8)
    assert result.returncode == 0, result.stdout + result.stderr
