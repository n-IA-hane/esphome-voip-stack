"""Direction-only re-INVITE uses the real SDP and dialog/socket response code."""
from pathlib import Path
import subprocess

import pytest
from test_refer_dialog_runtime import SOURCE, ROOT, block
from test_refer_wire_runtime import wire_source


def hold_source():
    code = wire_source()
    parser = (SOURCE / "sip_message.cpp").read_text()
    sdp = (SOURCE / "sip_sdp.cpp").read_text()
    transport = (SOURCE / "sip_transport.cpp").read_text()
    response = (SOURCE / "sip_request_response.cpp").read_text()
    extra = ""
    for marker in ("bool parse_rtpmap_format(", "bool parse_audio_media_line("):
        extra += block(parser, marker) + "\n"
    extra += block(transport, "class ScopedMediaProposal {") + ";\n"
    extra += "unsigned encoder_calls=0;size_t pcm_to_rtp_payload(const uint8_t*in,size_t size,const AudioFormat&,uint8_t*out,size_t capacity){++encoder_calls;assert(size<=capacity);memcpy(out,in,size);return size;}\n"
    code = code.replace("struct SipPhoneTransport {", extra + "\nstruct SipPhoneTransport {")
    declarations = ""
    for old, marker, source in (
        ("std::string build_sdp_offer_(){", "std::string SipTransport::build_sdp_offer_(", sdp),
        ("bool learn_remote_rtp_from_sdp_(", "bool SipTransport::learn_remote_rtp_from_sdp_(", sdp),
        ("void set_media_config_(", "void SipTransport::set_media_config_(", transport),
        ("bool send_response_(", "bool SipTransport::send_response_(", response),
    ):
        body = block(source, marker)
        declaration = body[:body.index("{")].replace("SipTransport::", "").strip()
        declaration = declaration.replace("bool remote_is_answer, bool direction_only)", "bool remote_is_answer = false, bool direction_only = false)")
        if "send_response_" in marker:
            declaration = 'bool send_response_(uint16_t,const char*,const std::string & = "",const std::string & = "")'
        code = code.replace(block(code, old), declaration + ";", 1) + "\n" + body
    for marker, source in (
        ("std::string SipTransport::wrap_sdp_envelope_(", sdp),
        ("void SipTransport::capture_remote_media_shape_(", sdp),
        ("std::string SipTransport::rejected_media_answer_(", sdp),
        ("std::string SipTransport::build_sdp_answer_(", sdp),
        ("bool SipTransport::handle_reinvite_(", transport),
        ("bool SipTransport::received_audio_format_(", transport),
        ("void SipTransport::send_audio_frame(", transport),
    ):
        body = block(source, marker)
        declarations += body[:body.index("{")].replace("SipTransport::", "").strip() + ";\n"
        code += "\n" + body
    declarations = declarations.replace("const char *direction) const;", 'const char *direction = "sendrecv") const;')
    code = code.replace(" uint16_t sip_port_=5060;", declarations + r'''
 AudioFormatList offer_tx_formats_,offer_rx_formats_;int media_config_lock_=0;
 struct PreparedConverter {unsigned prepares=0;bool prepare_additional(const RtpAudioPayloads&,const AudioFormat&){++prepares;return true;}}pcm_receive_converter_;
 void defer_udp_provisional_response_(const std::string&,uint32_t,uint16_t){assert(false);}
 uint16_t rtp_port_=40000;size_t udp_max_payload_=1200;
 std::atomic<bool>rtp_running_{true};int rtp_socket_=-1;Mutex rtp_socket_mutex_;
 std::atomic<uint16_t>rtp_sequence_{0};std::atomic<uint32_t>rtp_timestamp_{0},rtp_tx_packets_{0},rtp_tx_bytes_{0};uint32_t rtp_ssrc_=42;
 uint16_t sip_port_=5060;''', 1)
    code = code.replace("enum class MediaLifecyclePhase {IDLE}", "enum class MediaLifecyclePhase {IDLE,ACTIVE,CLEANING,SHUTTING_DOWN}")
    code = code.replace("void local_ip_for_peer_(uint32_t,std::string*out){", "void local_ip_for_peer_(uint32_t,std::string*out)const{")
    code = code.replace("RtpAudioPayloads*p=nullptr){", "RtpAudioPayloads*p=nullptr)const{")
    return '#define USE_ESPHOME_VOIP_STACK_SPEAKER\n#define portENTER_CRITICAL(...) ((void)0)\n#define portEXIT_CRITICAL(...) ((void)0)\n' + code


