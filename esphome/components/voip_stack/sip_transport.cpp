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

#include "audio_core_task_utils.h"
#include "esphome/components/network/util.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "net_utils.h"

namespace esphome {
namespace voip_stack {

static const char *const TAG = "voip_stack.sip";

namespace {

static constexpr uint32_t SIP_T1_MS = 500;
static constexpr uint32_t SIP_T2_MS = 4000;
static constexpr uint32_t SIP_TRANSACTION_TIMEOUT_MS = 64 * SIP_T1_MS;

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
    out.media_lifecycle_phase = static_cast<uint8_t>(
        this->media_lifecycle_phase_.load(std::memory_order_acquire));
#endif
  }
  out.call_active = this->media_active_.load(std::memory_order_acquire);
  out.pending_invite = this->outgoing_invite_pending_.load(std::memory_order_acquire);
  out.sip_tcp = this->remote_sip_tcp_.load(std::memory_order_acquire);
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
                                     uint8_t tx_payload_type, uint8_t rx_payload_type) {
  portENTER_CRITICAL(&this->media_config_lock_);
  this->selected_tx_format_ = tx;
  this->selected_rx_format_ = rx;
  this->rtp_tx_payload_type_ = tx_payload_type;
  this->rtp_rx_payload_type_ = rx_payload_type;
  portEXIT_CRITICAL(&this->media_config_lock_);
}

void SipTransport::get_media_config_(AudioFormat *tx, AudioFormat *rx,
                                     uint8_t *tx_payload_type, uint8_t *rx_payload_type) const {
  portENTER_CRITICAL(&this->media_config_lock_);
  if (tx != nullptr) *tx = this->selected_tx_format_;
  if (rx != nullptr) *rx = this->selected_rx_format_;
  if (tx_payload_type != nullptr) *tx_payload_type = this->rtp_tx_payload_type_;
  if (rx_payload_type != nullptr) *rx_payload_type = this->rtp_rx_payload_type_;
  portEXIT_CRITICAL(&this->media_config_lock_);
}

void SipTransport::set_audio_formats(const AudioFormatList &tx, const AudioFormatList &rx) {
  this->offer_tx_formats_ = tx;
  this->offer_rx_formats_ = rx;
  if (this->offer_tx_formats_.count == 0) audio_format_list_default(&this->offer_tx_formats_);
  if (this->offer_rx_formats_.count == 0) audio_format_list_default(&this->offer_rx_formats_);
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
  const int socket = this->sip_tcp_client_socket_.load(std::memory_order_acquire);
  this->tcp_connect_requested_.store(false, std::memory_order_release);
  {
    LockGuard lock(this->tcp_tx_pending_mutex_);
    this->tcp_tx_pending_.clear();
  }
  this->sip_tcp_client_close_requested_.store(true, std::memory_order_release);
  if (socket >= 0) shutdown(socket, SHUT_RDWR);
  this->wake_sip_task_();
}

void SipTransport::close_tcp_client_from_sip_task_() {
  LockGuard send_lock(this->tcp_send_mutex_);
  const int socket = this->sip_tcp_client_socket_.exchange(-1, std::memory_order_acq_rel);
  this->sip_tcp_client_close_requested_.store(false, std::memory_order_release);
  this->sip_tcp_client_ip_v4_.store(0, std::memory_order_release);
  if (socket >= 0) close(socket);
  this->sip_tcp_rx_buffer_.clear();
}

void SipTransport::handle_tcp_peer_loss_() {
  bool dialog_lost = false;
  {
    LockGuard lock(this->dialog_mutex_);
    dialog_lost = !this->call_id_.empty() ||
                  this->outgoing_invite_pending_.load(std::memory_order_acquire) ||
                  this->media_active_.load(std::memory_order_acquire) ||
                  this->terminal_transaction_pending_locked_();
    this->close_tcp_client_from_sip_task_();
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
    this->remote_sip_tcp_.store(false, std::memory_order_release);
    if (dialog_lost) this->reset_dialog_();
  }
  if (dialog_lost) this->emit_connection_change_(false);
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

  // Stop new SIP work first. A strong wait is required here because this
  // method is also the destructor boundary: returning with a worker that can
  // still dereference SipTransport would be a use-after-free, not a safe leak.
  this->tcp_connect_requested_.store(false, std::memory_order_release);
  {
    LockGuard lock(this->tcp_tx_pending_mutex_);
    this->tcp_tx_pending_.clear();
  }
  this->sip_tcp_client_close_requested_.store(true,
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
  this->rtp_sequence_.store(static_cast<uint16_t>(esp_random()), std::memory_order_release);
  this->rtp_timestamp_.store(esp_random(), std::memory_order_release);
  this->rtp_ssrc_ = esp_random();
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

  if (this->completed_invite_.awaiting_ack) {
    // RFC 3261: a UAS cannot originate BYE until the ACK for its 2xx arrives.
    // The existing ACK/timeout owner performs that BYE and keeps disconnect()
    // from silently erasing the confirmed dialog meanwhile.
    this->terminate_after_invite_ack_ = true;
    this->wake_sip_task_();
    return;
  }

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
    this->tcp_tx_pending_.clear();
  }
  this->tcp_connect_ip_v4_.store(ip_v4, std::memory_order_release);
  this->tcp_connect_port_.store(sip_port, std::memory_order_release);
  this->tcp_connect_requested_.store(true, std::memory_order_release);
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
  this->media_active_.store(true, std::memory_order_release);
}

void SipTransport::close_media_session_() {
  this->media_active_.store(false, std::memory_order_release);
}

void SipTransport::reset_dialog_() {
  // Callers already own dialog_mutex_. Holding the media lock until every
  // negotiated field is cleared prevents a deferred main-loop start from
  // observing the old video configuration after teardown has completed.
  LockGuard media_lock(this->media_lifecycle_mutex_);
  this->reset_dialog_media_locked_();
}

void SipTransport::reset_dialog_media_locked_() {
  this->request_audio_path_stop_locked_();
  bool abort_queued_tcp_record = false;
  {
    // Serialize with promote_tcp_connect(): once that path swaps the queued
    // record it owns transmission; before the swap a reset can still retract
    // the record without leaving an orphan request at the peer.
    LockGuard send_lock(this->tcp_send_mutex_);
    LockGuard pending_lock(this->tcp_tx_pending_mutex_);
    abort_queued_tcp_record = !this->tcp_tx_pending_.empty();
    this->tcp_tx_pending_.clear();
    if (abort_queued_tcp_record) {
      this->tcp_connect_requested_.store(false, std::memory_order_release);
      this->sip_tcp_client_close_requested_.store(true,
                                                   std::memory_order_release);
    }
  }
  if (abort_queued_tcp_record) this->wake_sip_task_();
  this->call_id_.clear();
  this->local_tag_.clear();
  this->remote_tag_.clear();
  this->branch_.clear();
  this->local_uri_.clear();
  this->local_contact_uri_.clear();
  this->remote_uri_.clear();
  this->remote_target_uri_.clear();
  this->last_invite_via_.clear();
  this->last_invite_from_.clear();
  this->last_invite_to_.clear();
  this->last_invite_cseq_.clear();
  this->last_invite_response_.clear();
  this->last_invite_cseq_number_ = 0;
  this->remote_dialog_cseq_ = 0;
  this->last_invite_peer_ip_v4_ = 0;
  this->last_invite_peer_port_ = 0;
  this->caller_route_.clear();
  this->caller_name_.clear();
  this->dest_route_.clear();
  this->dest_name_.clear();
  this->peer_supports_from_change_ = false;
  this->connected_identity_sent_ = false;
  this->dialog_originated_ = false;
  this->remote_offer_media_lines_.clear();
  this->remote_audio_media_index_ = -1;
  this->remote_video_media_index_ = -1;
  this->remote_media_shape_overflow_ = false;
  this->close_media_session_();
  this->outgoing_invite_pending_.store(false, std::memory_order_release);
  this->cancel_requested_.store(false, std::memory_order_release);
  this->terminate_after_invite_ack_ = false;
  this->clear_udp_transactions_();
  this->reset_rtp_latch_();
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  this->pending_video_direction_invite_.clear();
  this->confirmed_local_sdp_.clear();
  this->reset_video_negotiation_();
  this->emit_video_active_state_(false);
  this->emit_video_send_state_(false, false);
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
  if (method == "INVITE") {
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

void SipTransport::pump_udp_retransmits_() {
  const uint32_t now = millis();
  bool reset_terminal_dialog = false;
  bool invite_timed_out = false;
  bool ack_timed_out = false;
  std::string timed_out_call_id;
  LockGuard lock(this->dialog_mutex_);
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
      } else if (std::strcmp(method, "UPDATE") != 0) {
        reset_terminal_dialog = true;
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
  if (reset_terminal_dialog) {
    this->reset_dialog_();
  }
  if (invite_timed_out) {
    SipSignal signal;
    signal.type = SipSignalType::FINAL_RESPONSE;
    signal.status_code = 408;
    signal.call_id = timed_out_call_id;
    signal.reason = "timeout";
    this->reset_dialog_();
    this->emit_sip_signal_(signal);
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
    this->emit_sip_signal_(signal);
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
  if (this->remote_sip_tcp_.load(std::memory_order_acquire)) {
    return this->send_sip_tcp_(message);
  }
  if (this->sip_socket_ < 0 || ip_v4 == 0 || port == 0) return false;
  struct sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_addr.s_addr = htonl(ip_v4);
  dest.sin_port = htons(port);
  constexpr uint8_t MAX_ATTEMPTS = 3;
  int sent = -1;
  int err = 0;
  for (uint8_t attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
    sent = sendto(this->sip_socket_, message.data(), message.size(), 0,
                  reinterpret_cast<struct sockaddr *>(&dest), sizeof(dest));
    if (sent == static_cast<int>(message.size())) break;
    err = errno;
    if (!socket_errno_is_transient_tx_pressure(err) || attempt + 1 == MAX_ATTEMPTS) break;
    ESP_LOGW(TAG, "SIP TX deferred by %s; retrying (%u/%u)", socket_errno_name(err),
             static_cast<unsigned>(attempt + 1), static_cast<unsigned>(MAX_ATTEMPTS));
    delay(2);
  }
  if (sent != static_cast<int>(message.size())) {
    ESP_LOGW(TAG, "SIP TX failed: %s (%d: %s)", socket_errno_name(err), err,
             socket_errno_text(err));
    return false;
  }
  char ip[16];
  struct in_addr a{};
  a.s_addr = htonl(ip_v4);
  inet_ntoa_r(a, ip, sizeof(ip));
  ESP_LOGI(TAG, "SIP TX %u bytes to %s:%u", (unsigned) message.size(), ip, (unsigned) port);
  return true;
}

bool SipTransport::send_sip_tcp_(const std::string &message) {
  // SIP commands can originate from the component/main task while the SIP
  // task sends responses. Serialize the complete record so partial TCP writes
  // from two callers can never interleave into an invalid SIP message.
  LockGuard send_lock(this->tcp_send_mutex_);
  if (message.empty()) return false;
  const bool replacing_session =
      this->sip_tcp_client_close_requested_.load(std::memory_order_acquire) ||
      this->tcp_connect_requested_.load(std::memory_order_acquire);
  const int socket = this->sip_tcp_client_socket_.load(std::memory_order_acquire);
  if (socket < 0 || replacing_session) {
    if (this->remote_sip_tcp_.load(std::memory_order_acquire)) {
      {
        LockGuard lock(this->tcp_tx_pending_mutex_);
        if (!this->tcp_tx_pending_.empty()) {
          ESP_LOGW(TAG, "SIP TCP connect still pending; refusing to replace queued %s with a newer request",
                   this->tcp_tx_pending_.rfind("INVITE ", 0) == 0 ? "INVITE" : "message");
          return false;
        }
        this->tcp_tx_pending_ = message;
      }
      this->wake_sip_task_();
      return true;
    }
    return false;
  }
  return this->send_sip_tcp_record_(message, socket);
}

bool SipTransport::send_sip_tcp_record_(const std::string &message, int socket) {
  // Caller owns tcp_send_mutex_. Keep this primitive allocation-free so the
  // SIP task can flush the one queued record atomically when connect finishes.
  if (message.empty() || socket < 0) return false;
  size_t sent_total = 0;
  while (sent_total < message.size()) {
    const int sent = send(socket, message.data() + sent_total, message.size() - sent_total, 0);
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
  ESP_LOGI(TAG, "SIP TCP TX %u bytes", (unsigned) message.size());
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
  this->get_media_config_(&old_tx, &old_rx, &old_tx_pt, &old_rx_pt);
  const uint32_t old_audio_ip =
      this->remote_rtp_ip_v4_.load(std::memory_order_acquire);
  const uint16_t old_audio_port =
      this->remote_rtp_port_.load(std::memory_order_acquire);
  const auto old_media_lines = this->remote_offer_media_lines_;
  const int8_t old_audio_index = this->remote_audio_media_index_;
  const int8_t old_video_index = this->remote_video_media_index_;
  const bool old_shape_overflow = this->remote_media_shape_overflow_;
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
      this->learn_remote_rtp_from_sdp_(sdp, default_ip, true);
  AudioFormat new_tx;
  AudioFormat new_rx;
  uint8_t new_tx_pt = 0;
  uint8_t new_rx_pt = 0;
  this->get_media_config_(&new_tx, &new_rx, &new_tx_pt, &new_rx_pt);
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

  this->set_media_config_(old_tx, old_rx, old_tx_pt, old_rx_pt);
  this->remote_rtp_ip_v4_.store(old_audio_ip, std::memory_order_release);
  this->remote_rtp_port_.store(old_audio_port, std::memory_order_release);
  this->remote_offer_media_lines_ = old_media_lines;
  this->remote_audio_media_index_ = old_audio_index;
  this->remote_video_media_index_ = old_video_index;
  this->remote_media_shape_overflow_ = old_shape_overflow;
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
      parsed && new_tx == old_tx && new_rx == old_rx &&
      new_tx_pt == old_tx_pt && new_rx_pt == old_rx_pt &&
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
  if (!this->running_.load(std::memory_order_acquire) ||
      this->transport_stopping_.load(std::memory_order_acquire)) {
    return false;
  }
  if (this->terminal_transaction_pending_locked_()) {
    ESP_LOGW(TAG,
             "SIP INVITE deferred: prior terminal transaction is pending");
    return false;
  }
  {
    LockGuard media_lock(this->media_lifecycle_mutex_);
    if (this->media_lifecycle_phase_.load(std::memory_order_acquire) !=
        MediaLifecyclePhase::IDLE) {
      ESP_LOGW(TAG, "SIP INVITE deferred: prior media lifecycle not idle");
      return false;
    }
  }
  // A fresh INVITE must never inherit tags or transaction state from a
  // cancelled dialog whose final response was lost.
  this->reset_dialog_();
  this->call_id_ = call_id;
  this->sdp_session_id_ = esp_random();
  if (this->sdp_session_id_ == 0) this->sdp_session_id_ = 1;
  this->sdp_session_version_ = 0;
  this->dialog_originated_ = true;
  this->caller_route_ = caller_route;
  this->caller_name_ = caller_name;
  this->dest_route_ = dest_route;
  this->dest_name_ = dest_name;
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
      "sip:" + sip_uri_user_encode(this->caller_route_) + "@" + local_ip +
      ":" + std::to_string(this->sip_port_) + ";transport=" + uri_transport;
  const std::string remote_identity_uri =
      "sip:" + sip_uri_user_encode(this->dest_route_) + "@" +
      std::string(ip_text) + ":" +
      std::to_string(this->remote_sip_port_.load(std::memory_order_acquire)) +
      ";transport=" + uri_transport;
  this->local_uri_ = sip_name_addr(local_identity_uri, this->caller_name_);
  this->local_contact_uri_ = sip_name_addr(local_identity_uri);
  this->remote_uri_ = sip_name_addr(remote_identity_uri, this->dest_name_);
  this->remote_target_uri_ = strip_angle_uri(this->remote_uri_);
  ESP_LOGI(TAG, "SIP INVITE call_id=%s from=%s to=%s", this->call_id_.c_str(),
           this->caller_name_.c_str(), this->dest_name_.c_str());
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
  this->confirmed_local_sdp_ = sdp;
#endif
  SipRequestOptions options;
  options.cseq_number = this->invite_cseq_;
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
  uint8_t packet[1500];
  const uint8_t bps = tx_format.container_bytes_per_sample();
  const size_t input_bytes = bytes;
  const uint32_t samples = bps == 0 || tx_format.channels == 0
      ? 0
      : static_cast<uint32_t>(input_bytes / bps / tx_format.channels);
  bytes = pcm_to_rtp_payload(pcm, bytes, tx_format, packet + 12, sizeof(packet) - 12);
  if (bytes == 0 || bytes > this->udp_max_payload_) {
    // Sequence numbers count packets, while timestamps follow the sampling
    // clock. A locally discarded PCM frame therefore advances only time.
    this->rtp_timestamp_.fetch_add(samples, std::memory_order_acq_rel);
    return;
  }
  if (this->media_proposal_epoch_.load(std::memory_order_acquire) !=
      proposal_epoch) {
    this->rtp_timestamp_.fetch_add(samples, std::memory_order_acq_rel);
    return;
  }
  LockGuard socket_lock(this->rtp_socket_mutex_);
  if (!this->rtp_running_.load(std::memory_order_acquire) || this->rtp_socket_ < 0) return;
  const uint32_t ip = this->remote_rtp_ip_v4_.load(std::memory_order_acquire);
  const uint16_t port = this->remote_rtp_port_.load(std::memory_order_acquire);
  if (ip == 0 || port == 0) return;
  if (this->media_proposal_epoch_.load(std::memory_order_acquire) !=
      proposal_epoch) {
    this->rtp_timestamp_.fetch_add(samples, std::memory_order_acq_rel);
    return;
  }
  packet[0] = 0x80;
  packet[1] = tx_payload_type & 0x7F;
  const uint16_t seq = this->rtp_sequence_.fetch_add(1, std::memory_order_acq_rel);
  packet[2] = static_cast<uint8_t>(seq >> 8);
  packet[3] = static_cast<uint8_t>(seq & 0xFF);
  const uint32_t ts = this->rtp_timestamp_.fetch_add(samples, std::memory_order_acq_rel);
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

bool SipTransport::send_ringing(const std::string &call_id) {
  LockGuard lock(this->dialog_mutex_);
  if (!call_id.empty()) this->call_id_ = call_id;
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
  if (!call_id.empty()) this->call_id_ = call_id;
  uint8_t tx_payload_type = 96;
  uint8_t rx_payload_type = 96;
  this->get_media_config_(nullptr, nullptr, &tx_payload_type, &rx_payload_type);
  this->set_media_config_(dest_to_caller_format, caller_to_dest_format,
                          tx_payload_type, rx_payload_type);
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
    answer = this->build_sdp_answer_();
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
    this->open_media_session_();
  }
  return sent;
}

bool SipTransport::send_cancel(const std::string &call_id) {
  LockGuard lock(this->dialog_mutex_);
  return this->send_cancel_unlocked_(call_id);
}

bool SipTransport::send_cancel_unlocked_(const std::string &call_id) {
  if (!call_id.empty()) this->call_id_ = call_id;
  if (!this->outgoing_invite_pending_.load(std::memory_order_acquire)) {
    return this->send_bye_unlocked_(call_id);
  }
  if (this->remote_sip_tcp_.load(std::memory_order_acquire)) {
    bool cancelled_before_flush = false;
    {
      // This lock is shared with promote_tcp_connect(). If the INVITE still
      // occupies the one pre-connect slot, it has not reached the wire and
      // must be retracted rather than followed by a meaningless CANCEL. If
      // promotion already owns/sent it, release the lock and send a real
      // CANCEL on the established stream below.
      LockGuard send_lock(this->tcp_send_mutex_);
      LockGuard pending_lock(this->tcp_tx_pending_mutex_);
      if (this->tcp_tx_pending_.rfind("INVITE ", 0) == 0 &&
          header_value(this->tcp_tx_pending_, "Call-ID") == this->call_id_) {
        this->tcp_tx_pending_.clear();
        this->tcp_connect_requested_.store(false, std::memory_order_release);
        this->sip_tcp_client_close_requested_.store(
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
  SipRequestOptions options;
  options.cseq_number = this->invite_cseq_;
  this->cancel_requested_.store(true, std::memory_order_release);
  const bool sent = this->send_request_("CANCEL", "", options);
  if (sent) {
    // INVITE retransmission stops once cancellation begins. Keep the dialog
    // identifiers until the final 487 so it can be ACKed correctly.
    this->clear_invite_transaction_();
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
  if (!call_id.empty()) this->call_id_ = call_id;
  // BYE is the latency-critical dialog action. Video/audio workers can take
  // time to leave codec and socket calls, so put the SIP request on the wire
  // before performing subordinate media teardown.
  const bool sent = this->send_request_("BYE");
  this->stop_audio_path();
  return sent;
}

bool SipTransport::send_final_response(const std::string &call_id,
                                       uint16_t status,
                                       const std::string &reason) {
  LockGuard lock(this->dialog_mutex_);
  if (!call_id.empty()) this->call_id_ = call_id;
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
                                              const std::string &method) {
  CompletedServerTransaction *completed = method == "INVITE" ? &this->completed_invite_
                                                               : &this->completed_control_;
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
      completed->udp == !this->remote_sip_tcp_.load(std::memory_order_acquire);
  if (completed->method != method || completed->call_id != call_id ||
      completed->cseq != cseq_number(cseq) || cseq_method(cseq) != method ||
      completed->peer_ip_v4 != peer_ip || !transport_matches ||
      (!completed->branch.empty() && completed->branch != branch)) {
    return false;
  }
  ESP_LOGI(TAG, "SIP %s retransmission replaying cached response for call_id=%s",
           method.c_str(), call_id.c_str());
  this->send_sip_(completed->response, peer_ip, ntohs(src.sin_port));
  return true;
}

void SipTransport::remember_completed_response_(const std::string &request, uint32_t peer_ip_v4,
                                                uint16_t peer_port,
                                                const std::string &method,
                                                const std::string &response) {
  if (response.empty() || peer_ip_v4 == 0 || peer_port == 0 ||
      (method != "INVITE" && method != "CANCEL" && method != "BYE" &&
       method != "UPDATE") ||
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
                                                const sockaddr_in &src) {
  if (this->completed_invite_client_.empty()) return false;
  if (millis() - this->completed_invite_client_.completed_ms > SIP_TRANSACTION_TIMEOUT_MS) {
    this->completed_invite_client_.clear();
    return false;
  }
  if (response.rfind("SIP/2.0 ", 0) != 0 || response.size() < 12) return false;
  const int status = std::atoi(response.substr(8, 3).c_str());
  const std::string cseq = header_value(response, "CSeq");
  const uint32_t peer_ip = ntohl(src.sin_addr.s_addr);
  const bool transport_matches =
      this->completed_invite_client_.udp ==
      !this->remote_sip_tcp_.load(std::memory_order_acquire);
  if (status < 200 || cseq_method(cseq) != "INVITE" ||
      header_value(response, "Call-ID") != this->completed_invite_client_.call_id ||
      cseq_number(cseq) != this->completed_invite_client_.cseq ||
      peer_ip != this->completed_invite_client_.response_ip_v4 ||
      !transport_matches ||
      (!this->completed_invite_client_.branch.empty() &&
       via_branch(header_value(response, "Via")) != this->completed_invite_client_.branch)) {
    return false;
  }
  ESP_LOGI(TAG, "SIP INVITE final retransmission replaying ACK for call_id=%s",
           this->completed_invite_client_.call_id.c_str());
  this->send_sip_(this->completed_invite_client_.ack,
                  this->completed_invite_client_.ack_ip_v4,
                  this->completed_invite_client_.ack_port);
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
  if (this->replay_completed_response_(message, src, "INVITE")) {
    return true;
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
                       active_peer_ip == src_ip && !this->caller_name_.empty() &&
                       incoming_caller_name == this->dest_name_;
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
  const bool in_dialog_invite =
      !incoming_call_id.empty() && incoming_call_id == this->call_id_ &&
      this->media_active_.load(std::memory_order_acquire) &&
      !this->remote_tag_.empty() && !this->local_tag_.empty() &&
      tag_from_header(incoming_from) == this->remote_tag_ &&
      tag_from_header(incoming_to) == this->local_tag_;
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
  this->call_id_ = incoming_call_id;
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
  bool media_compatible = false;
  {
    LockGuard media_lock(this->media_lifecycle_mutex_);
    media_compatible = this->learn_remote_rtp_from_sdp_(body, src_ip);
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
  this->caller_name_ = from_user;
  this->dest_name_ = to_user;
  this->caller_route_ = sip_route_id(
      sip_user_from_header(incoming_from), VOIP_STACK_MAX_ROUTE_ID_LEN);
  this->dest_route_ = sip_route_id(
      sip_user_from_header(sip_request_uri(message)),
      VOIP_STACK_MAX_ROUTE_ID_LEN);
  if (this->caller_route_.empty()) {
    this->caller_route_ = sip_route_id(
        header_value(message, "X-Voip-Stack-Caller-Route"),
        VOIP_STACK_MAX_ROUTE_ID_LEN);
  }
  if (this->dest_route_.empty()) {
    this->dest_route_ = sip_route_id(
        header_value(message, "X-Voip-Stack-Dest-Route"),
        VOIP_STACK_MAX_ROUTE_ID_LEN);
  }
  if (this->caller_route_.empty()) this->caller_route_ = this->caller_name_;
  if (this->dest_route_.empty()) this->dest_route_ = this->dest_name_;

  ESP_LOGI(TAG, "SIP INVITE accepted into FSM call_id=%s", this->call_id_.c_str());
  AudioFormat selected_tx;
  AudioFormat selected_rx;
  this->get_media_config_(&selected_tx, &selected_rx, nullptr, nullptr);
  SipSignal signal;
  signal.type = SipSignalType::INVITE;
  signal.call_id = this->call_id_;
  signal.caller_route = this->caller_route_;
  signal.caller_name = this->caller_name_;
  signal.dest_route = this->dest_route_;
  signal.dest_name = this->dest_name_;
  signal.caller_tx_formats.formats[0] = selected_rx;
  signal.caller_tx_formats.count = 1;
  signal.caller_rx_formats.formats[0] = selected_tx;
  signal.caller_rx_formats.count = 1;
  signal.selected_rx_format = selected_rx;
  signal.selected_tx_format = selected_tx;
  this->emit_sip_signal_(signal);
  return true;
}

bool SipTransport::handle_reinvite_(const std::string &message,
                                    const sockaddr_in &src) {
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
  this->get_media_config_(&old_tx, &old_rx, &old_tx_pt, &old_rx_pt);
  const uint32_t old_audio_ip =
      this->remote_rtp_ip_v4_.load(std::memory_order_acquire);
  const uint16_t old_audio_port =
      this->remote_rtp_port_.load(std::memory_order_acquire);
  const auto old_media_lines = this->remote_offer_media_lines_;
  const int8_t old_audio_index = this->remote_audio_media_index_;
  const int8_t old_video_index = this->remote_video_media_index_;
  const bool old_shape_overflow = this->remote_media_shape_overflow_;
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
    this->set_media_config_(old_tx, old_rx, old_tx_pt, old_rx_pt);
    this->remote_rtp_ip_v4_.store(old_audio_ip, std::memory_order_release);
    this->remote_rtp_port_.store(old_audio_port, std::memory_order_release);
    this->remote_offer_media_lines_ = old_media_lines;
    this->remote_audio_media_index_ = old_audio_index;
    this->remote_video_media_index_ = old_video_index;
    this->remote_media_shape_overflow_ = old_shape_overflow;
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
  if (!this->learn_remote_rtp_from_sdp_(message_body(message), src_ip)) {
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
  this->get_media_config_(&new_tx, &new_rx, &new_tx_pt, &new_rx_pt);
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
    this->emit_sip_signal_(signal);
    return true;
  }
#endif

  this->set_media_config_(new_tx, new_rx, new_tx_pt, new_rx_pt);
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
  if (this->replay_completed_response_(message, src, "UPDATE")) {
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
    this->dest_route_ = identity_route;
    this->dest_name_ = identity_name;
  } else {
    this->caller_route_ = identity_route;
    this->caller_name_ = identity_name;
  }
  SipSignal signal;
  signal.type = SipSignalType::CONNECTED_IDENTITY;
  signal.call_id = this->call_id_;
  signal.connected_route = identity_route;
  signal.connected_name = identity_name;
  this->emit_sip_signal_(signal);
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
  const bool ack_sent = this->send_request_("ACK", "", ack_options);
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
    uint32_t ack_ip =
        this->remote_ip_v4_.load(std::memory_order_acquire);
    uint16_t ack_port =
        this->remote_sip_port_.load(std::memory_order_acquire);
    uint32_t contact_ip = 0;
    uint16_t contact_port = 0;
    if (sip_uri_ipv4_target(this->remote_target_uri_, &contact_ip,
                            &contact_port)) {
      ack_ip = contact_ip;
      ack_port = contact_port;
    }
    completed.ack_ip_v4 = ack_ip;
    completed.ack_port = ack_port;
    completed.completed_ms = millis();
    completed.udp =
        !this->remote_sip_tcp_.load(std::memory_order_acquire);
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
      this->emit_sip_signal_(signal);
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
    this->emit_sip_signal_(signal);
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
    this->emit_sip_signal_(signal);
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
  if (this->replay_completed_invite_ack_(message, src)) return true;
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
      this->clear_invite_transaction_();
    }
  }
  if (status > 100 && status < 200 && method == "INVITE") {
    SipSignal signal;
    signal.type = SipSignalType::STATUS_180_RINGING;
    signal.status_code = static_cast<uint16_t>(status);
    signal.call_id = this->call_id_;
    this->emit_sip_signal_(signal);
    return true;
  }
  if (status >= 200 && status < 300) {
    if (method == "BYE") {
      ESP_LOGI(TAG, "SIP BYE completed call_id=%s", this->call_id_.c_str());
      this->clear_bye_transaction_();
      this->reset_dialog_();
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
      this->emit_sip_signal_(signal);
      return true;
    }
    bool media_ok = false;
    bool video_prepared = true;
    {
      LockGuard media_lock(this->media_lifecycle_mutex_);
      media_ok = this->learn_remote_rtp_from_sdp_(
          message_body(message), src_ip, true);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
      if (media_ok)
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
      this->emit_sip_signal_(signal);
      return true;
    }
    this->open_media_session_();
    SipSignal signal;
    signal.type = SipSignalType::STATUS_200_OK;
    signal.status_code = static_cast<uint16_t>(status);
    signal.call_id = this->call_id_;
    this->get_media_config_(&signal.selected_tx_format, &signal.selected_rx_format, nullptr, nullptr);
    this->emit_sip_signal_(signal);
    return true;
  }
  if (status >= 300) {
    if (method != "INVITE") {
      ESP_LOGW(TAG, "SIP %u response for %s", status, method.c_str());
      if (method == "BYE") {
        this->clear_bye_transaction_();
        this->reset_dialog_();
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
    this->send_invite_error_ack_();
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
    this->emit_sip_signal_(signal);
    this->reset_dialog_();
    return true;
  }
  return true;
}

void SipTransport::handle_sip_datagram_(const char *data, size_t len, const sockaddr_in &src) {
  LockGuard lock(this->dialog_mutex_);
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
  if (msg.rfind("SIP/2.0 ", 0) == 0) {
    this->handle_response_(msg, src);
    return;
  }
  const size_t first_space = msg.find(' ');
  const std::string method = first_space == std::string::npos ? "" : msg.substr(0, first_space);
  ESP_LOGI(TAG, "SIP RX method=%s len=%u", method.c_str(), (unsigned) len);
  if ((method == "CANCEL" || method == "BYE") &&
      this->replay_completed_response_(msg, src, method)) {
    return;
  }
  const SipEvent event = sip_event_from_method_(method);
  if (event != SipEvent::NONE) this->mark_sip_event_(event);
  if (method == "INVITE") {
    this->handle_invite_(msg, src);
  } else if (method == "ACK") {
    bool terminate_after_ack = false;
    const uint16_t completed_status =
        this->acknowledge_completed_invite_(msg, src,
                                            &terminate_after_ack);
    if (terminate_after_ack) {
      const std::string call_id = this->call_id_;
      if (!this->send_bye_unlocked_(call_id)) this->reset_dialog_();
      return;
    }
    if (completed_status >= 300) return;
    const std::string request_call_id = header_value(msg, "Call-ID");
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
    this->outgoing_invite_pending_.store(false, std::memory_order_release);
    this->open_media_session_();
    this->send_connected_identity_update_();
  } else if (method == "BYE") {
    if (this->reject_if_stale_dialog_(msg, src, "BYE")) return;
    const bool local_bye_pending = !this->pending_bye_.empty();
    if (!this->media_active_.load(std::memory_order_acquire) &&
        !local_bye_pending) {
      this->send_stateless_response_(msg, src, 481, "Call/Transaction Does Not Exist");
      return;
    }
    this->send_stateless_response_(msg, src, 200, "OK", "", true);
    SipSignal signal;
    signal.type = SipSignalType::BYE;
    signal.call_id = this->call_id_;
    signal.terminal_transaction_pending = local_bye_pending;
    this->emit_sip_signal_(signal);
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
    this->emit_sip_signal_(signal);
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
  if (std::strcmp(method_name, "BYE") == 0) {
    const std::string from_tag = tag_from_header(header_value(request, "From"));
    const std::string to_tag = tag_from_header(header_value(request, "To"));
    dialog_tags_match = !this->remote_tag_.empty() && !this->local_tag_.empty() &&
                        from_tag == this->remote_tag_ && to_tag == this->local_tag_ &&
                        cseq_method(header_value(request, "CSeq")) == "BYE" &&
                        cseq_number(header_value(request, "CSeq")) > this->remote_dialog_cseq_;
  }
  if (call_id_matches && host_matches && dialog_tags_match) return false;
  ESP_LOGW(TAG, "SIP %s ignored for stale call_id=%s current=%s",
           method_name, request_call_id.c_str(), this->call_id_.c_str());
  this->send_stateless_response_(request, src, 481, "Call/Transaction Does Not Exist");
  return true;
}

void SipTransport::handle_sip_stream_(int socket, const sockaddr_in &src) {
  char buf[1024];
  auto drop_tcp_stream = [&](const char *reason) {
    char ip[16];
    inet_ntoa_r(src.sin_addr, ip, sizeof(ip));
    ESP_LOGW(TAG, "%s from %s, dropping connection", reason, ip);
    this->handle_tcp_peer_loss_();
  };
  while (true) {
    const int n = recv(socket, buf, sizeof(buf), 0);
    if (n > 0) {
      this->sip_tcp_rx_buffer_.append(buf, static_cast<size_t>(n));
      if (this->sip_tcp_rx_buffer_.size() > MAX_SIP_TCP_RX_BUFFER) {
        drop_tcp_stream("SIP TCP RX buffer overflow");
        return;
      }
      continue;
    }
    if (n == 0) {
      ESP_LOGI(TAG, "SIP TCP peer closed");
      this->handle_tcp_peer_loss_();
      return;
    }
    const int err = errno;
    if (err == EWOULDBLOCK || err == EAGAIN || err == ENOTCONN || err == EINPROGRESS || err == EALREADY) break;
    ESP_LOGW(TAG, "SIP TCP RX failed: %s (%d: %s)", socket_errno_name(err), err, socket_errno_text(err));
    this->handle_tcp_peer_loss_();
    return;
  }

  while (true) {
    const size_t sep = this->sip_tcp_rx_buffer_.find("\r\n\r\n");
    if (sep == std::string::npos) return;
    size_t body_len = 0;
    if (!sip_content_length(this->sip_tcp_rx_buffer_, &body_len)) {
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
    if (this->sip_tcp_rx_buffer_.size() < total) return;
    const std::string msg = this->sip_tcp_rx_buffer_.substr(0, total);
    this->sip_tcp_rx_buffer_.erase(0, total);
    this->remote_sip_tcp_.store(true, std::memory_order_release);
    this->handle_sip_datagram_(msg.data(), msg.size(), src);
  }
}

void SipTransport::sip_task_trampoline_(void *param) {
  static_cast<SipTransport *>(param)->sip_task_();
}

void SipTransport::rtp_task_trampoline_(void *param) {
  static_cast<SipTransport *>(param)->rtp_task_();
}

void SipTransport::sip_task_() {
  uint8_t buf[2048];
  int connecting_fd = -1;
  uint32_t connect_deadline_ms = 0;
  uint32_t connecting_ip_v4 = 0;
  uint16_t connecting_port = 0;
  auto close_connecting = [&]() {
    if (connecting_fd >= 0) close(connecting_fd);
    connecting_fd = -1;
    connect_deadline_ms = 0;
    connecting_ip_v4 = 0;
    connecting_port = 0;
  };
  auto drop_tcp_pending = [this]() {
    LockGuard lock(this->tcp_tx_pending_mutex_);
    this->tcp_tx_pending_.clear();
  };
  auto fail_tcp_connect = [&](int err) {
    char ip[16];
    struct in_addr a{};
    a.s_addr = htonl(connecting_ip_v4);
    inet_ntoa_r(a, ip, sizeof(ip));
    ESP_LOGW(TAG, "SIP TCP connect to %s:%u failed: %s (%d: %s)",
             ip, (unsigned) connecting_port, socket_errno_name(err), err, socket_errno_text(err));
    close_connecting();
    this->tcp_connect_requested_.store(false, std::memory_order_release);
    drop_tcp_pending();
    {
      LockGuard lock(this->dialog_mutex_);
      if (!this->call_id_.empty()) this->reset_dialog_();
    }
    this->emit_connection_change_(false);
  };
  auto promote_tcp_connect = [&]() {
    const int promoted_fd = connecting_fd;
    const uint32_t promoted_ip = connecting_ip_v4;
    const uint16_t promoted_port = connecting_port;
    std::string pending;
    bool pending_sent = true;
    {
      LockGuard send_lock(this->tcp_send_mutex_);
      this->sip_tcp_client_socket_.store(promoted_fd, std::memory_order_release);
      this->sip_tcp_client_ip_v4_.store(promoted_ip, std::memory_order_release);
      this->sip_tcp_client_close_requested_.store(false, std::memory_order_release);
      this->tcp_connect_requested_.store(false, std::memory_order_release);
      this->sip_tcp_rx_buffer_.clear();
      {
        LockGuard lock(this->tcp_tx_pending_mutex_);
        pending.swap(this->tcp_tx_pending_);
      }
      if (!pending.empty()) {
        pending_sent = this->send_sip_tcp_record_(pending, promoted_fd);
      }
    }
    char ip[16];
    struct in_addr a{};
    a.s_addr = htonl(promoted_ip);
    inet_ntoa_r(a, ip, sizeof(ip));
    ESP_LOGI(TAG, "SIP TCP originate connected to %s:%u", ip, (unsigned) promoted_port);
    connecting_fd = -1;
    connect_deadline_ms = 0;
    connecting_ip_v4 = 0;
    connecting_port = 0;
    if (!pending_sent) this->handle_tcp_peer_loss_();
  };
  while (this->running_.load(std::memory_order_acquire)) {
    this->pump_udp_retransmits_();
    if (this->sip_tcp_client_close_requested_.load(std::memory_order_acquire)) {
      close_connecting();
      this->close_tcp_client_from_sip_task_();
    }
    if (this->tcp_connect_requested_.load(std::memory_order_acquire)) {
      close_connecting();
      this->close_tcp_client_from_sip_task_();
      connecting_ip_v4 = this->tcp_connect_ip_v4_.load(std::memory_order_acquire);
      connecting_port = this->tcp_connect_port_.load(std::memory_order_acquire);
      if (connecting_ip_v4 == 0 || connecting_port == 0) {
        this->tcp_connect_requested_.store(false, std::memory_order_release);
        drop_tcp_pending();
        {
          LockGuard lock(this->dialog_mutex_);
          if (!this->call_id_.empty()) this->reset_dialog_();
        }
        this->emit_connection_change_(false);
      } else {
        connecting_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (connecting_fd < 0) {
          fail_tcp_connect(errno);
        } else {
          int opt = 1;
          setsockopt(connecting_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
          int flags = fcntl(connecting_fd, F_GETFL, 0);
          fcntl(connecting_fd, F_SETFL, flags | O_NONBLOCK);

          struct sockaddr_in dest{};
          dest.sin_family = AF_INET;
          dest.sin_addr.s_addr = htonl(connecting_ip_v4);
          dest.sin_port = htons(connecting_port);
          const int rc = connect(connecting_fd, reinterpret_cast<struct sockaddr *>(&dest), sizeof(dest));
          if (rc == 0) {
            promote_tcp_connect();
          } else if (errno == EINPROGRESS) {
            connect_deadline_ms = millis() + 2000;
            this->tcp_connect_requested_.store(false, std::memory_order_release);
          } else {
            fail_tcp_connect(errno);
          }
        }
      }
    }
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
    const int tcp_client = this->sip_tcp_client_socket_.load(std::memory_order_acquire);
    if (tcp_client >= 0) {
      FD_SET(tcp_client, &readfds);
      max_fd = std::max(max_fd, tcp_client);
    }
    if (connecting_fd >= 0) {
      FD_SET(connecting_fd, &writefds);
      max_fd = std::max(max_fd, connecting_fd);
    }
    if (max_fd < 0) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }
    struct timeval timeout{};
    struct timeval *timeout_ptr = nullptr;
    uint32_t timeout_ms = 0;
    bool have_timeout = false;
    if (connecting_fd >= 0) {
      const uint32_t now = millis();
      timeout_ms = time_reached(now, connect_deadline_ms) ? 0 : connect_deadline_ms - now;
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
        if (this->completed_invite_.awaiting_ack) {
          const bool retransmits =
              this->completed_invite_.udp ||
              this->completed_invite_.status < 300;
          include_at(retransmits
                         ? this->completed_invite_.next_retransmit_ms
                         : this->completed_invite_.deadline_ms);
        }
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
    if (timeout_ptr != nullptr) {
      timeout.tv_sec = timeout_ms / 1000;
      timeout.tv_usec = (timeout_ms % 1000) * 1000;
    }
    const int ready = select(max_fd + 1, &readfds, &writefds, nullptr, timeout_ptr);
    if (connecting_fd >= 0 && time_reached(millis(), connect_deadline_ms)) {
      fail_tcp_connect(ETIMEDOUT);
      continue;
    }
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

    if (connecting_fd >= 0 && FD_ISSET(connecting_fd, &writefds)) {
      int so_error = 0;
      socklen_t len = sizeof(so_error);
      if (getsockopt(connecting_fd, SOL_SOCKET, SO_ERROR, &so_error, &len) == 0 && so_error == 0) {
        promote_tcp_connect();
      } else {
        fail_tcp_connect(so_error == 0 ? errno : so_error);
      }
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
        const int active_client = this->sip_tcp_client_socket_.load(std::memory_order_acquire);
        const uint32_t active_ip_v4 = this->sip_tcp_client_ip_v4_.load(std::memory_order_acquire);
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
             connecting_fd >= 0 ||
             this->tcp_connect_requested_.load(std::memory_order_acquire));
        if (replaces_owned_transport ||
            (this->dialog_active_() && active_client >= 0 &&
             active_ip_v4 != 0 && active_ip_v4 != accepted_ip_v4)) {
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
        this->sip_tcp_client_socket_.store(client, std::memory_order_release);
        this->sip_tcp_client_ip_v4_.store(accepted_ip_v4, std::memory_order_release);
        this->sip_tcp_client_close_requested_.store(false, std::memory_order_release);
        this->sip_tcp_rx_buffer_.clear();
        this->remote_sip_tcp_.store(true, std::memory_order_release);
        char ip[16];
        inet_ntoa_r(src.sin_addr, ip, sizeof(ip));
        ESP_LOGI(TAG, "SIP TCP accepted from %s:%u", ip, (unsigned) ntohs(src.sin_port));
      }
    }

    if (this->sip_socket_ >= 0 && FD_ISSET(this->sip_socket_, &readfds)) {
      struct sockaddr_in src{};
      socklen_t slen = sizeof(src);
      int n = recvfrom(this->sip_socket_, buf, sizeof(buf) - 1, 0,
                       reinterpret_cast<struct sockaddr *>(&src), &slen);
      if (n > 0) {
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
        const bool tcp_session_active =
            tcp_call_active && this->remote_sip_tcp_.load(std::memory_order_acquire) &&
            (this->sip_tcp_client_socket_.load(std::memory_order_acquire) >= 0 || connecting_fd >= 0 ||
             this->tcp_connect_requested_.load(std::memory_order_acquire));
        if (tcp_session_active) {
          // Both listeners remain open, but a stray UDP packet must never flip
          // an established TCP dialog to UDP or redirect its ACK/BYE traffic.
          ESP_LOGD(TAG, "SIP UDP datagram ignored while TCP signaling is active");
          continue;
        }
        if (this->sip_tcp_client_socket_.load(std::memory_order_acquire) >= 0) {
          // An idle keep-alive connection must not coexist with a new UDP
          // dialog, otherwise later TCP traffic could retarget the call.
          this->close_tcp_client_from_sip_task_();
        }
        this->remote_sip_tcp_.store(false, std::memory_order_release);
        buf[n] = 0;
        char ip[16];
        inet_ntoa_r(src.sin_addr, ip, sizeof(ip));
        ESP_LOGI(TAG, "SIP UDP RX %d bytes from %s:%u", n, ip, (unsigned) ntohs(src.sin_port));
        this->handle_sip_datagram_(reinterpret_cast<const char *>(buf), static_cast<size_t>(n), src);
      }
    }

    const int active_tcp_client = this->sip_tcp_client_socket_.load(std::memory_order_acquire);
    if (active_tcp_client >= 0 && FD_ISSET(active_tcp_client, &readfds)) {
      struct sockaddr_in src{};
      socklen_t slen = sizeof(src);
      getpeername(active_tcp_client, reinterpret_cast<struct sockaddr *>(&src), &slen);
      this->handle_sip_stream_(active_tcp_client, src);
    }
  }
  close_connecting();
  this->close_tcp_client_from_sip_task_();
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
      const int ready = select(socket + 1, &readfds, nullptr, nullptr, nullptr);
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
      uint8_t rx_payload_type = 96;
      this->get_media_config_(nullptr, &rx_format, nullptr, &rx_payload_type);
      if ((buf[1] & 0x7F) != rx_payload_type) continue;
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
      const size_t out_len = rtp_payload_to_pcm(payload, payload_len, rx_format, pcm, sizeof(pcm));
      if (out_len == 0 || out_len != rx_format.nominal_frame_bytes()) continue;
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
      this->rtp_rx_packets_.fetch_add(1, std::memory_order_acq_rel);
      this->rtp_rx_bytes_.fetch_add(static_cast<uint32_t>(n), std::memory_order_acq_rel);
      this->emit_audio_frame_(pcm, out_len, sequence, timestamp,
                              source_changed);
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
      if (!signal.call_id.empty()) this->emit_sip_signal_(signal);
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
