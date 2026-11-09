"""One real INVITE owner admits a single call across registered accounts."""

import subprocess

import pytest

from test_refer_dialog_runtime import SOURCE
from test_tcp_incoming_account_policy import incoming_account_source


@pytest.fixture(scope="module")
def global_busy_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("multiaccount-global-busy")
    defines = folder / "esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    code = incoming_account_source()
    # The shared single-account fixture uses a constant hardware RNG. Give
    # distinct registrations distinct deterministic transaction identities.
    rng = "uint32_t esp_random(){return 42;}"
    assert code.count(rng) == 1
    code = code.replace(rng, "uint32_t esp_random(){static uint32_t next=42;return ++next;}")
    code += (SOURCE.parents[2] / "tests/fixtures/multiaccount_global_busy.cpp").read_text()
    code += r'''
int main(int argc,char **argv) {
 assert(argc==5);
 Peer office(std::string(argv[1])=="tcp"),home(std::string(argv[2])=="tcp");
 SipTransport s;bind_account(s,office);bind_home(s,home);
 assert(s.registration_.is_registered("office")&&s.registration_.is_registered("home"));
 s.call_id_.clear();s.local_tag_.clear();s.remote_tag_.clear();s.media_active_=false;
 s.accept_direct_calls_=false;
 const bool home_first=std::string(argv[3])=="home",connected=std::string(argv[4])=="in_call";
 Peer &first=home_first?home:office,&second=home_first?office:home;
 const std::string first_account=home_first?"home":"office",second_account=home_first?"office":"home";
 const auto invite=initial_invite(first,first_account,"owned-first");
 first.send_to(s,invite);expect_response(first,100,"owned-first");
 assert(s.active_account_==(home_first?1:0)&&s.call_id_=="owned-first");
 assert(s.delivered.size()==1&&s.delivered[0].type==SipSignalType::INVITE);
 assert(s.send_response_(connected?200:180,connected?"OK":"Ringing"));
 const auto initial_response=first.drain();assert(initial_response.size()==1);
 assert(initial_response[0].rfind(connected?"SIP/2.0 200 ":"SIP/2.0 180 ",0)==0);
 if(connected) {
  first.send_to(s,request(first,"ACK",1,"","owned-first",s.local_tag_,s.remote_tag_));
  assert(!s.completed_invite_.awaiting_ack&&s.media_active_);
 }else assert(!s.media_active_);
 const ActiveSnapshot owned(s);
 const auto competing=initial_invite(second,second_account,"owned-second");
 second.send_to(s,competing);expect_response(second,486,"owned-second");owned.unchanged(s);
 assert(first.drain().empty());
 // The same first transaction is a retransmission, never a second call.
 first.send_to(s,invite);
 const auto replay=first.drain();assert(replay.size()==1&&replay[0]==initial_response[0]);
 owned.unchanged(s);assert(second.drain().empty());
 // A repeated competing INVITE must still leave the original owner intact.
 second.send_to(s,competing);expect_response(second,486,"owned-second");owned.unchanged(s);
 assert(first.drain().empty());
 // End only the original dialog, then prove that the other account can call.
 end_incoming(s,first,invite,connected);
 assert(s.registration_.is_registered("office")&&s.registration_.is_registered("home"));
 const auto next=initial_invite(second,second_account,"after-busy");
 second.send_to(s,next);expect_response(second,100,"after-busy");
 assert(s.call_id_=="after-busy"&&s.active_account_==(home_first?0:1));
 end_incoming(s,second,next,false);
 for(auto &flow:s.registration_flows_)s.close_tcp_flow_(flow);
 s.close_tcp_flow_(s.tcp_flow_);s.close_tcp_flow_(s.inbound_tcp_flow_);
 close(s.sip_socket_);
}
'''
    cpp = folder / "global_busy.cpp"
    cpp.write_text(code)
    binary = folder / "global_busy"
    result = subprocess.run([
        "g++", "-std=c++17", "-g", "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
        "-DUSE_ESPHOME_VOIP_REGISTRATION", "-I", str(folder), "-I", str(SOURCE.parents[2]),
        "-I", str(SOURCE), str(cpp), str(SOURCE / "sip_registration.cpp"), "-lcrypto", "-o", str(binary),
    ], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("office_transport", ["udp", "tcp"])
@pytest.mark.parametrize("home_transport", ["udp", "tcp"])
@pytest.mark.parametrize("first_account", ["home", "office"])
@pytest.mark.parametrize("phase", ["ringing", "in_call"])
def test_accounts_share_one_global_call_and_replay_only_the_owned_transaction(
    global_busy_binary, office_transport, home_transport, first_account, phase,
):
    result = subprocess.run([
        global_busy_binary, office_transport, home_transport, first_account, phase,
    ], capture_output=True, text=True, timeout=8)
    assert result.returncode == 0, result.stdout + result.stderr