@pytest.fixture(scope="module")
def hold_binary(tmp_path_factory):
    path = tmp_path_factory.mktemp("audio-hold")
    helpers = (Path(__file__).parent / "fixtures/refer_wire_main.cpp").read_text().split("void auth_case(")[0]
    main = r'''
AudioFormat pcm(){AudioFormat f;f.frame_ms=20;return f;}
std::string sdp(const std::string&direction,const std::string&ip="127.0.0.1",unsigned pt=96,unsigned rate=16000,unsigned port=41000){
 return "v=0\r\nc=IN IP4 "+ip+"\r\nm=audio "+std::to_string(port)+" RTP/AVP "+std::to_string(pt)+"\r\na=rtpmap:"+std::to_string(pt)+" L16/"+std::to_string(rate)+"/1\r\na=ptime:20\r\na="+direction+"\r\n";
}
int main(int argc,char**argv){
 const bool tcp=std::string(argv[1])=="tcp";const std::string scenario=argv[2];
 SipTransport t;Peer peer(tcp);setup(t,peer);t.media_lifecycle_phase_=SipTransport::MediaLifecyclePhase::ACTIVE;
 t.offer_tx_formats_.count=t.offer_rx_formats_.count=1;t.offer_tx_formats_.formats[0]=t.offer_rx_formats_.formats[0]=pcm();
 RtpAudioPayloads payloads;payloads.add(96,pcm());t.set_media_config_(pcm(),pcm(),96,96,&payloads);
 t.remote_rtp_ip_v4_=INADDR_LOOPBACK;t.remote_rtp_port_=41000;
 const auto call=t.call_id_;const auto before=t.selected_tx_format_;unsigned sequence=10;
 auto offer=[&](const std::string&direction,const std::string&ip="127.0.0.1",unsigned pt=96,unsigned rate=16000,unsigned port=41000){
  auto req=request(peer,"INVITE",sequence++,"",t.call_id_,t.local_tag_,t.remote_tag_);
  auto body=sdp(direction,ip,pt,rate,port);auto at=req.find("Content-Length: 0");
  req.replace(at,17,"Content-Type: application/sdp\r\nContent-Length: "+std::to_string(body.size()));req+=body;
  assert(t.handle_reinvite_(req,peer.address));auto replies=peer.drain();assert(replies.size()==1);return replies[0];
 };
 auto ack=[&](bool negative=false){auto req=request(peer,"ACK",sequence-1,"",t.call_id_,t.local_tag_,t.remote_tag_);if(negative)req.replace(req.find("z9hG4bKACK"),std::string("z9hG4bKACK").size(),"z9hG4bKINVITE");peer.send_to(t,req);assert(peer.drain().empty());};
 if(scenario=="inactive"){
  SipTransport initial;assert(!initial.learn_remote_rtp_from_sdp_(sdp("inactive"),INADDR_LOOPBACK));
 }
 if(scenario=="tx-gate"){
  Peer receiver(false);t.rtp_socket_=socket(AF_INET,SOCK_DGRAM,0);assert(t.rtp_socket_>=0);
  t.remote_rtp_port_=ntohs(receiver.address.sin_port);uint8_t samples[640]{};
  t.set_audio_direction_(2);t.send_audio_frame(samples,sizeof(samples));
  assert(encoder_calls==0&&t.rtp_sequence_==0&&t.rtp_timestamp_==320&&receiver.drain().empty());
  t.set_audio_direction_(3);t.send_audio_frame(samples,sizeof(samples));
  assert(encoder_calls==1&&t.rtp_sequence_==1&&t.rtp_timestamp_==640);char packet[1600];assert(recv(receiver.fd,packet,sizeof(packet),MSG_DONTWAIT)==652);close(t.rtp_socket_);
 }else if(scenario=="mutation"){
  for(auto which:{0,1,2,3}){auto reply=offer("sendonly",which==0?"192.0.2.5":"127.0.0.1",which==1?97:96,which==2?48000:16000,which==3?42000:41000);if(reply.rfind("SIP/2.0 488",0)!=0)fprintf(stderr,"mutation=%d reply=%s\n",which,reply.c_str());assert(reply.rfind("SIP/2.0 488",0)==0);ack(true);assert(t.get_audio_direction_()==3);}
 }else{
  const std::string held=scenario=="inactive"?"inactive":scenario=="recvonly"?"recvonly":"sendonly";
  auto reply=offer(held);assert(reply.rfind("SIP/2.0 200",0)==0);
  const auto answer=held=="sendonly"?"recvonly":held=="recvonly"?"sendonly":"inactive";
  assert(reply.find("a="+std::string(answer)+"\r\n")!=std::string::npos);ack();
  assert(t.selected_tx_format_==before&&t.selected_rx_format_==before&&t.negotiated_rx_payloads_==payloads);
  assert(!t.remote_audio_expected());AudioFormat format;
  assert(t.received_audio_format_(96,&format)==(held=="sendonly"));
  clock_ms+=20000;assert(!t.remote_audio_expected());
  reply=offer("sendrecv");assert(reply.rfind("SIP/2.0 200",0)==0);ack();
  assert(t.get_audio_direction_()==3&&t.remote_audio_expected());assert(t.selected_tx_format_==before&&t.selected_rx_format_==before);
 }
 assert(t.call_id_==call&&t.media_active_&&t.media_lifecycle_phase_==SipTransport::MediaLifecyclePhase::ACTIVE);
 assert(t.pcm_receive_converter_.prepares==0&&t.rtp_running_);
 t.close_tcp_flow_(t.tcp_flow_);close(t.sip_socket_);
}
'''
    cpp = path / "hold.cpp"
    cpp.write_text(hold_source() + "\n" + helpers + "\n" + main)
    binary = path / "hold"
    result = subprocess.run(["g++", "-std=c++17", "-g0", "-fsanitize=address,undefined", "-I", str(ROOT), str(cpp), "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("transport", ["udp", "tcp"])
@pytest.mark.parametrize("scenario", ["sendonly", "recvonly", "inactive", "mutation", "tx-gate"])
def test_existing_call_accepts_only_direction_changes(hold_binary, transport, scenario):
    result = subprocess.run([hold_binary, transport, scenario], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr
