#include "media_negotiator.h"
#include "sip_sdp_audio_offer.h"
#include <cassert>
using namespace esphome::voip_stack;

static AudioFormat pcm(unsigned rate,unsigned time){AudioFormat f;f.sample_rate=rate;f.frame_ms=time;return f;}
static AudioFormatList list(std::initializer_list<AudioFormat> values){AudioFormatList out;out.count=0;for(auto value:values)out.formats[out.count++]=value;return out;}
static std::string narrow(unsigned port,unsigned time,unsigned maximum,bool include_wide=false){
  auto body="v=0\r\nc=IN IP4 192.0.2.20\r\nm=audio "+std::to_string(port)+" RTP/AVP 97"+(include_wide?" 96":"")+
      "\r\na=rtpmap:97 L16/8000/1\r\n"+(include_wide?"a=rtpmap:96 L16/32000/1\r\n":"");
  if(time)body+="a=ptime:"+std::to_string(time)+"\r\n";
  if(maximum)body+="a=maxptime:"+std::to_string(maximum)+"\r\n";
  return body+"a=sendrecv\r\n";
}
static bool equal(const AudioFormatList &a,const AudioFormatList &b){if(a.count!=b.count)return false;for(unsigned i=0;i<a.count;++i)if(!(a.formats[i]==b.formats[i]))return false;return true;}
int main(int argc,char **argv){
  assert(argc==2);const std::string mode=argv[1];
  const auto tx=list({pcm(8000,20),pcm(8000,10),pcm(8000,32),pcm(32000,10)});
  auto rx=list({pcm(32000,10),pcm(8000,32),pcm(8000,20),pcm(8000,10)});
  AudioFormatList offered_tx;RtpAudioPayloads offered_rx;
  if(mode=="bounded-fallback"||mode=="absent-maxptime"||mode=="atomic-failure"){
    rx=list({pcm(32000,10),pcm(8000,32),pcm(8000,20)});
    if(mode=="atomic-failure"){
      offered_tx=list({pcm(8000,10)});assert(offered_rx.add(111,pcm(8000,10)));
      const auto before_tx=offered_tx;const auto before_rx=offered_rx;
      assert(!capture_audio_offer(narrow(40000,10,10),tx,rx,1200,&offered_tx,&offered_rx));
      assert(equal(offered_tx,before_tx)&&offered_rx==before_rx);return 0;
    }
    assert(capture_audio_offer(narrow(40000,10,mode=="bounded-fallback"?20:0),tx,rx,1200,&offered_tx,&offered_rx));
    AudioFormat captured;assert(offered_rx.find(97,&captured));assert(captured.frame_ms==(mode=="bounded-fallback"?20:32));
    assert(audio_format_list_contains(offered_tx,pcm(8000,32)));return 0;  // Our RX bound must not constrain TX.
  }
  rx=list({pcm(32000,10),pcm(8000,20),pcm(8000,10)});
  assert(capture_audio_offer(narrow(40000,10,20,true),tx,rx,1200,&offered_tx,&offered_rx));
  LocalAudioOffer local{tx,rx,offered_tx,offered_rx,1200};SdpAudioMedia empty,retained,next;
  assert(negotiate_audio_contract(narrow(48080,20,0),0xc0000214,true,false,local,empty,retained));
  assert(retained.tx==pcm(8000,20)&&retained.rx==pcm(8000,10));
  if(mode=="explicit-local-offer"){
    assert(capture_audio_offer(narrow(40000,20,20),tx,rx,1200,&offered_tx,&offered_rx));
    assert(negotiate_audio_contract(narrow(48082,20,0),0xc0000214,true,true,local,retained,next));
    assert(next.rx==pcm(8000,20));return 0;  // A newly sent offer owns its new policy, without blind retention.
  }
  if(mode=="remote-maxptime"){
    assert(negotiate_audio_contract(narrow(48082,20,10),0xc0000214,false,true,local,retained,next));
    assert(next.tx==pcm(8000,10)&&next.rx==pcm(8000,10));return 0;
  }
  if(mode=="retired-local-rx"){
    rx=list({pcm(32000,10),pcm(8000,20)});
    assert(negotiate_audio_contract(narrow(48082,20,0),0xc0000214,false,true,local,retained,next));
    assert(next.rx==pcm(8000,20));return 0;
  }
  assert(mode=="retained-complete-contract");
  assert(negotiate_audio_contract(narrow(48082,20,0,true),0xc0000214,false,true,local,retained,next));
  assert(next.rx==retained.rx&&next.rx_pt==retained.rx_pt);
  assert(next.rx_payloads.count==2&&next.rx_payloads.find(96,nullptr)&&!(next.rx_payloads==retained.rx_payloads));
  // The transport's unchanged prepared-table gate, not the parser, rejects the added resource.
}
