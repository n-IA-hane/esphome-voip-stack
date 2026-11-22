"""The production SDP owner separates a call's receive intent from camera privacy."""

from test_imageattr_runtime import sdp_source, compile_run


def test_two_queued_native_commands_cannot_replace_the_admitted_video_intent(tmp_path):
    import subprocess
    from test_correlated_call_behavior import correlated_fixture_source, consumer_root, write_consumer_includes

    source = tmp_path / "queued_video_intent.cpp"
    source.write_text("#include <vector>\n" + correlated_fixture_source() + r'''
int main(){
 for(bool send:{false,true})for(bool receive:{false,true}){
  VoipStack phone;phone.sink_ready=false;
  phone.transport.value.remote_ip_v4=0xc0000214;phone.transport.value.remote_sip_port=5060;
  std::vector<std::function<void()>> native_queue;
  native_queue.emplace_back([&](){phone.start_call_on_line("Desk","direct",send,receive);});
  native_queue.emplace_back([&](){phone.start_call_on_line("Other","direct",!send,!receive);});
  for(auto &command:native_queue)command();
  assert(phone.call_state_==CallState::CALLING&&phone.invites==0);
  assert(phone.pending_media_start_==VoipStack::ApplicationMediaStart::ORIGINATE);
  assert(phone.identity.dest_route=="Desk");
  assert(phone.transport.video_intent.specified&&phone.transport.video_intent.send==send&&
         phone.transport.video_intent.receive==receive);
  phone.sink_ready=true;phone.pump_application_media_start_();
  assert(phone.invites==1&&phone.transport.originations==1&&phone.identity.dest_route=="Desk");
  assert(phone.transport.video_intent.send==send&&phone.transport.video_intent.receive==receive);
 }
}
''')
    binary = tmp_path / "queued_video_intent"
    write_consumer_includes(tmp_path)
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-DUSE_ESP32",
        "-DUSE_ESPHOME_VOIP_STACK_VIDEO", "-DUSE_ESPHOME_VOIP_STACK_SPEAKER",
        "-I", str(tmp_path), "-I", str(consumer_root()),
        "-I", str(consumer_root() / "esphome/components/voip_stack"),
        str(source), "-o", str(binary)], check=True, capture_output=True, text=True)
    subprocess.run([str(binary)], check=True)


def test_explicit_audio_receive_send_and_bidirectional_calls_are_truthful(tmp_path):
    source = sdp_source() + r'''
int main(){
 const std::string audio="v=0\r\nm=audio 40000 RTP/AVP 96\r\na=rtpmap:96 L16/16000/1\r\n";
 for(bool send:{false,true})for(bool receive:{false,true}){
  SipTransport call;call.video_source_=&call;call.video_send_requested_=true;
  call.video_call_intent_={true,send,receive};
  const auto offer=call.append_video_sdp_(audio,"192.0.2.1",false);
  assert(call.video_send_requested_&&"A call request must not change the native camera preference");
  assert(offer.substr(0,audio.size())==audio);
  if(!send&&!receive){assert(offer==audio);continue;}
  assert(offer.find("m=video 40002 RTP/AVP 26\r\n")!=std::string::npos);
  const std::string direction=send?(receive?"sendrecv":"sendonly"):"recvonly";
  assert(offer.find("a="+direction+"\r\n")!=std::string::npos);
  assert(offer.find("a=x-")==std::string::npos);
 }
 SipTransport private_camera;private_camera.video_source_=&private_camera;
 private_camera.video_call_intent_={true,true,true};private_camera.video_send_requested_=false;
 const auto receive=private_camera.append_video_sdp_(audio,"192.0.2.1",false);
 assert(receive.find("a=recvonly\r\n")!=std::string::npos);
 assert(!private_camera.video_send_requested_);
 SipTransport no_camera;no_camera.video_call_intent_={true,true,false};
 assert(no_camera.append_video_sdp_(audio,"192.0.2.1",false)==audio);
 SipTransport native;native.video_source_=&native;native.video_send_requested_=true;
 assert(native.append_video_sdp_(audio,"192.0.2.1",false).find("a=sendrecv\r\n")!=std::string::npos);
 native.video_call_intent_={true,false,false};
 assert(native.append_video_sdp_(audio,"192.0.2.1",false)==audio);
 const auto capabilities=native.append_video_sdp_("","192.0.2.1",false,true);
 assert(capabilities.find("m=video 0 RTP/AVP 26\r\n")!=std::string::npos);
}
'''
    compile_run(tmp_path, source)
