"""The firmware answers every offered PCM shape and packet time within its limits.

The phone transmits 16 kHz and prefers to receive 48 kHz. A peer may still
offer any rate, any packet time and any maxptime, alone or combined. Each
combination must be either refused or accepted inside the transmit budget,
the peer's maxptime and the phone's own format lists.
"""
import subprocess

from test_sdp_receive_payloads import ROOT, sdp_fixture_source

MATRIX = r'''
#include <cstdio>
#include <vector>
AudioFormat pcm(unsigned rate, unsigned frame_ms) {AudioFormat f;f.sample_rate=rate;f.frame_ms=frame_ms;return f;}
struct Shape {int pt; const char *map; unsigned rate; bool supported;};
static const std::vector<Shape> SHAPES = {
 {96,"L16/48000/1",48000,true},{97,"L16/16000/1",16000,true},{98,"L16/32000/1",32000,true},
 {99,"L16/8000/1",8000,true},{8,"PCMA/8000",8000,false},{0,"PCMU/8000",8000,false},
};
std::string remote(const std::vector<int>&picks,int ptime,int maxptime){
 std::string sdp="v=0\r\nc=IN IP4 192.0.2.2\r\nm=audio 40002 RTP/AVP";
 for(int i:picks)sdp+=" "+std::to_string(SHAPES[i].pt);
 sdp+="\r\n";
 for(int i:picks)sdp+="a=rtpmap:"+std::to_string(SHAPES[i].pt)+" "+SHAPES[i].map+"\r\n";
 if(ptime)sdp+="a=ptime:"+std::to_string(ptime)+"\r\n";
 if(maxptime)sdp+="a=maxptime:"+std::to_string(maxptime)+"\r\n";
 return sdp;
}
int main(){
 const std::vector<std::vector<int>> offers={{0},{1},{2},{3},{4},{5},{0,1},{1,0},{1,3},{0,2},{4,5},{1,4},{0,1,4,5},{3,1,0}};
 unsigned accepted=0,refused=0,violations=0;
 for(const auto&picks:offers)for(int ptime:{0,5,10,16,20,30,40,60})for(int maxptime:{0,10,20,60}){
  if(ptime&&maxptime&&ptime>maxptime)continue;
  SipTransport s;
  s.offer_tx_formats_.count=2;s.offer_tx_formats_.formats[0]=pcm(16000,20);s.offer_tx_formats_.formats[1]=pcm(16000,10);
  s.offer_rx_formats_.count=3;s.offer_rx_formats_.formats[0]=pcm(48000,10);
  s.offer_rx_formats_.formats[1]=pcm(16000,20);s.offer_rx_formats_.formats[2]=pcm(16000,10);
  const std::string sdp=remote(picks,ptime,maxptime);
  bool usable=false;for(int i:picks)usable=usable||(SHAPES[i].supported&&SHAPES[i].rate==16000);
  std::string label;for(int i:picks)label+=std::string(label.empty()?"":"+")+SHAPES[i].map;
  label+=" ptime="+std::to_string(ptime)+" maxptime="+std::to_string(maxptime);
  auto fail=[&](const std::string&why){++violations;std::printf("VIOLATION %s: %s\n",label.c_str(),why.c_str());};
  if(!s.learn_remote_rtp_from_sdp_(sdp,0)){
   ++refused;
   // The phone can always exchange 16 kHz in both directions when it is offered.
   if(usable&&(!maxptime||maxptime>=10))fail("refused although 16 kHz PCM was offered");
   continue;
  }
  ++accepted;
  bool tx_offered=false,rx_offered=false;
  for(int i:picks){tx_offered=tx_offered||SHAPES[i].pt==s.tx_pt;rx_offered=rx_offered||SHAPES[i].pt==s.rx_pt;}
  if(!tx_offered||!rx_offered)fail("selected a payload type outside the offer");
  if(s.tx.sample_rate!=16000)fail("transmit rate "+std::to_string(s.tx.sample_rate)+" is not a local transmit rate");
  if(s.rx.sample_rate!=48000&&s.rx.sample_rate!=16000)fail("receive rate "+std::to_string(s.rx.sample_rate)+" is not a local receive rate");
  const unsigned bytes=s.tx.sample_rate*s.tx.frame_ms/1000*2;
  if(bytes>1200)fail("transmit packet of "+std::to_string(bytes)+" bytes");
  if(maxptime&&s.tx.frame_ms>(unsigned)maxptime)fail("transmit packet time "+std::to_string(s.tx.frame_ms)+" above maxptime");
  if(s.tx.frame_ms==0||s.rx.frame_ms==0)fail("zero packet time");
 }
 std::printf("accepted=%u refused=%u violations=%u\n",accepted,refused,violations);
 return 0;
}
'''


def test_every_pcm_offer_is_refused_or_answered_within_limits(tmp_path):
    cpp = tmp_path / "matrix.cpp"
    cpp.write_text(sdp_fixture_source() + MATRIX)
    binary = tmp_path / "matrix"
    built = subprocess.run(["g++", "-std=c++17", "-g", "-fsanitize=address,undefined", "-I", str(ROOT),
                            str(cpp), "-o", str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
    assert ran.returncode == 0, ran.stdout[-2000:] + ran.stderr[-2000:]
    summary = ran.stdout.strip().splitlines()[-1]
    assert "violations=0" in summary, ran.stdout[-4000:]
    accepted = int(summary.split("accepted=")[1].split()[0])
    assert accepted > 150, summary
