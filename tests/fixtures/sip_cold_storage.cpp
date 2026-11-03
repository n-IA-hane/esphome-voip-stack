#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esphome/core/helpers.h"
#include "sip_registration.h"
#include "sip_mwi.h"
template<class...T>void host_log(const char*,const char*,const T&...){ }
#define ESP_LOGE(...) host_log(__VA_ARGS__)
#define ESP_LOGI(...) host_log(__VA_ARGS__)
const char*socket_errno_name(int){return "host-error";}
const char*socket_errno_text(int error){return std::strerror(error);}
namespace probe {
unsigned attempts=0,releases=0,created_tasks=0,joined_tasks=0,binds=0,initializations=0;
std::map<void*,size_t> live;
void*allocate(size_t n,unsigned flags){
 assert(flags==1);++attempts;if(attempts==FAIL_STAGE)return nullptr;
 auto*p=std::malloc(n);assert(p);assert(live.emplace(p,n).second);return p;
}
void release(void*p){if(!p)return;assert(live.erase(p)==1);++releases;std::free(p);}
}
namespace esphome::voip_stack {
constexpr int pdTRUE=1,portMAX_DELAY=100;
struct Mutex{};struct LockGuard{explicit LockGuard(Mutex&) {}};
template<class T>void reset_string(T&s){s.clear();}
void*xSemaphoreCreateBinaryStatic(int*p){return p;}
int xSemaphoreTake(void*,int){return pdTRUE;}
void xTaskNotifyGive(void*){}
namespace voip_audio_core {
bool start_pinned_task(void(*)(void*),const char*,unsigned,void*,unsigned,int,bool,const char*,void**handle,int*,int**stack){
 ++probe::created_tasks;*handle=reinterpret_cast<void*>(probe::created_tasks);*stack=nullptr;return true;
}
void cleanup_pinned_task(void**handle,int**,unsigned){assert(*handle);++probe::joined_tasks;*handle=nullptr;}
}
constexpr const char*TAG="cold";
struct SipPhoneTransport{virtual~SipPhoneTransport()=default;virtual bool start()=0;};
#ifdef USE_ESPHOME_VOIP_MWI
struct Borrower:SipMwi{
 const SipAccountConfig*config_at(size_t i)const{return &this->account_(i).config();}
};
#endif
class SipTransport:public SipPhoneTransport {
public:
 // REAL_DELETER
 // REAL_SCRATCH
#ifdef USE_ESPHOME_VOIP_REGISTRATION
 SipRegistration registration_;
 Borrower mwi_;
#endif
 // REAL_READY
 static void*operator new(size_t)noexcept;static void operator delete(void*)noexcept;
 SipTransport(uint16_t port,uint16_t rtp,size_t,const std::string&,bool psram):sip_port_(port),rtp_port_(rtp),task_stacks_in_psram_(psram){}
 ~SipTransport()override;
 bool start()override;void stop();
 bool bind_udp_(int*fd,uint16_t,const char*){return bind_socket(fd,SOCK_DGRAM);}
 bool bind_tcp_(int*fd,uint16_t,const char*){return bind_socket(fd,SOCK_STREAM);}
 bool bind_socket(int*fd,int type){
  ++probe::binds;*fd=socket(AF_INET,type,0);assert(*fd>=0);
  sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  assert(bind(*fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0);return true;
 }
 static constexpr unsigned kSipTaskStackBytes=12288,kSipTaskPriority=4,kRtpTaskStackBytes=4096,kRtpTaskPriority=5;
 static void sip_task_trampoline_(void*){}static void rtp_task_trampoline_(void*){}
 void set_sip_signaling_transport(bool){}
 template<class...T>void set_audio_formats(T...){ }
 template<class...T>void set_audio_callback(T...){ }
 template<class...T>void set_sip_signal_callback(T...){ }
 template<class...T>void set_connection_callback(T...){ }
 template<class...T>void set_accept_callback(T...){ }
 template<class...T>void set_dialog_active_callback(T...){ }
 template<class...T>void set_media_quiesced_callback(T...){ }
 void emit_connection_change_(bool){}void cancel_registration_dns_(){}
 void wake_sip_task_(){}void stop_audio_path(){}
 void release_refer_dialog_(){}
 struct{std::atomic<bool>connect_requested{false},close_requested{false};std::string pending;}tcp_flow_;
 Mutex tcp_tx_pending_mutex_,media_lifecycle_mutex_;
 std::atomic<bool>running_{false},transport_stopping_{false},rtp_task_quiesced_{true},rtp_task_terminate_{false};
 enum class MediaLifecyclePhase{IDLE,SHUTTING_DOWN};std::atomic<MediaLifecyclePhase>media_lifecycle_phase_{MediaLifecyclePhase::IDLE};
 void*sip_task_handle_=nullptr,*rtp_task_handle_=nullptr,*sip_task_done_=nullptr,*rtp_task_done_=nullptr,*rtp_cleanup_done_=nullptr;
 int sip_task_tcb_{},rtp_task_tcb_{},sip_task_done_storage_{},rtp_task_done_storage_{},rtp_cleanup_done_storage_{};
 int*sip_task_stack_=nullptr,*rtp_task_stack_=nullptr;
 int sip_socket_=-1,sip_tcp_listener_socket_=-1,sip_wake_socket_=-1;uint16_t sip_port_,rtp_port_,sip_wake_port_{};
 bool task_stacks_in_psram_;std::array<std::unique_ptr<int>,4>completed_refers_{};
 bool refer_signal_pending_=false;std::string logical_call_id_,logical_wire_id_;
};
// REAL_ALLOCATOR
// REAL_DESTRUCTOR
// REAL_START
// REAL_STOP
struct VoipStack{
 std::unique_ptr<SipTransport>transport_;uint16_t sip_port_=0,rtp_port_=0;size_t udp_max_payload_=2048;
 bool task_stacks_in_psram_=true;enum class TransportType{UDP,TCP};TransportType protocol_=TransportType::TCP;
 int tx_audio_formats_=0,rx_audio_formats_=0;
 static void transport_audio_callback_(){}static void transport_sip_signal_callback_(){}
 static void transport_connection_callback_(){}static void transport_accept_callback_(){}
 static void transport_dialog_active_callback_(){}static void transport_media_quiesced_callback_(){}
 void initialize_registration_(){assert(transport_->initialized());++probe::initializations;}
 bool setup_transport_();
};
// REAL_FACTORY
}
using namespace esphome::voip_stack;
#ifdef USE_ESPHOME_VOIP_REGISTRATION
std::string field(const std::string&m,const std::string&key){auto p=m.find("\r\n"+key+": ");assert(p!=std::string::npos);p+=key.size()+4;return m.substr(p,m.find("\r\n",p)-p);}
std::string response(const std::string&m){std::string r="SIP/2.0 200 OK\r\n";for(auto k:{"Via","From","To","Call-ID","CSeq","Contact"})r+=std::string(k)+": "+field(m,k)+"\r\n";return r+"Content-Length: 0\r\n\r\n";}
void accounts(SipTransport&t){
 auto&r=t.registration_;assert(r.initialized());std::vector<std::string>wire;
 SipRegistration::Hooks h;h.context=&wire;h.send=[](void*c,size_t,std::string_view m,uint32_t,uint16_t,bool){static_cast<std::vector<std::string>*>(c)->emplace_back(m);return true;};
 h.resolve=[](void*,size_t,const std::string&){return uint32_t(INADDR_LOOPBACK);};h.local_ip=[](void*,uint32_t){return std::string("127.0.0.1");};
 h.random=[](){static uint32_t value=100;return ++value;};
 r.set_hooks(h);r.set_keepalive_interval(15000);std::array<const SipRegistration::Account*,4>identities{};
 for(unsigned i=0;i<4;++i){SipAccountConfig c;c.id="line"+std::to_string(i);c.registrar=c.domain="pbx.example";c.username="test";c.password=std::string(96,'s');assert(c.valid());assert(r.set_account(c,0));assert(r.find(c.id)==int(i));identities[i]=&r.account(i);}
 SipAccountConfig extra=r.account(0).config;extra.id="fifth";assert(!r.set_account(extra,0));r.tick(1);assert(wire.size()==4);
 for(auto&m:wire){assert(r.handle_response(response(m),INADDR_LOOPBACK,5060,false,-1,2));}
 wire.clear();
 for(unsigned i=0;i<4;++i){assert(r.is_registered("line"+std::to_string(i)));t.mwi_.update_registered_account(i,r.account(i),2);assert(t.mwi_.config_at(i)==&r.account(i).config);}
 assert(r.match_incoming("esphome-line2",INADDR_LOOPBACK,false,-1)==2);
 assert(r.request_registration("line0",false,3));r.tick(3);assert(wire.size()==1);assert(r.handle_response(response(wire[0]),INADDR_LOOPBACK,5060,false,-1,4));assert(!r.is_registered("line0"));
 assert(r.remove_account("line3"));assert(r.find("line3")==-1);extra.id="replacement";assert(r.set_account(extra,5));
 r.connection_lost(1,6);r.resolution_complete(2,INADDR_LOOPBACK,7);uint32_t deadline=0;assert(r.next_deadline(7,&deadline));
 for(unsigned i=0;i<4;++i){assert(&r.account(i)==identities[i]);t.mwi_.update_registered_account(i,r.account(i),8);assert(t.mwi_.config_at(i)==&r.account(i).config);}
 t.stop();assert(probe::joined_tasks==2);for(unsigned i=0;i<4;++i)assert(t.mwi_.config_at(i)->id==r.account(i).config.id);
}
#endif
int main(){
 {
  VoipStack v;bool ok=v.setup_transport_();
  if(FAIL_STAGE){assert(!ok&&!v.transport_);assert(probe::binds==0&&probe::created_tasks==0&&probe::initializations==0);}
  else{
   assert(ok&&v.transport_&&v.transport_->initialized());assert(internal_objects>=1);assert(probe::binds==3&&probe::created_tasks==2);
#ifdef USE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM
#ifdef USE_ESPHOME_VOIP_REGISTRATION
   assert(probe::attempts==2);
#else
   assert(probe::attempts==1);
#endif
#else
   assert(probe::attempts==0);
#endif
   auto*scratch=v.transport_->sip_rx_scratch_.data();assert(scratch&&v.transport_->sip_rx_scratch_.size()==2048);for(unsigned i=0;i<2048;++i)assert(scratch[i]==0);
   std::memset(scratch,0x5a,2048);
#ifdef USE_ESPHOME_VOIP_REGISTRATION
   accounts(*v.transport_);
#else
   v.transport_->stop();assert(probe::joined_tasks==2);
#endif
   assert(v.transport_->sip_rx_scratch_.data()==scratch&&scratch[2047]==0x5a);
   v.transport_->stop();assert(probe::joined_tasks==2);
   assert(v.transport_->start());assert(v.transport_->sip_rx_scratch_.data()==scratch);
   v.transport_->stop();assert(probe::joined_tasks==4);
  }
 }
 assert(probe::live.empty());assert(probe::created_tasks==probe::joined_tasks);
 std::puts("COLD_STORAGE_OK");
}
