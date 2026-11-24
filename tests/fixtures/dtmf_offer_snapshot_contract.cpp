#include "media_negotiator.h"
#include "sip_sdp_audio_offer.h"
#include <cassert>
#include <cstring>
#include <iostream>
using namespace esphome::voip_stack;
static std::string session(const std::string &events, const std::string &maps) {
  return "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=Independent SDP\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio 40000 RTP/AVP 96 97 "+events+
    "\r\na=rtpmap:96 L16/48000/1\r\na=rtpmap:97 L16/16000/1\r\na=ptime:10\r\na=maxptime:20\r\na=sendrecv\r\n"+maps;
}
int main(int argc,char **argv) {
  assert(argc==2);const std::string mode=argv[1];AudioFormatList tx,rx;tx.count=rx.count=2;
  tx.formats[0]={16000,PcmFormat::S16LE,1,20,AudioCodec::PCM};tx.formats[1]={48000,PcmFormat::S16LE,1,10,AudioCodec::PCM};
  rx.formats[0]=tx.formats[1];rx.formats[1]=tx.formats[0];rx.formats[1].frame_ms=10;
  std::string offered="103 104",maps="a=rtpmap:103 telephone-event/48000\r\na=fmtp:103 0-15\r\na=rtpmap:104 telephone-event/16000\r\na=fmtp:104 0-15\r\n";
  std::string answer_events="103 104",answer_maps=maps;unsigned expected_rx=103,expected_tx=104,expected_mask=0xffff;bool invalid_offer=false,missing_rx=false,missing_tx=false;DtmfFormat retained_event;
  if(mode=="canonical-uac"){offered="120 122 126";maps="a=rtpmap:120 telephone-event/8000\r\na=rtpmap:122 telephone-event/16000\r\na=rtpmap:126 telephone-event/48000\r\n";expected_rx=126;}
  if(mode=="renumbered-answer"){answer_events="108 109";answer_maps="a=rtpmap:108 telephone-event/48000\r\na=rtpmap:109 telephone-event/16000\r\n";expected_tx=109;}
  if(mode=="event-intersection"){maps="a=rtpmap:103 telephone-event/48000\r\na=fmtp:103 0-11\r\na=rtpmap:104 telephone-event/16000\r\na=fmtp:104 0-11\r\n";expected_mask=0x0fff;}
  if(mode=="fallback-8000"){offered="109";maps="a=rtpmap:109 telephone-event/8000\r\n";answer_events="107";answer_maps="a=rtpmap:107 telephone-event/8000\r\n";expected_rx=109;expected_tx=107;}
  if(mode=="missing-clock"||mode=="retired-clock"){offered="104";maps="a=rtpmap:104 telephone-event/16000\r\n";missing_rx=true;}
  if(mode=="introduced-clock"){offered="104";maps="a=rtpmap:104 telephone-event/16000\r\n";answer_events="103";answer_maps="a=rtpmap:103 telephone-event/48000\r\n";missing_rx=missing_tx=true;}
  if(mode=="disjoint-events"){maps="a=rtpmap:103 telephone-event/48000\r\na=fmtp:103 0-3\r\na=rtpmap:104 telephone-event/16000\r\na=fmtp:104 0-3\r\n";answer_maps="a=rtpmap:103 telephone-event/48000\r\na=fmtp:103 4-7\r\na=rtpmap:104 telephone-event/16000\r\na=fmtp:104 4-7\r\n";missing_rx=missing_tx=true;}
  if(mode=="duplicate-clock"){offered+=" 106";maps+="a=rtpmap:106 telephone-event/48000\r\n";}
  if(mode=="duplicate-pt"){maps+="a=rtpmap:103 telephone-event/16000\r\n";invalid_offer=true;}
  if(mode=="audio-pt-collision"){offered="96";maps="a=rtpmap:96 telephone-event/48000\r\n";invalid_offer=true;}
  if(mode=="malformed-events"){maps="a=rtpmap:103 telephone-event/48000\r\na=fmtp:103 broken\r\n";offered="103";invalid_offer=true;}
  if(mode=="retained-alias-order"||mode=="retired-alias"){
    tx.count=rx.count=1;rx.formats[0]={16000,PcmFormat::S16LE,1,10,AudioCodec::PCM};
    offered=mode=="retired-alias"?"106":"106 122";maps="a=rtpmap:106 telephone-event/16000\r\na=fmtp:106 0-11\r\n";
    if(mode!="retired-alias")maps+="a=rtpmap:122 telephone-event/16000\r\na=fmtp:122 0-11\r\n";
    answer_events="106";answer_maps="a=rtpmap:106 telephone-event/16000\r\na=fmtp:106 0-11\r\n";expected_tx=106;expected_rx=mode=="retired-alias"?106:122;expected_mask=0x0fff;retained_event={16000,0x0fff,122};
  }
  if(mode=="split-alias-event-masks"){
    tx.count=rx.count=1;rx.formats[0]={16000,PcmFormat::S16LE,1,10,AudioCodec::PCM};
    offered="106 122";maps="a=rtpmap:106 telephone-event/16000\r\na=fmtp:106 0-7\r\na=rtpmap:122 telephone-event/16000\r\na=fmtp:122 8-15\r\n";
    answer_events="108";answer_maps="a=rtpmap:108 telephone-event/16000\r\na=fmtp:108 8-15\r\n";expected_tx=108;expected_rx=122;expected_mask=0xff00;
  }
  const auto offer=session(offered,maps);auto answer=session(answer_events,answer_maps);const auto at=answer.find("a=ptime:10");answer.replace(at,10,"a=ptime:20");
  AudioFormatList offered_tx;RtpAudioPayloads offered_rx;DtmfPayloads events;uint8_t clocks=0x55;
  events.add({8000,0x0001,110});const auto original_events=events;const auto original_tx=offered_tx;const auto original_rx=offered_rx;
  const bool captured=capture_audio_offer(offer,tx,rx,1200,&offered_tx,&offered_rx,&clocks,&events);
  if(invalid_offer){assert(!captured&&clocks==0x55&&std::memcmp(&events,&original_events,sizeof(events))==0&&offered_tx.count==original_tx.count&&offered_rx==original_rx);std::cout<<"atomic_invalid_offer=true\n";return 0;}
  assert(captured);SdpAudioMedia retained,media;retained.tx=retained.rx=disabled_audio_format();DtmfConfig dtmf;
  // An intentionally broad advisory clock mask cannot authorize a mapping
  // absent from the actual local offer value.
  const LocalAudioOffer local{tx,rx,offered_tx,offered_rx,1200,0x7f,RTP_AUDIO_RECEIVE_PAYLOAD_BYTES,&events,&retained_event};
  MediaNegotiationTrace trace;
  const bool accepted=negotiate_audio_contract(answer,0x7f000001,true,true,local,retained,media,dtmf,trace);if(!accepted)std::cerr<<"ANSWER\n"<<answer<<"\nOFFERED_TX="<<unsigned(offered_tx.count)<<" OFFERED_RX="<<unsigned(offered_rx.count)<<"\n";assert(accepted);
  if(missing_rx)assert(!dtmf.rx.valid());else assert(dtmf.rx.valid()&&dtmf.rx.payload_type==expected_rx&&dtmf.rx.events==expected_mask);
  if(missing_tx)assert(!dtmf.tx.valid());else assert(dtmf.tx.valid()&&dtmf.tx.payload_type==expected_tx&&dtmf.tx.events==expected_mask);
  std::cout<<"actual_offer_contract=true rx_pt="<<unsigned(dtmf.rx.payload_type)<<" tx_pt="<<unsigned(dtmf.tx.payload_type)<<"\n";
}
