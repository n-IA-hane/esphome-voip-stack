"""Actual SDP selection follows explicit direction preferences."""
import subprocess

from test_sdp_receive_payloads import ROOT, sdp_fixture_source


def test_plain_pcm_negotiates_independent_rates_and_opus_keeps_local_rates(tmp_path):
    source = sdp_fixture_source() + r'''
AudioFormat pcm(unsigned rate) {AudioFormat f;f.sample_rate=rate;f.frame_ms=10;return f;}
std::string remote(unsigned rate,bool d1){
 return std::string("v=0\r\nc=IN IP4 192.0.2.2\r\nm=audio 40002 RTP/AVP 96 97\r\n")+
  "a=rtpmap:96 L16/48000/1\r\na=rtpmap:97 L16/"+std::to_string(rate)+"/1\r\na=ptime:10\r\n"+
  (d1?"a=x-voip-stack-flow:96 sendrecv\r\na=x-voip-stack-flow:97 sendrecv\r\n":"");
}
int main(){
 for(unsigned rate:{16000,24000,32000}) {
  SipTransport s;s.offer_tx_formats_.count=1;s.offer_tx_formats_.formats[0]=pcm(rate);
  s.offer_rx_formats_.count=2;s.offer_rx_formats_.formats[0]=pcm(48000);s.offer_rx_formats_.formats[1]=pcm(rate);
  assert(s.learn_remote_rtp_from_sdp_(remote(rate,false),0));
  assert(s.tx.sample_rate==rate && s.rx.sample_rate==48000 && s.rx_pt!=s.tx_pt);
  assert(s.learn_remote_rtp_from_sdp_(remote(rate,true),0));
  assert(s.tx.sample_rate==rate && s.rx.sample_rate==48000);
 }
 SipTransport none;none.offer_tx_formats_.count=none.offer_rx_formats_.count=1;
 none.offer_tx_formats_.formats[0]=pcm(16000);none.offer_rx_formats_.formats[0]=pcm(48000);
 assert(!none.learn_remote_rtp_from_sdp_(remote(16000,false),0));
 SipTransport opus;auto tx=pcm(16000),rx=pcm(48000);
 tx.codec=rx.codec=AudioCodec::OPUS;tx.frame_ms=rx.frame_ms=20;
 opus.offer_tx_formats_.count=opus.offer_rx_formats_.count=1;
 opus.offer_tx_formats_.formats[0]=tx;opus.offer_rx_formats_.formats[0]=rx;
 assert(opus.learn_remote_rtp_from_sdp_("v=0\r\nc=IN IP4 192.0.2.2\r\nm=audio 40002 RTP/AVP 96\r\na=rtpmap:96 opus/48000/2\r\na=ptime:20\r\n",0));
 assert(opus.tx.sample_rate==16000 && opus.rx.sample_rate==48000);
}
'''
    cpp = tmp_path / "sdp.cpp"
    cpp.write_text(source)
    binary = tmp_path / "sdp"
    subprocess.run(["g++", "-std=c++17", "-I", str(ROOT), str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
