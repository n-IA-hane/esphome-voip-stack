#include "sip_transport.h"
#include "sip_message.h"

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

#include "audio_core_task_utils.h"
#include "esphome/components/network/util.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "net_utils.h"

namespace esphome {
namespace voip_stack {

static const char *const TAG = "voip_stack.sip";

namespace {


// Reuse small dialog fields, but return large call-scoped SIP/SDP buffers to
// the heap at the terminal barrier. Keeping every peak capacity permanently
// resident leaves S3 full profiles without enough contiguous internal heap for
// a fresh ESPHome API inventory after Home Assistant reconnects.
static constexpr size_t RETAINED_SIP_STRING_CAPACITY = 256;
static void reset_string(std::string &value) {
  if (value.capacity() > RETAINED_SIP_STRING_CAPACITY) {
    std::string{}.swap(value);
  } else {
    value.clear();
  }
}

static void reset_string(SipSignalingString &value) { value.clear(); }

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

void *SipTransport::operator new(size_t size) noexcept {
  return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void SipTransport::operator delete(void *ptr) noexcept { heap_caps_free(ptr); }

SipTransport::SipTransport(uint16_t sip_port, uint16_t rtp_port, size_t udp_max_payload,
                           const std::string &remote_host,
                           bool task_stacks_in_psram)
    : sip_port_(sip_port), rtp_port_(rtp_port), udp_max_payload_(udp_max_payload),
      task_stacks_in_psram_(task_stacks_in_psram) {
  audio_format_list_default(&this->offer_tx_formats_);
  audio_format_list_default(&this->offer_rx_formats_);
  this->rtp_ssrc_ = esp_random();
  this->parse_remote_(remote_host);
}

SipTransport::~SipTransport() { this->stop(); }

const char *SipTransport::sip_event_name_(SipEvent event) {
  switch (event) {
    case SipEvent::INVITE: return "INVITE";
    case SipEvent::ACK: return "ACK";
    case SipEvent::CANCEL: return "CANCEL";
    case SipEvent::BYE: return "BYE";
    case SipEvent::OPTIONS: return "OPTIONS";
    case SipEvent::UPDATE: return "UPDATE";
    case SipEvent::RESPONSE: return "SIP_RESPONSE";
    case SipEvent::NONE:
    default: return "";
  }
}

SipTransport::SipEvent SipTransport::sip_event_from_method_(const std::string &method) {
  if (method == "INVITE") return SipEvent::INVITE;
  if (method == "ACK") return SipEvent::ACK;
  if (method == "CANCEL") return SipEvent::CANCEL;
  if (method == "BYE") return SipEvent::BYE;
  if (method == "OPTIONS") return SipEvent::OPTIONS;
  if (method == "UPDATE") return SipEvent::UPDATE;
  return SipEvent::NONE;
}

void SipTransport::mark_sip_event_(SipEvent event, uint16_t status) {
  this->last_sip_event_.store(static_cast<uint8_t>(event), std::memory_order_release);
  if (status != 0) {
    this->last_sip_status_code_.store(status, std::memory_order_release);
  }
}

SipTransportSnapshot SipTransport::snapshot() const {
  SipTransportSnapshot out;
  out.running = this->running_.load(std::memory_order_acquire);
  out.rtp_running = this->rtp_running_.load(std::memory_order_acquire);
  {
    LockGuard lock(this->dialog_mutex_);
    out.terminal_transaction_pending =
        this->terminal_transaction_pending_locked_();
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
    out.video_running =
        this->video_session_ != nullptr && this->video_session_->is_running() &&
        this->video_negotiated_ &&
        (this->video_send_enabled_ || this->video_receive_enabled_);
    out.video_send_enabled = this->video_send_enabled_;
    out.video_send_requested =
        this->video_send_requested_.load(std::memory_order_acquire);
    out.video_send_change_pending =
        this->pending_video_direction_invite_.pending();
    if (this->video_session_ != nullptr) {
      out.video_tx_packets = this->video_session_->tx_packets();
      out.video_rx_packets = this->video_session_->rx_packets();
      out.video_tx_access_units =
          this->video_session_->tx_access_units();
      out.video_rx_access_units =
          this->video_session_->rx_access_units();
    }
#endif
  }
  out.media_lifecycle_phase = static_cast<uint8_t>(
      this->media_lifecycle_phase_.load(std::memory_order_acquire));
  out.call_active = this->media_active_.load(std::memory_order_acquire);
  out.pending_invite = this->outgoing_invite_pending_.load(std::memory_order_acquire);
  out.sip_tcp = this->remote_sip_tcp_.load(std::memory_order_acquire);
  out.remote_ip_v4 = this->remote_ip_v4_.load(std::memory_order_acquire);
  out.remote_sip_port = this->remote_sip_port_.load(std::memory_order_acquire);
  out.remote_rtp_port = this->remote_rtp_port_.load(std::memory_order_acquire);
  this->get_media_config_(&out.selected_tx_format, &out.selected_rx_format, nullptr, nullptr);
  out.rtp_tx_packets = this->rtp_tx_packets_.load(std::memory_order_acquire);
  out.rtp_rx_packets = this->rtp_rx_packets_.load(std::memory_order_acquire);
  out.rtp_tx_bytes = this->rtp_tx_bytes_.load(std::memory_order_acquire);
  out.rtp_rx_bytes = this->rtp_rx_bytes_.load(std::memory_order_acquire);
  out.last_sip_status_code = this->last_sip_status_code_.load(std::memory_order_acquire);
  out.last_sip_event = SipTransport::sip_event_name_(
      static_cast<SipEvent>(this->last_sip_event_.load(std::memory_order_acquire)));
  return out;
}

void SipTransport::set_media_config_(const AudioFormat &tx, const AudioFormat &rx,
                                     uint8_t tx_payload_type, uint8_t rx_payload_type,
                                     const RtpAudioPayloads *rx_payloads) {
  portENTER_CRITICAL(&this->media_config_lock_);
  this->selected_tx_format_ = tx;
  this->selected_rx_format_ = rx;
  const uint8_t prepared = (tx.is_valid() ? 1U : 0U) | (rx.is_valid() ? 2U : 0U);
  this->audio_direction_state_.store(prepared | (prepared << 2U), std::memory_order_release);
  this->audio_receive_enabled_.store(rx.is_valid(), std::memory_order_release);
  this->rtp_tx_payload_type_ = tx_payload_type;
  this->rtp_rx_payload_type_ = rx_payload_type;
  this->negotiated_rx_payloads_ = rx_payloads != nullptr ? *rx_payloads : RtpAudioPayloads{};
  if (rx_payloads == nullptr && rx.is_valid()) this->negotiated_rx_payloads_.add(rx_payload_type, rx);
  portEXIT_CRITICAL(&this->media_config_lock_);
}

void SipTransport::get_media_config_(AudioFormat *tx, AudioFormat *rx,
                                     uint8_t *tx_payload_type, uint8_t *rx_payload_type,
                                     RtpAudioPayloads *rx_payloads) const {
  portENTER_CRITICAL(&this->media_config_lock_);
  if (tx != nullptr) *tx = this->selected_tx_format_;
  if (rx != nullptr) *rx = this->selected_rx_format_;
  if (tx_payload_type != nullptr) *tx_payload_type = this->rtp_tx_payload_type_;
  if (rx_payload_type != nullptr) *rx_payload_type = this->rtp_rx_payload_type_;
  if (rx_payloads != nullptr) *rx_payloads = this->negotiated_rx_payloads_;
  portEXIT_CRITICAL(&this->media_config_lock_);
}

bool SipTransport::received_audio_format_(uint8_t pt, AudioFormat *format) const {
  portENTER_CRITICAL(&this->media_config_lock_);
  const bool found = (this->get_audio_direction_() & 2U) != 0 &&
                     this->negotiated_rx_payloads_.find(pt, format);
  portEXIT_CRITICAL(&this->media_config_lock_);
  return found;
}

void SipTransport::set_audio_formats(const AudioFormatList &tx, const AudioFormatList &rx) {
  this->offer_tx_formats_ = tx;
  this->offer_rx_formats_ = rx;
#ifdef USE_ESPHOME_VOIP_STACK_OPUS
  const AudioFormat *opus_tx = nullptr;
  const AudioFormat *opus_rx = nullptr;
  for (uint8_t i = 0; i < this->offer_tx_formats_.count && opus_tx == nullptr; i++)
    if (this->offer_tx_formats_.formats[i].codec == AudioCodec::OPUS)
      opus_tx = &this->offer_tx_formats_.formats[i];
  for (uint8_t i = 0; i < this->offer_rx_formats_.count && opus_rx == nullptr; i++)
    if (this->offer_rx_formats_.formats[i].codec == AudioCodec::OPUS)
      opus_rx = &this->offer_rx_formats_.formats[i];
  this->opus_codec_ready_ = this->opus_codec_.configure(
      opus_tx, opus_rx, this->udp_max_payload_);
#endif
  ESP_LOGI(TAG, "SIP media capabilities: tx=%u rx=%u",
           (unsigned) this->offer_tx_formats_.count,
           (unsigned) this->offer_rx_formats_.count);
}

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
void SipTransport::set_video_endpoints(EncodedVideoSource *source,
                                       EncodedVideoSink *sink) {
  this->video_source_ = source;
  this->video_sink_ = sink;
  if (this->video_session_ != nullptr) {
    this->video_session_->set_source(source);
    this->video_session_->set_sink(sink);
    if (!this->video_session_->prepare_buffers())
      ESP_LOGE(TAG, "Video endpoint buffers unavailable at setup");
  }
}

void SipTransport::set_video_config(VideoCodec codec, uint16_t rtp_port,
                                    uint8_t offer_payload_type,
                                    size_t max_rtp_payload) {
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  if (codec != VideoCodec::JPEG || offer_payload_type != 26) {
    ESP_LOGE(TAG, "JPEG video build requires codec JPEG and static PT 26");
  }
#else
  if (codec != VideoCodec::H264 || offer_payload_type < 96) {
    ESP_LOGE(TAG, "H.264 video build requires codec H264 and a dynamic PT");
  }
#endif
  this->video_codec_ = codec;
  this->video_rtp_port_ = rtp_port;
  this->video_offer_payload_type_ = offer_payload_type;
  this->video_max_rtp_payload_ = max_rtp_payload;
  if (this->video_session_ == nullptr) {
    this->video_session_ =
        std::make_unique<VideoRtpSession>(this->task_stacks_in_psram_);
  }
  this->video_session_->set_source(this->video_source_);
  this->video_session_->set_sink(this->video_sink_);
  this->video_session_->set_local_port(this->video_rtp_port_);
  this->video_session_->set_max_payload(this->video_max_rtp_payload_);
  if (!this->video_session_->prepare_buffers())
    ESP_LOGE(TAG, "Video endpoint buffers unavailable at setup");
}

VideoCapability SipTransport::local_video_send_capability_() const {
  VideoCapability capability;
  if (this->video_source_ == nullptr) {
    capability.max_fps = 0;
    return capability;
  }
  capability = this->video_source_->get_video_capability();
  if ((this->video_codec_ == VideoCodec::JPEG && !capability.is_jpeg()) ||
      (this->video_codec_ == VideoCodec::H264 && !capability.is_h264())) {
    ESP_LOGE(TAG, "Configured video codec does not match the source capability");
    return {};
  }
  capability.payload_type =
      capability.is_jpeg() ? 26 : this->video_offer_payload_type_;
  capability.packetization_mode = capability.is_h264() ? 1 : 0;
  capability.rtcp_feedback_pli = capability.is_h264();
  capability.rtcp_feedback_fir = capability.is_h264();
  capability.clock_rate = 90000;
  return capability;
}

VideoCapability SipTransport::local_video_receive_capability_() const {
  VideoCapability capability;
  if (this->video_sink_ == nullptr) {
    capability.max_fps = 0;
    return capability;
  }
  capability = this->video_sink_->get_receive_video_capability();
  if ((this->video_codec_ == VideoCodec::JPEG && !capability.is_jpeg()) ||
      (this->video_codec_ == VideoCodec::H264 && !capability.is_h264())) {
    ESP_LOGE(TAG, "Configured video codec does not match the sink capability");
    return {};
  }
  capability.payload_type =
      capability.is_jpeg() ? 26 : this->video_offer_payload_type_;
  capability.packetization_mode = capability.is_h264() ? 1 : 0;
  capability.rtcp_feedback_pli = capability.is_h264();
  capability.rtcp_feedback_fir = capability.is_h264();
  capability.clock_rate = 90000;
  return capability;
}

VideoCapability SipTransport::local_video_direction_capability_(
    const VideoCapability &negotiated, bool send) const {
  VideoCapability capability =
      send ? this->local_video_send_capability_()
           : this->local_video_receive_capability_();
  if (!capability.valid() ||
      capability.encoding != negotiated.encoding ||
      !negotiated.valid()) {
    return {};
  }
  // PT, clock and codec parameters are the shared RTP contract. Dimensions
  // stay directional: RFC 2435 carries each JPEG frame's actual width/height
  // in-band, and an encoder need not have the same envelope as the decoder.
  capability.payload_type = negotiated.payload_type;
  capability.clock_rate = negotiated.clock_rate;
  capability.packetization_mode = negotiated.packetization_mode;
  capability.level_asymmetry_allowed =
      negotiated.level_asymmetry_allowed;
  capability.rtcp_feedback_pli =
      capability.rtcp_feedback_pli && negotiated.rtcp_feedback_pli;
  capability.rtcp_feedback_fir =
      capability.rtcp_feedback_fir && negotiated.rtcp_feedback_fir;
  if (negotiated.max_bitrate_bps != 0 &&
      (capability.max_bitrate_bps == 0 ||
       negotiated.max_bitrate_bps < capability.max_bitrate_bps)) {
    capability.max_bitrate_bps = negotiated.max_bitrate_bps;
  }
  capability.max_fps =
      std::min(capability.max_fps, negotiated.max_fps);
  return capability;
}

bool SipTransport::prepare_video_session_locked_() {
  if (!this->video_negotiated_) return true;
  if (this->video_session_ == nullptr) return false;
  if (this->video_session_->is_running()) {
    return this->video_session_->negotiation_matches(
               this->negotiated_video_capability_,
               this->local_video_direction_capability_(
                   this->negotiated_video_capability_, true),
               this->local_video_direction_capability_(
                   this->negotiated_video_capability_, false),
               this->remote_video_ip_v4_, this->remote_video_rtp_port_,
               this->remote_video_rtcp_ip_v4_,
               this->remote_video_rtcp_port_) &&
           this->video_session_->can_request_media_direction(
               this->video_send_enabled_, this->video_receive_enabled_);
  }
  return this->video_session_->set_negotiated(
             this->negotiated_video_capability_,
             this->local_video_direction_capability_(
                 this->negotiated_video_capability_, true),
             this->local_video_direction_capability_(
                 this->negotiated_video_capability_, false),
             this->remote_video_ip_v4_, this->remote_video_rtp_port_,
             this->remote_video_rtcp_ip_v4_,
             this->remote_video_rtcp_port_, this->video_send_enabled_,
             this->video_receive_enabled_) &&
         this->video_session_->start(false);
}

void SipTransport::reset_video_negotiation_() {
  this->video_offered_ = false;
  this->video_negotiated_ = false;
  this->video_send_enabled_ = false;
  this->video_receive_enabled_ = false;
  this->remote_video_ip_v4_ = 0;
  this->remote_video_rtp_port_ = 0;
  this->remote_video_rtcp_ip_v4_ = 0;
  this->remote_video_rtcp_port_ = 0;
  this->negotiated_video_capability_ = {};
}
#endif

bool SipTransport::parse_remote_(const std::string &host) {
  if (host.empty()) {
    this->remote_ip_v4_.store(0, std::memory_order_release);
    this->remote_rtp_ip_v4_.store(0, std::memory_order_release);
    return false;
  }
  struct in_addr a{};
  if (inet_aton(host.c_str(), &a) == 0 || a.s_addr == 0) {
    this->remote_ip_v4_.store(0, std::memory_order_release);
    this->remote_rtp_ip_v4_.store(0, std::memory_order_release);
    return false;
  }
  const uint32_t ip_v4 = ntohl(a.s_addr);
  this->remote_ip_v4_.store(ip_v4, std::memory_order_release);
  this->remote_rtp_ip_v4_.store(ip_v4, std::memory_order_release);
  return true;
}

bool SipTransport::bind_udp_(int *fd, uint16_t port, const char *label) {
  *fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (*fd < 0) {
    const int err = errno;
    ESP_LOGE(TAG, "Failed to create %s socket: %s (%d: %s)",
             label, socket_errno_name(err), err, socket_errno_text(err));
    return false;
  }
  int flags = fcntl(*fd, F_GETFL, 0);
  fcntl(*fd, F_SETFL, flags | O_NONBLOCK);
  if (strcmp(label, "RTP") == 0) {
    const int rx_buffer = kRtpSocketRxBufferBytes;
    setsockopt(*fd, SOL_SOCKET, SO_RCVBUF, &rx_buffer, sizeof(rx_buffer));
    const int tos = 0xB8;
    setsockopt(*fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
  }
  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);
  if (bind(*fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
    const int err = errno;
    ESP_LOGE(TAG, "%s bind on UDP/%u failed: %s (%d: %s)",
             label, (unsigned) port, socket_errno_name(err), err, socket_errno_text(err));
    close(*fd);
    *fd = -1;
    return false;
  }
  return true;
}

bool SipTransport::bind_tcp_(int *fd, uint16_t port, const char *label) {
  *fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (*fd < 0) {
    const int err = errno;
    ESP_LOGE(TAG, "Failed to create %s TCP socket: %s (%d: %s)",
             label, socket_errno_name(err), err, socket_errno_text(err));
    return false;
  }
  int opt = 1;
  setsockopt(*fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  setsockopt(*fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
  int flags = fcntl(*fd, F_GETFL, 0);
  fcntl(*fd, F_SETFL, flags | O_NONBLOCK);
  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);
  if (bind(*fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
    const int err = errno;
    ESP_LOGE(TAG, "%s bind on TCP/%u failed: %s (%d: %s)",
             label, (unsigned) port, socket_errno_name(err), err, socket_errno_text(err));
    close(*fd);
    *fd = -1;
    return false;
  }
  if (listen(*fd, 2) < 0) {
    const int err = errno;
    ESP_LOGE(TAG, "%s listen on TCP/%u failed: %s (%d: %s)",
             label, (unsigned) port, socket_errno_name(err), err, socket_errno_text(err));
    close(*fd);
    *fd = -1;
    return false;
  }
  return true;
}

bool SipTransport::start() {
  if (this->running_.load(std::memory_order_acquire)) return true;
#ifdef USE_ESPHOME_VOIP_STACK_OPUS
  if (!this->opus_codec_ready_) {
    ESP_LOGE(TAG, "Opus codec resources are unavailable");
    return false;
  }
#endif
  if (this->sip_task_handle_ != nullptr || this->rtp_task_handle_ != nullptr) {
    ESP_LOGE(TAG, "Cannot start SIP transport while a previous task is still owned");
    return false;
  }
  if (this->sip_task_done_ == nullptr) {
    this->sip_task_done_ = xSemaphoreCreateBinaryStatic(&this->sip_task_done_storage_);
  }
  if (this->sip_task_done_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create SIP task completion signal");
    return false;
  }
  xSemaphoreTake(this->sip_task_done_, 0);
  this->transport_stopping_.store(false, std::memory_order_release);
  this->media_lifecycle_phase_.store(MediaLifecyclePhase::IDLE,
                                      std::memory_order_release);
  if (!this->bind_udp_(&this->sip_socket_, this->sip_port_, "SIP")) return false;
  if (!this->bind_tcp_(&this->sip_tcp_listener_socket_, this->sip_port_, "SIP")) {
    close(this->sip_socket_);
    this->sip_socket_ = -1;
    return false;
  }
  if (!this->bind_udp_(&this->sip_wake_socket_, 0, "SIP wake")) {
    close(this->sip_socket_);
    this->sip_socket_ = -1;
    close(this->sip_tcp_listener_socket_);
    this->sip_tcp_listener_socket_ = -1;
    return false;
  }
  struct sockaddr_in wake_addr {};
  socklen_t wake_addr_len = sizeof(wake_addr);
  if (getsockname(this->sip_wake_socket_,
                  reinterpret_cast<struct sockaddr *>(&wake_addr),
                  &wake_addr_len) < 0 ||
      wake_addr.sin_port == 0) {
    const int err = errno;
    ESP_LOGE(TAG, "Failed to resolve SIP wake socket: %s (%d: %s)",
             socket_errno_name(err), err, socket_errno_text(err));
    close(this->sip_wake_socket_);
    this->sip_wake_socket_ = -1;
    close(this->sip_socket_);
    this->sip_socket_ = -1;
    close(this->sip_tcp_listener_socket_);
    this->sip_tcp_listener_socket_ = -1;
    return false;
  }
  this->sip_wake_port_ = ntohs(wake_addr.sin_port);
  this->running_.store(true, std::memory_order_release);
  if (!voip_audio_core::start_pinned_task(SipTransport::sip_task_trampoline_, "voip_sip",
                                          kSipTaskStackBytes, this, kSipTaskPriority, 1,
                                          this->task_stacks_in_psram_, TAG,
                                          &this->sip_task_handle_, &this->sip_task_tcb_,
                                          &this->sip_task_stack_)) {
    this->running_.store(false, std::memory_order_release);
    close(this->sip_socket_);
    this->sip_socket_ = -1;
    close(this->sip_tcp_listener_socket_);
    this->sip_tcp_listener_socket_ = -1;
    close(this->sip_wake_socket_);
    this->sip_wake_socket_ = -1;
    this->sip_wake_port_ = 0;
    return false;
  }
  if (this->rtp_task_done_ == nullptr) {
    this->rtp_task_done_ = xSemaphoreCreateBinaryStatic(&this->rtp_task_done_storage_);
  }
  if (this->rtp_cleanup_done_ == nullptr) {
    this->rtp_cleanup_done_ =
        xSemaphoreCreateBinaryStatic(&this->rtp_cleanup_done_storage_);
  }
  if (this->rtp_task_done_ == nullptr || this->rtp_cleanup_done_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create RTP lifecycle signals");
    this->stop();
    return false;
  }
  xSemaphoreTake(this->rtp_task_done_, 0);
  xSemaphoreTake(this->rtp_cleanup_done_, 0);
  this->rtp_task_quiesced_.store(true, std::memory_order_release);
  this->rtp_task_terminate_.store(false, std::memory_order_release);
  if (!voip_audio_core::start_pinned_task(SipTransport::rtp_task_trampoline_, "voip_rtp",
                                          kRtpTaskStackBytes, this, kRtpTaskPriority, 1,
                                          this->task_stacks_in_psram_, TAG,
                                          &this->rtp_task_handle_, &this->rtp_task_tcb_,
                                          &this->rtp_task_stack_)) {
    this->stop();
    return false;
  }
  ESP_LOGI(TAG, "SIP listening on UDP+TCP/%u, RTP base UDP/%u", (unsigned) this->sip_port_, (unsigned) this->rtp_port_);
  this->emit_connection_change_(true);
  return true;
}

void SipTransport::request_tcp_client_close_() {
  LockGuard send_lock(this->tcp_send_mutex_);
  const int socket = this->tcp_flow_.socket.load(std::memory_order_acquire);
  this->tcp_flow_.connect_requested.store(false, std::memory_order_release);
  {
    LockGuard lock(this->tcp_tx_pending_mutex_);
    reset_string(this->tcp_flow_.pending);
  }
  this->tcp_flow_.close_requested.store(true, std::memory_order_release);
  if (socket >= 0) shutdown(socket, SHUT_RDWR);
  this->wake_sip_task_();
}

void SipTransport::close_tcp_flow_(SipTcpFlow &flow) {
  LockGuard send_lock(this->tcp_send_mutex_);
  const int socket = flow.socket.exchange(-1, std::memory_order_acq_rel);
  flow.close_requested.store(false, std::memory_order_release);
  flow.peer_ip.store(0, std::memory_order_release);
  if (socket >= 0) close(socket);
  if (flow.connecting_socket >= 0) close(flow.connecting_socket);
  flow.connecting_socket = -1;
  flow.connect_deadline = 0;
  flow.connecting_ip = 0;
  flow.connecting_port = 0;
  reset_string(flow.rx_buffer);
}

void SipTransport::close_tcp_client_from_sip_task_() {
  this->close_tcp_flow_(this->tcp_flow_);
}

bool SipTransport::close_tcp_dialog_() {
  const bool dialog_lost = this->remote_sip_tcp_.load(std::memory_order_acquire) &&
      (!this->call_id_.empty() || this->outgoing_invite_pending_.load(std::memory_order_acquire) ||
       this->media_active_.load(std::memory_order_acquire) || this->terminal_transaction_pending_locked_());
  this->close_tcp_client_from_sip_task_();
  this->tcp_flow_.connect_requested.store(false, std::memory_order_release);
  { LockGuard pending_lock(this->tcp_tx_pending_mutex_); reset_string(this->tcp_flow_.pending); }
    // Completed transactions intentionally survive an ordinary dialog reset
    // so UDP retransmissions can still be answered. A dead TCP connection is
    // different: records tied to that byte stream can neither be completed nor
    // replayed, and retaining awaiting_ack would freeze the next call for 64*T1.
    if (!this->completed_invite_.empty() &&
        !this->completed_invite_.udp) {
      this->completed_invite_.clear();
      this->terminate_after_invite_ack_ = false;
    }
    if (!this->completed_control_.empty() &&
        !this->completed_control_.udp) {
      this->completed_control_.clear();
    }
    if (!this->completed_invite_client_.empty() &&
        !this->completed_invite_client_.udp) {
      this->completed_invite_client_.clear();
    }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
    if (!this->completed_video_direction_invite_.empty() &&
        !this->completed_video_direction_invite_.udp) {
      this->completed_video_direction_invite_.clear();
    }
#endif
  if (dialog_lost && this->end_refer_invite_usage_(false)) return false;
  this->remote_sip_tcp_.store(false, std::memory_order_release);
  if (dialog_lost) this->reset_dialog_();
  return dialog_lost;
}

void SipTransport::handle_tcp_peer_loss_(SipTcpFlow *flow) {
#ifdef USE_ESPHOME_VOIP_MWI
  if (flow == &this->inbound_control_flow_) {
    this->close_tcp_flow_(*flow); this->inbound_control_deadline_ = 0; return;
  }
#endif
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  const int account = this->registration_flow_index_(flow);
  if (account >= 0) {
    bool dialog_lost = false;
    {
      LockGuard lock(this->dialog_mutex_);
      // A main-loop account replacement has already retired this old flow.
      // Its delayed EOF must neither cancel a newly queued record nor apply
      // failure/backoff to the replacement account now occupying the slot.
      if (flow->close_requested.load(std::memory_order_acquire)) {
        this->close_tcp_flow_(*flow);
        return;
      }
      const int lost_socket = flow->socket.load(std::memory_order_acquire);
      this->retire_registration_flow_(account);
      this->close_tcp_flow_(*flow);
      this->registration_.connection_lost(account, millis());
#ifdef USE_ESPHOME_VOIP_MWI
      this->mwi_.connection_lost(account, millis());
#endif
      if (this->refer_dialog_ && this->refer_dialog_->active_account_ == account &&
          !this->refer_dialog_->call_id_.empty() &&
          (this->refer_dialog_->dialog_account_socket_ < 0 ||
           this->refer_dialog_->dialog_account_socket_ == lost_socket)) {
        ReferScope scope(*this);
        if (this->close_tcp_dialog_()) {
          SipSignal signal;
          signal.type = SipSignalType::PROTOCOL_ERROR;
          signal.status_code = 503;
          this->emit_sip_signal_(std::move(signal));
        }
      }
      this->pump_refer_();
      if (this->active_account_ == account && !this->call_id_.empty() &&
          (this->dialog_account_socket_ < 0 || this->dialog_account_socket_ == lost_socket))
        dialog_lost = this->close_tcp_dialog_();
    }
    if (dialog_lost) this->emit_connection_change_(false);
    return;
  }
  if (flow == &this->tcp_flow_ && this->active_account_ >= 0 &&
      this->dialog_account_socket_ != flow->socket.load(std::memory_order_acquire)) {
    // An unrelated inbound TCP peer must never end an account-owned dialog.
    this->close_tcp_flow_(*flow);
    return;
  }
#endif
  bool dialog_lost = false;
  {
    LockGuard lock(this->dialog_mutex_);
    if (this->refer_dialog_ && flow == &this->refer_dialog_->tcp_flow_) {
      {
        ReferScope scope(*this);
        if (this->close_tcp_dialog_()) {
          SipSignal signal;
          signal.type = SipSignalType::PROTOCOL_ERROR;
          signal.status_code = 503;
          this->emit_sip_signal_(std::move(signal));
        }
      }
      this->pump_refer_();
    } else {
      dialog_lost = this->close_tcp_dialog_();
    }
  }
  if (dialog_lost) this->emit_connection_change_(false);
}

SipTransport::SipTcpFlow *SipTransport::tcp_flow_for_socket_(int socket) {
  if (socket < 0) return nullptr;
#ifdef USE_ESPHOME_VOIP_MWI
  if (this->inbound_control_flow_.socket.load(std::memory_order_acquire) == socket) return &this->inbound_control_flow_;
#endif
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  for (auto &flow : this->registration_flows_)
    if (flow.socket.load(std::memory_order_acquire) == socket) return &flow;
#endif
  if (this->tcp_flow_.socket.load(std::memory_order_acquire) == socket) return &this->tcp_flow_;
  if (this->refer_dialog_ && this->refer_dialog_->tcp_flow_.socket.load(std::memory_order_acquire) == socket)
    return &this->refer_dialog_->tcp_flow_;
  return nullptr;
}

void SipTransport::release_refer_dialog_() {
  if (!this->refer_dialog_) return;
  this->close_tcp_flow_(this->refer_dialog_->tcp_flow_);
  this->refer_dialog_.reset();
  this->refer_media_waiting_.store(false, std::memory_order_release);
}

void SipTransport::fail_tcp_flow_(SipTcpFlow &flow, int error) {
  char ip[16];
  struct in_addr address{};
  address.s_addr = htonl(flow.connecting_ip);
  inet_ntoa_r(address, ip, sizeof(ip));
  ESP_LOGW(TAG, "SIP TCP connect to %s:%u failed: %s (%d: %s)", ip,
           (unsigned) flow.connecting_port, socket_errno_name(error), error, socket_errno_text(error));
  this->handle_tcp_peer_loss_(&flow);
}

void SipTransport::promote_tcp_flow_(SipTcpFlow &flow) {
  bool sent = true;
  {
    LockGuard send_lock(this->tcp_send_mutex_);
    flow.socket.store(flow.connecting_socket, std::memory_order_release);
    flow.peer_ip.store(flow.connecting_ip, std::memory_order_release);
    flow.close_requested.store(false, std::memory_order_release);
    flow.connect_requested.store(false, std::memory_order_release);
    reset_string(flow.rx_buffer);
    std::string pending;
    { LockGuard pending_lock(this->tcp_tx_pending_mutex_); pending = flow.pending.str(); flow.pending.clear(); }
    if (!pending.empty()) sent = this->send_sip_tcp_record_(pending, flow.connecting_socket);
    flow.connecting_socket = -1;
    flow.connect_deadline = 0;
    flow.connecting_ip = 0;
    flow.connecting_port = 0;
  }
  if (!sent) this->handle_tcp_peer_loss_(&flow);
}

void SipTransport::prepare_tcp_flow_(SipTcpFlow &flow) {
  if (flow.close_requested.load(std::memory_order_acquire)) this->close_tcp_flow_(flow);
  if (!flow.connect_requested.load(std::memory_order_acquire)) return;
  this->close_tcp_flow_(flow);
  flow.connecting_ip = flow.target_ip.load(std::memory_order_acquire);
  flow.connecting_port = flow.target_port.load(std::memory_order_acquire);
  if (flow.connecting_ip == 0 || flow.connecting_port == 0) {
    this->fail_tcp_flow_(flow, EINVAL);
    return;
  }
  flow.connecting_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (flow.connecting_socket < 0) {
    this->fail_tcp_flow_(flow, errno);
    return;
  }
  int opt = 1;
  setsockopt(flow.connecting_socket, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
  const int flags = fcntl(flow.connecting_socket, F_GETFL, 0);
  fcntl(flow.connecting_socket, F_SETFL, flags | O_NONBLOCK);
  struct sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_addr.s_addr = htonl(flow.connecting_ip);
  target.sin_port = htons(flow.connecting_port);
  const int result = connect(flow.connecting_socket, reinterpret_cast<struct sockaddr *>(&target), sizeof(target));
  if (result == 0) {
    this->promote_tcp_flow_(flow);
  } else if (errno == EINPROGRESS) {
    flow.connect_deadline = millis() + 2000;
    flow.connect_requested.store(false, std::memory_order_release);
  } else {
    this->fail_tcp_flow_(flow, errno);
  }
}

void SipTransport::wake_sip_task_() {
  if (this->sip_task_handle_ != nullptr) {
    xTaskNotifyGive(this->sip_task_handle_);
  }
  const int socket = this->sip_wake_socket_;
  const uint16_t port = this->sip_wake_port_;
  if (socket < 0 || port == 0) return;
  struct sockaddr_in self{};
  self.sin_family = AF_INET;
  self.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  self.sin_port = htons(port);
  constexpr uint8_t WAKE_BYTE = 1;
  if (sendto(socket, &WAKE_BYTE, sizeof(WAKE_BYTE), 0,
             reinterpret_cast<struct sockaddr *>(&self), sizeof(self)) < 0 &&
      !this->running_.load(std::memory_order_acquire)) {
    // During teardown a failed loopback send must not strand select(). The
    // private wake socket is recreated on the next transport start.
    shutdown(socket, SHUT_RDWR);
  }
}

void SipTransport::wake_rtp_task_() {
  const int socket = this->rtp_socket_;
  if (socket >= 0) {
    // The active task is blocked in select(), so wake it through the socket.
    // Also giving a task notification here would survive the inner media loop
    // and produce a second, false quiesced completion before the task parks.
    struct sockaddr_in self{};
    self.sin_family = AF_INET;
    self.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    self.sin_port = htons(this->rtp_port_);
    constexpr uint8_t WAKE_BYTE = 1;
    if (sendto(socket, &WAKE_BYTE, sizeof(WAKE_BYTE), 0,
               reinterpret_cast<struct sockaddr *>(&self), sizeof(self)) < 0) {
      // No periodic timeout is needed: force select() awake and let the RTP
      // worker close this per-call socket after it has left recv.
      shutdown(socket, SHUT_RDWR);
    }
  } else if (this->rtp_task_handle_ != nullptr) {
    // No socket means the task is parked in ulTaskNotifyTake().
    xTaskNotifyGive(this->rtp_task_handle_);
  }
}

void SipTransport::stop() {
  this->transport_stopping_.store(true, std::memory_order_release);
  const bool was_running =
      this->running_.exchange(false, std::memory_order_acq_rel);
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  this->cancel_registration_dns_();
#endif

  // Stop new SIP work first. A strong wait is required here because this
  // method is also the destructor boundary: returning with a worker that can
  // still dereference SipTransport would be a use-after-free, not a safe leak.
  this->tcp_flow_.connect_requested.store(false, std::memory_order_release);
  {
    LockGuard lock(this->tcp_tx_pending_mutex_);
    reset_string(this->tcp_flow_.pending);
  }
  this->tcp_flow_.close_requested.store(true,
                                               std::memory_order_release);
  if (this->sip_task_handle_ != nullptr) {
    xSemaphoreTake(this->sip_task_done_, 0);
    this->wake_sip_task_();
    xSemaphoreTake(this->sip_task_done_, portMAX_DELAY);
    voip_audio_core::cleanup_pinned_task(&this->sip_task_handle_,
                                          &this->sip_task_stack_,
                                          kSipTaskStackBytes);
  }
  if (this->sip_socket_ >= 0) {
    close(this->sip_socket_);
    this->sip_socket_ = -1;
  }
  if (this->sip_tcp_listener_socket_ >= 0) {
    close(this->sip_tcp_listener_socket_);
    this->sip_tcp_listener_socket_ = -1;
  }
  if (this->sip_wake_socket_ >= 0) {
    close(this->sip_wake_socket_);
    this->sip_wake_socket_ = -1;
    this->sip_wake_port_ = 0;
  }

  this->stop_audio_path();
  if (this->rtp_task_handle_ != nullptr) {
    if (!this->rtp_task_quiesced_.load(std::memory_order_acquire)) {
      xSemaphoreTake(this->rtp_cleanup_done_, portMAX_DELAY);
    }
    {
      LockGuard media_lock(this->media_lifecycle_mutex_);
      this->media_lifecycle_phase_.store(
          MediaLifecyclePhase::SHUTTING_DOWN, std::memory_order_release);
    }
    xSemaphoreTake(this->rtp_task_done_, 0);
    this->rtp_task_terminate_.store(true, std::memory_order_release);
    xTaskNotifyGive(this->rtp_task_handle_);
    xSemaphoreTake(this->rtp_task_done_, portMAX_DELAY);
    voip_audio_core::cleanup_pinned_task(&this->rtp_task_handle_,
                                          &this->rtp_task_stack_,
                                          kRtpTaskStackBytes);
  }
  this->release_refer_dialog_();
  for (auto &completed : this->completed_refers_) completed.reset();
  this->refer_signal_pending_ = false;
  this->logical_call_id_.clear();
  this->logical_wire_id_.clear();
  if (was_running) this->emit_connection_change_(false);
}

bool SipTransport::is_connected() const {
  return this->running_.load(std::memory_order_acquire);
}

void SipTransport::disconnect() {
  LockGuard lock(this->dialog_mutex_);
  if (this->terminal_transaction_pending_locked_()) {
    ESP_LOGD(TAG,
             "SIP dialog reset deferred while terminal transaction is pending");
    return;
  }
  this->reset_dialog_();
}

bool SipTransport::start_audio_path() {
  LockGuard media_lock(this->media_lifecycle_mutex_);
  if (!this->running_.load(std::memory_order_acquire) ||
      this->transport_stopping_.load(std::memory_order_acquire)) {
    ESP_LOGW(TAG, "Transport is stopping; media start rejected");
    return false;
  }
  const MediaLifecyclePhase phase =
      this->media_lifecycle_phase_.load(std::memory_order_acquire);
  if (phase == MediaLifecyclePhase::CLEANING ||
      phase == MediaLifecyclePhase::SHUTTING_DOWN) {
    ESP_LOGW(TAG, "Media lifecycle is not ready for a new session");
    return false;
  }
  return this->prepare_media_path_locked_() &&
         this->commit_media_path_locked_();
}

bool SipTransport::prepare_media_path() {
  LockGuard media_lock(this->media_lifecycle_mutex_);
  if (!this->running_.load(std::memory_order_acquire) ||
      this->transport_stopping_.load(std::memory_order_acquire)) {
    ESP_LOGW(TAG, "Transport is stopping; media prepare rejected");
    return false;
  }
  const MediaLifecyclePhase phase =
      this->media_lifecycle_phase_.load(std::memory_order_acquire);
  if (phase == MediaLifecyclePhase::PREPARED ||
      (phase == MediaLifecyclePhase::ACTIVE &&
       this->rtp_running_.load(std::memory_order_acquire))) {
    return true;
  }
  if (phase != MediaLifecyclePhase::IDLE) {
    ESP_LOGW(TAG, "Media lifecycle is not ready for preparation");
    return false;
  }
  return this->prepare_media_path_locked_();
}

bool SipTransport::prepare_media_path_locked_() {
  const MediaLifecyclePhase phase =
      this->media_lifecycle_phase_.load(std::memory_order_acquire);
  if (phase == MediaLifecyclePhase::PREPARED ||
      (phase == MediaLifecyclePhase::ACTIVE &&
       this->rtp_running_.load(std::memory_order_acquire))) {
    return true;
  }
  if (phase != MediaLifecyclePhase::IDLE) {
    ESP_LOGE(TAG, "Media lifecycle is not idle");
    return false;
  }
  if (this->rtp_task_handle_ == nullptr ||
      this->rtp_task_terminate_.load(std::memory_order_acquire) ||
      !this->rtp_task_quiesced_.load(std::memory_order_acquire)) {
    ESP_LOGE(TAG, "RTP task is unavailable for a new media session");
    return false;
  }
  this->reset_rtp_latch_();
  this->audio_rx_discontinuity_.store(false, std::memory_order_release);
  this->rtp_sequence_.store(static_cast<uint16_t>(esp_random()), std::memory_order_release);
  this->rtp_timestamp_.store(esp_random(), std::memory_order_release);
  this->rtp_ssrc_ = esp_random();
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
  AudioFormat sink_format;
  RtpAudioPayloads rx_payloads;
  this->get_media_config_(nullptr, &sink_format, nullptr, nullptr, &rx_payloads);
  if (sink_format.is_valid() &&
      !this->pcm_receive_converter_.prepare(rx_payloads.pcm_only(), decoded_pcm_format(sink_format))) {
    ESP_LOGE(TAG, "Unable to prepare negotiated PCM receive formats");
    return false;
  }
#endif
#ifdef USE_ESPHOME_VOIP_STACK_OPUS
  AudioFormat prepared_tx;
  AudioFormat prepared_rx;
  RtpAudioPayloads prepared_payloads;
  this->get_media_config_(&prepared_tx, &prepared_rx, nullptr, nullptr, &prepared_payloads);
  // The preferred playout codec can be PCM while an advertised Opus fallback
  // uses the same PCM sink. Validate every decoder that can receive packets.
  for (uint8_t i = 0; i < prepared_payloads.count; ++i) {
    if (prepared_payloads.entries[i].format.codec == AudioCodec::OPUS) {
      if (!same_decoded_audio_geometry(prepared_payloads.entries[i].format, prepared_rx)) return false;
      prepared_rx = prepared_payloads.entries[i].format;
      break;
    }
  }
  if (!this->opus_codec_.ready_for(prepared_tx, prepared_rx)) {
    ESP_LOGE(TAG, "Negotiated Opus format was not preallocated at setup");
    return false;
  }
  this->opus_codec_.reset();
#endif
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_DEBUG
  this->audio_tx_slow_send_calls_ = 0;
  this->audio_tx_send_failures_ = 0;
  this->audio_tx_max_send_us_ = 0;
  this->audio_tx_last_debug_log_ms_ = millis();
#endif
  if (!this->bind_udp_(&this->rtp_socket_, this->rtp_port_, "RTP")) return false;
  xSemaphoreTake(this->rtp_cleanup_done_, 0);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  if (this->video_negotiated_) {
    if (!this->prepare_video_session_locked_()) {
      // Video is subordinate media. A late PSRAM/codec failure must not reject
      // an otherwise healthy SIP audio answer.
      ESP_LOGE(TAG,
               "Negotiated video failed to prepare; answering audio-only");
      if (this->video_session_ != nullptr)
        this->video_session_->request_stop();
      this->video_negotiated_ = false;
    }
  }
#endif
  // PREPARED owns the bound sockets and dormant codec resources, but the RTP
  // worker, camera source and renderer direction remain inactive until the
  // final SIP answer has been sent successfully.
  this->media_lifecycle_phase_.store(MediaLifecyclePhase::PREPARED,
                                      std::memory_order_release);
  return true;
}

bool SipTransport::commit_media_path() {
  bool committed = false;
  {
    LockGuard media_lock(this->media_lifecycle_mutex_);
    if (!this->running_.load(std::memory_order_acquire) ||
        this->transport_stopping_.load(std::memory_order_acquire)) {
      ESP_LOGW(TAG, "Transport is stopping; media commit rejected");
    } else {
      committed = this->commit_media_path_locked_();
    }
  }
  if (!committed)
    this->terminate_dialog_after_media_commit_failure_();
  return committed;
}

bool SipTransport::commit_media_path_locked_() {
  const MediaLifecyclePhase phase =
      this->media_lifecycle_phase_.load(std::memory_order_acquire);
  if (phase == MediaLifecyclePhase::ACTIVE &&
      this->rtp_running_.load(std::memory_order_acquire)) {
    return true;
  }
  if (phase != MediaLifecyclePhase::PREPARED) {
    ESP_LOGE(TAG, "Media lifecycle is not prepared");
    return false;
  }
  if (this->rtp_task_handle_ == nullptr ||
      this->rtp_task_terminate_.load(std::memory_order_acquire) ||
      !this->rtp_task_quiesced_.load(std::memory_order_acquire) ||
      this->rtp_socket_ < 0) {
    ESP_LOGE(TAG, "Prepared RTP resources are unavailable");
    return false;
  }

  this->rtp_task_quiesced_.store(false, std::memory_order_release);
  this->rtp_running_.store(true, std::memory_order_release);
  this->media_lifecycle_phase_.store(MediaLifecyclePhase::ACTIVE,
                                      std::memory_order_release);
  xTaskNotifyGive(this->rtp_task_handle_);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  if (this->video_negotiated_ &&
      !this->video_session_->request_media_direction(
          this->video_send_enabled_, this->video_receive_enabled_)) {
    // Video remains subordinate after the SIP transaction commits.
    ESP_LOGE(TAG, "Negotiated video failed to activate; keeping audio active");
    this->video_session_->request_stop();
    this->video_negotiated_ = false;
  }
  this->emit_video_active_state_(
      this->video_negotiated_ && this->video_session_ != nullptr &&
      this->video_session_->is_running() &&
      (this->video_send_enabled_ || this->video_receive_enabled_));
  this->emit_video_send_state_(
      this->video_negotiated_ && this->video_send_enabled_, false);
#endif
  return true;
}

void SipTransport::terminate_dialog_after_media_commit_failure_() {
  // commit_media_path() is the UAS post-200 path. Observe the server
  // transaction under its owning mutex only after releasing the media mutex,
  // preserving the established dialog->media lock order.
  LockGuard dialog_lock(this->dialog_mutex_);
  const bool accepted_dialog =
      !this->call_id_.empty() && !this->completed_invite_.empty() &&
      this->completed_invite_.status >= 200 &&
      this->completed_invite_.status < 300 &&
      this->completed_invite_.call_id == this->call_id_;
  if (!accepted_dialog) return;

  if (this->defer_bye_until_uas_ack_locked_()) return;

  // The ACK may have raced the main-loop commit. In that case the dialog is
  // already confirmed and can be terminated immediately with the normal BYE
  // transaction rather than being reset locally.
  if (!this->send_bye_unlocked_(this->call_id_)) {
    ESP_LOGE(TAG,
             "Failed to send BYE after post-answer media commit failure");
  }
}

void SipTransport::abort_media_path() {
  this->stop_audio_path();
}

void SipTransport::stop_audio_path() {
  LockGuard media_lock(this->media_lifecycle_mutex_);
  this->request_audio_path_stop_locked_();
}

void SipTransport::request_audio_path_stop_locked_() {
  this->close_media_session_();
  const MediaLifecyclePhase phase =
      this->media_lifecycle_phase_.load(std::memory_order_acquire);
  if (phase == MediaLifecyclePhase::CLEANING ||
      phase == MediaLifecyclePhase::SHUTTING_DOWN) {
    return;
  }
  const bool audio_was_running =
      this->rtp_running_.exchange(false, std::memory_order_acq_rel);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  const bool video_was_running =
      this->video_session_ != nullptr &&
      this->video_session_->is_running();
  if (video_was_running) this->emit_video_active_state_(false);
#else
  constexpr bool video_was_running = false;
#endif
  if (phase == MediaLifecyclePhase::IDLE && !audio_was_running &&
      !video_was_running) {
    return;
  }
  this->media_lifecycle_phase_.store(MediaLifecyclePhase::CLEANING,
                                      std::memory_order_release);
  if (this->rtp_task_handle_ == nullptr) {
    // setup()/destructor failure path only: no worker can own these resources.
    this->finish_audio_path_stop_();
    this->rtp_task_quiesced_.store(true, std::memory_order_release);
    this->media_lifecycle_phase_.store(MediaLifecyclePhase::IDLE,
                                        std::memory_order_release);
    return;
  }
  if (this->rtp_cleanup_done_ != nullptr)
    xSemaphoreTake(this->rtp_cleanup_done_, 0);
  this->rtp_task_quiesced_.store(false, std::memory_order_release);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  // Atomic gate+wake only. RTCP, source/sink callbacks and joins belong to the
  // RTP lifecycle worker, never to a SIP or ESPHome-loop caller.
  if (this->video_session_ != nullptr)
    this->video_session_->request_stop();
#endif
  if (audio_was_running)
    this->wake_rtp_task_();
  else
    xTaskNotifyGive(this->rtp_task_handle_);
}

void SipTransport::finish_audio_path_stop_() {
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  if (this->video_session_ != nullptr) this->video_session_->stop();
#endif
  LockGuard socket_lock(this->rtp_socket_mutex_);
  if (this->rtp_socket_ >= 0) {
    close(this->rtp_socket_);
    this->rtp_socket_ = -1;
  }
}

bool SipTransport::originate(const std::string &host, uint16_t port) {
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  if (this->next_account_ >= 0) {
    if (!this->parse_remote_(host)) return false;
    this->remote_sip_port_.store(port ? port : 5060, std::memory_order_release);
    return true;  // The persistent account flow already owns this next hop.
  }
#endif
  if (!this->parse_remote_(host)) return false;
  const uint16_t sip_port = port ? port : 5060;
  this->remote_sip_port_.store(sip_port, std::memory_order_release);
  if (!this->remote_sip_tcp_.load(std::memory_order_acquire)) {
    ESP_LOGI(TAG, "SIP UDP originate target set to %s:%u", host.c_str(), (unsigned) sip_port);
    return true;
  }

  this->request_tcp_client_close_();
  const uint32_t ip_v4 = this->remote_ip_v4_.load(std::memory_order_acquire);
  if (ip_v4 == 0) return false;
  {
    LockGuard lock(this->tcp_tx_pending_mutex_);
    reset_string(this->tcp_flow_.pending);
  }
  this->tcp_flow_.target_ip.store(ip_v4, std::memory_order_release);
  this->tcp_flow_.target_port.store(sip_port, std::memory_order_release);
  this->tcp_flow_.connect_requested.store(true, std::memory_order_release);
  this->wake_sip_task_();
  return true;
}

void SipTransport::set_remote(const std::string &ip, uint16_t port, uint16_t rtp_port) {
  this->parse_remote_(ip);
  if (port) this->remote_sip_port_.store(port, std::memory_order_release);
  if (rtp_port) this->remote_rtp_port_.store(rtp_port, std::memory_order_release);
}

void SipTransport::set_sip_signaling_transport(bool tcp) {
  const bool was_tcp = this->remote_sip_tcp_.exchange(tcp, std::memory_order_acq_rel);
  if (!tcp && was_tcp) this->request_tcp_client_close_();
}

void SipTransport::clear_invite_transaction_() {
  this->pending_invite_.clear();
}

void SipTransport::clear_bye_transaction_() {
  this->pending_bye_.clear();
}

void SipTransport::clear_udp_transactions_() {
  this->pending_invite_.clear();
  this->pending_cancel_.clear();
  this->pending_bye_.clear();
  this->pending_update_.clear();
}

void SipTransport::reset_rtp_latch_() {
  this->latched_rtp_ip_v4_.store(0, std::memory_order_release);
  this->latched_rtp_port_.store(0, std::memory_order_release);
  this->latched_rtp_ssrc_.store(0, std::memory_order_release);
  this->rtp_ssrc_latched_.store(false, std::memory_order_release);
}

void SipTransport::open_media_session_() {
  if (this->refer_context_active_) return;
  this->media_active_.store(true, std::memory_order_release);
}

void SipTransport::close_media_session_() {
  if (this->refer_context_active_) return;
  this->media_active_.store(false, std::memory_order_release);
}

void SipTransport::reset_dialog_() {
  // Callers already own dialog_mutex_. Holding the media lock until every
  // negotiated field is cleared prevents a deferred main-loop start from
  // observing the old video configuration after teardown has completed.
  LockGuard media_lock(this->media_lifecycle_mutex_);
  if (!this->refer_context_active_)
    this->refer_media_waiting_.store(false, std::memory_order_release);
  this->reset_dialog_media_locked_();
}

void SipTransport::reset_dialog_media_locked_() {
  if (!this->refer_context_active_) this->request_audio_path_stop_locked_();
  bool abort_queued_tcp_record = false;
  {
    // Serialize with promote_tcp_flow_(): once that path swaps the queued
    // record it owns transmission; before the swap a reset can still retract
    // the record without leaving an orphan request at the peer.
    LockGuard send_lock(this->tcp_send_mutex_);
    LockGuard pending_lock(this->tcp_tx_pending_mutex_);
    abort_queued_tcp_record = !this->tcp_flow_.pending.empty();
    reset_string(this->tcp_flow_.pending);
    if (abort_queued_tcp_record) {
      this->tcp_flow_.connect_requested.store(false, std::memory_order_release);
      this->tcp_flow_.close_requested.store(true,
                                                   std::memory_order_release);
    }
  }
  if (abort_queued_tcp_record) this->wake_sip_task_();
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  {
    this->active_account_ = -1; this->dialog_digest_ = {}; this->dialog_proxy_digest_ = {};
    this->dialog_auth_attempts_ = 0; this->dialog_proxy_auth_attempts_ = 0;
    this->authenticated_invite_body_.clear(); this->invite_authorization_.clear();
    this->dialog_account_socket_ = -1; this->dialog_route_count_ = 0;
  }
#endif
  reset_string(this->call_id_);
  reset_string(this->local_tag_);
  reset_string(this->remote_tag_);
  reset_string(this->branch_);
  reset_string(this->local_uri_);
  reset_string(this->local_contact_uri_);
  reset_string(this->remote_uri_);
  reset_string(this->remote_target_uri_);
  reset_string(this->last_invite_via_);
  reset_string(this->last_invite_from_);
  reset_string(this->last_invite_to_);
  reset_string(this->last_invite_cseq_);
  reset_string(this->last_invite_response_);
  this->last_invite_cseq_number_ = 0;
  this->remote_dialog_cseq_ = 0;
  this->last_invite_peer_ip_v4_ = 0;
  this->last_invite_peer_port_ = 0;
  this->caller_.clear();
  this->destination_.clear();
  this->peer_supports_from_change_ = false;
  this->connected_identity_sent_ = false;
  this->dialog_originated_ = false;
  this->delayed_offer_pending_ = false;
  this->remote_directional_audio_v1_ = false;
  this->remote_media_shape_.clear();
  this->close_media_session_();
  this->outgoing_invite_pending_.store(false, std::memory_order_release);
  this->cancel_requested_.store(false, std::memory_order_release);
  this->terminate_after_invite_ack_ = false;
  this->clear_udp_transactions_();
  this->pending_provisional_response_.clear();
  if (!this->refer_context_active_) this->reset_rtp_latch_();
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  if (!this->refer_context_active_) {
  this->pending_video_direction_invite_.clear();
  reset_string(this->confirmed_local_sdp_);
  this->reset_video_negotiation_();
  this->emit_video_active_state_(false);
  this->emit_video_send_state_(false, false);
  }
#endif
  this->sdp_session_id_ = 0;
  this->sdp_session_version_ = 0;
}

bool SipTransport::terminal_transaction_pending_locked_() const {
  return !this->pending_cancel_.empty() || !this->pending_bye_.empty() ||
         this->completed_invite_.awaiting_ack ||
         this->terminate_after_invite_ack_;
}

void SipTransport::remember_udp_transaction_(const std::string &method, const std::string &message,
                                             uint32_t ip_v4, uint16_t port) {
  if (message.empty() || ip_v4 == 0 || port == 0) {
    return;
  }
  UdpTransaction *txn = nullptr;
  if (method == "NOTIFY") {
    if (!this->refer_dialog_) return;
    txn = this->refer_dialog_->pending_notify_.empty()
        ? &this->refer_dialog_->pending_notify_ : &this->refer_dialog_->pending_notify_final_;
  } else if (method == "INVITE") {
    txn = &this->pending_invite_;
  } else if (method == "CANCEL") {
    txn = &this->pending_cancel_;
  } else if (method == "BYE") {
    txn = &this->pending_bye_;
  } else if (method == "UPDATE") {
    txn = &this->pending_update_;
  }
  if (txn == nullptr) return;
  const uint32_t now = millis();
  txn->request = message;
  txn->ip_v4 = ip_v4;
  txn->port = port;
  txn->interval_ms = SIP_T1_MS;
  txn->next_ms = now + txn->interval_ms;
  txn->deadline_ms = now + SIP_TRANSACTION_TIMEOUT_MS;
  txn->retries = 0;
  txn->udp = !this->remote_sip_tcp_.load(std::memory_order_acquire);
  txn->completed = false;
  this->wake_sip_task_();
}

void SipTransport::defer_udp_provisional_response_(
    const std::string &response, uint32_t ip_v4, uint16_t port) {
  if (response.empty() || ip_v4 == 0 || port == 0) return;
  const uint32_t now = millis();
  this->pending_provisional_response_.response = response;
  this->pending_provisional_response_.ip_v4 = ip_v4;
  this->pending_provisional_response_.port = port;
  this->pending_provisional_response_.interval_ms = 20;
  this->pending_provisional_response_.next_ms = now + 20;
  this->pending_provisional_response_.deadline_ms = now + SIP_T2_MS;
  this->pending_provisional_response_.retries = 0;
  this->wake_sip_task_();
  ESP_LOGW(TAG, "SIP UDP provisional response deferred to event-driven retry");
}

void SipTransport::pump_udp_retransmits_() {
  LockGuard lock(this->dialog_mutex_);
  this->pump_udp_retransmits_unlocked_();
}

void SipTransport::pump_udp_retransmits_unlocked_() {
  const uint32_t now = millis();
  bool reset_terminal_dialog = false;
  bool invite_timed_out = false;
  bool ack_timed_out = false;
  std::string timed_out_call_id;
  auto pump = [this, now, &reset_terminal_dialog, &invite_timed_out,
               &timed_out_call_id](UdpTransaction &txn, const char *method) {
    if (txn.empty() || txn.ip_v4 == 0 || txn.port == 0) return;
    if (time_reached(now, txn.deadline_ms)) {
      ESP_LOGW(TAG, "SIP %s %s transaction timed out after %u ms",
               txn.udp ? "UDP" : "TCP", method,
               (unsigned) SIP_TRANSACTION_TIMEOUT_MS);
      txn.clear();
      if (std::strcmp(method, "INVITE") == 0) {
        invite_timed_out = true;
        timed_out_call_id = this->call_id_;
        this->outgoing_invite_pending_.store(false, std::memory_order_release);
      } else if (std::strcmp(method, "UPDATE") != 0 && std::strcmp(method, "NOTIFY") != 0) {
        reset_terminal_dialog = true;
        timed_out_call_id = this->call_id_;
      }
      return;
    }
    if (!time_reached(now, txn.next_ms)) return;
    if (txn.completed) {
      txn.next_ms = txn.deadline_ms;
      return;
    }
    if (!txn.udp) {
      txn.next_ms = txn.deadline_ms;
      return;
    }
    const bool sent = this->send_sip_(txn.request, txn.ip_v4, txn.port);
    txn.retries++;
    if (sent) {
      ESP_LOGD(TAG, "SIP UDP %s retransmit #%u", method, (unsigned) txn.retries);
    } else {
      ESP_LOGW(TAG, "SIP UDP %s retransmit #%u failed", method, (unsigned) txn.retries);
    }
    txn.interval_ms = std::min<uint16_t>(static_cast<uint16_t>(txn.interval_ms * 2), SIP_T2_MS);
    txn.next_ms = now + txn.interval_ms;
    if (time_reached(txn.next_ms, txn.deadline_ms)) txn.next_ms = txn.deadline_ms;
  };

  if (this->outgoing_invite_pending_.load(std::memory_order_acquire)) {
    pump(this->pending_invite_, "INVITE");
  }
  pump(this->pending_cancel_, "CANCEL");
  pump(this->pending_bye_, "BYE");
  pump(this->pending_update_, "UPDATE");
  if (!this->refer_context_active_ && this->refer_dialog_) {
    pump(this->refer_dialog_->pending_notify_, "NOTIFY");
    pump(this->refer_dialog_->pending_notify_final_, "NOTIFY");
  }
  this->pump_refer_();
  auto &provisional = this->pending_provisional_response_;
  if (!provisional.empty() && time_reached(now, provisional.next_ms)) {
    if (time_reached(now, provisional.deadline_ms)) {
      ESP_LOGW(TAG, "SIP UDP provisional response retry expired");
      provisional.clear();
    } else {
      const bool sent = this->send_sip_(provisional.response,
                                        provisional.ip_v4,
                                        provisional.port);
      provisional.retries++;
      if (sent) {
        ESP_LOGI(TAG, "SIP UDP provisional response sent after %u retries",
                 static_cast<unsigned>(provisional.retries));
        provisional.clear();
      } else {
        provisional.interval_ms = std::min<uint16_t>(
            static_cast<uint16_t>(provisional.interval_ms * 2), SIP_T1_MS);
        provisional.next_ms = now + provisional.interval_ms;
        if (time_reached(provisional.next_ms, provisional.deadline_ms)) {
          provisional.next_ms = provisional.deadline_ms;
        }
      }
    }
  }
  if (this->completed_invite_.awaiting_ack) {
    if (time_reached(now, this->completed_invite_.deadline_ms)) {
      ESP_LOGW(TAG, "SIP %s INVITE final response timed out waiting for ACK after %u ms",
               this->completed_invite_.udp ? "UDP" : "TCP",
               (unsigned) SIP_TRANSACTION_TIMEOUT_MS);
      this->completed_invite_.awaiting_ack = false;
      const bool active_2xx_dialog = this->completed_invite_.status < 300 &&
                                     !this->call_id_.empty() &&
                                     this->call_id_ == this->completed_invite_.call_id &&
                                     this->last_invite_cseq_number_ == this->completed_invite_.cseq;
      if (active_2xx_dialog) {
        this->terminate_after_invite_ack_ = false;
        ack_timed_out = true;
        timed_out_call_id = this->call_id_;
      }
    } else if ((this->completed_invite_.udp ||
                this->completed_invite_.status < 300) &&
               time_reached(now, this->completed_invite_.next_retransmit_ms)) {
      const bool sent = this->send_sip_(this->completed_invite_.response,
                                        this->completed_invite_.peer_ip_v4,
                                        this->completed_invite_.peer_port);
      this->completed_invite_.retransmits++;
      if (sent) {
        ESP_LOGD(TAG, "SIP UDP INVITE final response retransmit #%u",
                 (unsigned) this->completed_invite_.retransmits);
      } else {
        ESP_LOGW(TAG, "SIP UDP INVITE final response retransmit #%u failed",
                 (unsigned) this->completed_invite_.retransmits);
      }
      this->completed_invite_.retransmit_interval_ms =
          std::min<uint16_t>(static_cast<uint16_t>(this->completed_invite_.retransmit_interval_ms * 2),
                             SIP_T2_MS);
      this->completed_invite_.next_retransmit_ms =
          now + this->completed_invite_.retransmit_interval_ms;
      if (time_reached(this->completed_invite_.next_retransmit_ms,
                       this->completed_invite_.deadline_ms)) {
        this->completed_invite_.next_retransmit_ms = this->completed_invite_.deadline_ms;
      }
    }
  }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  // This one in-dialog client transaction is also pumped for TCP: TCP does
  // not retransmit, but its final-response timeout and 491 retry deadline must
  // still wake the same event-driven SIP select loop.
  this->pump_video_direction_transaction_();
#endif
  // Completed transaction records are kept for 64*T1 so delayed UDP
  // retransmissions can be replayed. Expire them on the SIP task deadline,
  // even when no further packet arrives. Otherwise their std::string storage
  // survives every completed call and gradually consumes the small internal
  // heap needed by lwIP for the next socket send.
  auto expire_completed = [now](auto &transaction) {
    if (!transaction.empty() &&
        time_reached(now, transaction.completed_ms +
                              SIP_TRANSACTION_TIMEOUT_MS)) {
      transaction.clear();
    }
  };
  expire_completed(this->completed_invite_);
  expire_completed(this->completed_control_);
  expire_completed(this->completed_invite_client_);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  expire_completed(this->completed_video_direction_invite_);
#endif
  if (reset_terminal_dialog) {
    this->reset_dialog_();
    SipSignal signal;
    signal.type = SipSignalType::TERMINAL_COMPLETE;
    signal.status_code = 408;
    signal.call_id = timed_out_call_id;
    signal.reason = "terminal_timeout";
    this->emit_sip_signal_(std::move(signal));
  }
  if (invite_timed_out) {
    SipSignal signal;
    signal.type = SipSignalType::FINAL_RESPONSE;
    signal.status_code = 408;
    signal.call_id = timed_out_call_id;
    signal.reason = "timeout";
    this->reset_dialog_();
    this->emit_sip_signal_(std::move(signal));
  }
  if (ack_timed_out) {
    SipSignal signal;
    signal.type = SipSignalType::PROTOCOL_ERROR;
    signal.status_code = 408;
    signal.call_id = timed_out_call_id;
    signal.reason = "ack_timeout";
    const bool bye_pending = this->send_bye_unlocked_(timed_out_call_id);
    signal.terminal_transaction_pending = bye_pending;
    if (!bye_pending) this->reset_dialog_();
    this->emit_sip_signal_(std::move(signal));
  }
}

bool SipTransport::local_ip_for_peer_(uint32_t peer_ip_v4, std::string *out) const {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  bool ok = false;
  if (fd >= 0) {
    struct sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(9);
    peer.sin_addr.s_addr = htonl(peer_ip_v4);
    if (connect(fd, reinterpret_cast<struct sockaddr *>(&peer), sizeof(peer)) == 0) {
      struct sockaddr_in local{};
      socklen_t len = sizeof(local);
      if (getsockname(fd, reinterpret_cast<struct sockaddr *>(&local), &len) == 0 &&
          local.sin_addr.s_addr != 0) {
        char ip[16];
        inet_ntoa_r(local.sin_addr, ip, sizeof(ip));
        *out = ip;
        ok = true;
      }
    }
    close(fd);
  }
  if (ok) return true;
  char ip[network::IP_ADDRESS_BUFFER_SIZE];
  for (auto &address : network::get_ip_addresses()) {
    if (!address.is_ip4()) continue;
    address.str_to(ip);
    if (std::strcmp(ip, "0.0.0.0") != 0) {
      *out = ip;
      ESP_LOGW(TAG, "SIP local IP selected %s for peer %08x", ip, (unsigned) peer_ip_v4);
      return true;
    }
  }
  return false;
}

bool SipTransport::send_sip_(const std::string &message, uint32_t ip_v4, uint16_t port) {
  return this->send_sip_bytes_(message.data(), message.size(), ip_v4, port);
}

bool SipTransport::send_sip_(const SipSignalingString &message, uint32_t ip_v4,
                             uint16_t port) {
  return this->send_sip_bytes_(message.data(), message.size(), ip_v4, port);
}

bool SipTransport::send_sip_bytes_(const char *data, size_t size,
                                   uint32_t ip_v4, uint16_t port) {
  if (this->remote_sip_tcp_.load(std::memory_order_acquire)) {
    return this->send_sip_tcp_bytes_(data, size);
  }
  return this->send_sip_udp_bytes_(data, size, ip_v4, port);
}

bool SipTransport::send_sip_udp_bytes_(const char *data, size_t size,
                                      uint32_t ip_v4, uint16_t port) {
  if (this->sip_socket_ < 0 || ip_v4 == 0 || port == 0) return false;
  struct sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_addr.s_addr = htonl(ip_v4);
  dest.sin_port = htons(port);
  constexpr uint8_t MAX_ATTEMPTS = 3;
  int sent = -1;
  int err = 0;
  for (uint8_t attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
    sent = sendto(this->sip_socket_, data, size, 0,
                  reinterpret_cast<struct sockaddr *>(&dest), sizeof(dest));
    if (sent == static_cast<int>(size)) break;
    err = errno;
    if (!socket_errno_is_transient_tx_pressure(err) || attempt + 1 == MAX_ATTEMPTS) break;
    ESP_LOGW(TAG, "SIP TX deferred by %s; retrying (%u/%u)", socket_errno_name(err),
             static_cast<unsigned>(attempt + 1), static_cast<unsigned>(MAX_ATTEMPTS));
    delay(2);
  }
  if (sent != static_cast<int>(size)) {
    ESP_LOGW(TAG, "SIP TX failed: %s (%d: %s)", socket_errno_name(err), err,
             socket_errno_text(err));
    return false;
  }
  char ip[16];
  struct in_addr a{};
  a.s_addr = htonl(ip_v4);
  inet_ntoa_r(a, ip, sizeof(ip));
  ESP_LOGI(TAG, "SIP TX %u bytes to %s:%u", (unsigned) size, ip, (unsigned) port);
  return true;
}

bool SipTransport::send_sip_tcp_(const std::string &message) {
  return this->send_sip_tcp_bytes_(message.data(), message.size());
}

bool SipTransport::send_sip_tcp_bytes_(const char *data, size_t size) {
  // SIP commands can originate from the component/main task while the SIP
  // task sends responses. Serialize the complete record so partial TCP writes
  // from two callers can never interleave into an invalid SIP message.
  LockGuard send_lock(this->tcp_send_mutex_);
  if (size == 0) return false;
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  if (this->dispatch_socket_ >= 0 &&
      ((size >= 8 && std::memcmp(data, "SIP/2.0 ", 8) == 0) ||
       (size >= 4 && std::memcmp(data, "ACK ", 4) == 0)))
    return this->send_sip_tcp_record_(data, size, this->dispatch_socket_);
  if (this->active_account_ >= 0) {
    const int socket = this->dialog_account_socket_ >= 0 ? this->dialog_account_socket_ :
        this->registration_flows_[this->active_account_].socket.load(std::memory_order_acquire);
    return this->send_sip_tcp_record_(data, size, socket);
  }
#endif
  const bool replacing_session =
      this->tcp_flow_.close_requested.load(std::memory_order_acquire) ||
      this->tcp_flow_.connect_requested.load(std::memory_order_acquire);
  const int socket = this->tcp_flow_.socket.load(std::memory_order_acquire);
  if (socket < 0 || replacing_session) {
    if (this->remote_sip_tcp_.load(std::memory_order_acquire)) {
      {
        LockGuard lock(this->tcp_tx_pending_mutex_);
        if (!this->tcp_flow_.pending.empty()) {
          ESP_LOGW(TAG, "SIP TCP connect still pending; refusing to replace queued %s with a newer request",
                   this->tcp_flow_.pending.rfind("INVITE ", 0) == 0 ? "INVITE" : "message");
          return false;
        }
        this->tcp_flow_.pending.assign(data, size);
      }
      this->wake_sip_task_();
      return true;
    }
    return false;
  }
  return this->send_sip_tcp_record_(data, size, socket);
}

bool SipTransport::send_sip_tcp_record_(const std::string &message, int socket) {
  return this->send_sip_tcp_record_(message.data(), message.size(), socket);
}

bool SipTransport::send_sip_tcp_record_(const char *data, size_t size,
                                        int socket) {
  // Caller owns tcp_send_mutex_. Keep this primitive allocation-free so the
  // SIP task can flush the one queued record atomically when connect finishes.
  if (size == 0 || socket < 0) return false;
  size_t sent_total = 0;
  while (sent_total < size) {
    const int sent = send(socket, data + sent_total, size - sent_total, 0);
    if (sent <= 0) {
      const int err = errno;
      ESP_LOGW(TAG, "SIP TCP TX failed: %s (%d: %s)", socket_errno_name(err), err, socket_errno_text(err));
      // A partial SIP record cannot be resumed by a later caller. Closing the
      // stream lets the SIP task perform one coherent dialog teardown.
      shutdown(socket, SHUT_RDWR);
      return false;
    }
    sent_total += static_cast<size_t>(sent);
  }
  ESP_LOGI(TAG, "SIP TCP TX %u bytes", (unsigned) size);
  return true;
}

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
std::string SipTransport::build_video_direction_offer_(
    bool enabled, uint32_t session_version) const {
  if (this->confirmed_local_sdp_.empty()) return "";
  std::string offer = this->confirmed_local_sdp_;

  const size_t origin = offer.find("o=- ");
  const size_t origin_end =
      origin == std::string::npos ? std::string::npos
                                  : offer.find("\r\n", origin);
  if (origin == std::string::npos || origin_end == std::string::npos)
    return "";
  const size_t network = offer.find(" IN IP4 ", origin);
  if (network == std::string::npos || network >= origin_end) return "";
  offer.replace(origin, origin_end - origin,
                "o=- " + std::to_string(this->sdp_session_id_) + " " +
                    std::to_string(session_version) +
                    offer.substr(network, origin_end - network));

  const size_t video = offer.find("m=video ");
  if (video == std::string::npos) return "";
  const size_t video_line_end = offer.find("\r\n", video);
  if (video_line_end == std::string::npos ||
      offer.compare(video, sizeof("m=video 0") - 1, "m=video 0") == 0) {
    return "";
  }
  size_t video_end = offer.find("\r\nm=", video_line_end);
  if (video_end == std::string::npos) video_end = offer.size();
  const bool receive = this->video_receive_enabled_;
  const char *direction =
      enabled && receive ? "a=sendrecv"
      : enabled          ? "a=sendonly"
      : receive          ? "a=recvonly"
                         : "a=inactive";
  const char *directions[] = {
      "a=sendrecv", "a=sendonly", "a=recvonly", "a=inactive"};
  size_t direction_pos = std::string::npos;
  size_t direction_len = 0;
  for (const char *candidate : directions) {
    const size_t found = offer.find(candidate, video_line_end);
    if (found != std::string::npos && found < video_end) {
      direction_pos = found;
      direction_len = std::strlen(candidate);
      break;
    }
  }
  if (direction_pos != std::string::npos) {
    offer.replace(direction_pos, direction_len, direction);
  } else if (video_end == offer.size() && offer.size() >= 2 &&
             offer.compare(offer.size() - 2, 2, "\r\n") == 0) {
    offer.append(direction).append("\r\n");
  } else {
    offer.insert(video_end, std::string("\r\n") + direction);
  }
  return offer;
}

bool SipTransport::send_video_direction_reinvite_unlocked_(bool enabled,
                                                           bool retry) {
  if (!this->media_active_.load(std::memory_order_acquire) ||
      !this->video_negotiated_ || this->video_source_ == nullptr ||
      this->remote_tag_.empty()) {
    return false;
  }
  const bool previous_send =
      retry ? this->pending_video_direction_invite_.previous_send
            : this->video_send_enabled_;
  const uint8_t retry_count =
      retry ? this->pending_video_direction_invite_.retry_count : 0;
  const uint32_t session_version =
      retry ? this->pending_video_direction_invite_.session_version
            : this->sdp_session_version_ + 1;
  const std::string offer =
      retry ? this->pending_video_direction_invite_.offered_sdp
            : this->build_video_direction_offer_(enabled, session_version);
  if (offer.empty()) return false;

  // The first locally originated offer may be a video update to an incoming
  // dialog. Preserve its existing audio contract for answer validation too.
  AudioFormat offered_tx;
  this->get_media_config_(&offered_tx, nullptr, nullptr, nullptr,
                          &this->local_offered_rx_payloads_);
  this->local_offered_tx_formats_.count = 0;
  this->local_offered_tx_formats_.formats[0] = offered_tx;
  this->local_offered_tx_formats_.count = 1;
  this->local_offered_directional_audio_v1_ =
      offer.find("a=x-voip-stack-flow:") != std::string::npos;

  const uint32_t cseq = this->cseq_++;
  const std::string branch = "z9hG4bK" + make_token("");
  std::string request;
  SipRequestOptions options;
  options.cseq_number = cseq;
  options.branch_override = branch;
  options.remember_transaction = false;
  options.remember_invite_ack = false;
  options.formatted_request = &request;
  this->video_send_requested_.store(enabled, std::memory_order_release);
  if (!this->send_request_("INVITE", offer, options)) {
    this->video_send_requested_.store(previous_send,
                                      std::memory_order_release);
    this->emit_video_send_state_(previous_send, false);
    return false;
  }

  uint32_t target_ip =
      this->remote_ip_v4_.load(std::memory_order_acquire);
  uint16_t target_port =
      this->remote_sip_port_.load(std::memory_order_acquire);
  if (!this->remote_sip_tcp_.load(std::memory_order_acquire)) {
    uint32_t contact_ip = 0;
    uint16_t contact_port = 0;
    if (sip_uri_ipv4_target(this->remote_target_uri_, &contact_ip,
                            &contact_port)) {
      target_ip = contact_ip;
      target_port = contact_port;
    }
  }
  const uint32_t now = millis();
  auto &pending = this->pending_video_direction_invite_;
  pending.clear();
  pending.branch = branch;
  pending.offered_sdp = offer;
  pending.cseq = cseq;
  pending.session_version = session_version;
  pending.response_deadline_ms = now + SIP_TRANSACTION_TIMEOUT_MS;
  pending.target_send = enabled;
  pending.previous_send = previous_send;
  pending.retry_count = retry_count;
  pending.transaction.request = request;
  pending.transaction.ip_v4 = target_ip;
  pending.transaction.port = target_port;
  pending.transaction.interval_ms = SIP_T1_MS;
  pending.transaction.next_ms = now + SIP_T1_MS;
  pending.transaction.deadline_ms = pending.response_deadline_ms;
  pending.transaction.udp =
      !this->remote_sip_tcp_.load(std::memory_order_acquire);
  pending.transaction.completed = false;
  ESP_LOGI(TAG,
           "SIP video direction re-INVITE sent cseq=%u target=%s retry=%u",
           (unsigned) cseq, enabled ? "send" : "receive-only",
           (unsigned) retry_count);
  this->emit_video_send_state_(previous_send, true);
  this->wake_sip_task_();
  return true;
}

bool SipTransport::request_video_send(bool enabled) {
  LockGuard lock(this->dialog_mutex_);
  // Before a dialog exists this is the preference used by the initial SDP.
  // Do not turn an in-progress INVITE or teardown into an idle settings write.
  if (this->call_id_.empty() && this->video_source_ != nullptr &&
      !this->media_active_.load(std::memory_order_acquire) &&
      !this->outgoing_invite_pending_.load(std::memory_order_acquire) &&
      !this->transport_stopping_.load(std::memory_order_acquire) &&
      !this->terminal_transaction_pending_locked_()) {
    this->video_send_requested_.store(enabled, std::memory_order_release);
    this->emit_video_send_state_(enabled, false);
    return true;
  }
  if (!this->running_.load(std::memory_order_acquire) ||
      this->transport_stopping_.load(std::memory_order_acquire) ||
      !this->media_active_.load(std::memory_order_acquire) ||
      !this->video_negotiated_ || this->video_source_ == nullptr) {
    ESP_LOGW(TAG,
             "Video send direction rejected: no active negotiated video dialog");
    return false;
  }
  if (this->pending_video_direction_invite_.pending() ||
      this->outgoing_invite_pending_.load(std::memory_order_acquire) ||
      this->completed_invite_.awaiting_ack) {
    ESP_LOGW(TAG, "Video send direction rejected: INVITE transaction pending");
    return false;
  }
  if (enabled == this->video_send_enabled_) {
    this->video_send_requested_.store(enabled, std::memory_order_release);
    this->emit_video_send_state_(enabled, false);
    return true;
  }
  return this->send_video_direction_reinvite_unlocked_(enabled);
}

bool SipTransport::apply_video_direction_answer_(const std::string &sdp,
                                                 uint32_t default_ip,
                                                 bool *accepted_send) {
  if (accepted_send == nullptr || sdp.empty()) return false;
  AudioFormat old_tx;
  AudioFormat old_rx;
  uint8_t old_tx_pt = 0;
  uint8_t old_rx_pt = 0;
  const bool old_directional_audio = this->remote_directional_audio_v1_;
  const uint8_t old_audio_direction = this->get_audio_direction_();
  RtpAudioPayloads old_rx_payloads;
  this->get_media_config_(&old_tx, &old_rx, &old_tx_pt, &old_rx_pt, &old_rx_payloads);
  const uint32_t old_audio_ip =
      this->remote_rtp_ip_v4_.load(std::memory_order_acquire);
  const uint16_t old_audio_port =
      this->remote_rtp_port_.load(std::memory_order_acquire);
  const auto old_media_shape = this->remote_media_shape_;
  const bool old_video_offered = this->video_offered_;
  const bool old_video_negotiated = this->video_negotiated_;
  const bool old_video_send = this->video_send_enabled_;
  const bool old_video_receive = this->video_receive_enabled_;
  const uint32_t old_video_ip = this->remote_video_ip_v4_;
  const uint16_t old_video_port = this->remote_video_rtp_port_;
  const uint32_t old_video_rtcp_ip = this->remote_video_rtcp_ip_v4_;
  const uint16_t old_video_rtcp_port = this->remote_video_rtcp_port_;
  const VideoCapability old_capability =
      this->negotiated_video_capability_;

  ScopedMediaProposal proposal_scope(this->media_proposal_epoch_);
  const bool parsed =
      this->learn_remote_rtp_from_sdp_(sdp, default_ip, true, true);
  AudioFormat new_tx;
  AudioFormat new_rx;
  uint8_t new_tx_pt = 0;
  uint8_t new_rx_pt = 0;
  RtpAudioPayloads new_rx_payloads;
  this->get_media_config_(&new_tx, &new_rx, &new_tx_pt, &new_rx_pt, &new_rx_payloads);
  const uint8_t new_audio_direction = this->get_audio_direction_();
  const uint32_t new_audio_ip =
      this->remote_rtp_ip_v4_.load(std::memory_order_acquire);
  const uint16_t new_audio_port =
      this->remote_rtp_port_.load(std::memory_order_acquire);
  const bool new_video_negotiated = this->video_negotiated_;
  const bool new_video_send = this->video_send_enabled_;
  const bool new_video_receive = this->video_receive_enabled_;
  const uint32_t new_video_ip = this->remote_video_ip_v4_;
  const uint16_t new_video_port = this->remote_video_rtp_port_;
  const uint32_t new_video_rtcp_ip = this->remote_video_rtcp_ip_v4_;
  const uint16_t new_video_rtcp_port = this->remote_video_rtcp_port_;
  const VideoCapability new_capability =
      this->negotiated_video_capability_;

  this->set_media_config_(old_tx, old_rx, old_tx_pt, old_rx_pt, &old_rx_payloads);
  this->set_audio_direction_(old_audio_direction);
  this->remote_directional_audio_v1_ = old_directional_audio;
  this->remote_rtp_ip_v4_.store(old_audio_ip, std::memory_order_release);
  this->remote_rtp_port_.store(old_audio_port, std::memory_order_release);
  this->remote_media_shape_ = old_media_shape;
  this->video_offered_ = old_video_offered;
  this->video_negotiated_ = old_video_negotiated;
  this->video_send_enabled_ = old_video_send;
  this->video_receive_enabled_ = old_video_receive;
  this->remote_video_ip_v4_ = old_video_ip;
  this->remote_video_rtp_port_ = old_video_port;
  this->remote_video_rtcp_ip_v4_ = old_video_rtcp_ip;
  this->remote_video_rtcp_port_ = old_video_rtcp_port;
  this->negotiated_video_capability_ = old_capability;
  proposal_scope.finish();

  const bool same_audio =
      parsed && new_audio_direction == old_audio_direction &&
      new_tx == old_tx && new_rx == old_rx &&
      new_tx_pt == old_tx_pt && new_rx_pt == old_rx_pt &&
      new_rx_payloads == old_rx_payloads &&
      new_audio_ip == old_audio_ip && new_audio_port == old_audio_port;
  const bool same_video =
      new_video_negotiated &&
      new_capability.payload_type == old_capability.payload_type &&
      new_capability.clock_rate == old_capability.clock_rate &&
      new_capability.packetization_mode ==
          old_capability.packetization_mode &&
      new_capability.encoding == old_capability.encoding &&
      new_capability.profile_level_id == old_capability.profile_level_id &&
      new_capability.max_fps == old_capability.max_fps &&
      new_capability.level_asymmetry_allowed ==
          old_capability.level_asymmetry_allowed &&
      new_capability.max_bitrate_bps ==
          old_capability.max_bitrate_bps &&
      new_capability.rtcp_feedback_pli ==
          old_capability.rtcp_feedback_pli &&
      new_capability.rtcp_feedback_fir ==
          old_capability.rtcp_feedback_fir &&
      new_video_ip == old_video_ip && new_video_port == old_video_port &&
      new_video_rtcp_ip == old_video_rtcp_ip &&
      new_video_rtcp_port == old_video_rtcp_port &&
      new_video_receive == old_video_receive;
  if (!same_audio || !same_video) return false;
  if (new_video_send != old_video_send &&
      (this->video_session_ == nullptr ||
       !this->video_session_->request_send_direction(new_video_send))) {
    return false;
  }
  this->video_send_enabled_ = new_video_send;
  this->video_send_requested_.store(new_video_send,
                                    std::memory_order_release);
  *accepted_send = new_video_send;
  return true;
}

bool SipTransport::replay_completed_video_direction_ack_(
    const std::string &response, const sockaddr_in &src) {
  auto &completed = this->completed_video_direction_invite_;
  if (completed.empty()) return false;
  if (millis() - completed.completed_ms > SIP_TRANSACTION_TIMEOUT_MS) {
    completed.clear();
    return false;
  }
  const std::string cseq = header_value(response, "CSeq");
  const bool transport_matches =
      completed.udp ==
      !this->remote_sip_tcp_.load(std::memory_order_acquire);
  if (response.rfind("SIP/2.0 ", 0) != 0 ||
      cseq_method(cseq) != "INVITE" ||
      header_value(response, "Call-ID") != completed.call_id ||
      cseq_number(cseq) != completed.cseq ||
      ntohl(src.sin_addr.s_addr) != completed.response_ip_v4 ||
      !transport_matches ||
      via_branch(header_value(response, "Via")) != completed.branch) {
    return false;
  }
  ESP_LOGI(TAG,
           "SIP video re-INVITE final retransmission replaying ACK cseq=%u",
           (unsigned) completed.cseq);
  this->send_sip_(completed.ack, completed.ack_ip_v4,
                  completed.ack_port);
  return true;
}
#endif

bool SipTransport::send_invite(const std::string &call_id,
                               const std::string &caller_route,
                               const std::string &caller_name,
                               const std::string &dest_route,
                               const std::string &dest_name) {
  LockGuard lock(this->dialog_mutex_);
  return this->send_invite_unlocked_(call_id, caller_route, caller_name, dest_route, dest_name);
}

bool SipTransport::send_invite_unlocked_(const std::string &call_id,
    const std::string &caller_route, const std::string &caller_name,
    const std::string &dest_route, const std::string &dest_name) {
  if (!this->running_.load(std::memory_order_acquire) ||
      this->transport_stopping_.load(std::memory_order_acquire)) {
    return false;
  }
  if (this->terminal_transaction_pending_locked_()) {
    ESP_LOGW(TAG,
             "SIP INVITE deferred: prior terminal transaction is pending");
    return false;
  }
  if (!this->refer_context_active_) {
    LockGuard media_lock(this->media_lifecycle_mutex_);
    if (this->media_lifecycle_phase_.load(std::memory_order_acquire) !=
        MediaLifecyclePhase::IDLE) {
      ESP_LOGW(TAG, "SIP INVITE deferred: prior media lifecycle not idle");
      return false;
    }
  }
  if (!this->refer_context_active_) {
    this->logical_call_id_.clear();
    this->logical_wire_id_.clear();
  }
  // A fresh INVITE must never inherit tags or transaction state from a
  // cancelled dialog whose final response was lost.
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  const int prepared_account = this->refer_context_active_ ? this->active_account_ : this->next_account_;
#endif
  this->reset_dialog_();
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  this->active_account_ = prepared_account;
  if (!this->refer_context_active_) this->next_account_ = -1;
#endif
  this->call_id_ = call_id;
  this->sdp_session_id_ = esp_random();
  if (this->sdp_session_id_ == 0) this->sdp_session_id_ = 1;
  this->sdp_session_version_ = 0;
  this->dialog_originated_ = true;
  this->caller_ = {caller_route, caller_name};
  this->destination_ = {dest_route, dest_name};
  this->last_sip_status_code_.store(0, std::memory_order_release);
  const uint32_t ip = this->remote_ip_v4_.load(std::memory_order_acquire);
  if (ip == 0) {
    this->reset_dialog_();
    return false;
  }
  if (this->local_tag_.empty()) this->local_tag_ = make_token("tag");
  this->branch_ = "z9hG4bK" + make_token("");
  this->invite_cseq_ = this->cseq_;
  std::string local_ip = "0.0.0.0";
  this->local_ip_for_peer_(ip, &local_ip);
  struct in_addr a{};
  a.s_addr = htonl(ip);
  char ip_text[16];
  inet_ntoa_r(a, ip_text, sizeof(ip_text));
  const char *uri_transport = this->remote_sip_tcp_.load(std::memory_order_acquire) ? "tcp" : "udp";
  const std::string local_identity_uri =
      "sip:" + sip_uri_user_encode(this->caller_.route) + "@" + local_ip +
      ":" + std::to_string(this->sip_port_) + ";transport=" + uri_transport;
  const std::string remote_identity_uri = this->refer_context_active_ && this->refer_dialog_
      ? this->refer_dialog_->target_uri
      : "sip:" + sip_uri_user_encode(this->destination_.route) + "@" +
      std::string(ip_text) + ":" +
      std::to_string(this->remote_sip_port_.load(std::memory_order_acquire)) +
      ";transport=" + uri_transport;
  this->local_uri_ = sip_name_addr(local_identity_uri, this->caller_.name);
  this->local_contact_uri_ = sip_name_addr(local_identity_uri);
  this->remote_uri_ = sip_name_addr(remote_identity_uri, this->destination_.name);
  this->remote_target_uri_ = strip_angle_uri(this->remote_uri_);
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  if (this->active_account_ >= 0) {
    const auto &account = this->registration_.account(this->active_account_);
    this->local_uri_ = sip_name_addr("sip:" + account.config.username + "@" + account.config.domain, this->caller_.name);
    this->local_contact_uri_ = sip_name_addr(account.contact);
    const std::string account_target = this->refer_context_active_ && this->refer_dialog_
        ? this->refer_dialog_->target_uri
        : "sip:" + sip_uri_user_encode(dest_route) + "@" + account.config.domain;
    this->remote_uri_ = sip_name_addr(account_target, dest_name);
    this->remote_target_uri_ = strip_angle_uri(this->remote_uri_);
  }
#endif
  ESP_LOGI(TAG, "SIP INVITE call_id=%s from=%s to=%s", this->call_id_.c_str(),
           this->caller_.name.c_str(), this->destination_.name.c_str());
  std::string sdp;
  {
    LockGuard media_lock(this->media_lifecycle_mutex_);
    sdp = this->build_sdp_offer_();
  }
  if (sdp.empty()) {
    this->reset_dialog_();
    return false;
  }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  if (!this->refer_context_active_) this->confirmed_local_sdp_ = sdp;
#endif
  SipRequestOptions options;
  options.cseq_number = this->invite_cseq_;
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  if (this->active_account_ >= 0) this->authenticated_invite_body_ = sdp;
#endif
  const bool sent = this->send_request_("INVITE", sdp, options);
  if (sent) {
    this->outgoing_invite_pending_.store(true, std::memory_order_release);
    if (this->cseq_ <= this->invite_cseq_) this->cseq_ = this->invite_cseq_ + 1;
  } else {
    this->reset_dialog_();
  }
  return sent;
}

void SipTransport::send_audio_frame(const uint8_t *pcm, size_t bytes) {
  if (!this->rtp_running_.load(std::memory_order_acquire) || pcm == nullptr || bytes == 0) return;
  const uint32_t proposal_epoch =
      this->media_proposal_epoch_.load(std::memory_order_acquire);
  if ((proposal_epoch & 1U) != 0) return;
  // PCM conversion is the expensive part. Keep it outside the socket mutex;
  // teardown flips rtp_running_ first and the short final critical section
  // below prevents close-vs-send without delaying the media loop.
  AudioFormat tx_format;
  uint8_t tx_payload_type = 96;
  this->get_media_config_(&tx_format, nullptr, &tx_payload_type, nullptr);
  if (!tx_format.is_valid()) return;
  uint8_t packet[1500];
  const uint8_t bps = tx_format.container_bytes_per_sample();
  const size_t input_bytes = bytes;
  const uint32_t pcm_samples = bps == 0 || tx_format.channels == 0
      ? 0
      : static_cast<uint32_t>(input_bytes / bps / tx_format.channels);
  const uint32_t timestamp_samples = tx_format.codec == AudioCodec::OPUS
      ? static_cast<uint32_t>((48000ULL * tx_format.frame_ms) / 1000U)
      : pcm_samples;
  if ((this->get_audio_direction_() & 1U) == 0) {
    this->rtp_timestamp_.fetch_add(timestamp_samples, std::memory_order_acq_rel);
    return;
  }
#ifdef USE_ESPHOME_VOIP_STACK_OPUS
  if (tx_format.codec == AudioCodec::OPUS) {
    bytes = this->opus_codec_.encode(pcm, bytes, tx_format, packet + 12,
                                     sizeof(packet) - 12);
  } else
#endif
  {
    bytes = pcm_to_rtp_payload(pcm, bytes, tx_format, packet + 12,
                               sizeof(packet) - 12);
  }
  if (bytes == 0 || bytes > this->udp_max_payload_) {
    // Sequence numbers count packets, while timestamps follow the sampling
    // clock. A locally discarded PCM frame therefore advances only time.
    this->rtp_timestamp_.fetch_add(timestamp_samples, std::memory_order_acq_rel);
    return;
  }
  if (this->media_proposal_epoch_.load(std::memory_order_acquire) !=
      proposal_epoch) {
    this->rtp_timestamp_.fetch_add(timestamp_samples, std::memory_order_acq_rel);
    return;
  }
  LockGuard socket_lock(this->rtp_socket_mutex_);
  if (!this->rtp_running_.load(std::memory_order_acquire) || this->rtp_socket_ < 0) return;
  const uint32_t ip = this->remote_rtp_ip_v4_.load(std::memory_order_acquire);
  const uint16_t port = this->remote_rtp_port_.load(std::memory_order_acquire);
  if (ip == 0 || port == 0) return;
  if (this->media_proposal_epoch_.load(std::memory_order_acquire) !=
      proposal_epoch) {
    this->rtp_timestamp_.fetch_add(timestamp_samples, std::memory_order_acq_rel);
    return;
  }
  if ((this->get_audio_direction_() & 1U) == 0) {
    this->rtp_timestamp_.fetch_add(timestamp_samples, std::memory_order_acq_rel);
    return;
  }
  packet[0] = 0x80;
  packet[1] = tx_payload_type & 0x7F;
  const uint16_t seq = this->rtp_sequence_.fetch_add(1, std::memory_order_acq_rel);
  packet[2] = static_cast<uint8_t>(seq >> 8);
  packet[3] = static_cast<uint8_t>(seq & 0xFF);
  const uint32_t ts = this->rtp_timestamp_.fetch_add(timestamp_samples, std::memory_order_acq_rel);
  packet[4] = static_cast<uint8_t>(ts >> 24);
  packet[5] = static_cast<uint8_t>((ts >> 16) & 0xFF);
  packet[6] = static_cast<uint8_t>((ts >> 8) & 0xFF);
  packet[7] = static_cast<uint8_t>(ts & 0xFF);
  packet[8] = static_cast<uint8_t>(this->rtp_ssrc_ >> 24);
  packet[9] = static_cast<uint8_t>((this->rtp_ssrc_ >> 16) & 0xFF);
  packet[10] = static_cast<uint8_t>((this->rtp_ssrc_ >> 8) & 0xFF);
  packet[11] = static_cast<uint8_t>(this->rtp_ssrc_ & 0xFF);
  struct sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_addr.s_addr = htonl(ip);
  dest.sin_port = htons(port);
  if (!this->rtp_running_.load(std::memory_order_acquire)) return;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_DEBUG
  const uint32_t send_started_us = micros();
#endif
  const int sent = sendto(this->rtp_socket_, packet, 12 + bytes, 0,
                          reinterpret_cast<struct sockaddr *>(&dest), sizeof(dest));
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_DEBUG
  const uint32_t send_elapsed_us = micros() - send_started_us;
  this->audio_tx_max_send_us_ =
      std::max(this->audio_tx_max_send_us_, send_elapsed_us);
  if (send_elapsed_us >= 5000U) this->audio_tx_slow_send_calls_++;
  if (sent <= 0) this->audio_tx_send_failures_++;
  const uint32_t now = millis();
  if (now - this->audio_tx_last_debug_log_ms_ >= 5000U) {
    this->audio_tx_last_debug_log_ms_ = now;
    ESP_LOGI(TAG,
             "Audio RTP TX: packets=%u slow_send=%u send_fail=%u "
             "max_send_us=%u",
             (unsigned) this->rtp_tx_packets_.load(
                 std::memory_order_acquire),
             (unsigned) this->audio_tx_slow_send_calls_,
             (unsigned) this->audio_tx_send_failures_,
             (unsigned) this->audio_tx_max_send_us_);
  }
#endif
  if (sent > 0) {
    this->rtp_tx_packets_.fetch_add(1, std::memory_order_acq_rel);
    this->rtp_tx_bytes_.fetch_add(static_cast<uint32_t>(sent), std::memory_order_acq_rel);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_HOSTED_AUDIO_PACING
    if (this->video_session_ != nullptr)
      this->video_session_->notify_audio_packet_sent();
#endif
  }
}

#ifdef USE_ESPHOME_VOIP_STACK_OPUS
size_t SipTransport::decode_audio_payload(const uint8_t *payload,
                                          size_t payload_bytes,
                                          const AudioFormat &format,
                                          uint8_t *pcm,
                                          size_t pcm_capacity) {
  if (format.codec != AudioCodec::OPUS) return 0;
  return this->opus_codec_.decode(payload, payload_bytes, format, pcm,
                                  pcm_capacity);
}

#endif
void SipTransport::reset_audio_decoder() {
#ifdef USE_ESPHOME_VOIP_STACK_OPUS
  this->opus_codec_.reset_decoder();
#endif
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
  this->pcm_receive_converter_.reset();
#endif
}

size_t SipTransport::convert_received_pcm(const AudioFormat &source, const uint8_t *pcm,
                                           size_t bytes, uint8_t *output, size_t capacity) {
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
  return this->pcm_receive_converter_.convert(source, pcm, bytes, output, capacity);
#else
  return 0;
#endif
}

bool SipTransport::send_ringing(const std::string &call_id) {
  LockGuard lock(this->dialog_mutex_);
  if (!call_id.empty() && call_id != this->logical_call_id_) this->call_id_ = call_id;
  return this->send_response_(180, "Ringing");
}

void SipTransport::set_connected_identity(const std::string &route,
                                          const std::string &name) {
  LockGuard lock(this->dialog_mutex_);
  if (this->dialog_originated_ || this->call_id_.empty()) return;
  std::string identity_uri = strip_angle_uri(this->local_uri_);
  if (identity_uri.empty()) return;
  const std::string clean_route =
      sip_route_id(route, VOIP_STACK_MAX_ROUTE_ID_LEN);
  const size_t at = identity_uri.find('@', 4);
  if (!clean_route.empty() && identity_uri.rfind("sip:", 0) == 0 &&
      at != std::string::npos) {
    identity_uri.replace(4, at - 4, sip_uri_user_encode(clean_route));
  }
  const std::string updated = sip_name_addr(identity_uri, name);
  if (!updated.empty()) this->local_uri_ = updated;
}

bool SipTransport::send_answer(const std::string &call_id,
                               const AudioFormat &caller_to_dest_format,
                               const AudioFormat &dest_to_caller_format) {
  LockGuard lock(this->dialog_mutex_);
  if (!this->running_.load(std::memory_order_acquire) ||
      this->transport_stopping_.load(std::memory_order_acquire)) {
    return false;
  }
  if (!call_id.empty() && call_id != this->logical_call_id_) this->call_id_ = call_id;
  uint8_t tx_payload_type = 96;
  uint8_t rx_payload_type = 96;
  RtpAudioPayloads rx_payloads;
  this->get_media_config_(nullptr, nullptr, &tx_payload_type, &rx_payload_type, &rx_payloads);
  this->set_media_config_(dest_to_caller_format, caller_to_dest_format,
                          tx_payload_type, rx_payload_type, &rx_payloads);
  this->outgoing_invite_pending_.store(false, std::memory_order_release);
  std::string answer;
  {
    LockGuard media_lock(this->media_lifecycle_mutex_);
    const MediaLifecyclePhase phase =
        this->media_lifecycle_phase_.load(std::memory_order_acquire);
    if (phase == MediaLifecyclePhase::CLEANING ||
        phase == MediaLifecyclePhase::SHUTTING_DOWN) {
      return false;
    }
    answer = this->delayed_offer_pending_ ? this->build_sdp_offer_()
                                          : this->build_sdp_answer_();
  }
  if (answer.empty()) {
    const bool sent = this->send_response_(488, "Not Acceptable Here", "", "media_incompatible");
    this->reset_dialog_();
    return sent;
  }
  const bool sent = this->send_response_(200, "OK", answer);
  if (sent) {
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
    this->confirmed_local_sdp_ = answer;
#endif
    if (!this->delayed_offer_pending_) this->open_media_session_();
  }
  return sent;
}

bool SipTransport::send_cancel(const std::string &call_id) {
  LockGuard lock(this->dialog_mutex_);
  return this->send_cancel_unlocked_(call_id);
}

bool SipTransport::send_cancel_unlocked_(const std::string &call_id) {
  if (!call_id.empty() && call_id != this->logical_call_id_) this->call_id_ = call_id;
  if (!this->outgoing_invite_pending_.load(std::memory_order_acquire)) {
    return this->send_bye_unlocked_(call_id);
  }
  if (this->remote_sip_tcp_.load(std::memory_order_acquire)) {
    bool cancelled_before_flush = false;
    {
      // This lock is shared with promote_tcp_flow_(). If the INVITE still
      // occupies the one pre-connect slot, it has not reached the wire and
      // must be retracted rather than followed by a meaningless CANCEL. If
      // promotion already owns/sent it, release the lock and send a real
      // CANCEL on the established stream below.
      LockGuard send_lock(this->tcp_send_mutex_);
      LockGuard pending_lock(this->tcp_tx_pending_mutex_);
      if (this->tcp_flow_.pending.rfind("INVITE ", 0) == 0 &&
          header_value(this->tcp_flow_.pending, "Call-ID") == this->call_id_) {
        reset_string(this->tcp_flow_.pending);
        this->tcp_flow_.connect_requested.store(false, std::memory_order_release);
        this->tcp_flow_.close_requested.store(
            true, std::memory_order_release);
        cancelled_before_flush = true;
      }
    }
    if (cancelled_before_flush) {
      ESP_LOGI(TAG, "SIP TCP INVITE cancelled before first byte was sent");
      this->reset_dialog_();
      this->wake_sip_task_();
      return true;
    }
  }
  this->cancel_requested_.store(true, std::memory_order_release);
  const bool sent = this->send_invite_transaction_request_("CANCEL", "", this->pending_invite_, false, nullptr);
  if (sent) {
    // Stop retransmission but retain the original wire request until its final
    // response: the non-2xx ACK must reuse that exact transaction, even after180.
    this->pending_invite_.completed = true;
    this->pending_invite_.next_ms = this->pending_invite_.deadline_ms;
  } else {
    this->cancel_requested_.store(false, std::memory_order_release);
  }
  return sent;
}

bool SipTransport::send_bye(const std::string &call_id) {
  LockGuard lock(this->dialog_mutex_);
  return this->send_bye_unlocked_(call_id);
}

bool SipTransport::send_bye_unlocked_(const std::string &call_id) {
  // A local stop can trail the deferred remote-BYE notification. It must not
  // recreate a retired dialog or retarget a newer one from its application ID.
  if (this->call_id_.empty() ||
      (!call_id.empty() && call_id != this->call_id_ && call_id != this->logical_call_id_)) return false;
  const bool ended_invite = !this->refer_context_active_ && this->refer_dialog_ &&
      this->refer_dialog_->owner_invite_ended && this->call_id_ == this->refer_dialog_->owner_call_id;
  this->cancel_refer_();
  if (ended_invite) {
    const std::string terminated = this->call_id_;
    this->reset_dialog_();
    SipSignal signal;
    signal.type = SipSignalType::TERMINAL_COMPLETE;
    signal.call_id = terminated;
    this->emit_sip_signal_(std::move(signal));
    return true;
  }
  // A UAS dialog is not confirmed until the ACK for its 2xx arrives. A local
  // hangup can race the initial final-response send, especially when lwIP is
  // briefly short of pbuf memory. Retain the terminal intent under the INVITE
  // transaction instead of putting an invalid pre-ACK BYE on the wire.
  if (this->defer_bye_until_uas_ack_locked_()) {
    if (!this->refer_context_active_) this->stop_audio_path();
    return true;
  }
  // Connected-identity is observational and must not outlive termination.
  // Dropping its pending transaction also prevents a lower-CSeq UPDATE from
  // being retransmitted after the dialog's BYE.
  this->pending_update_.clear();
  // BYE is the latency-critical dialog action. Video/audio workers can take
  // time to leave codec and socket calls, so put the SIP request on the wire
  // before performing subordinate media teardown.
  const bool sent = this->send_request_("BYE");
  if (!this->refer_context_active_) this->stop_audio_path();
  return sent;
}

bool SipTransport::defer_bye_until_uas_ack_locked_() {
  const bool accepted_uas_dialog =
      this->completed_invite_.awaiting_ack &&
      this->completed_invite_.status >= 200 &&
      this->completed_invite_.status < 300 &&
      !this->call_id_.empty() &&
      this->completed_invite_.call_id == this->call_id_ &&
      this->last_invite_cseq_number_ == this->completed_invite_.cseq;
  if (!accepted_uas_dialog) return false;
  this->terminate_after_invite_ack_ = true;
  this->wake_sip_task_();
  return true;
}

bool SipTransport::send_final_response(const std::string &call_id,
                                       uint16_t status,
                                       const std::string &reason) {
  LockGuard lock(this->dialog_mutex_);
  if (!call_id.empty() && call_id != this->logical_call_id_) this->call_id_ = call_id;
  if (this->outgoing_invite_pending_.load(std::memory_order_acquire)) {
    return this->send_cancel_unlocked_(call_id);
  }
  const char *phrase = "Busy Here";
  if (status == 603) phrase = "Decline";
  else if (status == 488) phrase = "Not Acceptable Here";
  else if (status == 487) phrase = "Request Terminated";
  else if (status == 500) phrase = "Server Internal Error";
  const bool sent = this->send_response_(status, phrase, "", reason);
  this->reset_dialog_();
  return sent;
}

bool SipTransport::replay_completed_response_(const std::string &request, const sockaddr_in &src,
                                              const std::string &method, bool incoming_udp) {
  size_t body_length = 0;
  const size_t separator = request.find("\r\n\r\n");
  const bool has_length = !header_value(request, "Content-Length").empty() ||
                          !header_value(request, "l").empty();
  if (separator == std::string::npos || !sip_content_length(request, &body_length) ||
      (has_length && request.size() - separator - 4 != body_length)) return false;
  CompletedServerTransaction *completed = method == "REFER" && this->refer_dialog_
      ? &this->refer_dialog_->accepted_refer
      : method == "INVITE" ? &this->completed_invite_ : &this->completed_control_;
  for (auto &entry : this->completed_refers_) {
    if (!entry || time_reached(millis(), entry->expires_ms)) continue;
    auto &candidate = method == "REFER" ? entry->accepted : entry->control;
    if (candidate.method == method && candidate.call_id == header_value(request, "Call-ID") &&
        candidate.cseq == cseq_number(header_value(request, "CSeq"))) {
      completed = &candidate;
      break;
    }
  }
  if (completed->empty()) return false;
  if (millis() - completed->completed_ms > SIP_TRANSACTION_TIMEOUT_MS) {
    completed->clear();
    return false;
  }
  const std::string call_id = header_value(request, "Call-ID");
  const std::string cseq = header_value(request, "CSeq");
  const std::string branch = via_branch(header_value(request, "Via"));
  const uint32_t peer_ip = ntohl(src.sin_addr.s_addr);
  const bool transport_matches =
      completed->udp == incoming_udp;
  if (completed->method != method || completed->call_id != call_id ||
      completed->cseq != cseq_number(cseq) || cseq_method(cseq) != method ||
      completed->peer_ip_v4 != peer_ip || !transport_matches ||
      (!completed->branch.empty() && completed->branch != branch)) {
    return false;
  }
  ESP_LOGI(TAG, "SIP %s retransmission replaying cached response for call_id=%s",
           method.c_str(), call_id.c_str());
  if (incoming_udp) {
    this->send_sip_udp_bytes_(completed->response.data(), completed->response.size(),
                              peer_ip, ntohs(src.sin_port));
  } else {
    this->send_sip_(completed->response, peer_ip, ntohs(src.sin_port));
  }
  return true;
}

void SipTransport::remember_completed_response_(const std::string &request, uint32_t peer_ip_v4,
                                                uint16_t peer_port,
                                                const std::string &method,
                                                const std::string &response) {
  if (response.empty() || peer_ip_v4 == 0 || peer_port == 0 ||
      (method != "INVITE" && method != "CANCEL" && method != "BYE" &&
       method != "UPDATE" && method != "REFER") ||
      response.rfind("SIP/2.0 ", 0) != 0 || response.size() < 12) {
    return;
  }
  uint32_t parsed_status = 0;
  if (!parse_decimal_u32(response.substr(8, 3), 699, &parsed_status) || parsed_status < 200) return;
  CompletedServerTransaction *completed = method == "INVITE" ? &this->completed_invite_
                                                               : &this->completed_control_;
  const std::string call_id = header_value(request, "Call-ID");
  const uint32_t request_cseq = cseq_number(header_value(request, "CSeq"));
  const std::string request_branch = via_branch(header_value(request, "Via"));
  if (call_id.empty() || request_cseq == 0) return;
  if (method == "INVITE" && completed->awaiting_ack &&
      completed->call_id == this->call_id_ && this->last_invite_cseq_number_ != 0 &&
      (completed->call_id != call_id || completed->cseq != request_cseq ||
       completed->branch != request_branch)) {
    // Do not let a colliding/re-INVITE stateless error evict the active UAS
    // final response before its ACK arrives.
    return;
  }
  const uint32_t now = millis();
  completed->method = method;
  completed->call_id = call_id;
  completed->cseq = request_cseq;
  completed->branch = request_branch;
  completed->from_tag = tag_from_header(header_value(response, "From"));
  completed->to_tag = tag_from_header(header_value(response, "To"));
  completed->response = response;
  completed->peer_ip_v4 = peer_ip_v4;
  completed->peer_port = peer_port;
  completed->status = static_cast<uint16_t>(parsed_status);
  completed->completed_ms = now;
  completed->next_retransmit_ms = now + SIP_T1_MS;
  completed->deadline_ms = now + SIP_TRANSACTION_TIMEOUT_MS;
  completed->retransmit_interval_ms = SIP_T1_MS;
  completed->retransmits = 0;
  completed->udp = !this->remote_sip_tcp_.load(std::memory_order_acquire);
  // Every final INVITE response is completed by ACK. UDP retransmits every
  // final response; 2xx is retransmitted by the UAS core even on TCP because
  // its ACK is a separate transaction.
  completed->awaiting_ack = method == "INVITE";
  if (completed->awaiting_ack) this->wake_sip_task_();
}

uint16_t SipTransport::acknowledge_completed_invite_(
    const std::string &request, const sockaddr_in &src,
    bool *terminate_after_ack) {
  if (terminate_after_ack != nullptr) *terminate_after_ack = false;
  if (this->completed_invite_.empty()) return 0;
  const uint32_t now = millis();
  if (time_reached(now, this->completed_invite_.completed_ms + SIP_TRANSACTION_TIMEOUT_MS)) {
    this->completed_invite_.clear();
    return 0;
  }
  const std::string cseq = header_value(request, "CSeq");
  const uint32_t peer_ip = ntohl(src.sin_addr.s_addr);
  const std::string branch = via_branch(header_value(request, "Via"));
  const bool transport_matches =
      this->completed_invite_.udp != this->remote_sip_tcp_.load(std::memory_order_acquire);
  const bool transaction_matches =
      cseq_method(cseq) == "ACK" && cseq_number(cseq) == this->completed_invite_.cseq &&
      header_value(request, "Call-ID") == this->completed_invite_.call_id &&
      peer_ip == this->completed_invite_.peer_ip_v4 && transport_matches &&
      tag_from_header(header_value(request, "From")) == this->completed_invite_.from_tag &&
      tag_from_header(header_value(request, "To")) == this->completed_invite_.to_tag &&
      (this->completed_invite_.status < 300 ||
       this->completed_invite_.branch.empty() || branch == this->completed_invite_.branch);
  if (!transaction_matches) return 0;
  this->completed_invite_.awaiting_ack = false;
  if (this->completed_invite_.status < 300 &&
      this->terminate_after_invite_ack_) {
    this->terminate_after_invite_ack_ = false;
    if (terminate_after_ack != nullptr) *terminate_after_ack = true;
  }
  ESP_LOGI(TAG, "SIP ACK completed cached INVITE server transaction call_id=%s",
           this->completed_invite_.call_id.c_str());
  return this->completed_invite_.status;
}

bool SipTransport::replay_completed_invite_ack_(const std::string &response,
                                                const sockaddr_in &src, bool incoming_udp) {
  size_t body_length = 0;
  const size_t separator = response.find("\r\n\r\n");
  const bool has_length = !header_value(response, "Content-Length").empty() ||
                          !header_value(response, "l").empty();
  if (separator == std::string::npos || !sip_content_length(response, &body_length) ||
      (has_length && response.size() - separator - 4 != body_length)) return false;
  CompletedInviteClientTransaction *completed = &this->completed_invite_client_;
  for (auto &entry : this->completed_refers_) {
    if (entry && !time_reached(millis(), entry->expires_ms) &&
        entry->invite.call_id == header_value(response, "Call-ID")) {
      completed = &entry->invite;
      break;
    }
  }

  if (completed->empty()) return false;
  if (millis() - completed->completed_ms > SIP_TRANSACTION_TIMEOUT_MS) {
    completed->clear();
    return false;
  }
  if (response.rfind("SIP/2.0 ", 0) != 0 || response.size() < 12) return false;
  const int status = std::atoi(response.substr(8, 3).c_str());
  const std::string cseq = header_value(response, "CSeq");
  const uint32_t peer_ip = ntohl(src.sin_addr.s_addr);
  const bool transport_matches =
      completed->udp == incoming_udp;
  if (status < 200 || cseq_method(cseq) != "INVITE" ||
      header_value(response, "Call-ID") != completed->call_id ||
      cseq_number(cseq) != completed->cseq ||
      peer_ip != completed->response_ip_v4 ||
      !transport_matches ||
      (!completed->branch.empty() &&
       via_branch(header_value(response, "Via")) != completed->branch)) {
    return false;
  }
  ESP_LOGI(TAG, "SIP INVITE final retransmission replaying ACK for call_id=%s",
           completed->call_id.c_str());
  if (incoming_udp) {
    this->send_sip_udp_bytes_(completed->ack.data(), completed->ack.size(),
                              completed->ack_ip_v4, completed->ack_port);
  } else {
    this->send_sip_(completed->ack, completed->ack_ip_v4, completed->ack_port);
  }
  return true;
}

void SipTransport::remember_completed_invite_ack_(const std::string &request,
                                                  uint32_t target_ip_v4,
                                                  uint16_t target_port) {
  if (request.empty() || target_ip_v4 == 0 || target_port == 0) return;
  this->completed_invite_client_.call_id = this->call_id_;
  this->completed_invite_client_.cseq = this->invite_cseq_;
  this->completed_invite_client_.branch = this->branch_;
  this->completed_invite_client_.ack = request;
  this->completed_invite_client_.response_ip_v4 =
      this->remote_ip_v4_.load(std::memory_order_acquire);
  this->completed_invite_client_.ack_ip_v4 = target_ip_v4;
  this->completed_invite_client_.ack_port = target_port;
  this->completed_invite_client_.completed_ms = millis();
  this->completed_invite_client_.udp =
      !this->remote_sip_tcp_.load(std::memory_order_acquire);
}

bool SipTransport::handle_invite_(const std::string &message, const sockaddr_in &src) {
  const std::string body = message_body(message);
  const uint32_t src_ip = ntohl(src.sin_addr.s_addr);
  const std::string incoming_call_id = header_value(message, "Call-ID");
  const std::string incoming_via = header_values(message, "Via");
  const std::string incoming_from = header_value(message, "From");
  const std::string incoming_to = header_value(message, "To");
  const std::string incoming_cseq = header_value(message, "CSeq");
  const uint32_t incoming_cseq_number = cseq_number(incoming_cseq);
  if (incoming_call_id.empty() || incoming_via.empty() || incoming_from.empty() || incoming_to.empty() ||
      incoming_cseq_number == 0 || cseq_method(incoming_cseq) != "INVITE" ||
      tag_from_header(incoming_from).empty()) {
    ESP_LOGW(TAG, "SIP INVITE rejected: missing or invalid transaction headers");
    return this->send_stateless_response_(message, src, 400, "Bad Request");
  }
    if (this->replay_completed_response_(message, src, "INVITE",
#ifdef USE_ESPHOME_VOIP_REGISTRATION
                                        this->dispatch_socket_ < 0
#else
                                        !this->remote_sip_tcp_.load(std::memory_order_acquire)
#endif
                                        )) {
    return true;
  }
  const std::string incoming_from_tag = tag_from_header(incoming_from);
  const std::string incoming_to_tag = tag_from_header(incoming_to);
  const bool matching_dialog = incoming_call_id == this->call_id_ &&
      !this->local_tag_.empty() && !this->remote_tag_.empty() &&
      incoming_to_tag == this->local_tag_ && incoming_from_tag == this->remote_tag_;
  // RFC 3261 sections 8.2.2 and 12.2.2: a To-tag addresses a dialog,
  // never a new initial INVITE. Validate before changing any owned metadata.
  if (!incoming_to_tag.empty() && !matching_dialog) {
    return this->send_stateless_response_(message, src, 481,
                                          "Call/Transaction Does Not Exist");
  }
  if (this->terminal_transaction_pending_locked_()) {
    const bool same_dialog = incoming_call_id == this->call_id_;
    ESP_LOGW(TAG,
             "SIP INVITE deferred while prior dialog transaction completes");
    return this->send_stateless_response_(
        message, src, same_dialog ? 500 : 503,
        same_dialog ? "Server Internal Error" : "Service Unavailable",
        "transaction_pending", false, 1);
  }
  std::string incoming_caller_name =
      sip_display_name_from_header(incoming_from, VOIP_STACK_MAX_NAME_LEN);
  std::string incoming_dest_name =
      sip_display_name_from_header(incoming_to, VOIP_STACK_MAX_NAME_LEN);
  if (incoming_caller_name.empty()) {
    incoming_caller_name = sip_header_token(
        header_value(message, "X-Voip-Stack-Caller-Name"),
        VOIP_STACK_MAX_NAME_LEN);
  }
  if (incoming_dest_name.empty()) {
    incoming_dest_name = sip_header_token(
        header_value(message, "X-Voip-Stack-Dest-Name"),
        VOIP_STACK_MAX_NAME_LEN);
  }
  if (incoming_caller_name.empty()) {
    incoming_caller_name = sip_header_token(sip_user_from_header(incoming_from), VOIP_STACK_MAX_NAME_LEN);
  }
  if (incoming_dest_name.empty()) {
    incoming_dest_name = sip_header_token(sip_user_from_header(incoming_to), VOIP_STACK_MAX_NAME_LEN);
  }
  if (!incoming_call_id.empty() && !this->call_id_.empty() && incoming_call_id != this->call_id_) {
    const uint32_t active_peer_ip = this->remote_ip_v4_.load(std::memory_order_acquire);
    const bool glare = this->outgoing_invite_pending_.load(std::memory_order_acquire) &&
                       active_peer_ip == src_ip && !this->caller_.name.empty() &&
                       incoming_caller_name == this->destination_.name;
    if (!glare) {
      ESP_LOGW(TAG, "SIP INVITE rejected busy: active_call_id=%s incoming_call_id=%s",
               this->call_id_.c_str(), incoming_call_id.c_str());
      return this->send_stateless_response_(message, src, 486, "Busy Here", "busy", true);
    }
    // One transport owns one dialog. Do not destroy the active client INVITE
    // (and its CANCEL/ACK lifetime) to accept a crossed initial INVITE in the
    // same storage. Ask the peer to retry after this transaction completes.
    ESP_LOGW(TAG, "SIP glare with %s: retaining local INVITE",
             incoming_caller_name.c_str());
    return this->send_stateless_response_(
        message, src, 491, "Request Pending", "glare", true,
        static_cast<int>(esp_random() % 3U));
  }
  const uint32_t active_peer_ip = this->remote_ip_v4_.load(std::memory_order_acquire);
  if (!this->call_id_.empty() && incoming_call_id == this->call_id_ && active_peer_ip != 0 &&
      src_ip != active_peer_ip) {
    ESP_LOGW(TAG, "SIP INVITE rejected for active call_id=%s from unexpected peer",
             incoming_call_id.c_str());
    return this->send_stateless_response_(message, src, 481,
                                          "Call/Transaction Does Not Exist");
  }
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  int incoming_account = -1;
  if (!matching_dialog && this->call_id_.empty()) {
    incoming_account = this->registration_.match_incoming(sip_user_from_header(sip_request_uri(message)), src_ip,
        this->dispatch_socket_ >= 0, this->registration_socket_index_(this->dispatch_socket_));
    if (!this->accept_direct_calls_ && incoming_account < 0)
      return this->send_stateless_response_(message, src, 403, "Forbidden");
  }
#endif
  const bool in_dialog_invite = matching_dialog;
  if (in_dialog_invite &&
      incoming_cseq_number == this->last_invite_cseq_number_ &&
      via_branch(incoming_via) != via_branch(this->last_invite_via_)) {
    ESP_LOGW(TAG, "SIP merged in-dialog INVITE rejected for call_id=%s",
             incoming_call_id.c_str());
    return this->send_stateless_response_(message, src, 482,
                                          "Loop Detected");
  }
  if (in_dialog_invite) {
    return this->handle_reinvite_(message, src);
  }
  if (!incoming_call_id.empty() && incoming_call_id == this->call_id_ &&
      this->last_invite_cseq_number_ == incoming_cseq_number) {
    const std::string incoming_branch = via_branch(incoming_via);
    const std::string active_branch = via_branch(this->last_invite_via_);
    if (!active_branch.empty() && incoming_branch != active_branch) {
      ESP_LOGW(TAG, "SIP merged INVITE rejected for call_id=%s", incoming_call_id.c_str());
      return this->send_stateless_response_(message, src, 482, "Loop Detected");
    }
    if (!this->last_invite_response_.empty()) {
      ESP_LOGD(TAG, "SIP INVITE retransmission replaying latest provisional response");
      this->send_sip_(this->last_invite_response_, src_ip, ntohs(src.sin_port));
      return true;
    }
  }
  if (!this->call_id_.empty()) {
    return this->send_stateless_response_(message, src, 486, "Busy Here", "busy");
  }
  {
    LockGuard media_lock(this->media_lifecycle_mutex_);
    const MediaLifecyclePhase phase =
        this->media_lifecycle_phase_.load(std::memory_order_acquire);
    if (phase == MediaLifecyclePhase::CLEANING ||
        phase == MediaLifecyclePhase::SHUTTING_DOWN) {
      // A prior call still owns worker/socket teardown. Reject the new
      // transaction before mutating dialog metadata so the peer can retry
      // cleanly instead of receiving a late local transport failure.
      return this->send_stateless_response_(
          message, src, 503, "Service Unavailable", "media_cleanup", true, 1);
    }
  }
  this->remote_ip_v4_.store(src_ip, std::memory_order_release);
  this->remote_sip_port_.store(ntohs(src.sin_port), std::memory_order_release);
  this->logical_call_id_.clear();
  this->logical_wire_id_.clear();
  this->call_id_ = incoming_call_id;
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  this->active_account_ = incoming_account;
  this->dialog_account_socket_ = incoming_account >= 0 ? this->dispatch_socket_ : -1;
  if (incoming_account >= 0) this->remember_registration_routes_(message, false);
#endif
  this->sdp_session_id_ = esp_random();
  if (this->sdp_session_id_ == 0) this->sdp_session_id_ = 1;
  this->sdp_session_version_ = 0;
  this->dialog_originated_ = false;
  this->last_invite_via_ = incoming_via;
  this->last_invite_from_ = incoming_from;
  this->last_invite_to_ = incoming_to;
  this->last_invite_cseq_ = incoming_cseq;
  this->last_invite_cseq_number_ = incoming_cseq_number;
  this->remote_dialog_cseq_ = incoming_cseq_number;
  this->last_invite_peer_ip_v4_ = src_ip;
  this->last_invite_peer_port_ = ntohs(src.sin_port);
  this->remote_tag_ = tag_from_header(this->last_invite_from_);
  this->peer_supports_from_change_ =
      sip_option_supported(message, "from-change");
  if (this->local_tag_.empty()) this->local_tag_ = make_token("tag");
  const std::string remote_identity_uri = strip_angle_uri(this->last_invite_from_);
  this->remote_uri_ = remote_identity_uri.empty()
                          ? ""
                          : sip_name_addr(remote_identity_uri,
                                          incoming_caller_name);
  this->remote_target_uri_ = strip_angle_uri(header_value(message, "Contact"));
  if (this->remote_target_uri_.empty()) {
    this->remote_target_uri_ = strip_angle_uri(this->last_invite_from_);
  }
  const std::string local_identity_uri =
      strip_angle_uri(this->last_invite_to_);
  this->local_uri_ = sip_name_addr(
      local_identity_uri,
      incoming_dest_name);
  std::string local_contact_ip = "0.0.0.0";
  this->local_ip_for_peer_(src_ip, &local_contact_ip);
  const char *contact_transport = this->remote_sip_tcp_.load(std::memory_order_acquire) ? "tcp" : "udp";
  const std::string local_contact_user = sip_uri_user_encode(sip_user_from_header(this->last_invite_to_));
  this->local_contact_uri_ =
      "<sip:" + local_contact_user + "@" + local_contact_ip + ":" +
      std::to_string(this->sip_port_) + ";transport=" + contact_transport + ">";
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  if (this->active_account_ >= 0)
    this->local_contact_uri_ = sip_name_addr(this->registration_.account(this->active_account_).contact);
#endif
  bool media_compatible = false;
  {
    LockGuard media_lock(this->media_lifecycle_mutex_);
    if (body.empty()) {
      AudioFormat shared;
      const auto &tx = this->offer_tx_formats_;
      const auto &rx = this->offer_rx_formats_;
      media_compatible = choose_common_audio_format(
          tx.count == 0 ? rx : tx, rx.count == 0 ? tx : rx, &shared);
      if (media_compatible) {
        this->set_media_config_(tx.count == 0 ? disabled_audio_format() : shared,
                                rx.count == 0 ? disabled_audio_format() : shared, 96, 96);
        this->delayed_offer_pending_ = true;
      }
    } else {
      this->delayed_offer_pending_ = false;
      media_compatible = this->learn_remote_rtp_from_sdp_(body, src_ip);
    }
  }
  if (!media_compatible) {
    const bool sent = this->send_response_(488, "Not Acceptable Here");
    this->reset_dialog_();
    return sent;
  }
  this->send_response_(100, "Trying");

  std::string from_user = incoming_caller_name;
  std::string to_user = incoming_dest_name;
  if (from_user.empty() || to_user.empty()) {
    const bool sent = this->send_response_(400, "Bad Request");
    this->reset_dialog_();
    return sent;
  }
  this->caller_.name = from_user;
  this->destination_.name = to_user;
  this->caller_.route = sip_route_id(
      sip_user_from_header(incoming_from), VOIP_STACK_MAX_ROUTE_ID_LEN);
  this->destination_.route = sip_route_id(
      sip_user_from_header(sip_request_uri(message)),
      VOIP_STACK_MAX_ROUTE_ID_LEN);
  if (this->caller_.route.empty()) {
    this->caller_.route = sip_route_id(
        header_value(message, "X-Voip-Stack-Caller-Route"),
        VOIP_STACK_MAX_ROUTE_ID_LEN);
  }
  if (this->destination_.route.empty()) {
    this->destination_.route = sip_route_id(
        header_value(message, "X-Voip-Stack-Dest-Route"),
        VOIP_STACK_MAX_ROUTE_ID_LEN);
  }
  if (this->caller_.route.empty()) this->caller_.route = this->caller_.name;
  if (this->destination_.route.empty())
    this->destination_.route = this->destination_.name;

  ESP_LOGI(TAG, "SIP INVITE accepted into FSM call_id=%s", this->call_id_.c_str());
  AudioFormat selected_tx;
  AudioFormat selected_rx;
  this->get_media_config_(&selected_tx, &selected_rx, nullptr, nullptr);
  SipSignal signal;
  signal.type = SipSignalType::INVITE;
  signal.call_id = this->call_id_;
  signal.caller_route = this->caller_.route;
  signal.caller_name = this->caller_.name;
  signal.dest_route = this->destination_.route;
  signal.dest_name = this->destination_.name;
  signal.caller_tx_formats.formats[0] = selected_rx;
  signal.caller_tx_formats.count = selected_rx.is_valid() ? 1 : 0;
  signal.caller_rx_formats.formats[0] = selected_tx;
  signal.caller_rx_formats.count = selected_tx.is_valid() ? 1 : 0;
  signal.selected_rx_format = selected_rx;
  signal.selected_tx_format = selected_tx;
  signal.media_answer_pending = this->delayed_offer_pending_;
  this->emit_sip_signal_(std::move(signal));
  return true;
}

bool SipTransport::handle_reinvite_(const std::string &message,
                                    const sockaddr_in &src) {
  if (this->refer_dialog_ && this->refer_dialog_->owner_invite_ended && !this->refer_retiring_)
    return this->send_stateless_response_(message, src, 481, "Call/Transaction Does Not Exist");
  if (this->refer_dialog_ && !this->refer_retiring_)
    return this->send_stateless_response_(message, src, 491, "Request Pending");
  const uint32_t src_ip = ntohl(src.sin_addr.s_addr);
  const uint32_t incoming_cseq =
      cseq_number(header_value(message, "CSeq"));
  const std::string incoming_from = header_value(message, "From");
  const std::string incoming_to = header_value(message, "To");
  const std::string content_type =
      trim_copy(header_value(message, "Content-Type"));
  const size_t content_type_end = content_type.find(';');
  std::string media_type = content_type.substr(0, content_type_end);
  for (char &ch : media_type) {
    ch = static_cast<char>(
        std::tolower(static_cast<unsigned char>(ch)));
  }

  // RFC 3261 section 14: a re-INVITE belongs to the existing dialog and must
  // not create a second call lifecycle. Reject stale/cross-dialog requests
  // without disturbing the established media session.
  if (!this->media_active_.load(std::memory_order_acquire) ||
      src_ip != this->remote_ip_v4_.load(std::memory_order_acquire) ||
      tag_from_header(incoming_from) != this->remote_tag_ ||
      tag_from_header(incoming_to) != this->local_tag_) {
    return this->send_stateless_response_(
        message, src, 481, "Call/Transaction Does Not Exist", "", true);
  }
  if (this->completed_invite_.awaiting_ack) {
    // RFC 3261 section 14.2: a UAS that receives another INVITE before the
    // prior INVITE transaction is ACKed answers 500 with randomized
    // Retry-After. The occupied transaction cache must not be replaced.
    return this->send_stateless_response_(
        message, src, 500, "Server Internal Error", "request_pending",
        false, static_cast<int>(esp_random() % 11U));
  }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  if (this->pending_video_direction_invite_.pending()) {
    // 491 is reserved for glare with our own outstanding in-dialog INVITE.
    return this->send_stateless_response_(
        message, src, 491, "Request Pending");
  }
#endif
  if (incoming_cseq <= this->remote_dialog_cseq_) {
    return this->send_stateless_response_(
        message, src, 500, "Server Internal Error", "stale_cseq", true,
        static_cast<int>(esp_random() % 11U));
  }
  if (media_type != "application/sdp" || message_body(message).empty()) {
    return this->send_stateless_response_(
        message, src, 415, "Unsupported Media Type", "", true);
  }

  // handle_sip_datagram_ already owns dialog_mutex_. Serialize the complete
  // media proposal/admission/commit below against deferred main-loop starts
  // and concurrent teardown. No code under this lock reacquires dialog_mutex_.
  LockGuard media_lock(this->media_lifecycle_mutex_);
  const MediaLifecyclePhase phase =
      this->media_lifecycle_phase_.load(std::memory_order_acquire);
  if (phase == MediaLifecyclePhase::CLEANING ||
      phase == MediaLifecyclePhase::SHUTTING_DOWN ||
      !this->media_active_.load(std::memory_order_acquire)) {
    return this->send_stateless_response_(
        message, src, 491, "Request Pending");
  }

  AudioFormat old_tx;
  AudioFormat old_rx;
  uint8_t old_tx_pt = 0;
  uint8_t old_rx_pt = 0;
  const bool old_directional_audio = this->remote_directional_audio_v1_;
  const uint8_t old_audio_direction = this->get_audio_direction_();
  RtpAudioPayloads old_rx_payloads;
  this->get_media_config_(&old_tx, &old_rx, &old_tx_pt, &old_rx_pt, &old_rx_payloads);
  const uint32_t old_audio_ip =
      this->remote_rtp_ip_v4_.load(std::memory_order_acquire);
  const uint16_t old_audio_port =
      this->remote_rtp_port_.load(std::memory_order_acquire);
  const auto old_media_shape = this->remote_media_shape_;
  const std::string old_last_invite_via = this->last_invite_via_;
  const std::string old_last_invite_from = this->last_invite_from_;
  const std::string old_last_invite_to = this->last_invite_to_;
  const std::string old_last_invite_cseq = this->last_invite_cseq_;
  const std::string old_last_invite_response =
      this->last_invite_response_;
  const uint32_t old_last_invite_cseq_number =
      this->last_invite_cseq_number_;
  const uint32_t old_remote_dialog_cseq = this->remote_dialog_cseq_;
  const uint32_t old_last_invite_peer_ip =
      this->last_invite_peer_ip_v4_;
  const uint16_t old_last_invite_peer_port =
      this->last_invite_peer_port_;
  const std::string old_remote_target_uri = this->remote_target_uri_;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  const bool old_video_offered = this->video_offered_;
  const bool old_video_negotiated = this->video_negotiated_;
  const bool old_video_send = this->video_send_enabled_;
  const bool old_video_receive = this->video_receive_enabled_;
  const uint32_t old_video_ip = this->remote_video_ip_v4_;
  const uint16_t old_video_port = this->remote_video_rtp_port_;
  const uint32_t old_video_rtcp_ip = this->remote_video_rtcp_ip_v4_;
  const uint16_t old_video_rtcp_port = this->remote_video_rtcp_port_;
  const VideoCapability old_video_capability =
      this->negotiated_video_capability_;
#endif

  const auto restore_old_media = [&]() {
    this->set_media_config_(old_tx, old_rx, old_tx_pt, old_rx_pt, &old_rx_payloads);
    this->set_audio_direction_(old_audio_direction);
  this->remote_directional_audio_v1_ = old_directional_audio;
    this->remote_rtp_ip_v4_.store(old_audio_ip, std::memory_order_release);
    this->remote_rtp_port_.store(old_audio_port, std::memory_order_release);
    this->remote_media_shape_ = old_media_shape;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
    this->video_offered_ = old_video_offered;
    this->video_negotiated_ = old_video_negotiated;
    this->video_send_enabled_ = old_video_send;
    this->video_receive_enabled_ = old_video_receive;
    this->remote_video_ip_v4_ = old_video_ip;
    this->remote_video_rtp_port_ = old_video_port;
    this->remote_video_rtcp_ip_v4_ = old_video_rtcp_ip;
    this->remote_video_rtcp_port_ = old_video_rtcp_port;
    this->negotiated_video_capability_ = old_video_capability;
#endif
  };
  const auto restore_old_dialog_metadata = [&]() {
    this->last_invite_via_ = old_last_invite_via;
    this->last_invite_from_ = old_last_invite_from;
    this->last_invite_to_ = old_last_invite_to;
    this->last_invite_cseq_ = old_last_invite_cseq;
    this->last_invite_response_ = old_last_invite_response;
    this->last_invite_cseq_number_ = old_last_invite_cseq_number;
    this->remote_dialog_cseq_ = old_remote_dialog_cseq;
    this->last_invite_peer_ip_v4_ = old_last_invite_peer_ip;
    this->last_invite_peer_port_ = old_last_invite_peer_port;
    this->remote_target_uri_ = old_remote_target_uri;
  };

  ScopedMediaProposal proposal_scope(this->media_proposal_epoch_);
  if (!this->learn_remote_rtp_from_sdp_(message_body(message), src_ip, false, true)) {
    restore_old_media();
    return this->send_stateless_response_(
        message, src, 488, "Not Acceptable Here", "media_incompatible",
        true);
  }

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  const uint32_t proposed_sdp_version = this->sdp_session_version_ + 1;
  this->sdp_session_version_ = proposed_sdp_version;
#endif
  const std::string answer = this->build_sdp_answer_();
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  this->sdp_session_version_ = proposed_sdp_version - 1;
#endif
  if (answer.empty()) {
    restore_old_media();
    return this->send_stateless_response_(
        message, src, 488, "Not Acceptable Here", "media_incompatible",
        true);
  }

  // SDP parsing writes into the transport's live media fields. Capture the
  // proposal and restore the established session before replying so neither
  // the audio task nor the video child can observe a half-committed re-INVITE.
  AudioFormat new_tx;
  AudioFormat new_rx;
  uint8_t new_tx_pt = 0;
  uint8_t new_rx_pt = 0;
  RtpAudioPayloads new_rx_payloads;
  this->get_media_config_(&new_tx, &new_rx, &new_tx_pt, &new_rx_pt, &new_rx_payloads);
  const uint8_t new_audio_direction = this->get_audio_direction_();
  const uint32_t new_audio_ip =
      this->remote_rtp_ip_v4_.load(std::memory_order_acquire);
  const uint16_t new_audio_port =
      this->remote_rtp_port_.load(std::memory_order_acquire);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  const bool new_video_offered = this->video_offered_;
  const bool new_video_negotiated = this->video_negotiated_;
  const bool new_video_send = this->video_send_enabled_;
  const bool new_video_receive = this->video_receive_enabled_;
  const uint32_t new_video_ip = this->remote_video_ip_v4_;
  const uint16_t new_video_port = this->remote_video_rtp_port_;
  const uint32_t new_video_rtcp_ip = this->remote_video_rtcp_ip_v4_;
  const uint16_t new_video_rtcp_port = this->remote_video_rtcp_port_;
  const VideoCapability new_video_capability =
      this->negotiated_video_capability_;
#endif
  restore_old_media();
  proposal_scope.finish();

  const bool same_audio =
      new_tx == old_tx && new_rx == old_rx &&
      new_tx_pt == old_tx_pt && new_rx_pt == old_rx_pt &&
      new_rx_payloads == old_rx_payloads &&
      new_audio_ip == old_audio_ip && new_audio_port == old_audio_port;
  if (!same_audio) {
    return this->send_stateless_response_(
        message, src, 488, "Not Acceptable Here",
        "audio_renegotiation_unsupported", true);
  }

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  // Keep one authoritative media child. A video attachment can be admitted
  // before the final response when this re-INVITE adds it to an audio-only
  // dialog. Once established, codec, PT and transport endpoints stay fixed:
  // an ordinary session refresh is harmless, while a mutation is rejected
  // without disturbing the working media negotiated by the prior offer.
  const bool same_video_media =
      old_video_capability.payload_type == new_video_capability.payload_type &&
      old_video_capability.clock_rate == new_video_capability.clock_rate &&
      old_video_capability.max_fps == new_video_capability.max_fps &&
      old_video_capability.packetization_mode ==
          new_video_capability.packetization_mode &&
      old_video_capability.level_asymmetry_allowed ==
          new_video_capability.level_asymmetry_allowed &&
      old_video_capability.encoding == new_video_capability.encoding &&
      old_video_capability.profile_level_id ==
          new_video_capability.profile_level_id &&
      old_video_capability.max_bitrate_bps ==
          new_video_capability.max_bitrate_bps &&
      old_video_capability.rtcp_feedback_pli ==
          new_video_capability.rtcp_feedback_pli &&
      old_video_capability.rtcp_feedback_fir ==
          new_video_capability.rtcp_feedback_fir &&
      old_video_ip == new_video_ip &&
      old_video_port == new_video_port &&
      old_video_rtcp_ip == new_video_rtcp_ip &&
      old_video_rtcp_port == new_video_rtcp_port;
  const bool same_video_direction =
      old_video_send == new_video_send &&
      old_video_receive == new_video_receive;
  if (old_video_negotiated && new_video_negotiated &&
      !same_video_media) {
    restore_old_media();
    return this->send_stateless_response_(
        message, src, 488, "Not Acceptable Here",
        "video_renegotiation_unsupported", true);
  }

  bool new_video_prepared = false;
  bool video_direction_requested = false;
  bool video_remove_requested = false;
  const bool replace_video_direction =
      old_video_negotiated && new_video_negotiated &&
      !same_video_direction;
  if (!old_video_negotiated && new_video_negotiated) {
    if (this->video_session_ == nullptr) {
      restore_old_media();
      return this->send_stateless_response_(
          message, src, 488, "Not Acceptable Here",
          "video_resources_unavailable", true);
    }
    if (this->video_session_->is_running()) {
      if (!this->video_session_->negotiation_matches(
              new_video_capability,
              this->local_video_direction_capability_(
                  new_video_capability, true),
              this->local_video_direction_capability_(
                  new_video_capability, false),
              new_video_ip, new_video_port,
              new_video_rtcp_ip, new_video_rtcp_port) ||
          !this->video_session_->can_request_media_direction(
              new_video_send, new_video_receive)) {
        restore_old_media();
        return this->send_stateless_response_(
            message, src, 488, "Not Acceptable Here",
            "video_renegotiation_unsupported", true);
      }
      video_direction_requested = true;
    } else if (!this->video_session_->set_negotiated(
                   new_video_capability,
                   this->local_video_direction_capability_(
                       new_video_capability, true),
                   this->local_video_direction_capability_(
                       new_video_capability, false),
                   new_video_ip, new_video_port,
                   new_video_rtcp_ip, new_video_rtcp_port, new_video_send,
                   new_video_receive) ||
               !this->video_session_->start(false)) {
      if (this->video_session_ != nullptr)
        this->video_session_->request_stop();
      restore_old_media();
      return this->send_stateless_response_(
          message, src, 488, "Not Acceptable Here",
          "video_resources_unavailable", true);
    } else {
      new_video_prepared = true;
    }
    video_direction_requested = true;
  } else if ((replace_video_direction ||
              (old_video_negotiated && !new_video_negotiated)) &&
             (this->video_session_ == nullptr ||
              !this->video_session_->can_request_media_direction(
                  new_video_negotiated && new_video_send,
                  new_video_negotiated && new_video_receive))) {
    restore_old_media();
    return this->send_stateless_response_(
        message, src, 488, "Not Acceptable Here",
        "video_resources_unavailable", true);
  } else if (replace_video_direction ||
             (old_video_negotiated && !new_video_negotiated)) {
    video_remove_requested =
        old_video_negotiated && !new_video_negotiated;
    video_direction_requested = !video_remove_requested;
  }
#endif

  this->last_invite_via_ = header_values(message, "Via");
  this->last_invite_from_ = incoming_from;
  this->last_invite_to_ = incoming_to;
  this->last_invite_cseq_ = header_value(message, "CSeq");
  this->last_invite_cseq_number_ = incoming_cseq;
  this->remote_dialog_cseq_ = incoming_cseq;
  this->last_invite_peer_ip_v4_ = src_ip;
  this->last_invite_peer_port_ = ntohs(src.sin_port);
  const std::string refreshed_target =
      strip_angle_uri(header_value(message, "Contact"));
  if (!refreshed_target.empty())
    this->remote_target_uri_ = refreshed_target;

  // Resource admission above has completed, so the final response never
  // promises a newly-added video stream that failed to allocate. Existing
  // media is left untouched until this offer/answer commits.
  if (!this->send_response_(200, "OK", answer)) {
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
    if (new_video_prepared && this->video_session_ != nullptr)
      this->video_session_->request_stop();
#endif
    restore_old_media();
    restore_old_dialog_metadata();
    return false;
  }

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  // The final response commits offer/answer. Only now may the prepared camera
  // producer or receive presentation become active.
  if (video_remove_requested) {
    // Publish the inactive direction before terminating the RTP child so the
    // camera and renderer leave video mode immediately on m=video 0.
    this->video_session_->request_media_direction(false, false);
    this->video_session_->request_stop();
  } else if (video_direction_requested &&
      !this->video_session_->request_media_direction(
          new_video_negotiated && new_video_send,
          new_video_negotiated && new_video_receive)) {
    ESP_LOGE(TAG,
             "Prepared video direction failed after 200; terminating dialog");
    // A UAS must not originate BYE before the ACK for its 2xx response. Gate
    // media now and let the ACK path (or its timeout) terminate the dialog.
    // media_lifecycle_mutex_ is already held, so use the lock-aware primitive.
    this->request_audio_path_stop_locked_();
    this->terminate_after_invite_ack_ = true;
    SipSignal signal;
    signal.type = SipSignalType::MEDIA_INCOMPATIBLE;
    signal.status_code = 500;
    signal.call_id = this->call_id_;
    signal.reason = "video_activation_failed";
    signal.terminal_transaction_pending = true;
    this->emit_sip_signal_(std::move(signal));
    return true;
  }
#endif

  // Admission proved the prepared codec/PT contract is unchanged. Commit only
  // direction, leaving the existing buffers, converters and descriptors intact.
  if ((new_audio_direction & 2U) != 0 && (old_audio_direction & 2U) == 0)
    this->audio_rx_discontinuity_.store(true, std::memory_order_release);
  this->set_audio_direction_(new_audio_direction);
  this->remote_rtp_ip_v4_.store(new_audio_ip, std::memory_order_release);
  this->remote_rtp_port_.store(new_audio_port, std::memory_order_release);
  this->reset_rtp_latch_();
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  this->video_offered_ = new_video_offered;
  this->video_negotiated_ = new_video_negotiated;
  this->video_send_enabled_ = new_video_send;
  this->video_receive_enabled_ = new_video_receive;
  this->remote_video_ip_v4_ = new_video_ip;
  this->remote_video_rtp_port_ = new_video_port;
  this->remote_video_rtcp_ip_v4_ = new_video_rtcp_ip;
  this->remote_video_rtcp_port_ = new_video_rtcp_port;
  this->negotiated_video_capability_ = new_video_capability;
#endif
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  this->sdp_session_version_ = proposed_sdp_version;
  this->confirmed_local_sdp_ = answer;
  this->emit_video_active_state_(
      this->video_negotiated_ && this->video_session_ != nullptr &&
      this->video_session_->is_running() &&
      (this->video_send_enabled_ || this->video_receive_enabled_));
  this->emit_video_send_state_(
      this->video_negotiated_ && this->video_send_enabled_, false);
#endif
  ESP_LOGI(TAG, "SIP re-INVITE accepted in existing dialog call_id=%s cseq=%u",
           this->call_id_.c_str(), (unsigned) incoming_cseq);
  return true;
}

bool SipTransport::handle_update_(const std::string &message,
                                  const sockaddr_in &src) {
  if (this->replay_completed_response_(message, src, "UPDATE", !this->remote_sip_tcp_.load(std::memory_order_acquire))) {
    return true;
  }
  const std::string call_id = header_value(message, "Call-ID");
  const std::string from = header_value(message, "From");
  const std::string to = header_value(message, "To");
  const std::string cseq = header_value(message, "CSeq");
  const uint32_t sequence = cseq_number(cseq);
  if (!this->media_active_.load(std::memory_order_acquire) ||
      call_id.empty() || call_id != this->call_id_ ||
      cseq_method(cseq) != "UPDATE" || sequence == 0 ||
      tag_from_header(from) != this->remote_tag_ ||
      tag_from_header(to) != this->local_tag_) {
    return this->send_stateless_response_(
        message, src, 481, "Call/Transaction Does Not Exist", "", true);
  }
  if (sequence <= this->remote_dialog_cseq_) {
    return this->send_stateless_response_(
        message, src, 500, "Server Internal Error", "stale_cseq", true,
        static_cast<int>(esp_random() % 11U));
  }
  if (!message_body(message).empty()) {
    return this->send_stateless_response_(
        message, src, 488, "Not Acceptable Here",
        "media_update_unsupported", true);
  }
  const std::string identity_uri = strip_angle_uri(from);
  const std::string identity_route = sip_route_id(
      sip_user_from_header(from), VOIP_STACK_MAX_ROUTE_ID_LEN);
  if (identity_uri.empty() || identity_route.empty()) {
    return this->send_stateless_response_(
        message, src, 400, "Bad Request", "", true);
  }
  const std::string contact = header_value(message, "Contact");
  const std::string refreshed_target = strip_angle_uri(contact);
  if (!contact.empty() && refreshed_target.empty()) {
    return this->send_stateless_response_(
        message, src, 400, "Bad Request", "", true);
  }
  if (!this->send_stateless_response_(message, src, 200, "OK", "", true)) {
    return false;
  }

  std::string identity_name = sip_display_name_from_header(
      from, VOIP_STACK_MAX_NAME_LEN);
  if (identity_name.empty()) identity_name = identity_route;
  this->remote_uri_ = sip_name_addr(identity_uri, identity_name);
  if (!refreshed_target.empty()) this->remote_target_uri_ = refreshed_target;
  this->remote_dialog_cseq_ = sequence;
  if (this->dialog_originated_) {
    this->destination_ = {identity_route, identity_name};
  } else {
    this->caller_ = {identity_route, identity_name};
  }
  SipSignal signal;
  signal.type = SipSignalType::CONNECTED_IDENTITY;
  signal.call_id = this->call_id_;
  signal.connected_route = identity_route;
  signal.connected_name = identity_name;
  this->emit_sip_signal_(std::move(signal));
  return true;
}

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
bool SipTransport::handle_video_direction_response_(
    const std::string &message, const sockaddr_in &src, uint16_t status,
    uint32_t response_cseq) {
  auto &pending = this->pending_video_direction_invite_;
  if (!pending.pending() || pending.waiting_retry ||
      response_cseq != pending.cseq) {
    return false;
  }
  const uint32_t src_ip = ntohl(src.sin_addr.s_addr);
  const bool tcp = !pending.transaction.udp;
  const uint32_t expected_response_ip =
      tcp ? this->remote_ip_v4_.load(std::memory_order_acquire)
          : pending.transaction.ip_v4;
  if (header_value(message, "Call-ID") != this->call_id_ ||
      src_ip != expected_response_ip ||
      via_branch(header_value(message, "Via")) != pending.branch ||
      tag_from_header(header_value(message, "From")) != this->local_tag_ ||
      tag_from_header(header_value(message, "To")) != this->remote_tag_) {
    ESP_LOGD(TAG, "SIP video re-INVITE response ignored: transaction mismatch");
    return true;
  }

  this->mark_sip_event_(SipEvent::RESPONSE, status);
  if (status < 200) {
    pending.transaction.completed = true;
    pending.transaction.next_ms = pending.response_deadline_ms;
    return true;
  }

  if (status < 300) {
    const std::string contact =
        strip_angle_uri(header_value(message, "Contact"));
    if (!contact.empty()) this->remote_target_uri_ = contact;
  }
  std::string ack;
  SipRequestOptions ack_options;
  ack_options.cseq_number = pending.cseq;
  ack_options.cseq_method = "ACK";
  ack_options.branch_override =
      status < 300 ? "z9hG4bK" + make_token("") : pending.branch;
  ack_options.remember_transaction = false;
  ack_options.remember_invite_ack = false;
  ack_options.formatted_request = &ack;
  uint32_t ack_ip = pending.transaction.ip_v4;
  uint16_t ack_port = pending.transaction.port;
  ack_options.destination_ip = &ack_ip;
  ack_options.destination_port = &ack_port;
  const bool ack_sent = status < 300
      ? this->send_request_("ACK", "", ack_options)
      : this->send_invite_error_ack_(message, pending.transaction, false, &ack);
  if (!ack_sent) {
    ESP_LOGW(TAG,
             "SIP video re-INVITE ACK initial send failed; retaining it for "
             "a retransmitted final response");
  }
  if (!ack.empty()) {
    auto &completed = this->completed_video_direction_invite_;
    completed.clear();
    completed.call_id = this->call_id_;
    completed.cseq = pending.cseq;
    completed.branch = pending.branch;
    completed.ack = ack;
    completed.response_ip_v4 = src_ip;
    completed.ack_ip_v4 = ack_ip;
    completed.ack_port = ack_port;
    completed.completed_ms = millis();
    completed.udp = status >= 300 ? pending.transaction.udp
        : !this->remote_sip_tcp_.load(std::memory_order_acquire);
  }

  const bool previous_send = pending.previous_send;
  const bool target_send = pending.target_send;
  const uint32_t accepted_version = pending.session_version;
  const std::string accepted_offer = pending.offered_sdp;
  const uint8_t retry_count = pending.retry_count;
  if (status >= 200 && status < 300) {
    bool accepted_send = previous_send;
    bool media_ok = false;
    {
      LockGuard media_lock(this->media_lifecycle_mutex_);
      media_ok = this->apply_video_direction_answer_(
          message_body(message), src_ip, &accepted_send);
    }
    if (!media_ok) {
      ESP_LOGE(TAG,
               "SIP video re-INVITE 2xx changed established media; "
               "terminating the invalid dialog");
      pending.clear();
      this->video_send_requested_.store(previous_send,
                                        std::memory_order_release);
      this->emit_video_send_state_(previous_send, false);
      const bool bye_sent = this->send_bye_unlocked_(this->call_id_);
      SipSignal signal;
      signal.type = SipSignalType::MEDIA_INCOMPATIBLE;
      signal.status_code = 488;
      signal.call_id = this->call_id_;
      signal.reason = "video_reinvite_answer_incompatible";
      signal.terminal_transaction_pending = bye_sent;
      if (!bye_sent) this->reset_dialog_();
      this->emit_sip_signal_(std::move(signal));
      return true;
    }
    this->sdp_session_version_ = accepted_version;
    this->confirmed_local_sdp_ = accepted_offer;
    pending.clear();
    this->emit_video_send_state_(accepted_send, false);
    ESP_LOGI(TAG,
             "SIP video direction committed cseq=%u requested=%s accepted=%s",
             (unsigned) response_cseq, target_send ? "send" : "receive-only",
             accepted_send ? "send" : "receive-only");
    return true;
  }

  if (status == 491 && retry_count == 0) {
    const uint32_t retry_delay =
        this->dialog_originated_ ? 2100U + (esp_random() % 1901U)
                                 : esp_random() % 2001U;
    pending.transaction.clear();
    pending.cseq = 0;
    pending.branch.clear();
    pending.response_deadline_ms = 0;
    pending.waiting_retry = true;
    pending.retry_count = 1;
    pending.retry_at_ms = millis() + retry_delay;
    ESP_LOGI(TAG,
             "SIP video direction glare; one retry scheduled in %u ms",
             (unsigned) retry_delay);
    this->wake_sip_task_();
    return true;
  }

  pending.clear();
  this->video_send_requested_.store(previous_send,
                                    std::memory_order_release);
  this->emit_video_send_state_(previous_send, false);
  ESP_LOGW(TAG,
           "SIP video direction rejected status=%u; established media retained",
           (unsigned) status);
  if (status == 408 || status == 481) {
    const std::string call_id = this->call_id_;
    SipSignal signal;
    signal.type = SipSignalType::PROTOCOL_ERROR;
    signal.status_code = status;
    signal.call_id = call_id;
    signal.reason =
        status == 481 ? "dialog_not_found" : "video_reinvite_timeout";
    // RFC 3261 section 12.2.1.2: a 408/481 to an in-dialog request means the
    // dialog is no longer usable. Attempt a standards-based BYE first and
    // fall back to local cleanup only if it cannot be put on the wire.
    const bool bye_pending = this->send_bye_unlocked_(call_id);
    signal.terminal_transaction_pending = bye_pending;
    if (!bye_pending) this->reset_dialog_();
    this->emit_sip_signal_(std::move(signal));
  }
  return true;
}

void SipTransport::pump_video_direction_transaction_() {
  auto &pending = this->pending_video_direction_invite_;
  if (!pending.pending()) return;
  const uint32_t now = millis();
  if (pending.waiting_retry) {
    if (!time_reached(now, pending.retry_at_ms)) return;
    const bool target = pending.target_send;
    if (!this->send_video_direction_reinvite_unlocked_(target, true)) {
      const bool previous = pending.previous_send;
      pending.clear();
      this->video_send_requested_.store(previous,
                                        std::memory_order_release);
      this->emit_video_send_state_(previous, false);
      ESP_LOGW(TAG, "SIP video direction retry could not be sent");
    }
    return;
  }
  if (time_reached(now, pending.response_deadline_ms)) {
    const bool previous = pending.previous_send;
    const std::string call_id = this->call_id_;
    pending.clear();
    this->video_send_requested_.store(previous,
                                      std::memory_order_release);
    this->emit_video_send_state_(previous, false);
    ESP_LOGE(TAG, "SIP video direction re-INVITE timed out");
    const bool bye_sent = this->send_bye_unlocked_(call_id);
    SipSignal signal;
    signal.type = SipSignalType::PROTOCOL_ERROR;
    signal.status_code = 408;
    signal.call_id = call_id;
    signal.reason = "video_reinvite_timeout";
    signal.terminal_transaction_pending = bye_sent;
    if (!bye_sent) this->reset_dialog_();
    this->emit_sip_signal_(std::move(signal));
    return;
  }
  if (!pending.transaction.udp ||
      pending.transaction.completed ||
      !time_reached(now, pending.transaction.next_ms)) {
    return;
  }
  const bool sent =
      this->send_sip_(pending.transaction.request,
                      pending.transaction.ip_v4,
                      pending.transaction.port);
  pending.transaction.retries++;
  ESP_LOGD(TAG, "SIP UDP video re-INVITE retransmit #%u%s",
           (unsigned) pending.transaction.retries,
           sent ? "" : " failed");
  pending.transaction.interval_ms =
      std::min<uint16_t>(
          static_cast<uint16_t>(pending.transaction.interval_ms * 2),
          SIP_T2_MS);
  pending.transaction.next_ms =
      now + pending.transaction.interval_ms;
  if (time_reached(pending.transaction.next_ms,
                   pending.response_deadline_ms)) {
    pending.transaction.next_ms = pending.response_deadline_ms;
  }
}
#endif

bool SipTransport::handle_response_(const std::string &message, const sockaddr_in &src) {
  const uint32_t src_ip = ntohl(src.sin_addr.s_addr);
  if (message.rfind("SIP/2.0 ", 0) != 0 || message.size() < 12) return false;
  if (!std::isdigit(static_cast<unsigned char>(message[8])) ||
      !std::isdigit(static_cast<unsigned char>(message[9])) ||
      !std::isdigit(static_cast<unsigned char>(message[10]))) {
    return false;
  }
  const int status = std::atoi(message.substr(8, 3).c_str());
  if (status < 100 || status > 699) return false;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  if (this->replay_completed_video_direction_ack_(message, src)) return true;
#endif
  #ifdef USE_ESPHOME_VOIP_REGISTRATION
  if (this->replay_completed_invite_ack_(message, src, this->dispatch_socket_ < 0)) return true;
#else
  if (this->replay_completed_invite_ack_(message, src, !this->remote_sip_tcp_.load(std::memory_order_acquire))) return true;
#endif
  if (cseq_method(header_value(message, "CSeq")) == "NOTIFY" && this->refer_dialog_) {
    for (auto *transaction : {&this->refer_dialog_->pending_notify_, &this->refer_dialog_->pending_notify_final_}) {
      if (!transaction->empty() &&
          header_value(message, "Call-ID") == header_value(transaction->request, "Call-ID") &&
          header_value(message, "CSeq") == header_value(transaction->request, "CSeq") &&
          via_branch(header_value(message, "Via")) == via_branch(header_value(transaction->request, "Via")) &&
          src_ip == transaction->ip_v4 && ntohs(src.sin_port) == transaction->port) {
        if (status >= 200) transaction->clear();
        return true;
      }
    }
    return false;
  }
  const std::string response_call_id = header_value(message, "Call-ID");
  if (response_call_id.empty() || this->call_id_.empty() || response_call_id != this->call_id_) {
    ESP_LOGD(TAG, "SIP response ignored for stale/unknown call_id=%s current=%s",
             response_call_id.empty() ? "(empty)" : response_call_id.c_str(),
             this->call_id_.empty() ? "(none)" : this->call_id_.c_str());
    return true;
  }
  const std::string response_cseq = header_value(message, "CSeq");
  const std::string method = cseq_method(response_cseq);
  const uint32_t response_cseq_number = cseq_number(response_cseq);
  if (method.empty() || response_cseq_number == 0) {
    ESP_LOGD(TAG, "SIP response ignored without a valid CSeq");
    return true;
  }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  if (method == "INVITE" &&
      this->handle_video_direction_response_(
          message, src, static_cast<uint16_t>(status),
          response_cseq_number)) {
    return true;
  }
#endif
  if ((method == "INVITE" || method == "CANCEL") && response_cseq_number != this->invite_cseq_) {
    ESP_LOGD(TAG, "SIP response ignored for stale CSeq %u %s (active INVITE CSeq=%u)",
             (unsigned) response_cseq_number, method.c_str(), (unsigned) this->invite_cseq_);
    return true;
  }
  if (method != "INVITE" && method != "CANCEL" && method != "BYE" &&
      method != "UPDATE") {
    ESP_LOGD(TAG, "SIP response ignored for unsupported transaction method %s", method.c_str());
    return true;
  }
  const std::string response_from_tag = tag_from_header(header_value(message, "From"));
  if (this->local_tag_.empty() || response_from_tag != this->local_tag_) {
    ESP_LOGD(TAG, "SIP response ignored for mismatched local From tag");
    return true;
  }
  if (method == "BYE") {
    if (this->pending_bye_.empty() ||
        response_cseq_number != cseq_number(header_value(this->pending_bye_.request, "CSeq")) ||
        via_branch(header_value(message, "Via")) != via_branch(header_value(this->pending_bye_.request, "Via"))) {
      ESP_LOGD(TAG, "SIP response ignored for mismatched BYE transaction");
      return true;
    }
  }
  if (method == "UPDATE") {
    if (this->pending_update_.empty() ||
        response_cseq_number != cseq_number(
            header_value(this->pending_update_.request, "CSeq")) ||
        via_branch(header_value(message, "Via")) != via_branch(
            header_value(this->pending_update_.request, "Via"))) {
      ESP_LOGD(TAG, "SIP response ignored for mismatched UPDATE transaction");
      return true;
    }
  }
  if (method == "INVITE" || method == "CANCEL") {
    const std::string response_branch = via_branch(header_value(message, "Via"));
    if (this->branch_.empty() || response_branch.empty() || response_branch != this->branch_) {
      ESP_LOGD(TAG, "SIP response ignored for mismatched %s transaction branch", method.c_str());
      return true;
    }
  }
  if (!this->remote_sip_tcp_.load(std::memory_order_acquire)) {
    uint32_t expected_response_ip = this->remote_ip_v4_.load(std::memory_order_acquire);
    const UdpTransaction *transaction = method == "BYE" ? &this->pending_bye_
                                        : method == "CANCEL" ? &this->pending_cancel_
                                        : method == "UPDATE" ? &this->pending_update_
                                                              : &this->pending_invite_;
    if (!transaction->empty() && transaction->ip_v4 != 0) expected_response_ip = transaction->ip_v4;
    if (expected_response_ip != 0 && src_ip != expected_response_ip) {
      ESP_LOGD(TAG, "SIP response ignored for %s from unexpected peer", method.c_str());
      return true;
    }
  }
  const std::string response_to_tag = tag_from_header(header_value(message, "To"));
  if (status > 100) {
    if (!response_to_tag.empty() && !this->remote_tag_.empty() && response_to_tag != this->remote_tag_) {
      ESP_LOGD(TAG, "SIP response ignored for missing/mismatched remote To tag");
      return true;
    }
    if (response_to_tag.empty() && method != "INVITE") {
      ESP_LOGD(TAG, "SIP response ignored without a remote To tag");
      return true;
    }
    if (this->remote_tag_.empty() && !response_to_tag.empty()) this->remote_tag_ = response_to_tag;
  }
  if (method == "INVITE" && status > 100 &&
      sip_option_supported(message, "from-change")) {
    this->peer_supports_from_change_ = true;
  }
  // Only a response belonging to the active dialog may retarget subsequent
  // ACK/BYE traffic. Stale or spoofed responses must be side-effect free.
  this->remote_ip_v4_.store(src_ip, std::memory_order_release);
  this->remote_sip_port_.store(ntohs(src.sin_port), std::memory_order_release);
  this->mark_sip_event_(SipEvent::RESPONSE, static_cast<uint16_t>(status));
  if (method == "INVITE" && status >= 100) {
    if (status < 200 && !this->pending_invite_.empty()) {
      // A provisional response stops INVITE retransmission but not Timer B.
      // Keep the bounded transaction until its original 64*T1 deadline so a
      // peer that sends 100/180 and then disappears cannot strand the call.
      this->pending_invite_.completed = true;
      this->pending_invite_.next_ms = this->pending_invite_.deadline_ms;
    } else {
      if (status >= 300)
        this->send_invite_error_ack_(message, this->pending_invite_, true, nullptr);
      this->clear_invite_transaction_();
    }
  }
  if (status > 100 && status < 200 && method == "INVITE") {
    SipSignal signal;
    signal.type = SipSignalType::STATUS_180_RINGING;
    signal.status_code = static_cast<uint16_t>(status);
    signal.call_id = this->call_id_;
    this->emit_sip_signal_(std::move(signal));
    return true;
  }
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  if ((method == "INVITE" || method == "BYE" || method == "UPDATE") && (status == 401 || status == 407) &&
      this->active_account_ >= 0 && this->retry_authenticated_request_(message, method, status)) return true;
#endif
  if (status >= 200 && status < 300) {
#ifdef USE_ESPHOME_VOIP_REGISTRATION
    this->dialog_auth_attempts_ = 0; this->dialog_proxy_auth_attempts_ = 0;
    if (method == "INVITE" && this->active_account_ >= 0) this->remember_registration_routes_(message, true);
#endif
    if (method == "BYE") {
      const std::string completed_call_id = this->call_id_;
      ESP_LOGI(TAG, "SIP BYE completed call_id=%s", completed_call_id.c_str());
      this->clear_bye_transaction_();
      this->reset_dialog_();
      SipSignal signal;
      signal.type = SipSignalType::TERMINAL_COMPLETE;
      signal.status_code = static_cast<uint16_t>(status);
      signal.call_id = completed_call_id;
      this->emit_sip_signal_(std::move(signal));
      return true;
    }
    if (method == "CANCEL") {
      ESP_LOGI(TAG, "SIP CANCEL completed call_id=%s", this->call_id_.c_str());
      if (!this->pending_cancel_.empty()) {
        // Stop retransmitting CANCEL, but retain the transaction until Timer F
        // while waiting for the INVITE's final 487.
        this->pending_cancel_.completed = true;
        this->pending_cancel_.next_ms = this->pending_cancel_.deadline_ms;
      }
      return true;
    }
    if (method == "UPDATE") {
      ESP_LOGI(TAG, "SIP connected identity UPDATE completed call_id=%s",
               this->call_id_.c_str());
      this->pending_update_.clear();
      return true;
    }
    if (method != "INVITE") {
      ESP_LOGI(TAG, "SIP %u response for %s ignored", status, method.c_str());
      return true;
    }
    this->outgoing_invite_pending_.store(false, std::memory_order_release);
    const std::string contact_target = strip_angle_uri(header_value(message, "Contact"));
    if (!contact_target.empty()) this->remote_target_uri_ = contact_target;
    SipRequestOptions options;
    options.cseq_number = this->invite_cseq_;
    this->send_request_("ACK", "", options);
    if (this->remote_tag_.empty()) {
      SipSignal signal;
      signal.type = SipSignalType::PROTOCOL_ERROR;
      signal.status_code = 500;
      signal.call_id = this->call_id_;
      signal.reason = "malformed_2xx";
      this->reset_dialog_();
      this->emit_sip_signal_(std::move(signal));
      return true;
    }
    bool media_ok = false;
    bool video_prepared = true;
    {
      LockGuard media_lock(this->media_lifecycle_mutex_);
      media_ok = this->learn_remote_rtp_from_sdp_(
          message_body(message), src_ip, true);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
      if (media_ok && !this->refer_context_active_)
        video_prepared = this->prepare_video_session_locked_();
#endif
    }
    if (this->cancel_requested_.load(std::memory_order_acquire)) {
      // CANCEL crossed the final 2xx. The dialog is confirmed despite the
      // cancellation, so ACK it and immediately terminate it with BYE.
      if (!this->send_bye_unlocked_(this->call_id_)) {
        this->reset_dialog_();
      }
      return true;
    }
    if (!media_ok || !video_prepared) {
      // A 2xx INVITE response still requires ACK. Terminate the confirmed SIP
      // dialog instead of promising media whose negotiated resources could
      // not be admitted locally.
      const bool bye_pending = this->send_bye_unlocked_(this->call_id_);
      SipSignal signal;
      signal.type = SipSignalType::MEDIA_INCOMPATIBLE;
      signal.status_code = 488;
      signal.call_id = this->call_id_;
      signal.reason = "media_incompatible";
      signal.terminal_transaction_pending = bye_pending;
      if (!bye_pending) this->reset_dialog_();
      this->emit_sip_signal_(std::move(signal));
      return true;
    }
    this->open_media_session_();
    SipSignal signal;
    signal.type = SipSignalType::STATUS_200_OK;
    signal.status_code = static_cast<uint16_t>(status);
    signal.call_id = this->call_id_;
    this->get_media_config_(&signal.selected_tx_format, &signal.selected_rx_format, nullptr, nullptr);
    this->emit_sip_signal_(std::move(signal));
    return true;
  }
  if (status >= 300) {
    if (method != "INVITE") {
      ESP_LOGW(TAG, "SIP %u response for %s", status, method.c_str());
      if (method == "BYE") {
        const std::string completed_call_id = this->call_id_;
        this->clear_bye_transaction_();
        this->reset_dialog_();
        SipSignal signal;
        signal.type = SipSignalType::TERMINAL_COMPLETE;
        signal.status_code = static_cast<uint16_t>(status);
        signal.call_id = completed_call_id;
        this->emit_sip_signal_(std::move(signal));
      } else if (method == "CANCEL") {
        if (!this->pending_cancel_.empty()) {
          this->pending_cancel_.completed = true;
          this->pending_cancel_.next_ms = this->pending_cancel_.deadline_ms;
        }
      } else if (method == "UPDATE") {
        this->pending_update_.clear();
      }
      return true;
    }
    this->outgoing_invite_pending_.store(false, std::memory_order_release);
    std::string reason = sip_header_token(header_value(message, "X-Voip-Stack-Decline-Reason"));
    if (reason.empty()) reason = reason_text_from_header(header_value(message, "Reason"));
    if (reason.empty()) reason = sip_failure_reason_(status);
    SipSignal signal;
    signal.type = status == 401 ? SipSignalType::AUTH_REQUIRED
                : status == 407 ? SipSignalType::PROXY_AUTH_REQUIRED
                : status == 488 ? SipSignalType::MEDIA_INCOMPATIBLE
                                : SipSignalType::FINAL_RESPONSE;
    signal.status_code = static_cast<uint16_t>(status);
    signal.call_id = this->call_id_;
    signal.reason = reason;
    this->emit_sip_signal_(std::move(signal));
    this->reset_dialog_();
    return true;
  }
  return true;
}

void SipTransport::handle_sip_datagram_(const char *data, size_t len, const sockaddr_in &src) {
  LockGuard lock(this->dialog_mutex_);
  this->handle_sip_datagram_unlocked_(data, len, src);
}

void SipTransport::handle_sip_datagram_unlocked_(const char *data, size_t len, const sockaddr_in &src) {
  const std::string msg(data, len);
  size_t declared_body_len = 0;
  const std::string content_length = header_value(msg, "Content-Length");
  const size_t body_separator = msg.find("\r\n\r\n");
  const bool invalid_framing = !sip_content_length(msg, &declared_body_len) ||
                               (!content_length.empty() &&
                                (body_separator == std::string::npos ||
                                 declared_body_len != msg.size() - body_separator - 4));
  if (invalid_framing) {
    ESP_LOGW(TAG, "SIP message rejected: invalid Content-Length framing");
    if (msg.rfind("SIP/2.0 ", 0) != 0) {
      this->send_stateless_response_(msg, src, 400, "Bad Request");
    }
    return;
  }
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  if (this->registration_ingress_(msg, src, this->dispatch_socket_)) return;
#endif
  if (!this->refer_context_active_ && this->refer_dialog_ && header_value(msg, "Call-ID") == this->refer_wire_id_) {
    {
      ReferScope scope(*this);
      this->handle_sip_datagram_unlocked_(data, len, src);
    }
    this->pump_refer_();
    return;
  }
  if (msg.rfind("SIP/2.0 ", 0) == 0) {
    this->handle_response_(msg, src);
    this->pump_refer_();
    return;
  }
  const size_t first_space = msg.find(' ');
  const std::string method = first_space == std::string::npos ? "" : msg.substr(0, first_space);
  ESP_LOGI(TAG, "SIP RX method=%s len=%u", method.c_str(), (unsigned) len);
  if ((method == "CANCEL" || method == "BYE" || method == "REFER") &&
      this->replay_completed_response_(msg, src, method,
#ifdef USE_ESPHOME_VOIP_REGISTRATION
                                         this->dispatch_socket_ < 0
#else
                                         !this->remote_sip_tcp_.load(std::memory_order_acquire)
#endif
                                         )) {
    return;
  }
  const SipEvent event = sip_event_from_method_(method);
  if (event != SipEvent::NONE) this->mark_sip_event_(event);
  if (this->refer_context_active_ && (method == "INVITE" || method == "UPDATE")) {
    this->send_stateless_response_(msg, src, 491, "Request Pending");
  } else if (method == "REFER") {
    this->handle_refer_(msg, src);
  } else if (method == "INVITE") {
#ifdef USE_ESPHOME_VOIP_REGISTRATION
    if (this->call_id_.empty()) this->remote_sip_tcp_.store(this->dispatch_socket_ >= 0, std::memory_order_release);
#endif
    this->handle_invite_(msg, src);
  } else if (method == "ACK") {
    const std::string request_call_id = header_value(msg, "Call-ID");
    bool terminate_after_ack = false;
    const uint16_t completed_status =
        this->acknowledge_completed_invite_(msg, src,
                                            &terminate_after_ack);
    if (terminate_after_ack) {
      const std::string call_id = this->call_id_;
      if (!this->send_bye_unlocked_(call_id)) this->reset_dialog_();
      return;
    }
    if (completed_status >= 300) {
      // ACK is the terminal event for a non-2xx INVITE server transaction.
      // The dialog may already have been detached after CANCEL, but the call
      // lifecycle must still cross its cleanup barrier and become reusable.
      SipSignal signal;
      signal.type = SipSignalType::TERMINAL_COMPLETE;
      signal.status_code = completed_status;
      signal.call_id = request_call_id;
      this->emit_sip_signal_(std::move(signal));
      return;
    }
    const uint32_t expected_ip = this->remote_ip_v4_.load(std::memory_order_acquire);
    const uint32_t request_cseq = cseq_number(header_value(msg, "CSeq"));
    const bool valid_ack = completed_status >= 200 && completed_status < 300 &&
                           !request_call_id.empty() && !this->call_id_.empty() &&
                           request_call_id == this->call_id_ &&
                           (expected_ip == 0 || ntohl(src.sin_addr.s_addr) == expected_ip) &&
                           this->last_invite_cseq_number_ != 0 &&
                           request_cseq == this->last_invite_cseq_number_ &&
                           cseq_method(header_value(msg, "CSeq")) == "ACK" &&
                           tag_from_header(header_value(msg, "From")) == this->remote_tag_ &&
                           tag_from_header(header_value(msg, "To")) == this->local_tag_;
    if (!valid_ack) {
      ESP_LOGD(TAG, "SIP ACK ignored for stale/invalid call_id=%s current=%s",
               request_call_id.empty() ? "(empty)" : request_call_id.c_str(),
               this->call_id_.empty() ? "(none)" : this->call_id_.c_str());
      return;
    }
    // A failed answer or local stop already owns the BYE. Replayed ACKs must
    // not reopen media or publish another completion during that transaction.
    if (!this->pending_bye_.empty()) return;
    bool media_answer_completed = false;
    if (this->delayed_offer_pending_) {
      const std::string answer = message_body(msg);
      bool accepted = false;
      if (!answer.empty()) {
        LockGuard media_lock(this->media_lifecycle_mutex_);
        accepted = this->learn_remote_rtp_from_sdp_(
            answer, ntohl(src.sin_addr.s_addr), true);
      }
      this->delayed_offer_pending_ = false;
      if (!accepted) {
        ESP_LOGW(TAG, "SIP delayed offer ACK missing a compatible SDP answer");
        const std::string failed_call_id = this->call_id_;
        if (!this->send_bye_unlocked_(this->call_id_)) this->reset_dialog_();
        SipSignal signal;
        signal.type = SipSignalType::MEDIA_INCOMPATIBLE;
        signal.status_code = 488;
        signal.call_id = failed_call_id;
        signal.reason = "media_incompatible";
        signal.terminal_transaction_pending = this->terminal_transaction_pending_locked_();
        this->emit_sip_signal_(std::move(signal));
        return;
      }
      media_answer_completed = true;
    }
    this->outgoing_invite_pending_.store(false, std::memory_order_release);
    this->open_media_session_();
    this->send_connected_identity_update_();
    if (media_answer_completed) {
      SipSignal signal;
      signal.type = SipSignalType::MEDIA_READY;
      signal.call_id = this->call_id_;
      this->get_media_config_(&signal.selected_tx_format, &signal.selected_rx_format,
                              nullptr, nullptr);
      this->emit_sip_signal_(std::move(signal));
    }
  } else if (method == "BYE") {
    if (this->reject_if_stale_dialog_(msg, src, "BYE")) return;
    const bool local_bye_pending = !this->pending_bye_.empty();
    if (!this->media_active_.load(std::memory_order_acquire) &&
        !local_bye_pending) {
      this->send_stateless_response_(msg, src, 481, "Call/Transaction Does Not Exist");
      return;
    }
    this->send_stateless_response_(msg, src, 200, "OK", "", true);
    if (!local_bye_pending && this->end_refer_invite_usage_(true)) {
      this->remote_dialog_cseq_ = cseq_number(header_value(msg, "CSeq"));
      return;
    }
    SipSignal signal;
    signal.type = SipSignalType::BYE;
    signal.call_id = this->call_id_;
    signal.terminal_transaction_pending = local_bye_pending;
    this->emit_sip_signal_(std::move(signal));
    if (!local_bye_pending) this->reset_dialog_();
  } else if (method == "CANCEL") {
    if (this->reject_if_stale_dialog_(msg, src, "CANCEL")) return;
    const uint32_t incoming_cseq_number = cseq_number(header_value(msg, "CSeq"));
    const std::string incoming_via = header_value(msg, "Via");
    const std::string incoming_branch = via_branch(incoming_via);
    const std::string invite_branch = via_branch(this->last_invite_via_);
    const std::string incoming_from_tag = tag_from_header(header_value(msg, "From"));
    const bool same_transaction_via = !incoming_branch.empty() && !invite_branch.empty()
                                          ? incoming_branch == invite_branch
                                          : incoming_via == this->last_invite_via_;
    if (cseq_method(header_value(msg, "CSeq")) != "CANCEL" ||
        this->media_active_.load(std::memory_order_acquire) || this->last_invite_cseq_number_ == 0 ||
        incoming_cseq_number != this->last_invite_cseq_number_ || !same_transaction_via) {
      this->send_stateless_response_(msg, src, 481, "Call/Transaction Does Not Exist");
      return;
    }
    if (incoming_from_tag.empty() || incoming_from_tag != this->remote_tag_) {
      this->send_stateless_response_(msg, src, 481, "Call/Transaction Does Not Exist");
      return;
    }
    this->send_stateless_response_(msg, src, 200, "OK", "", true);
    this->send_response_(487, "Request Terminated");
    SipSignal signal;
    signal.type = SipSignalType::CANCEL;
    signal.status_code = 487;
    signal.call_id = this->call_id_;
    signal.reason = "cancelled";
    this->emit_sip_signal_(std::move(signal));
    this->reset_dialog_();
  } else if (method == "UPDATE") {
    this->handle_update_(msg, src);
  } else if (method == "OPTIONS") {
    this->send_stateless_response_(msg, src, 200, "OK");
  } else if (sip_method_known_(method)) {
    this->send_stateless_response_(msg, src, 405, "Method Not Allowed");
  } else {
    this->send_stateless_response_(msg, src, 501, "Not Implemented");
  }
}

bool SipTransport::reject_if_stale_dialog_(const std::string &request, const sockaddr_in &src,
                                           const char *method_name) {
  const std::string request_call_id = header_value(request, "Call-ID");
  const bool call_id_matches =
      !request_call_id.empty() && !this->call_id_.empty() && request_call_id == this->call_id_;
  const uint32_t expected_ip = this->remote_ip_v4_.load(std::memory_order_acquire);
  const bool host_matches = expected_ip == 0 || ntohl(src.sin_addr.s_addr) == expected_ip;
  bool dialog_tags_match = true;
  if (std::strcmp(method_name, "BYE") == 0 || std::strcmp(method_name, "REFER") == 0) {
    const std::string from_tag = tag_from_header(header_value(request, "From"));
    const std::string to_tag = tag_from_header(header_value(request, "To"));
    dialog_tags_match = !this->remote_tag_.empty() && !this->local_tag_.empty() &&
                        from_tag == this->remote_tag_ && to_tag == this->local_tag_ &&
                        cseq_method(header_value(request, "CSeq")) == method_name &&
                        cseq_number(header_value(request, "CSeq")) > this->remote_dialog_cseq_;
  }
  if (call_id_matches && host_matches && dialog_tags_match) return false;
  ESP_LOGW(TAG, "SIP %s ignored for stale call_id=%s current=%s",
           method_name, request_call_id.c_str(), this->call_id_.c_str());
  this->send_stateless_response_(request, src, 481, "Call/Transaction Does Not Exist");
  return true;
}

void SipTransport::handle_sip_stream_(int socket, const sockaddr_in &src) {
  char *const buf = this->sip_rx_scratch_.data();
  auto *flow = this->tcp_flow_for_socket_(socket);
  if (flow == nullptr) return;
  auto drop_tcp_stream = [&](const char *reason) {
    char ip[16];
    inet_ntoa_r(src.sin_addr, ip, sizeof(ip));
    ESP_LOGW(TAG, "%s from %s, dropping connection", reason, ip);
    this->handle_tcp_peer_loss_(flow);
  };
  bool peer_closed = false;
  while (true) {
    const int n = recv(socket, buf, 1024, 0);
    if (n > 0) {
      flow->rx_buffer.append(buf, static_cast<size_t>(n));
      if (flow->rx_buffer.size() > MAX_SIP_TCP_RX_BUFFER) {
        drop_tcp_stream("SIP TCP RX buffer overflow");
        return;
      }
      continue;
    }
    if (n == 0) {
      ESP_LOGI(TAG, "SIP TCP peer closed");
      peer_closed = true;
      break;
    }
    const int err = errno;
    if (err == EINTR) continue;
    if (err == EWOULDBLOCK || err == EAGAIN || err == ENOTCONN || err == EINPROGRESS || err == EALREADY) break;
    ESP_LOGW(TAG, "SIP TCP RX failed: %s (%d: %s)", socket_errno_name(err), err, socket_errno_text(err));
    peer_closed = true;
    break;
  }

  while (true) {
    flow = this->tcp_flow_for_socket_(socket);
    if (flow == nullptr) return;
    const size_t sep = flow->rx_buffer.find("\r\n\r\n");
    if (sep == std::string::npos) {
      if (peer_closed) this->handle_tcp_peer_loss_(flow);
      return;
    }
    size_t body_len = 0;
    if (!sip_content_length(flow->rx_buffer, &body_len)) {
      drop_tcp_stream("SIP TCP invalid or ambiguous Content-Length");
      return;
    }
    if (body_len > MAX_SIP_BODY_BYTES) {
      drop_tcp_stream("SIP TCP Content-Length exceeds limit");
      return;
    }
    const size_t total = sep + 4 + body_len;
    if (total > MAX_SIP_TCP_RX_BUFFER) {
      drop_tcp_stream("SIP TCP framed message exceeds RX buffer");
      return;
    }
    if (flow->rx_buffer.size() < total) {
      if (peer_closed) this->handle_tcp_peer_loss_(flow);
      return;
    }
    const std::string msg = flow->rx_buffer.substr(0, total);
    flow->rx_buffer.erase(0, total);
#ifdef USE_ESPHOME_VOIP_MWI
    if (flow == &this->inbound_control_flow_ && msg.rfind("INVITE ", 0) == 0) {
      LockGuard lock(this->dialog_mutex_);
      if (this->call_id_.empty()) {
        this->dispatch_socket_ = socket;
        this->send_stateless_response_(msg, src, 503, "Service Unavailable", "retry_new_connection", false, 1);
        this->dispatch_socket_ = -1;
        this->close_tcp_flow_(this->inbound_control_flow_); return;
      }
    }
#endif
#ifdef USE_ESPHOME_VOIP_REGISTRATION
    this->dispatch_socket_ = socket;
#endif
    this->handle_sip_datagram_(msg.data(), msg.size(), src);
#ifdef USE_ESPHOME_VOIP_REGISTRATION
    this->dispatch_socket_ = -1;
#endif
#ifdef USE_ESPHOME_VOIP_MWI
    // This is the already-budgeted transient third client slot. Release it
    // after complete control records so another PBX can notify during calls.
    if (flow == &this->inbound_control_flow_ && flow->rx_buffer.empty()) {
      this->close_tcp_flow_(*flow); this->inbound_control_deadline_ = 0; return;
    }
#endif
  }
}

void SipTransport::handle_sip_udp_datagram_(char *buf, int n, const sockaddr_in &src) {
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  {
    LockGuard lock(this->dialog_mutex_);
    if (this->registration_ingress_(std::string(buf, n), src, -1)) return;
  }
#endif
  // Retired transactions retain their own transport after a mixed handoff.
  // Replay before the active-dialog gate, without retargeting that dialog.
  {
    LockGuard lock(this->dialog_mutex_);
    const std::string message(buf, static_cast<size_t>(n));
    const std::string method = message.substr(0, message.find(' '));
    if (this->replay_completed_response_(message, src, method, true) ||
        this->replay_completed_invite_ack_(message, src, true)) return;
  }
  bool tcp_call_active = false;
  {
    LockGuard lock(this->dialog_mutex_);
    tcp_call_active =
        !this->call_id_.empty() ||
        this->outgoing_invite_pending_.load(std::memory_order_acquire) ||
        this->media_active_.load(std::memory_order_acquire) ||
        this->terminal_transaction_pending_locked_();
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
    tcp_call_active =
        tcp_call_active ||
        this->pending_video_direction_invite_.pending();
#endif
  }
  bool tcp_session_active =
      tcp_call_active && this->remote_sip_tcp_.load(std::memory_order_acquire) &&
      (this->tcp_flow_.socket.load(std::memory_order_acquire) >= 0 || this->tcp_flow_.connecting_socket >= 0 ||
       this->tcp_flow_.connect_requested.load(std::memory_order_acquire));
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  tcp_session_active = tcp_session_active ||
      (tcp_call_active && this->remote_sip_tcp_.load(std::memory_order_acquire) && this->active_account_ >= 0 &&
       (this->dialog_account_socket_ >= 0 ||
        this->registration_flows_[this->active_account_].socket.load(std::memory_order_acquire) >= 0));
#endif
  bool refer_udp = false;
  {
    LockGuard lock(this->dialog_mutex_);
    refer_udp = this->refer_dialog_ && !this->refer_dialog_->remote_sip_tcp_ &&
        header_value(std::string(buf, static_cast<size_t>(n)), "Call-ID") == this->refer_wire_id_;
  }
  if (tcp_session_active && !refer_udp) {
#ifdef USE_ESPHOME_VOIP_REGISTRATION
    LockGuard lock(this->dialog_mutex_);
    const std::string message(buf, n);
    if (message.rfind("INVITE ", 0) == 0) this->send_stateless_response_(message, src, 486, "Busy Here");
#endif
    ESP_LOGD(TAG, "SIP UDP datagram ignored while TCP signaling is active");
    return;
  }
  if (!refer_udp && this->tcp_flow_.socket.load(std::memory_order_acquire) >= 0) {
    // An idle keep-alive connection must not coexist with a new UDP
    // dialog, otherwise later TCP traffic could retarget the call.
    this->close_tcp_client_from_sip_task_();
  }
  if (!refer_udp) this->remote_sip_tcp_.store(false, std::memory_order_release);
  buf[n] = 0;
  char ip[16];
  inet_ntoa_r(src.sin_addr, ip, sizeof(ip));
  ESP_LOGI(TAG, "SIP UDP RX %d bytes from %s:%u", n, ip, (unsigned) ntohs(src.sin_port));
  this->handle_sip_datagram_(reinterpret_cast<const char *>(buf), static_cast<size_t>(n), src);
}

void SipTransport::sip_task_trampoline_(void *param) {
  static_cast<SipTransport *>(param)->sip_task_();
}

void SipTransport::rtp_task_trampoline_(void *param) {
  static_cast<SipTransport *>(param)->rtp_task_();
}

void SipTransport::sip_task_() {
  char *const buf = this->sip_rx_scratch_.data();
  while (this->running_.load(std::memory_order_acquire)) {
    this->pump_udp_retransmits_();
#ifdef USE_ESPHOME_VOIP_REGISTRATION
    {
      LockGuard lock(this->dialog_mutex_); this->pump_registration_dns_(); this->registration_.tick(millis());
#ifdef USE_ESPHOME_VOIP_MWI
      this->pump_registration_mwi_(millis());
#endif
    }
    for (auto &flow : this->registration_flows_) this->prepare_tcp_flow_(flow);
#endif
    this->prepare_tcp_flow_(this->tcp_flow_);
    if (this->refer_dialog_) this->prepare_tcp_flow_(this->refer_dialog_->tcp_flow_);
    fd_set readfds;
    fd_set writefds;
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    int max_fd = -1;
    if (this->sip_wake_socket_ >= 0) {
      FD_SET(this->sip_wake_socket_, &readfds);
      max_fd = std::max(max_fd, this->sip_wake_socket_);
    }
    if (this->sip_socket_ >= 0) {
      FD_SET(this->sip_socket_, &readfds);
      max_fd = std::max(max_fd, this->sip_socket_);
    }
    if (this->sip_tcp_listener_socket_ >= 0) {
      FD_SET(this->sip_tcp_listener_socket_, &readfds);
      max_fd = std::max(max_fd, this->sip_tcp_listener_socket_);
    }
        for (auto *flow :
#ifdef USE_ESPHOME_VOIP_REGISTRATION
         this->signaling_flows_()
#else
         std::array<SipTcpFlow *, 2>{&this->tcp_flow_, this->refer_dialog_ ? &this->refer_dialog_->tcp_flow_ : nullptr}
#endif
         ) {
      if (flow == nullptr) continue;
      const int socket = flow->socket.load(std::memory_order_acquire);
      if (socket >= 0) { FD_SET(socket, &readfds); max_fd = std::max(max_fd, socket); }
      if (flow->connecting_socket >= 0) {
        FD_SET(flow->connecting_socket, &writefds);
        max_fd = std::max(max_fd, flow->connecting_socket);
      }
    }
    if (max_fd < 0) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }
#ifdef USE_ESPHOME_VOIP_MWI
    const int inbound_control = this->inbound_control_flow_.socket.load(std::memory_order_acquire);
    if (inbound_control >= 0) { FD_SET(inbound_control, &readfds); max_fd = std::max(max_fd, inbound_control); }
#endif
    struct timeval timeout{};
    struct timeval *timeout_ptr = nullptr;
    uint32_t timeout_ms = 0;
    bool have_timeout = false;
        for (auto *flow :
#ifdef USE_ESPHOME_VOIP_REGISTRATION
         this->signaling_flows_()
#else
         std::array<SipTcpFlow *, 2>{&this->tcp_flow_, this->refer_dialog_ ? &this->refer_dialog_->tcp_flow_ : nullptr}
#endif
         ) {
      if (flow == nullptr || flow->connecting_socket < 0) continue;
      const uint32_t now = millis();
      const uint32_t remaining = time_reached(now, flow->connect_deadline) ? 0 : flow->connect_deadline - now;
      if (!have_timeout || remaining < timeout_ms) timeout_ms = remaining;
      have_timeout = true;
      timeout_ptr = &timeout;
    }
    {
      const uint32_t now = millis();
      uint32_t next_ms = 0;
      bool have_sip_timeout = false;
      auto include_at = [now, &next_ms, &have_sip_timeout](uint32_t deadline) {
        const uint32_t delta = time_reached(now, deadline) ? 0 : deadline - now;
        if (!have_sip_timeout || delta < next_ms) {
          next_ms = delta;
          have_sip_timeout = true;
        }
      };
      {
        LockGuard lock(this->dialog_mutex_);
        auto include_txn = [&include_at](const UdpTransaction &txn) {
          if (txn.empty()) return;
          include_at(txn.udp && !txn.completed ? txn.next_ms
                                               : txn.deadline_ms);
        };
        if (this->outgoing_invite_pending_.load(std::memory_order_acquire)) {
          include_txn(this->pending_invite_);
        }
        include_txn(this->pending_cancel_);
        include_txn(this->pending_bye_);
        include_txn(this->pending_update_);
        if (this->refer_dialog_) {
          include_txn(this->refer_dialog_->pending_invite_);
          include_txn(this->refer_dialog_->pending_cancel_);
          include_txn(this->refer_dialog_->pending_bye_);
          include_txn(this->refer_dialog_->pending_notify_);
          include_txn(this->refer_dialog_->pending_notify_final_);
          include_at(this->refer_expires_);
        }
        for (const auto &completed : this->completed_refers_) {
          if (completed) include_at(completed->expires_ms);
        }
        if (!this->pending_provisional_response_.empty()) {
          include_at(this->pending_provisional_response_.next_ms);
        }
        if (this->completed_invite_.awaiting_ack) {
          const bool retransmits =
              this->completed_invite_.udp ||
              this->completed_invite_.status < 300;
          include_at(retransmits
                         ? this->completed_invite_.next_retransmit_ms
                         : this->completed_invite_.deadline_ms);
        }
        auto include_completed_expiry = [&include_at](const auto &transaction) {
          if (!transaction.empty()) {
            include_at(transaction.completed_ms +
                       SIP_TRANSACTION_TIMEOUT_MS);
          }
        };
        include_completed_expiry(this->completed_invite_);
        include_completed_expiry(this->completed_control_);
        include_completed_expiry(this->completed_invite_client_);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
        include_completed_expiry(this->completed_video_direction_invite_);
#endif
      }
      if (have_sip_timeout && (!have_timeout || next_ms < timeout_ms)) {
        timeout_ms = next_ms;
        have_timeout = true;
        timeout_ptr = &timeout;
      }
    }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
    {
      const uint32_t now = millis();
      LockGuard lock(this->dialog_mutex_);
      const auto &pending = this->pending_video_direction_invite_;
      if (pending.pending()) {
        const uint32_t deadline =
            pending.waiting_retry
                ? pending.retry_at_ms
                : (pending.transaction.udp &&
                           !pending.transaction.completed
                       ? pending.transaction.next_ms
                       : pending.response_deadline_ms);
        const uint32_t delta =
            time_reached(now, deadline) ? 0 : deadline - now;
        if (!have_timeout || delta < timeout_ms) {
          timeout_ms = delta;
          have_timeout = true;
          timeout_ptr = &timeout;
        }
      }
    }
#endif
#ifdef USE_ESPHOME_VOIP_REGISTRATION
    {
      LockGuard lock(this->dialog_mutex_); uint32_t registration_timeout = 0;
      if (this->registration_.next_deadline(millis(), &registration_timeout) &&
          (!have_timeout || registration_timeout < timeout_ms)) {
        timeout_ms = registration_timeout; have_timeout = true; timeout_ptr = &timeout;
      }
#ifdef USE_ESPHOME_VOIP_MWI
      if (this->mwi_.next_deadline(millis(), &registration_timeout) &&
          (!have_timeout || registration_timeout < timeout_ms)) {
        timeout_ms = registration_timeout; have_timeout = true; timeout_ptr = &timeout;
      }
      if (this->inbound_control_flow_.socket.load(std::memory_order_acquire) >= 0) {
        const uint32_t now = millis();
        const uint32_t remaining = time_reached(now, this->inbound_control_deadline_) ? 0 : this->inbound_control_deadline_ - now;
        if (!have_timeout || remaining < timeout_ms) {
          timeout_ms = remaining; have_timeout = true; timeout_ptr = &timeout;
        }
      }
#endif
    }
#endif
    if (timeout_ptr != nullptr) {
      timeout.tv_sec = timeout_ms / 1000;
      timeout.tv_usec = (timeout_ms % 1000) * 1000;
    }
    const int ready = ::select(max_fd + 1, &readfds, &writefds, nullptr, timeout_ptr);
#ifdef USE_ESPHOME_VOIP_MWI
    if (this->inbound_control_flow_.socket.load(std::memory_order_acquire) >= 0 &&
        time_reached(millis(), this->inbound_control_deadline_)) this->handle_tcp_peer_loss_(&this->inbound_control_flow_);
#endif
    bool connect_expired = false;
        for (auto *flow :
#ifdef USE_ESPHOME_VOIP_REGISTRATION
         this->signaling_flows_()
#else
         std::array<SipTcpFlow *, 2>{&this->tcp_flow_, this->refer_dialog_ ? &this->refer_dialog_->tcp_flow_ : nullptr}
#endif
         ) {
      if (flow != nullptr && flow->connecting_socket >= 0 && time_reached(millis(), flow->connect_deadline)) {
        this->fail_tcp_flow_(*flow, ETIMEDOUT);
        connect_expired = true;
      }
    }
    if (connect_expired) continue;
    if (ready <= 0) {
      continue;
    }

    if (this->sip_wake_socket_ >= 0 &&
        FD_ISSET(this->sip_wake_socket_, &readfds)) {
      uint8_t wake_bytes[16];
      while (recv(this->sip_wake_socket_, wake_bytes, sizeof(wake_bytes), 0) >
             0) {
      }
      if (!this->running_.load(std::memory_order_acquire)) continue;
    }

        for (auto *flow :
#ifdef USE_ESPHOME_VOIP_REGISTRATION
         this->signaling_flows_()
#else
         std::array<SipTcpFlow *, 2>{&this->tcp_flow_, this->refer_dialog_ ? &this->refer_dialog_->tcp_flow_ : nullptr}
#endif
         ) {
      if (flow == nullptr || flow->connecting_socket < 0 || !FD_ISSET(flow->connecting_socket, &writefds)) continue;
      int error = 0;
      socklen_t len = sizeof(error);
      if (getsockopt(flow->connecting_socket, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0)
        this->promote_tcp_flow_(*flow);
      else
        this->fail_tcp_flow_(*flow, error == 0 ? errno : error);
    }

    if (this->sip_tcp_listener_socket_ >= 0 && FD_ISSET(this->sip_tcp_listener_socket_, &readfds)) {
      struct sockaddr_in src{};
      socklen_t slen = sizeof(src);
      int client = accept(this->sip_tcp_listener_socket_, reinterpret_cast<struct sockaddr *>(&src), &slen);
      if (client >= 0) {
        int opt = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        int flags = fcntl(client, F_GETFL, 0);
        fcntl(client, F_SETFL, flags | O_NONBLOCK);
        const uint32_t accepted_ip_v4 = ntohl(src.sin_addr.s_addr);
        const int active_client = this->tcp_flow_.socket.load(std::memory_order_acquire);
        const uint32_t active_ip_v4 = this->tcp_flow_.peer_ip.load(std::memory_order_acquire);
        bool signaling_owned = false;
        bool signaling_uses_tcp = false;
        {
          LockGuard lock(this->dialog_mutex_);
          signaling_owned =
              !this->call_id_.empty() ||
              this->outgoing_invite_pending_.load(std::memory_order_acquire) ||
              this->media_active_.load(std::memory_order_acquire) ||
              this->terminal_transaction_pending_locked_();
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
          signaling_owned =
              signaling_owned ||
              this->pending_video_direction_invite_.pending();
#endif
          signaling_uses_tcp =
              this->remote_sip_tcp_.load(std::memory_order_acquire);
        }
        const bool replaces_owned_transport =
            signaling_owned &&
            (!signaling_uses_tcp || active_client >= 0 ||
             this->tcp_flow_.connecting_socket >= 0 ||
             this->tcp_flow_.connect_requested.load(std::memory_order_acquire));
        if (replaces_owned_transport ||
            (this->dialog_active_() && active_client >= 0 &&
             active_ip_v4 != 0 && active_ip_v4 != accepted_ip_v4)) {
#ifdef USE_ESPHOME_VOIP_MWI
          if (this->park_inbound_control_(client, src)) continue;
#endif
          char ip[16];
          inet_ntoa_r(src.sin_addr, ip, sizeof(ip));
          ESP_LOGW(TAG,
                   "SIP TCP accept rejected: signaling transport owned by %s",
                   ip);
          close(client);
          continue;
        }
        if (!this->should_accept_session_() && active_client < 0) {
          close(client);
          continue;
        }
        this->close_tcp_client_from_sip_task_();
        this->tcp_flow_.socket.store(client, std::memory_order_release);
        this->tcp_flow_.peer_ip.store(accepted_ip_v4, std::memory_order_release);
        this->tcp_flow_.close_requested.store(false, std::memory_order_release);
        this->tcp_flow_.rx_buffer.clear();
        this->remote_sip_tcp_.store(true, std::memory_order_release);
        char ip[16];
        inet_ntoa_r(src.sin_addr, ip, sizeof(ip));
        ESP_LOGI(TAG, "SIP TCP accepted from %s:%u", ip, (unsigned) ntohs(src.sin_port));
      }
    }

    if (this->sip_socket_ >= 0 && FD_ISSET(this->sip_socket_, &readfds)) {
      struct sockaddr_in src{};
      socklen_t slen = sizeof(src);
      int n = recvfrom(this->sip_socket_, buf, this->sip_rx_scratch_.size() - 1, 0,
                       reinterpret_cast<struct sockaddr *>(&src), &slen);
      if (n > 0) {
        this->handle_sip_udp_datagram_(buf, n, src);
      }
    }

    // A commit can exchange the two owners while dispatching a record. Resolve
    // each saved descriptor again instead of retaining a pointer across dispatch.
#ifdef USE_ESPHOME_VOIP_REGISTRATION
    std::array<int, 2 + SipRegistration::CAPACITY> sockets{};
    auto flows = this->signaling_flows_();
    for (size_t i = 0; i < sockets.size(); ++i) sockets[i] = flows[i] == nullptr ? -1 : flows[i]->socket.load(std::memory_order_acquire);
#else
    const int sockets[] = {this->tcp_flow_.socket.load(std::memory_order_acquire),
        this->refer_dialog_ ? this->refer_dialog_->tcp_flow_.socket.load(std::memory_order_acquire) : -1};
#endif
#ifdef USE_ESPHOME_VOIP_MWI
    const int control_socket = this->inbound_control_flow_.socket.load(std::memory_order_acquire);
    if (control_socket >= 0 && FD_ISSET(control_socket, &readfds)) {
      sockaddr_in source{}; socklen_t length = sizeof(source);
      if (getpeername(control_socket, reinterpret_cast<sockaddr *>(&source), &length) == 0)
        this->handle_sip_stream_(control_socket, source);
    }
#endif
    for (int socket : sockets) {
      if (socket < 0 || !FD_ISSET(socket, &readfds) || this->tcp_flow_for_socket_(socket) == nullptr) continue;
      struct sockaddr_in src{};
      socklen_t length = sizeof(src);
      if (getpeername(socket, reinterpret_cast<struct sockaddr *>(&src), &length) == 0)
        this->handle_sip_stream_(socket, src);
    }
  }
#ifdef USE_ESPHOME_VOIP_REGISTRATION
  for (auto &flow : this->registration_flows_) this->close_tcp_flow_(flow);
#endif
#ifdef USE_ESPHOME_VOIP_MWI
  this->close_tcp_flow_(this->inbound_control_flow_);
#endif
  this->close_tcp_client_from_sip_task_();
  if (this->refer_dialog_) this->close_tcp_flow_(this->refer_dialog_->tcp_flow_);
  if (this->sip_task_done_ != nullptr) {
    xSemaphoreGive(this->sip_task_done_);
  }
  vTaskDelete(nullptr);
}

void SipTransport::rtp_task_() {
  uint8_t buf[1600];
  // The wire payload is capped at 1488 bytes. L24-in-S32 expands by 4/3
  // while decoding, so 2 KiB covers every format accepted by the schema.
  uint8_t pcm[2048];
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (!this->rtp_task_terminate_.load(std::memory_order_acquire) &&
           this->rtp_running_.load(std::memory_order_acquire)) {
      const int socket = this->rtp_socket_;
      if (socket < 0) {
        break;
      }
      fd_set readfds;
      FD_ZERO(&readfds);
      FD_SET(socket, &readfds);
      const int ready = ::select(socket + 1, &readfds, nullptr, nullptr, nullptr);
      if (ready < 0) {
        if (errno != EINTR) {
          const int err = errno;
          ESP_LOGW(TAG, "RTP select failed: %s (%d: %s)",
                   socket_errno_name(err), err, socket_errno_text(err));
          break;
        }
        continue;
      }
      if (ready == 0 || !FD_ISSET(socket, &readfds)) {
        continue;
      }
      struct sockaddr_in src{};
      socklen_t slen = sizeof(src);
      int n = recvfrom(socket, buf, sizeof(buf), 0,
                       reinterpret_cast<struct sockaddr *>(&src), &slen);
      if (n <= 12 || (buf[0] & 0xC0) != 0x80) {
        continue;
      }
      if (!this->media_active_.load(std::memory_order_acquire)) {
        continue;
      }
      const uint32_t proposal_epoch =
          this->media_proposal_epoch_.load(std::memory_order_acquire);
      if ((proposal_epoch & 1U) != 0) continue;
      const uint32_t src_ip = ntohl(src.sin_addr.s_addr);
      const uint16_t src_port = ntohs(src.sin_port);
      const uint32_t expected_ip = this->remote_rtp_ip_v4_.load(std::memory_order_acquire);
      if (expected_ip != 0 && src_ip != expected_ip) {
        continue;
      }
      const uint8_t csrc_count = buf[0] & 0x0F;
      size_t header = 12u + static_cast<size_t>(csrc_count) * 4u;
      if (static_cast<size_t>(n) <= header) {
        continue;
      }
      if ((buf[0] & 0x10) != 0) {
        if (static_cast<size_t>(n) < header + 4) continue;
        const uint16_t ext_len = static_cast<uint16_t>((buf[header + 2] << 8) | buf[header + 3]);
        header += 4u + static_cast<size_t>(ext_len) * 4u;
        if (static_cast<size_t>(n) <= header) continue;
      }
      size_t payload_len = static_cast<size_t>(n) - header;
      if ((buf[0] & 0x20) != 0 && payload_len > 0) {
        const uint8_t pad = buf[n - 1];
        if (pad == 0 || pad > payload_len) continue;
        payload_len -= pad;
      }
      AudioFormat rx_format;
      if (!this->received_audio_format_(buf[1] & 0x7F, &rx_format)) continue;
      const uint16_t sequence = static_cast<uint16_t>((buf[2] << 8) | buf[3]);
      const uint32_t timestamp = (static_cast<uint32_t>(buf[4]) << 24) |
                                 (static_cast<uint32_t>(buf[5]) << 16) |
                                 (static_cast<uint32_t>(buf[6]) << 8) |
                                 static_cast<uint32_t>(buf[7]);
      const uint8_t *payload = buf + header;
      const uint32_t ssrc = (static_cast<uint32_t>(buf[8]) << 24) |
                            (static_cast<uint32_t>(buf[9]) << 16) |
                            (static_cast<uint32_t>(buf[10]) << 8) |
                            static_cast<uint32_t>(buf[11]);
      const uint8_t *audio_data = payload;
      size_t audio_bytes = payload_len;
      if (rx_format.codec == AudioCodec::PCM) {
        audio_bytes = rtp_payload_to_pcm(payload, payload_len, rx_format, pcm,
                                         sizeof(pcm));
        audio_data = pcm;
        if (audio_bytes == 0 ||
            audio_bytes != rx_format.nominal_frame_bytes()) continue;
      }
      if (this->media_proposal_epoch_.load(std::memory_order_acquire) !=
          proposal_epoch) {
        continue;
      }
      const bool ssrc_latched =
          this->rtp_ssrc_latched_.load(std::memory_order_acquire);
      bool source_changed = false;
      if (ssrc_latched &&
          this->latched_rtp_ip_v4_.load(std::memory_order_acquire) != src_ip) {
        continue;
      }
      if (ssrc_latched &&
          this->latched_rtp_ssrc_.load(std::memory_order_acquire) != ssrc) {
        // RFC 3550 permits a restarted endpoint to choose a new SSRC. The
        // already-latched IP:port is the one-to-one SIP media identity; accept
        // a codec-valid source change on that exact tuple and reset downstream
        // sequence state instead of rejecting the new source forever.
        if (this->latched_rtp_port_.load(std::memory_order_acquire) !=
            src_port) {
          continue;
        }
        const uint32_t previous_ssrc =
            this->latched_rtp_ssrc_.load(std::memory_order_acquire);
        this->latched_rtp_ssrc_.store(ssrc, std::memory_order_release);
        source_changed = true;
        ESP_LOGI(TAG, "RTP source changed SSRC %08" PRIx32 " -> %08" PRIx32,
                 previous_ssrc, ssrc);
      } else if (!ssrc_latched) {
        this->latched_rtp_ip_v4_.store(src_ip, std::memory_order_release);
        this->latched_rtp_port_.store(src_port, std::memory_order_release);
        this->latched_rtp_ssrc_.store(ssrc, std::memory_order_release);
        this->remote_rtp_port_.store(src_port, std::memory_order_release);
        this->rtp_ssrc_latched_.store(true, std::memory_order_release);
      } else if (this->latched_rtp_port_.load(std::memory_order_acquire) != src_port) {
        // Keep the SSRC identity but follow a legitimate NAT port rebind.
        this->latched_rtp_port_.store(src_port, std::memory_order_release);
        this->remote_rtp_port_.store(src_port, std::memory_order_release);
      }
      if ((this->get_audio_direction_() & 2U) == 0) continue;
      source_changed |= this->audio_rx_discontinuity_.exchange(false, std::memory_order_acq_rel);
      this->rtp_rx_packets_.fetch_add(1, std::memory_order_acq_rel);
      this->rtp_rx_bytes_.fetch_add(static_cast<uint32_t>(n), std::memory_order_acq_rel);
      this->emit_audio_frame_(audio_data, audio_bytes, sequence, timestamp,
                              source_changed, pack_audio_format(rx_format));
    }

    // A socket/select failure must not leave the transport claiming an active
    // media worker that has actually parked. Gate the path and clean every
    // subordinate video/socket resource through the same event-driven exit.
    const bool worker_failed =
        this->rtp_running_.exchange(false, std::memory_order_acq_rel);
    if (worker_failed) {
      ESP_LOGE(TAG, "RTP worker exited unexpectedly; quiescing media");
      this->close_media_session_();
      {
        LockGuard media_lock(this->media_lifecycle_mutex_);
        if (this->media_lifecycle_phase_.load(std::memory_order_acquire) ==
            MediaLifecyclePhase::ACTIVE) {
          this->media_lifecycle_phase_.store(
              MediaLifecyclePhase::CLEANING, std::memory_order_release);
        }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
        if (this->video_session_ != nullptr)
          this->video_session_->request_stop();
#endif
      }
    }

    bool cleanup_requested = false;
    {
      LockGuard media_lock(this->media_lifecycle_mutex_);
      cleanup_requested =
          this->media_lifecycle_phase_.load(std::memory_order_acquire) ==
          MediaLifecyclePhase::CLEANING;
    }
    if (cleanup_requested) {
      this->finish_audio_path_stop_();
    }

    {
      LockGuard media_lock(this->media_lifecycle_mutex_);
      if (this->media_lifecycle_phase_.load(std::memory_order_acquire) ==
          MediaLifecyclePhase::CLEANING) {
        this->media_lifecycle_phase_.store(MediaLifecyclePhase::IDLE,
                                            std::memory_order_release);
      }
      this->rtp_task_quiesced_.store(true, std::memory_order_release);
      if (this->rtp_cleanup_done_ != nullptr)
        xSemaphoreGive(this->rtp_cleanup_done_);
      this->emit_media_quiesced_();
    }

    if (worker_failed) {
      SipSignal signal;
      signal.type = SipSignalType::PROTOCOL_ERROR;
      signal.status_code = 500;
      signal.reason = "rtp_worker_failed";
      {
        LockGuard dialog_lock(this->dialog_mutex_);
        signal.call_id = this->call_id_;
        if (!signal.call_id.empty()) {
          signal.terminal_transaction_pending =
              this->send_bye_unlocked_(signal.call_id);
          if (!signal.terminal_transaction_pending)
            this->reset_dialog_();
        }
      }
      if (!signal.call_id.empty()) this->emit_sip_signal_(std::move(signal));
    }

    if (this->rtp_task_terminate_.load(std::memory_order_acquire)) break;
  }
  if (this->rtp_task_done_ != nullptr) {
    xSemaphoreGive(this->rtp_task_done_);
  }
  vTaskDelete(nullptr);
}

}  // namespace voip_stack
}  // namespace esphome

#endif  // USE_ESP32 && USE_ESPHOME_VOIP_SIP_TRANSPORT
