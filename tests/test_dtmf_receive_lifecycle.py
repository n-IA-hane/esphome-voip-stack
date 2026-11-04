"""The actual receiver and main-loop delivery retain their generation contracts."""
import subprocess
from test_dtmf_worker_runtime import worker_source
from test_refer_dialog_runtime import SOURCE, block


def test_received_reserved_bit_and_retired_loop_events(tmp_path):
    source='#include \"voip_fsm.h\"\n'+worker_source().split('uint16_t bind_udp(')[0]
    actual_emit=block((SOURCE/'transport.h').read_text(),'void emit_dtmf_(')
    source=source.replace('void emit_dtmf_(char c,uint32_t){digits.push_back(c);}',
        'void(*on_dtmf_)(void*,char,uint32_t)=nullptr;void*on_dtmf_ctx_=nullptr;'+actual_emit)
    header=(SOURCE/'voip_stack.h').read_text()
    source+=block(header,'struct DtmfEvent {')+';\n'
    source+=r'''
struct EventQueue{DtmfEvent data[8]{};unsigned count=0;};
unsigned uxQueueMessagesWaiting(EventQueue*q){return q->count;}
int xQueueSend(EventQueue*q,const void*p,int){if(q->count==8)return 0;q->data[q->count++]=*static_cast<const DtmfEvent*>(p);return 1;}
int xQueueReceive(EventQueue*q,void*p,int){if(!q->count)return 0;*static_cast<DtmfEvent*>(p)=q->data[0];for(unsigned i=1;i<q->count;++i)q->data[i-1]=q->data[i];--q->count;return 1;}
struct VoipStack {
 using DtmfEvent=::DtmfEvent;
 struct Trigger{std::vector<std::string>seen;unsigned limit=100;void(*refill)(void*)=nullptr;void*context=nullptr;void trigger(std::string value){seen.push_back(value);assert(seen.size()<=limit);if(refill)refill(context);}}dtmf_trigger_;
 std::atomic<CallState>call_state_{CallState::IN_CALL};SipTransport*transport_=nullptr;EventQueue queue;EventQueue*dtmf_events_=&queue;
 unsigned wakes=0;void enable_loop_soon_any_context(){++wakes;}
 static void transport_dtmf_callback_(void*,char,uint32_t);void pump_dtmf_events_();
};
'''
    impl=(SOURCE/'voip_stack.cpp').read_text()
    for name in ['void VoipStack::transport_dtmf_callback_(', 'void VoipStack::pump_dtmf_events_(']:
        source+=block(impl,name)+'\n'
    source+=r'''
int main(){
 SipTransport transport;transport.remote_rtp_port_=60000;
 VoipStack application;application.transport_=&transport;
 transport.on_dtmf_=VoipStack::transport_dtmf_callback_;transport.on_dtmf_ctx_=&application;
 DtmfReceiver receiver;
 uint8_t event[]{11,0x40,1,0x40};
 for(unsigned packet=0;packet<4;++packet){event[1]=0x40|(packet?0x80:0);assert(transport.handle_dtmf_packet_(event,4,109,42,1000,60000,8,&receiver));}
 assert(application.queue.count==1&&application.wakes==1);
 application.pump_dtmf_events_();assert(application.dtmf_trigger_.seen==std::vector<std::string>{"#"});
 // Rejected RTP does not move peer identity or produce a second callback.
 assert(transport.handle_dtmf_packet_(event,4,109,42,2000,60001,8,&receiver));
 event[2]=event[3]=0;assert(transport.handle_dtmf_packet_(event,4,109,42,2000,60000,8,&receiver));
 assert(application.queue.count==0);event[2]=1;
 assert(transport.handle_dtmf_packet_(event,4,109,42,2000,60000,8,&receiver));
 assert(application.queue.count==1);
 const auto retired=transport.dtmf_generation();transport.close_media_session_();transport.media_active_=true;
 assert(transport.dtmf_generation()!=retired);application.pump_dtmf_events_();
 assert(application.dtmf_trigger_.seen.size()==1&&application.queue.count==0);
 // A wire datagram has no SIP Call-ID. Same tuple/PT after reuse is a
 // separate limitation, not a proof supplied by the local queue epoch.
 assert(transport.handle_dtmf_packet_(event,4,109,42,2000,60000,8,&receiver));
 application.pump_dtmf_events_();assert(application.dtmf_trigger_.seen.size()==2);
 // A trigger can enqueue more work. One loop turn must still have a budget.
 const unsigned before=application.dtmf_trigger_.seen.size();
 application.dtmf_trigger_.limit=before+8;
 application.dtmf_trigger_.context=&application;
 application.dtmf_trigger_.refill=[](void*p){auto*a=static_cast<VoipStack*>(p);VoipStack::transport_dtmf_callback_(a,'3',a->transport_->dtmf_generation());};
 VoipStack::transport_dtmf_callback_(&application,'3',transport.dtmf_generation());
 application.pump_dtmf_events_();assert(application.dtmf_trigger_.seen.size()==before+8&&application.queue.count==1);
 application.dtmf_trigger_.refill=nullptr;application.dtmf_trigger_.limit=100;application.pump_dtmf_events_();assert(application.queue.count==0);
 puts("PASS R-bit ignored; duplicate events collapsed; retired loop delivery discarded; bounded loop turn");
 puts("LIMITATION: delayed RTP on an identically reused tuple has no SIP identity");
}
'''
    cpp=tmp_path/'receiver.cpp'
    cpp.write_text(source)
    binary=tmp_path/'receiver'
    built=subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
        '-fsanitize=address,undefined','-fno-pie','-no-pie','-pthread','-I',str(SOURCE),str(cpp),'-o',str(binary)],capture_output=True,text=True)
    assert built.returncode==0,built.stderr
    run=subprocess.run([str(binary)],capture_output=True,text=True,timeout=5)
    assert run.returncode==0,run.stdout+run.stderr
    print(run.stdout,end='')
