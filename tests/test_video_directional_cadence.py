"""Encoding recommendation cannot throttle the opposite local video direction."""
import json
import subprocess

import pytest

from test_imageattr_runtime import COMPONENT, compile_run, sdp_source
from test_refer_dialog_runtime import block


@pytest.mark.parametrize("tx_fps,rx_fps,peer_fps", [(15, 2, 10), (2, 15, 10), (15, 2, 0), (15, 15, 15)])
@pytest.mark.parametrize("codec", ["jpeg", "h264"])
def test_production_sdp_and_direction_caps_preserve_independent_frame_budgets(tmp_path, tx_fps, rx_fps, peer_fps, codec):
    code = sdp_source().replace(" void reset_video_negotiation_();", " VideoCapability local_video_direction_capability_(const VideoCapability&,bool)const;\n void reset_video_negotiation_();")
    if codec == "h264":
        # The unchanged firmware baseline sets JPEG's rtpmap_seen in H264
        # builds too. This isolated gate keeps other diagnostics as errors.
        code = '#pragma GCC diagnostic ignored "-Wunused-but-set-variable"\n' + code
        code = code.replace("#define USE_ESPHOME_VOIP_STACK_VIDEO_JPEG", "#define USE_ESPHOME_VOIP_STACK_VIDEO_H264")
        message = (COMPONENT / "sip_message.cpp").read_text()
        rtp = (COMPONENT / "video_rtp.cpp").read_text()
        helpers = "\n".join(block(message, signature) for signature in
                            ["std::string fmtp_parameter(", "bool parse_video_rtcp_feedback("])
        helpers += "\nnamespace esphome::voip_stack {\n" + "\n".join(block(rtp, signature) for signature in
            ["uint8_t hex_nibble(", "bool parse_profile_level_id(", "int h264_level_value(",
             "bool h264_same_subprofile(", "bool h264_level_fits("]) + "\n}\n"
        code = code.replace("struct SipTransport {", helpers + "\nstruct SipTransport {")
    code += block((COMPONENT / "sip_transport.cpp").read_text(), "VideoCapability SipTransport::local_video_direction_capability_(") + r'''
#include <iostream>
int main(int argc,char**argv){
 assert(argc==4);SipTransport call;call.video_source_=&call;call.video_send_requested_=true;
 call.tx.max_fps=std::stoi(argv[1]);call.rx.max_fps=std::stoi(argv[2]);const unsigned peer_fps=std::stoi(argv[3]);
 const std::string offer="v=0\r\no=- 1 1 IN IP4 192.0.2.2\r\nc=IN IP4 192.0.2.2\r\nm=video 42002 RTP/AVP 26\r\na=rtpmap:26 JPEG/90000\r\na=sendrecv\r\n"+
    (peer_fps?"a=framerate:"+std::to_string(peer_fps)+"\r\n":"");
 assert(call.learn_remote_video_from_sdp_(offer,0,false));
 const auto tx=call.local_video_direction_capability_(call.negotiated_video_capability_,true);
 const auto rx=call.local_video_direction_capability_(call.negotiated_video_capability_,false);
 const unsigned expected_tx=peer_fps?std::min<unsigned>(call.tx.max_fps,peer_fps):call.tx.max_fps;
 std::cerr<<"tx="<<unsigned(tx.max_fps)<<" expected_tx="<<expected_tx<<" rx="<<unsigned(rx.max_fps)<<" expected_rx="<<unsigned(call.rx.max_fps)<<"\n";
 assert(tx.max_fps==expected_tx&&rx.max_fps==call.rx.max_fps);
 RtpFrameCadence90k cadence;cadence.reset(tx.max_fps);unsigned frames=0;
 for(unsigned n=0;n<250;++n)frames+=cadence.accept(n*1800U); // Actual50fps source timestamps, five seconds.
 assert(frames>=expected_tx*5&&frames<=expected_tx*5+1);
 cadence.accept(90000000U);assert(!cadence.accept(90000001U)); // No unlimited recovery burst.
 std::cout<<call.append_video_sdp_("","192.0.2.1",true);
}
'''
    if codec == "h264":
        code = code.replace("call.tx.max_fps=", 'call.tx.encoding=call.rx.encoding="H264";call.tx.payload_type=call.rx.payload_type=call.video_offer_payload_type_=103;call.tx.profile_level_id=call.rx.profile_level_id="42e01e";call.tx.packetization_mode=call.rx.packetization_mode=1;call.tx.max_fps=')
        code = code.replace("RTP/AVP 26", "RTP/AVP 103").replace("a=rtpmap:26 JPEG/90000", "a=rtpmap:103 H264/90000\\r\\na=fmtp:103 packetization-mode=1;profile-level-id=42e01e;level-asymmetry-allowed=1")
    # Keep the common compiler/sanitizer contract, then independently parse the
    # exact serialized answer instead of asserting a production string builder.
    compile_run(tmp_path, code, str(tx_fps), str(rx_fps), str(peer_fps))
    result = subprocess.run([tmp_path / "imageattr", str(tx_fps), str(rx_fps), str(peer_fps)],
                            capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr
    attrs = dict(line[2:].split(":", 1) for line in result.stdout.splitlines() if line.startswith("a=") and ":" in line)
    assert attrs["framerate"] == str(rx_fps)
    assert "a=sendrecv" in result.stdout
    (tmp_path / "directional-receipt.json").write_text(json.dumps({"tx": tx_fps, "rx": rx_fps,
        "peer_receive_recommendation": peer_fps, "answer": result.stdout, "observed": result.stderr}, indent=2) + "\n")
