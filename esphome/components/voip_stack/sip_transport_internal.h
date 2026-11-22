#pragma once

#include "sip_transport.h"
#include "sip_message.h"
#include "sip_record.h"
#include "sip_dialog_message.h"
#include "sip_udp_receive.h"
#ifdef USE_ESPHOME_VOIP_PAGING
#include "sip_answer_mode.h"
#endif

#if defined(USE_ESP32) && defined(USE_ESPHOME_VOIP_SIP_TRANSPORT)

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <inttypes.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
#include <esp_timer.h>
#endif

#include "audio_core_task_utils.h"
#include "esphome/components/network/util.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "net_utils.h"

namespace esphome {
namespace voip_stack {
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
#define VOIP_STARTUP_EVENT(event) this->record_startup_(StartupEvent::event)
#else
#define VOIP_STARTUP_EVENT(event) ((void) 0)
#endif

[[maybe_unused]] static const char *const TAG = "voip_stack.sip";

namespace {


// Reuse small dialog fields, but return large call-scoped SIP/SDP buffers to
// the heap at the terminal barrier. Keeping every peak capacity permanently
// resident leaves S3 full profiles without enough contiguous internal heap for
// a fresh ESPHome API inventory after Home Assistant reconnects.
[[maybe_unused]] static constexpr size_t RETAINED_SIP_STRING_CAPACITY = 256;
inline void reset_string(std::string &value) {
  if (value.capacity() > RETAINED_SIP_STRING_CAPACITY) {
    std::string{}.swap(value);
  } else {
    value.clear();
  }
}

inline void reset_string(SipSignalingString &value) { value.clear(); }

class ScopedMediaProposal {
 public:
  explicit ScopedMediaProposal(std::atomic<uint32_t> &epoch) : epoch_(epoch) {
    this->epoch_.fetch_add(1, std::memory_order_acq_rel);
  }
  ~ScopedMediaProposal() { this->finish(); }
  void finish() {
    if (!this->active_) return;
    this->epoch_.fetch_add(1, std::memory_order_acq_rel);
    this->active_ = false;
  }

 private:
  std::atomic<uint32_t> &epoch_;
  bool active_{true};
};

}  // namespace

}  // namespace voip_stack
}  // namespace esphome

#endif  // USE_ESP32 && USE_ESPHOME_VOIP_SIP_TRANSPORT
