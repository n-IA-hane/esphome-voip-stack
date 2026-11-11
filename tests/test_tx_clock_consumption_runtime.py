"""The actual mic read charges sampling time once, even during SDP proposals."""

import subprocess

import pytest

from test_audio_hold_runtime import hold_source
from test_refer_dialog_runtime import ROOT, SOURCE, block


@pytest.fixture(scope="module")
def consumed_clock(tmp_path_factory):
    folder = tmp_path_factory.mktemp("consumed-clock")
    core = folder / "esphome/core"
    core.mkdir(parents=True)
    (core / "defines.h").write_text("")
    source = hold_source().split("\nint main(", 1)[0]
    producer = (SOURCE / "voip_audio.cpp").read_text()
    start = producer.index("    const auto consumed_context =")
    end = producer.index("this->send_chunk_(audio_chunk, frame_bytes, consumed_context);", start)
    consumed = producer[start:end + len("this->send_chunk_(audio_chunk, frame_bytes, consumed_context);")]
    source += r'''
struct MicBuffer {
 std::vector<uint8_t>pcm;std::function<void()>on_read;
 size_t read(uint8_t*out,size_t n,unsigned){
  if(pcm.size()<n)return 0;std::memcpy(out,pcm.data(),n);pcm.erase(pcm.begin(),pcm.begin()+n);
  if(on_read)on_read();return n;
 }
};
struct VoipStack {
 SipTransport*transport_;MicBuffer*mic_buffer_;AudioFormat active;
 bool is_tx_stream_ready_()const{return transport_->rtp_running_&&active.is_valid();}
 size_t tx_audio_chunk_bytes_()const{return active.nominal_frame_bytes();}
 void send_chunk_(const uint8_t*,size_t,const TransportAudioTxContext&);
 bool read_tx_chunk_(uint8_t*);
 void consume(){uint8_t audio_chunk[4096];const auto tx_format=active;const auto frame_bytes=active.nominal_frame_bytes();
  for(unsigned once=0;once<1;++once){ CONSUMED }
 }
};
'''.replace("CONSUMED", consumed)
    source += "\n" + block(producer, "void VoipStack::send_chunk_(const uint8_t *data, size_t length, const TransportAudioTxContext &context)")
    source += "\n" + block(producer, "bool VoipStack::read_tx_chunk_(")
    source += r'''
uint32_t timestamp(const uint8_t*p){return uint32_t(p[4])<<24|uint32_t(p[5])<<16|uint32_t(p[6])<<8|p[7];}
int main(int argc,char**argv){
 assert(argc==2);const std::string mode=argv[1];SipTransport t;
 AudioFormat pcm;pcm.sample_rate=mode=="opus"?48000:16000;pcm.frame_ms=10;
 if(mode=="g722")pcm.codec=AudioCodec::G722;if(mode=="opus")pcm.codec=AudioCodec::OPUS;
 t.tx=pcm;t.tx_pt=96;t.remote_rtp_ip_v4_=INADDR_LOOPBACK;
 int sink=socket(AF_INET,SOCK_DGRAM,0);assert(sink>=0);sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 assert(bind(sink,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);socklen_t size=sizeof(address);
 assert(getsockname(sink,reinterpret_cast<sockaddr*>(&address),&size)==0);t.remote_rtp_port_=ntohs(address.sin_port);t.rtp_socket_=socket(AF_INET,SOCK_DGRAM,0);
 MicBuffer buffer;VoipStack owner{&t,&buffer,pcm};uint8_t packet[4096];
 auto fill=[&](){buffer.pcm.resize(pcm.nominal_frame_bytes(),7);};
 auto none=[&](){assert(recv(sink,packet,sizeof(packet),MSG_DONTWAIT)<0);};
 const auto before=t.rtp_timestamp_.load();const auto sequence=t.rtp_sequence_.load();
 auto context=t.audio_tx_frame_context(pcm);assert(context.valid);
 owner.consume();assert(t.rtp_timestamp_==before); // No frame was read.
 if(mode=="invalid"){
  t.send_audio_frame(nullptr,pcm.nominal_frame_bytes(),context);t.send_audio_frame(packet,0,context);
  t.send_audio_frame(packet,pcm.nominal_frame_bytes()-1,context);context.valid=false;
  t.send_audio_frame(packet,pcm.nominal_frame_bytes(),context);assert(t.rtp_timestamp_==before&&t.rtp_sequence_==sequence);none();
 }else if(mode=="new-session"){
  fill();buffer.on_read=[&](){++t.audio_tx_clock_generation_;++t.rtp_ssrc_;t.rtp_timestamp_=42;};
  owner.consume();assert(buffer.pcm.empty()&&t.rtp_timestamp_==42&&t.rtp_sequence_==sequence);none();
 }else if(mode=="new-clock"){
  fill();buffer.on_read=[&](){t.tx.sample_rate=48000;};owner.consume();
  assert(buffer.pcm.empty()&&t.rtp_timestamp_==before&&t.rtp_sequence_==sequence);none();
 }else{
  // The source contract is captured before the real read. The SDP proposal
  // advertises a different temporary geometry while this exact frame is consumed.
  fill();buffer.on_read=[&](){t.media_proposal_epoch_=1;t.tx.sample_rate=32000;t.tx.frame_ms=20;};
  owner.consume();const uint32_t ticks=mode=="g722"?80:mode=="opus"?480:160;
  assert(buffer.pcm.empty()&&t.rtp_timestamp_==before+ticks&&t.rtp_sequence_==sequence);none();
  buffer.on_read=nullptr;fill();owner.consume();
  assert(buffer.pcm.empty()&&t.rtp_timestamp_==before+2*ticks&&t.rtp_sequence_==sequence);none();
  if(mode=="pcm"){
   t.tx=pcm;t.media_proposal_epoch_=2;fill();owner.consume();
   const auto bytes=recv(sink,packet,sizeof(packet),MSG_DONTWAIT);assert(bytes==12+int(pcm.nominal_frame_bytes()));
   assert(timestamp(packet)==before+2*ticks&&t.rtp_timestamp_==before+3*ticks);
   assert((uint16_t(packet[2])<<8|packet[3])==sequence&&t.rtp_sequence_==sequence+1);
  }
 }
 close(sink);close(t.rtp_socket_);
}
'''
    cpp = folder / "consumed.cpp"
    cpp.write_text(source)
    binary = folder / "consumed"
    built = subprocess.run(["g++", "-std=c++17", "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
        "-I", str(folder), "-I", str(ROOT), "-I", str(SOURCE), str(cpp), "-o", str(binary)],
        capture_output=True, text=True, timeout=45)
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize("mode", ["pcm", "g722", "opus", "invalid", "new-session", "new-clock"])
def test_consumed_frame_uses_its_retained_clock_without_sending_during_proposal(consumed_clock, mode):
    result = subprocess.run([str(consumed_clock), mode], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr
