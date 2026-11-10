"""UA OPTIONS is advisory SDP on real sockets, without acquiring media ownership."""
import subprocess

import pytest

from test_refer_dialog_runtime import ROOT, SOURCE, block
from test_refer_wire_runtime import wire_source


def options_source():
    code = wire_source(full_invite=True)
    source = (SOURCE / "sip_transport.cpp").read_text()
    sdp = (SOURCE / "sip_sdp.cpp").read_text()
    _parser = (SOURCE / "sip_message.cpp").read_text()
    code = code.replace("struct SipPhoneTransport {",
                        block(_parser, "bool parse_rtpmap_format(") + "\nstruct SipPhoneTransport {", 1)
    code = code.replace('void local_ip_for_peer_(uint32_t,std::string*out)', 'void local_ip_for_peer_(uint32_t,std::string*out) const')
    # Compose newly available production helpers; the unchanged base still
    # compiles and exposes its original empty OPTIONS reply for the RED witness.
    for marker, owner in [("std::string SipTransport::build_sdp_capabilities_(", sdp),
                          ("std::string SipTransport::wrap_sdp_envelope_(", sdp),
                          ("std::string SipTransport::wrap_sdp_envelope_version_(", sdp),
                          ("void SipTransport::handle_options_(", source)]:
        if marker not in owner:
            continue
        body = block(owner, marker)
        decl = body[:body.index("{")].replace("SipTransport::", "").strip() + ";"
        if marker == "std::string SipTransport::build_sdp_capabilities_(":
            code = code.replace(block(code, "std::string build_sdp_capabilities_("), decl, 1)
        elif marker == "void SipTransport::handle_options_(":
            continue
        else:
            code = code.replace("using SipTcpFlow=::SipTcpFlow;", "using SipTcpFlow=::SipTcpFlow;\n" + decl, 1)
        code += "\n" + body
    peers = (ROOT / "tests/fixtures/refer_wire_main.cpp").read_text()
    code += "\n" + peers[:peers.index("std::string begin(")]
    return code + r'''
#include <iostream>
AudioFormat pcm(unsigned rate,unsigned frame=10){AudioFormat f;f.sample_rate=rate;f.frame_ms=frame;return f;}
int main(int argc,char **argv){
 assert(argc==4);const std::string state=argv[2],accept=argv[3];
 Peer peer(std::string(argv[1])=="tcp");SipTransport s;setup(s,peer);
 s.offer_tx_formats_.count=2;s.offer_tx_formats_.formats[0]=pcm(16000,20);s.offer_tx_formats_.formats[1]=pcm(16000,10);
 s.offer_rx_formats_.count=accept=="known"||accept=="unrepresentable"?2:3;s.offer_rx_formats_.formats[0]=pcm(48000);
 s.offer_rx_formats_.formats[1]=pcm(16000,accept=="known"?10:20);
 if(s.offer_rx_formats_.count==3)s.offer_rx_formats_.formats[2]=pcm(16000,10);
 if(state=="idle"){s.call_id_.clear();s.local_tag_.clear();s.remote_tag_.clear();s.media_active_=false;}
 const auto id=s.call_id_,local=s.local_tag_,remote=s.remote_tag_,target=s.remote_target_uri_;
 const auto session=s.sdp_session_id_,version=s.sdp_session_version_;
 const auto ip=s.remote_ip_v4_.load();const auto port=s.remote_sip_port_.load();
 const auto fd=s.tcp_flow_.socket.load(),rtp=s.rtp_socket_;const auto events=s.delivered.size();
 const auto tx=s.selected_tx_format_,rx=s.selected_rx_format_;const auto maps=s.negotiated_rx_payloads_;
 std::string extra=accept=="plain"?"Accept: text/plain\r\n":
   accept=="empty"?"Accept:\r\n":accept=="zero"?"Accept: */*;q=1, application/sdp;q=0\r\n":
   accept=="wildcard"?"Accept: application/*\r\n":"Accept: application/sdp\r\n";
 if(accept=="absent")extra.clear();
 const bool in_dialog=state=="dialog";
 auto query=request(peer,"OPTIONS",4,extra,in_dialog?"original":"probe",in_dialog?"local":"","remote");
 peer.send_to(s,query);auto replies=peer.drain();assert(replies.size()==1);
 assert(header_value(replies[0],"Call-ID")==header_value(query,"Call-ID"));
 assert(header_value(replies[0],"CSeq")=="4 OPTIONS");
 assert(s.call_id_==id&&s.local_tag_==local&&s.remote_tag_==remote&&s.remote_target_uri_==target);
 assert(s.remote_ip_v4_==ip&&s.remote_sip_port_==port&&s.tcp_flow_.socket==fd&&s.rtp_socket_==rtp);
 assert(s.delivered.size()==events&&(s.selected_tx_format_==tx)&&(s.selected_rx_format_==rx)&&s.negotiated_rx_payloads_==maps);
 if(accept=="unique"&&state!="busy"){
  auto next=query;auto at=next.find("CSeq: 4 OPTIONS");next.replace(at,15,"CSeq: 5 OPTIONS");
  peer.send_to(s,next);auto second=peer.drain();assert(second.size()==1);
  auto origin=[](const std::string &m){const auto at=m.find("o=");return m.substr(at,m.find("\r\n",at)-at);};
  assert(origin(replies[0])!=origin(second[0])&&"RFC 3264 capability session IDs must be unique");
 }
 assert(s.sdp_session_id_==session&&s.sdp_session_version_==version);
 std::cout<<replies[0];s.close_tcp_flow_(s.tcp_flow_);close(s.sip_socket_);
}
'''


