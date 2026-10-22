// The application watchdog observes the real wire fixture's transport. Media
// samples and the scheduler are adapters; SIP lifetime is production code.
#include "esphome/components/voip_stack/voip_fsm.h"
struct TransportHealth {
  SipTransport *sip;
  bool remote_audio_expected() const { return sip->remote_audio_expected(); }
  uint32_t received_audio_packets() const { return 0; }
  uint16_t last_sip_status_code() const { return 180; }
};
struct VoipStack {
  TransportHealth health;
  TransportHealth *transport_{&health};
  std::atomic<esphome::voip_stack::CallState> call_state_{esphome::voip_stack::CallState::IN_CALL};
  std::atomic<uint32_t> media_timeout_rtp_rx_packets_{0},last_peer_audio_ms_{100};
  std::atomic<bool> first_audio_received_{false};
  uint32_t ringing_timeout_ms_=0,ringing_start_time_=0,calling_start_time_=0;
  static constexpr uint32_t INVITE_NO_RESPONSE_TIMEOUT_MS=5000,MEDIA_TIMEOUT_MS=15000;
  unsigned terminations=0;
  explicit VoipStack(SipTransport &sip):health{&sip}{}
  bool has_speaker_() const { return true; }
  std::string get_current_call_id_() const { return "original"; }
  void fire_timeout_decline_(){assert(false);}
  void fire_unanswered_invite_timeout_(){assert(false);}
  void request_call_termination_(const esphome::voip_stack::TerminationIntent &){
    ++terminations;health.sip->cancel_refer_();
  }
  void handle_call_timeouts_(uint32_t,uint32_t);
};
void watchdog_case(bool old_tcp,bool new_tcp,unsigned outcome) {
  SipTransport sip;Peer original(old_tcp),target(new_tcp);setup(sip,original);
  auto invite=begin(sip,original,target);
  assert(sip.remote_audio_expected());
  VoipStack phone(sip);
  original.send_to(sip,request(original,"BYE",3));
  auto ended=original.drain();assert(ended.size()==1&&ended[0].rfind("SIP/2.0 200",0)==0);
  target.send_to(sip,response(invite,180));
  clock_ms=20100;
  phone.handle_call_timeouts_(clock_ms,0);
  assert(phone.terminations==0&&"REFER ringing must survive the media watchdog");
  assert(target.drain().empty()&&original.drain().empty());
  if(outcome==408){clock_ms=33100;sip.pump_refer_();}
  else target.send_to(sip,response(invite,outcome));
  auto answer=target.drain();
  if(outcome!=408)assert(answer.size()==1&&answer[0].rfind("ACK ",0)==0);
  auto notified=original.drain();
  assert(notified.size()==1&&message_body(notified[0]).rfind("SIP/2.0 "+std::to_string(outcome),0)==0);
  assert(header_value(notified[0],"Subscription-State").find("terminated")==0);
  if(sip.refer_dialog_)original.send_to(sip,response(notified[0],200));
  sip.pump_refer_();
  if(outcome==200){
    assert(sip.media_active_&&sip.remote_audio_expected());
    phone.handle_call_timeouts_(35099,0);assert(phone.terminations==0);
    phone.handle_call_timeouts_(35100,0);assert(phone.terminations==1);
  }else{
    assert(sip.call_id_.empty()&&!sip.media_active_);
    assert(sip.delivered.size()==1&&sip.delivered[0].type==SipSignalType::BYE);
  }
  cleanup(sip);
}
