#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <map>
#define ESP_LOGE(...) ((void)0)
#define tskNO_AFFINITY -1
constexpr const char *TAG="test";
std::map<void*,size_t> live;
unsigned allocations=0,fail_at=0;
void *allocate(size_t n){if(++allocations==fail_at)return nullptr;void*p=::operator new(n);assert(live.emplace(p,n).second);return p;}
void release(void*p,size_t n){assert(p&&live.count(p)&&live.at(p)==n);live.erase(p);::operator delete(p);}
template<class T>struct RAMAllocator {enum{ALLOC_INTERNAL};RAMAllocator(int=0){}T*allocate(size_t n){return static_cast<T*>(::allocate(n*sizeof(T)));}void deallocate(T*p,size_t n){release(p,n*sizeof(T));}};
struct Ring {void*p;size_t n;explicit Ring(size_t size):p(allocate(size)),n(size){}~Ring(){if(p)release(p,n);}};
struct Transport {static unsigned instances;Transport(){++instances;}~Transport(){--instances;}};unsigned Transport::instances=0;
struct RtpJitterBuffer {static unsigned instances;RtpJitterBuffer(uint8_t*,size_t,uint8_t,uint8_t){++instances;}~RtpJitterBuffer(){--instances;}};unsigned RtpJitterBuffer::instances=0;
namespace voip_audio_core {
std::unique_ptr<Ring> create_internal(size_t n,const char*){auto p=std::make_unique<Ring>(n);return p->p?std::move(p):nullptr;}
std::unique_ptr<Ring> create_prefer_psram(size_t n,const char*s){return create_internal(n,s);}
bool start_pinned_task(void(*)(void*),const char*,size_t n,void*,int,int,bool,const char*,void**handle,void*,void**stack){*stack=allocate(n);if(!*stack)return false;*handle=*stack;return true;}
void force_delete_pinned_task(void**handle,void**stack,size_t n){if(*stack)release(*stack,n);*handle=*stack=nullptr;}
}
enum class PcmFormat{S16LE};
struct Format {PcmFormat pcm_format=PcmFormat::S16LE;size_t nominal_frame_bytes()const{return 320;}};
struct Formats {uint8_t count=1;Format formats[1];};
struct VoipStack {
 static constexpr unsigned kRxQueuedFrames=16,kRxPrebufferFrames=2,kTxTaskPriority=15,kRxTaskPriority=15;
 bool buffers_in_psram_=false,dc_offset_removal_=true,audio_task_stacks_in_psram_=false;
 bool has_microphone_()const {
#ifdef USE_ESPHOME_VOIP_STACK_MIC
 return true;
#else
 return false;
#endif
 }
 bool has_speaker_()const {
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
 return true;
#else
 return false;
#endif
 }
 size_t tx_audio_buffer_bytes_()const{return 640;}size_t tx_audio_max_chunk_bytes_()const{return 320;}size_t mic_processing_samples_()const{return 160;}
 std::atomic<int16_t*> mic_converted_{nullptr};
 bool ensure_mic_processing_buffer_(){mic_converted_=RAMAllocator<int16_t>().allocate(160);return mic_converted_!=nullptr;}
 std::unique_ptr<Ring> mic_buffer_;
 std::unique_ptr<RtpJitterBuffer> rx_jitter_buffer_;
 std::unique_ptr<Transport> transport_;
 Format tx_audio_format_,rx_audio_format_;Formats rx_audio_formats_;
 uint8_t *tx_audio_chunk_=nullptr,*rx_audio_chunk_=nullptr,*rx_network_chunk_=nullptr,*rx_jitter_pcm_storage_=nullptr,*rx_silence_chunk_=nullptr;
 size_t tx_audio_chunk_alloc_bytes_=0,rx_audio_chunk_alloc_bytes_=0,rx_jitter_frame_alloc_bytes_=0;
 void *tx_task_handle_=nullptr,*tx_task_stack_=nullptr,*rx_task_handle_=nullptr,*rx_task_stack_=nullptr;
 int tx_task_tcb_=0,rx_task_tcb_=0;size_t tx_task_stack_bytes_=256,rx_task_stack_bytes_=256;
 static void tx_task(void*){}static void rx_task(void*){}
 bool allocate_setup_buffers_();bool start_runtime_tasks_();void cleanup_partial_setup_();
 bool initialize(){
  if(!allocate_setup_buffers_()){cleanup_partial_setup_();return false;}
  transport_=std::make_unique<Transport>();
  if(!start_runtime_tasks_()){cleanup_partial_setup_();return false;}
  return true;
 }
 void assert_clean(){
  assert(live.empty());assert(!Transport::instances&&!RtpJitterBuffer::instances);
  assert(!tx_task_handle_&&!rx_task_handle_&&!tx_task_stack_&&!rx_task_stack_);
  assert(!mic_converted_&&!mic_buffer_&&!rx_jitter_buffer_&&!transport_);
  assert(!tx_audio_chunk_&&!rx_audio_chunk_&&!rx_network_chunk_&&!rx_jitter_pcm_storage_&&!rx_silence_chunk_);
  assert(!tx_audio_chunk_alloc_bytes_&&!rx_audio_chunk_alloc_bytes_&&!rx_jitter_frame_alloc_bytes_);
 }
};
// PRODUCTION_METHODS
int main(){
 VoipStack baseline;assert(baseline.initialize());const unsigned stages=allocations;
 baseline.cleanup_partial_setup_();baseline.assert_clean();
 for(unsigned failure=1;failure<=stages;failure++){
  allocations=0;fail_at=failure;VoipStack stack;
  assert(!stack.initialize());stack.assert_clean();stack.cleanup_partial_setup_();stack.assert_clean();
  allocations=0;fail_at=0;assert(stack.initialize());assert(!live.empty());
  stack.cleanup_partial_setup_();stack.cleanup_partial_setup_();stack.assert_clean();
 }
}