@pytest.fixture(scope="module")
def options_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("options-ua")
    cpp = folder / "options.cpp"
    cpp.write_text(options_source())
    exe = folder / "options"
    result = subprocess.run(["g++", "-std=c++17", "-g", "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-I", str(ROOT), str(cpp), "-o", str(exe)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return exe


@pytest.mark.parametrize("transport", ["udp", "tcp"])
@pytest.mark.parametrize("state", ["idle", "dialog", "busy"])
@pytest.mark.parametrize("accept", ["sdp", "known", "absent", "wildcard", "plain", "empty", "zero", "unique", "unrepresentable"])
def test_options_body_and_admission_preserve_owner(options_binary, transport, state, accept):
    result = subprocess.run([options_binary, transport, state, accept], capture_output=True, timeout=6)
    assert result.returncode == 0, result.stderr.decode()
    head, body = result.stdout.split(b"\r\n\r\n", 1)
    lines = head.decode().split("\r\n")
    expected = 486 if state == "busy" else 200
    assert lines[0].startswith(f"SIP/2.0 {expected} ")
    headers = dict(line.split(": ", 1) for line in lines[1:] if ": " in line)
    assert int(headers["Content-Length"]) == len(body)
    if state == "busy" or accept in {"plain", "empty", "zero", "unrepresentable"}:
        assert not body
        return
    assert headers["Content-Type"] == "application/sdp"
    fields = body.decode().split("\r\n")
    audio = next(line.split() for line in fields if line.startswith("m=audio "))
    assert audio[1] == "0"
    maps = {line.split()[1] for line in fields if line.startswith("a=rtpmap:")}
    assert maps == {"L16/16000/1", "L16/48000/1"}
    assert sum(line.startswith("a=x-voip-stack-flow:") for line in fields) == 2
    assert "a=x-voip-stack-independent-ptime:1" in fields
    assert "a=x-voip-stack-audio-tx:L16/16000/1/20,L16/16000/1/10" in fields
    assert ("a=x-voip-stack-audio-rx:L16/48000/1/10,L16/16000/1/" +
            ("10" if accept == "known" else "20,L16/16000/1/10")) in fields
    hints = [line for line in fields if line.startswith(("a=ptime:", "a=maxptime:"))]
    assert hints == ["a=ptime:10", "a=maxptime:10"]
