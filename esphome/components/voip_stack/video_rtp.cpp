#include "video_rtp.h"

#if defined(USE_ESP32) && defined(USE_ESPHOME_VOIP_STACK_VIDEO)

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <inttypes.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <esp_heap_caps.h>
#include <esp_system.h>
#include <lwip/sockets.h>

namespace esphome {
namespace voip_stack {

static const char *const TAG = "voip_stack.video";

namespace {

uint32_t read_be32(const uint8_t *data) {
  return (static_cast<uint32_t>(data[0]) << 24) |
         (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) |
         static_cast<uint32_t>(data[3]);
}

void write_be16(uint8_t *data, uint16_t value) {
  data[0] = static_cast<uint8_t>(value >> 8);
  data[1] = static_cast<uint8_t>(value);
}

void write_be32(uint8_t *data, uint32_t value) {
  data[0] = static_cast<uint8_t>(value >> 24);
  data[1] = static_cast<uint8_t>(value >> 16);
  data[2] = static_cast<uint8_t>(value >> 8);
  data[3] = static_cast<uint8_t>(value);
}

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
bool next_annex_b_nal(const uint8_t *data, size_t size, size_t *cursor,
                      const uint8_t **nal, size_t *nal_size) {
  size_t start = *cursor;
  while (start + 3 < size) {
    if (data[start] == 0 && data[start + 1] == 0 &&
        ((data[start + 2] == 1) ||
         (start + 3 < size && data[start + 2] == 0 && data[start + 3] == 1))) {
      break;
    }
    start++;
  }
  if (start + 3 >= size) return false;
  const size_t prefix = data[start + 2] == 1 ? 3 : 4;
  const size_t payload = start + prefix;
  size_t end = payload;
  while (end + 3 < size) {
    if (data[end] == 0 && data[end + 1] == 0 &&
        (data[end + 2] == 1 ||
         (end + 3 < size && data[end + 2] == 0 && data[end + 3] == 1))) {
      break;
    }
    end++;
  }
  if (end + 3 >= size) end = size;
  while (end > payload && data[end - 1] == 0) end--;
  if (end <= payload) return false;
  *nal = data + payload;
  *nal_size = end - payload;
  *cursor = end;
  return true;
}

uint8_t hex_nibble(char value) {
  if (value >= '0' && value <= '9') return static_cast<uint8_t>(value - '0');
  if (value >= 'a' && value <= 'f') return static_cast<uint8_t>(value - 'a' + 10);
  if (value >= 'A' && value <= 'F') return static_cast<uint8_t>(value - 'A' + 10);
  return 0xFF;
}

bool parse_profile_level_id(const std::string &value, uint8_t *profile,
                            uint8_t *iop, uint8_t *level) {
  if (value.size() != 6 || profile == nullptr || iop == nullptr ||
      level == nullptr) {
    return false;
  }
  uint8_t bytes[3]{};
  for (size_t index = 0; index < 3; index++) {
    const uint8_t high = hex_nibble(value[index * 2]);
    const uint8_t low = hex_nibble(value[index * 2 + 1]);
    if (high == 0xFF || low == 0xFF) return false;
    bytes[index] = static_cast<uint8_t>((high << 4) | low);
  }
  *profile = bytes[0];
  *iop = bytes[1];
  *level = bytes[2];
  return true;
}

int h264_level_value(uint8_t profile, uint8_t iop, uint8_t level) {
  const bool level_1b =
      ((profile == 0x42 || profile == 0x4D || profile == 0x58) &&
       level == 11 && (iop & 0x10) != 0) ||
      ((profile != 0x42 && profile != 0x4D && profile != 0x58) &&
       level == 9);
  return level_1b ? 105 : static_cast<int>(level) * 10;
}
#endif

}  // namespace

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
bool h264_profile_level_id_from_annex_b(const uint8_t *data, size_t size,
                                        std::string *profile_level_id) {
  if (data == nullptr || profile_level_id == nullptr) return false;
  size_t cursor = 0;
  const uint8_t *nal = nullptr;
  size_t nal_size = 0;
  while (next_annex_b_nal(data, size, &cursor, &nal, &nal_size)) {
    if (nal_size >= 4 && (nal[0] & 0x1F) == 7) {
      char value[7];
      snprintf(value, sizeof(value), "%02x%02x%02x", nal[1], nal[2], nal[3]);
      *profile_level_id = value;
      return true;
    }
  }
  return false;
}

bool h264_same_subprofile(const std::string &left, const std::string &right) {
  uint8_t left_profile = 0;
  uint8_t left_iop = 0;
  uint8_t left_level = 0;
  uint8_t right_profile = 0;
  uint8_t right_iop = 0;
  uint8_t right_level = 0;
  if (!parse_profile_level_id(left, &left_profile, &left_iop, &left_level) ||
      !parse_profile_level_id(right, &right_profile, &right_iop,
                              &right_level)) {
    return false;
  }
  if (left_profile != 0x42 || right_profile != 0x42) return false;
  const bool left_constrained = (left_iop & 0x40) != 0;
  const bool right_constrained = (right_iop & 0x40) != 0;
  return left_constrained == right_constrained;
}

bool h264_level_fits(const std::string &source, const std::string &receiver) {
  if (!h264_same_subprofile(source, receiver)) return false;
  uint8_t source_profile = 0;
  uint8_t source_iop = 0;
  uint8_t source_level = 0;
  uint8_t receiver_profile = 0;
  uint8_t receiver_iop = 0;
  uint8_t receiver_level = 0;
  if (!parse_profile_level_id(source, &source_profile, &source_iop,
                              &source_level) ||
      !parse_profile_level_id(receiver, &receiver_profile, &receiver_iop,
                              &receiver_level)) {
    return false;
  }
  return h264_level_value(source_profile, source_iop, source_level) <=
         h264_level_value(receiver_profile, receiver_iop, receiver_level);
}
#endif

VideoRtpSession::VideoRtpSession(bool task_stack_in_psram)
    : task_stack_in_psram_(task_stack_in_psram) {
  this->task_done_ = xSemaphoreCreateBinaryStatic(&this->task_done_storage_);
  this->sender_task_done_ =
      xSemaphoreCreateBinaryStatic(&this->sender_task_done_storage_);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_HOSTED_AUDIO_PACING
  this->audio_pacing_ =
      xSemaphoreCreateBinaryStatic(&this->audio_pacing_storage_);
#endif
}

VideoRtpSession::~VideoRtpSession() {
  this->stop();
  // A timed stop deliberately retains task-owned memory instead of forcing a
  // delete through lwIP or a codec call. Destruction has no such escape hatch:
  // wait for both workers to self-delete before releasing anything they can
  // still reference.
  this->quiesce_tasks_();
  if (this->sink_started_.exchange(false, std::memory_order_acq_rel) &&
      this->sink_ != nullptr) {
    this->sink_->stop_video();
  }
  this->close_sockets_();
  if (this->reassembly_ != nullptr) {
    heap_caps_free(this->reassembly_);
    this->reassembly_ = nullptr;
  }
  for (auto &slot : this->tx_access_units_) {
    if (slot.data != nullptr) {
      heap_caps_free(slot.data);
      slot.data = nullptr;
    }
  }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  if (this->jpeg_quantization_cache_ != nullptr) {
    this->jpeg_depacketizer_.set_quantization_cache(nullptr, 0);
    heap_caps_free(this->jpeg_quantization_cache_);
    this->jpeg_quantization_cache_ = nullptr;
  }
#endif
}

bool VideoRtpSession::set_negotiated(
                                     const VideoCapability &capability,
                                     const VideoCapability &send_capability,
                                     const VideoCapability &receive_capability,
                                     uint32_t remote_ip_v4,
                                     uint16_t remote_rtp_port,
                                     uint32_t remote_rtcp_ip_v4,
                                     uint16_t remote_rtcp_port,
                                     bool send_enabled,
                                     bool receive_enabled) {
  if (this->running_.load(std::memory_order_acquire) ||
      this->source_started_ || !this->reap_sender_task_() ||
      !this->reap_receive_task_()) {
    ESP_LOGE(TAG, "Refusing to replace video negotiation while media is owned");
    return false;
  }
  if (this->sink_started_ && this->sink_ != nullptr) {
    this->sink_->stop_video();
    this->sink_started_ = false;
  }
  this->close_sockets_();
  // This is an RTP session boundary, including recovery after a worker that
  // exceeded the stop deadline and exited later. Never carry an incomplete
  // H.264 FU/STAP access unit into the replacement negotiation.
  this->reset_reassembly_();
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
  this->reset_h264_parameter_sets_();
#endif
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  // Dynamic RFC 2435 Q mappings belong to one negotiated RTP session and
  // must never leak into a later call.
  this->jpeg_depacketizer_.reset_session();
  this->jpeg_quantization_cache_allocation_failed_ = false;
  this->jpeg_parse_warning_emitted_ = false;
#endif
  this->capability_ = capability;
  this->send_capability_ = send_capability;
  this->receive_capability_ = receive_capability;
  this->remote_ip_v4_.store(remote_ip_v4, std::memory_order_release);
  this->remote_rtp_port_.store(remote_rtp_port, std::memory_order_release);
  this->remote_rtcp_ip_v4_.store(
      remote_rtcp_ip_v4 != 0 ? remote_rtcp_ip_v4 : remote_ip_v4,
      std::memory_order_release);
  this->remote_rtcp_port_.store(
      remote_rtcp_port != 0
          ? remote_rtcp_port
          : static_cast<uint16_t>(remote_rtp_port + 1),
      std::memory_order_release);
  this->send_enabled_ = send_enabled;
  this->receive_enabled_ = receive_enabled;
  this->send_prepared_.store(false, std::memory_order_release);
  this->receive_prepared_.store(false, std::memory_order_release);
  this->rx_reset_requested_.store(false, std::memory_order_release);
  this->rtcp_bye_requested_.store(false, std::memory_order_release);
  this->accept_remote_pli_ =
      send_capability.is_h264() && send_capability.rtcp_feedback_pli;
  this->accept_remote_fir_ =
      send_capability.is_h264() && send_capability.rtcp_feedback_fir;
  this->send_remote_pli_ =
      receive_capability.is_h264() && receive_capability.rtcp_feedback_pli;
  return true;
}

bool VideoRtpSession::negotiation_matches(
    const VideoCapability &capability,
    const VideoCapability &send_capability,
    const VideoCapability &receive_capability, uint32_t remote_ip_v4,
    uint16_t remote_rtp_port, uint32_t remote_rtcp_ip_v4,
    uint16_t remote_rtcp_port) const {
  const auto same_capability = [](const VideoCapability &left,
                                  const VideoCapability &right) {
    return left.payload_type == right.payload_type &&
           left.clock_rate == right.clock_rate &&
           left.width == right.width &&
           left.height == right.height &&
           left.max_fps == right.max_fps &&
           left.packetization_mode == right.packetization_mode &&
           left.level_asymmetry_allowed ==
               right.level_asymmetry_allowed &&
           left.max_bitrate_bps == right.max_bitrate_bps &&
           left.rtcp_feedback_pli == right.rtcp_feedback_pli &&
           left.rtcp_feedback_fir == right.rtcp_feedback_fir &&
           left.encoding == right.encoding &&
           left.profile_level_id == right.profile_level_id;
  };
  return this->running_.load(std::memory_order_acquire) &&
         same_capability(capability, this->capability_) &&
         same_capability(send_capability, this->send_capability_) &&
         same_capability(receive_capability,
                         this->receive_capability_) &&
         remote_ip_v4 ==
             this->remote_ip_v4_.load(std::memory_order_acquire) &&
         remote_rtp_port ==
             this->remote_rtp_port_.load(std::memory_order_acquire) &&
         (remote_rtcp_ip_v4 != 0 ? remote_rtcp_ip_v4 : remote_ip_v4) ==
             this->remote_rtcp_ip_v4_.load(std::memory_order_acquire) &&
         (remote_rtcp_port != 0
              ? remote_rtcp_port
              : static_cast<uint16_t>(remote_rtp_port + 1)) ==
             this->remote_rtcp_port_.load(std::memory_order_acquire);
}

bool VideoRtpSession::bind_socket_(int *fd, uint16_t port, const char *label) {
  *fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (*fd < 0) {
    ESP_LOGE(TAG, "%s socket failed: %d", label, errno);
    return false;
  }
  const int flags = fcntl(*fd, F_GETFL, 0);
  if (flags >= 0) fcntl(*fd, F_SETFL, flags | O_NONBLOCK);
  // RFC 8837's interactive-video marking keeps audio (EF) ahead of video
  // while still allowing Wi-Fi/WMM implementations to classify this flow.
  const int tos = 0x88;  // DSCP AF41, ECN not set.
  if (setsockopt(*fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) != 0) {
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_DEBUG
    ESP_LOGW(TAG, "%s could not set AF41 DSCP: %d", label, errno);
#endif
  }
  struct sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = htons(port);
  if (bind(*fd, reinterpret_cast<struct sockaddr *>(&local), sizeof(local)) != 0) {
    ESP_LOGE(TAG, "%s bind UDP/%u failed: %d", label, (unsigned) port, errno);
    close(*fd);
    *fd = -1;
    return false;
  }
  return true;
}

bool VideoRtpSession::start(bool activate) {
  if (this->running_.load(std::memory_order_acquire)) return true;
  // A timed-out teardown retains every object a worker could still touch.
  // Reap a worker that subsequently exited, or refuse to reuse its static
  // TCB/stack and sockets while it is still alive.
  if (!this->reap_sender_task_() || !this->reap_receive_task_()) {
    ESP_LOGE(TAG, "Video media worker from prior session is still active");
    return false;
  }
  this->close_sockets_();
  if (!this->capability_.valid() || this->remote_ip_v4_.load() == 0 ||
      this->remote_rtp_port_.load() == 0) {
    ESP_LOGW(TAG, "Video media not started: incomplete negotiation");
    return false;
  }
  const bool negotiated_send =
      this->send_enabled_.load(std::memory_order_acquire);
  const bool negotiated_receive =
      this->receive_enabled_.load(std::memory_order_acquire);
  if (negotiated_send && this->source_ == nullptr) return false;
  if (negotiated_receive && this->sink_ == nullptr) return false;

  const VideoCapability source_capability =
      this->source_ != nullptr ? this->source_->get_video_capability()
                               : VideoCapability{};
  const VideoCapability sink_capability =
      this->sink_ != nullptr ? this->sink_->get_receive_video_capability()
                             : VideoCapability{};
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  const bool source_compatible =
      this->send_capability_.valid() && source_capability.valid() &&
      source_capability.encoding == this->send_capability_.encoding &&
      this->send_capability_.is_jpeg();
  const bool sink_compatible =
      this->receive_capability_.valid() && sink_capability.valid() &&
      sink_capability.encoding == this->receive_capability_.encoding &&
      this->receive_capability_.is_jpeg();
#else
  const bool source_compatible =
      this->send_capability_.valid() && source_capability.valid() &&
      source_capability.encoding == this->send_capability_.encoding &&
      this->send_capability_.is_h264() &&
      h264_level_fits(source_capability.profile_level_id,
                      this->send_capability_.profile_level_id);
  const bool sink_compatible =
      this->receive_capability_.valid() && sink_capability.valid() &&
      sink_capability.encoding == this->receive_capability_.encoding &&
      this->receive_capability_.is_h264() &&
      h264_level_fits(this->receive_capability_.profile_level_id,
                      sink_capability.profile_level_id);
#endif
  if (negotiated_send && !source_compatible) {
    ESP_LOGE(TAG, "Negotiated video is incompatible with the local source");
    return false;
  }
  if (negotiated_receive && !sink_compatible) {
    ESP_LOGE(TAG, "Negotiated video is incompatible with the local sink");
    return false;
  }
  bool source_prepared =
      source_compatible &&
      this->source_->prepare_video(this->send_capability_);
  if (negotiated_send && !source_prepared) {
    ESP_LOGE(TAG, "Negotiated video source preparation failed");
    return false;
  }

  if (this->reassembly_ == nullptr && sink_compatible) {
    this->reassembly_ = static_cast<uint8_t *>(
        heap_caps_malloc(kMaxAccessUnitBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (this->reassembly_ == nullptr && negotiated_receive) {
      ESP_LOGE(TAG, "Video reassembly PSRAM allocation failed");
      return false;
    }
  }
  if (source_compatible) {
    for (auto &slot : this->tx_access_units_) {
      if (slot.data == nullptr) {
        slot.data = static_cast<uint8_t *>(heap_caps_malloc(
            kMaxAccessUnitBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      }
      if (slot.data == nullptr && negotiated_send) {
        ESP_LOGE(TAG, "Video TX access-unit PSRAM allocation failed");
        return false;
      }
    }
  }
  if (!this->bind_socket_(&this->rtp_socket_, this->local_rtp_port_, "video RTP") ||
      !this->bind_socket_(&this->rtcp_socket_, this->local_rtp_port_ + 1, "video RTCP") ||
      !this->bind_socket_(&this->wake_socket_, 0, "video wake")) {
    this->close_sockets_();
    return false;
  }
  struct sockaddr_in wake_addr {};
  socklen_t wake_addr_size = sizeof(wake_addr);
  if (getsockname(this->wake_socket_,
                  reinterpret_cast<struct sockaddr *>(&wake_addr),
                  &wake_addr_size) != 0 ||
      wake_addr.sin_port == 0) {
    ESP_LOGE(TAG, "Video wake socket address unavailable: %d", errno);
    this->close_sockets_();
    return false;
  }
  this->wake_port_ = ntohs(wake_addr.sin_port);
  // Preparation owns resources but no media direction. In particular a
  // re-INVITE cannot leak camera RTP or a stale receive frame before its 200.
  this->send_enabled_.store(false, std::memory_order_release);
  this->receive_enabled_.store(false, std::memory_order_release);
  this->send_prepared_.store(false, std::memory_order_release);
  this->receive_prepared_.store(false, std::memory_order_release);
  this->source_started_.store(false, std::memory_order_release);
  this->sink_started_.store(false, std::memory_order_release);
  this->rx_reset_requested_.store(false, std::memory_order_release);
  this->tx_sequence_.store(static_cast<uint16_t>(esp_random()), std::memory_order_release);
  this->tx_timestamp_offset_ = esp_random();
  this->tx_ssrc_ = esp_random();
  this->remote_ssrc_.store(0, std::memory_order_release);
  this->remote_ssrc_latched_.store(false, std::memory_order_release);
  this->latched_remote_rtp_port_.store(0, std::memory_order_release);
  this->latched_remote_rtcp_port_.store(0, std::memory_order_release);
  this->sequence_valid_ = false;
  this->tx_packets_.store(0, std::memory_order_release);
  this->rx_packets_.store(0, std::memory_order_release);
  this->tx_access_units_ok_.store(0, std::memory_order_release);
  this->rx_access_units_ok_.store(0, std::memory_order_release);
  this->tx_octets_.store(0, std::memory_order_release);
  this->last_tx_rtp_timestamp_.store(0, std::memory_order_release);
  this->last_tx_rtp_ms_.store(0, std::memory_order_release);
  this->dropped_access_units_.store(0, std::memory_order_release);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_HOSTED_AUDIO_PACING
  this->audio_pacing_started_.store(false, std::memory_order_release);
  this->reset_audio_pacing_burst_();
  xSemaphoreTake(this->audio_pacing_, 0);
#endif
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_DEBUG
  this->tx_backpressure_events_ = 0;
  this->tx_send_failures_ = 0;
  this->tx_max_access_unit_ms_ = 0;
  this->tx_slow_send_calls_ = 0;
  this->tx_max_send_us_ = 0;
  this->tx_last_debug_log_ms_ = millis();
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_HOSTED_AUDIO_PACING
  this->tx_audio_pacing_credit_waits_ = 0;
  this->tx_audio_pacing_max_wait_us_ = 0;
#endif
#endif
  this->last_pli_ms_ = 0;
  this->stop_had_active_session_.store(false, std::memory_order_release);
  this->rtcp_bye_requested_.store(false, std::memory_order_release);
  this->terminate_.store(false, std::memory_order_release);
  this->last_rtcp_ms_ = millis();
  this->running_.store(true, std::memory_order_release);
  xSemaphoreTake(this->task_done_, 0);
  this->task_done_observed_ = false;
  if (!voip_audio_core::start_managed_pinned_task(
          VideoRtpSession::task_trampoline_, "voip_video_rtp", kTaskStackBytes,
          this, kReceiveTaskPriority, 0, this->task_stack_in_psram_, TAG,
          &this->task_handle_, &this->task_tcb_, &this->task_stack_,
          &this->task_with_caps_)) {
    this->running_.store(false, std::memory_order_release);
    this->close_sockets_();
    return false;
  }

  bool receive_prepared =
      sink_compatible && this->reassembly_ != nullptr &&
      this->sink_->start_video(this->receive_capability_);
  if (receive_prepared &&
      !this->sink_->set_video_active(false)) {
    receive_prepared = false;
  }
  this->sink_started_.store(receive_prepared, std::memory_order_release);
  this->receive_prepared_.store(receive_prepared,
                                std::memory_order_release);
  if (negotiated_receive && !receive_prepared) {
    this->request_stop();
    return false;
  }

  bool tx_queue_prepared = true;
  for (const auto &slot : this->tx_access_units_)
    tx_queue_prepared = tx_queue_prepared && slot.data != nullptr;
  bool send_prepared = source_prepared && tx_queue_prepared;
  if (send_prepared && !this->start_sender_task_()) {
    send_prepared = false;
    if (negotiated_send) {
      this->request_stop();
      return false;
    }
  }
  this->send_prepared_.store(send_prepared, std::memory_order_release);
  if (activate &&
      !this->request_media_direction(negotiated_send,
                                     negotiated_receive)) {
    this->request_stop();
    return false;
  }
  ESP_LOGI(TAG,
           "Video RTP %s local=%u remote=%u PT=%u codec=%s dir=%s%s",
           activate ? "started" : "prepared",
           (unsigned) this->local_rtp_port_,
           (unsigned) this->remote_rtp_port_.load(),
           (unsigned) this->capability_.payload_type,
           this->capability_.encoding.c_str(),
           this->send_enabled_ ? "send" : "",
           this->receive_enabled_ ? "recv" : "");
  return true;
}

bool VideoRtpSession::request_send_direction(bool enabled) {
  return this->request_media_direction(
      enabled, this->receive_enabled_.load(std::memory_order_acquire));
}

bool VideoRtpSession::can_request_media_direction(
    bool send_enabled, bool receive_enabled) const {
  return this->running_.load(std::memory_order_acquire) &&
         (!send_enabled ||
          this->send_prepared_.load(std::memory_order_acquire)) &&
         (!receive_enabled ||
          this->receive_prepared_.load(std::memory_order_acquire));
}

bool VideoRtpSession::request_media_direction(bool send_enabled,
                                              bool receive_enabled) {
  LockGuard direction_lock(this->direction_mutex_);
  if (!this->can_request_media_direction(send_enabled, receive_enabled)) {
    return false;
  }
  const bool send_was_enabled =
      this->send_enabled_.load(std::memory_order_acquire);
  const bool receive_was_enabled =
      this->receive_enabled_.load(std::memory_order_acquire);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_HOSTED_AUDIO_PACING
  if (send_enabled != send_was_enabled) {
    this->audio_pacing_started_.store(false, std::memory_order_release);
    this->reset_audio_pacing_burst_();
    if (this->audio_pacing_ != nullptr)
      xSemaphoreTake(this->audio_pacing_, 0);
  }
#endif

  bool receive_changed = false;
  if (receive_enabled != receive_was_enabled) {
    if (this->sink_ == nullptr ||
        !this->sink_->set_video_active(receive_enabled)) {
      return false;
    }
    receive_changed = true;
    if (!this->running_.load(std::memory_order_acquire) ||
        this->terminate_.load(std::memory_order_acquire)) {
      this->sink_->set_video_active(false);
      return false;
    }
  }

  if (send_enabled && !send_was_enabled) {
    bool source_started = false;
    {
      LockGuard source_lock(this->source_control_mutex_);
      if (this->running_.load(std::memory_order_acquire) &&
          !this->terminate_.load(std::memory_order_acquire) &&
          this->source_ != nullptr) {
        source_started = this->source_->start_video(
            VideoRtpSession::source_callback_, this,
            this->send_capability_);
      }
    }
    if (!source_started) {
      ESP_LOGE(TAG, "Video source failed to start after direction update");
      if (receive_changed)
        this->sink_->set_video_active(receive_was_enabled);
      return false;
    }
    this->source_started_.store(true, std::memory_order_release);
    this->send_enabled_.store(true, std::memory_order_release);
    if (!this->running_.load(std::memory_order_acquire) ||
        this->terminate_.load(std::memory_order_acquire)) {
      this->send_enabled_.store(false, std::memory_order_release);
      if (this->source_started_.exchange(false,
                                         std::memory_order_acq_rel)) {
        LockGuard source_lock(this->source_control_mutex_);
        this->source_->stop_video();
      }
      if (receive_changed) this->sink_->set_video_active(false);
      return false;
    }
  }
  if (!send_enabled && send_was_enabled) {
    this->send_enabled_.store(false, std::memory_order_release);
    if (this->source_started_.exchange(false, std::memory_order_acq_rel) &&
        this->source_ != nullptr) {
      LockGuard source_lock(this->source_control_mutex_);
      this->source_->stop_video();
    }
    // The sender exclusively releases an owned/ready AU slot. Waking it lets
    // it discard the now-disabled frame without signaling-side overwrite.
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_HOSTED_AUDIO_PACING
    if (this->audio_pacing_ != nullptr) xSemaphoreGive(this->audio_pacing_);
#endif
    if (this->sender_task_handle_ != nullptr)
      xTaskNotifyGive(this->sender_task_handle_);
  }

  if (!this->running_.load(std::memory_order_acquire) ||
      this->terminate_.load(std::memory_order_acquire)) {
    this->send_enabled_.store(false, std::memory_order_release);
    this->receive_enabled_.store(false, std::memory_order_release);
    if (this->source_started_.exchange(false, std::memory_order_acq_rel) &&
        this->source_ != nullptr) {
      LockGuard source_lock(this->source_control_mutex_);
      this->source_->stop_video();
    }
    if (this->sink_ != nullptr) this->sink_->set_video_active(false);
    return false;
  }

  this->send_enabled_.store(send_enabled, std::memory_order_release);
  this->receive_enabled_.store(receive_enabled, std::memory_order_release);
  if (!receive_enabled && receive_was_enabled) {
    this->rx_reset_requested_.store(true, std::memory_order_release);
    this->wake_task_();
  }
  ESP_LOGI(TAG, "Video RTP direction updated; dir=%s%s",
           send_enabled ? "send" : "",
           receive_enabled ? "recv" : "");
  return true;
}

void VideoRtpSession::request_stop() {
  const bool was_running = this->running_.exchange(false, std::memory_order_acq_rel);
  const bool had_active_direction =
      was_running &&
      (this->send_enabled_.load(std::memory_order_acquire) ||
       this->receive_enabled_.load(std::memory_order_acquire));
  if (had_active_direction) {
    this->stop_had_active_session_.store(true, std::memory_order_release);
    this->rtcp_bye_requested_.store(true, std::memory_order_release);
  }
  this->send_enabled_.store(false, std::memory_order_release);
  this->receive_enabled_.store(false, std::memory_order_release);
  this->sender_running_.store(false, std::memory_order_release);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_HOSTED_AUDIO_PACING
  this->audio_pacing_started_.store(false, std::memory_order_release);
  this->reset_audio_pacing_burst_();
  if (this->audio_pacing_ != nullptr)
    xSemaphoreTake(this->audio_pacing_, 0);
  if (this->audio_pacing_ != nullptr) xSemaphoreGive(this->audio_pacing_);
#endif
  if (this->sender_task_handle_ != nullptr)
    xTaskNotifyGive(this->sender_task_handle_);
  this->terminate_.store(true, std::memory_order_release);
  this->wake_task_();
}

void VideoRtpSession::stop() {
  LockGuard stop_lock(this->stop_mutex_);
  this->request_stop();
  // Source shutdown and both joins share one absolute budget. Any unfinished
  // worker retains every socket/buffer it may still reference until
  // destruction quiesces it, preserving the existing no-UAF policy.
  const TickType_t stop_started = xTaskGetTickCount();
  const TickType_t stop_budget = pdMS_TO_TICKS(kWorkerStopBudgetMs);
  // Everything below may wait for a camera callback, renderer task or socket
  // worker. SipTransport invokes it only from its lifecycle worker, after
  // BYE/CANCEL has already gated the public call path.
  if (this->source_started_.exchange(false, std::memory_order_acq_rel) &&
      this->source_ != nullptr) {
    LockGuard source_lock(this->source_control_mutex_);
    this->source_->stop_video();
  }
  const bool sender_stopped =
      this->stop_sender_task_(stop_started, stop_budget);
  const bool receiver_stopped =
      this->stop_receive_task_(stop_started, stop_budget);
  if (receiver_stopped &&
      this->sink_started_.exchange(false, std::memory_order_acq_rel) &&
      this->sink_ != nullptr) {
    this->sink_->stop_video();
  }
  if (sender_stopped && receiver_stopped) {
    this->close_sockets_();
  } else {
    const TickType_t stop_elapsed = xTaskGetTickCount() - stop_started;
    ESP_LOGE(TAG,
             "Video worker stop timeout after %u ms: sender=%s receiver=%s; "
             "retaining sockets and owned buffers",
             (unsigned) ((stop_elapsed * 1000U) / configTICK_RATE_HZ),
             sender_stopped ? "stopped" : "active",
             receiver_stopped ? "stopped" : "active");
  }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_DEBUG
  if (this->stop_had_active_session_.exchange(
          false, std::memory_order_acq_rel)) {
    ESP_LOGI(TAG,
             "Video session totals: tx=%u rx=%u completed_au=%u "
             "dropped_au=%u backpressure=%u send_fail=%u "
             "slow_send=%u max_send_us=%u max_au_ms=%u",
             (unsigned) this->tx_packets_.load(std::memory_order_acquire),
             (unsigned) this->rx_packets_.load(std::memory_order_acquire),
             (unsigned) this->tx_access_units_ok_.load(
                 std::memory_order_acquire),
             (unsigned) this->dropped_access_units_.load(
                 std::memory_order_acquire),
             (unsigned) this->tx_backpressure_events_,
             (unsigned) this->tx_send_failures_,
             (unsigned) this->tx_slow_send_calls_,
             (unsigned) this->tx_max_send_us_,
             (unsigned) this->tx_max_access_unit_ms_);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_HOSTED_AUDIO_PACING
    ESP_LOGI(TAG,
             "Video audio-first pacing: credit_waits=%u max_wait_us=%u",
             (unsigned) this->tx_audio_pacing_credit_waits_,
             (unsigned) this->tx_audio_pacing_max_wait_us_);
#endif
  }
#endif
  if (receiver_stopped) this->reset_reassembly_();
}

bool VideoRtpSession::reap_receive_task_() {
  if (this->task_handle_ == nullptr) {
    this->task_done_observed_ = false;
    return true;
  }
  if (!this->task_done_observed_) {
    if (this->task_done_ == nullptr ||
        xSemaphoreTake(this->task_done_, 0) != pdTRUE) {
      return false;
    }
    this->task_done_observed_ = true;
  }
  voip_audio_core::cleanup_managed_pinned_task(
      &this->task_handle_, &this->task_stack_, kTaskStackBytes,
      this->task_with_caps_);
  if (this->task_handle_ == nullptr) {
    this->task_done_observed_ = false;
    return true;
  }
  return false;
}

bool VideoRtpSession::stop_receive_task_(TickType_t stop_started,
                                         TickType_t stop_budget) {
  this->terminate_.store(true, std::memory_order_release);
  this->wake_task_();
  if (this->reap_receive_task_()) return true;
  const TickType_t elapsed = xTaskGetTickCount() - stop_started;
  const TickType_t remaining =
      elapsed < stop_budget ? stop_budget - elapsed : 0;
  if (!this->task_done_observed_ && this->task_done_ != nullptr &&
      remaining > 0 &&
      xSemaphoreTake(this->task_done_, remaining) == pdTRUE) {
    this->task_done_observed_ = true;
    this->reap_receive_task_();
  }
  return this->task_handle_ == nullptr;
}

void VideoRtpSession::close_sockets_() {
  LockGuard lock(this->socket_mutex_);
  if (this->rtp_socket_ >= 0) close(this->rtp_socket_);
  if (this->rtcp_socket_ >= 0) close(this->rtcp_socket_);
  if (this->wake_socket_ >= 0) close(this->wake_socket_);
  this->rtp_socket_ = -1;
  this->rtcp_socket_ = -1;
  this->wake_socket_ = -1;
  this->wake_port_ = 0;
}

void VideoRtpSession::source_callback_(void *ctx,
                                       const EncodedVideoAccessUnit &access_unit) {
  if (ctx == nullptr) return;
  static_cast<VideoRtpSession *>(ctx)->queue_access_unit_(access_unit);
}

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_HOSTED_AUDIO_PACING
void VideoRtpSession::notify_audio_packet_sent() {
  if (!this->running_.load(std::memory_order_acquire) ||
      !this->sender_running_.load(std::memory_order_acquire) ||
      !this->send_enabled_.load(std::memory_order_acquire) ||
      this->audio_pacing_ == nullptr) {
    return;
  }
  this->audio_pacing_started_.store(true, std::memory_order_release);
  // A binary semaphore is intentional: completed audio packets collapse into
  // one event while video is busy. The sender expands that event into one
  // bounded local burst, so no unbounded credit backlog can accumulate.
  xSemaphoreGive(this->audio_pacing_);
}

void VideoRtpSession::reset_audio_pacing_burst_() {
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  this->audio_pacing_burst_remaining_.store(0, std::memory_order_release);
#endif
}

bool VideoRtpSession::wait_for_audio_pacing_() {
  if (this->audio_pacing_ == nullptr) return false;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  uint8_t remaining =
      this->audio_pacing_burst_remaining_.load(std::memory_order_acquire);
  while (remaining > 0) {
    if (this->audio_pacing_burst_remaining_.compare_exchange_weak(
            remaining, static_cast<uint8_t>(remaining - 1),
            std::memory_order_acq_rel, std::memory_order_acquire)) {
      return this->running_.load(std::memory_order_acquire) &&
             this->sender_running_.load(std::memory_order_acquire) &&
             this->send_enabled_.load(std::memory_order_acquire);
    }
  }
#endif

  // Before the first successful microphone RTP send there is no competing
  // uplink flow to protect. Let the lower-priority video worker send complete
  // startup media without a timer. Once audio starts, every H.264 access unit
  // (or bounded JPEG packet burst) consumes one collapsed audio completion.
  // This follows Espressif's audio-first media loop while never splitting an
  // H.264 dependency unit across an artificial pacing wait.
  if (!this->audio_pacing_started_.load(std::memory_order_acquire)) {
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
    this->audio_pacing_burst_remaining_.store(
        kVideoPacketsPerAudioCredit - 1, std::memory_order_release);
#endif
    return this->running_.load(std::memory_order_acquire) &&
           this->sender_running_.load(std::memory_order_acquire) &&
           this->send_enabled_.load(std::memory_order_acquire);
  }

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_DEBUG
  const uint32_t wait_started_us = micros();
  this->tx_audio_pacing_credit_waits_++;
#endif
  // No periodic timer or polling: the next successful audio RTP enqueue or
  // request_stop() wakes the video sender.
  const BaseType_t acquired =
      xSemaphoreTake(this->audio_pacing_, portMAX_DELAY);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_DEBUG
  const uint32_t waited_us = micros() - wait_started_us;
  this->tx_audio_pacing_max_wait_us_ =
      std::max(this->tx_audio_pacing_max_wait_us_, waited_us);
#endif
  const bool active =
      acquired == pdTRUE &&
      this->running_.load(std::memory_order_acquire) &&
      this->sender_running_.load(std::memory_order_acquire) &&
      this->send_enabled_.load(std::memory_order_acquire);
  if (!active) {
    this->reset_audio_pacing_burst_();
    return false;
  }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  this->audio_pacing_burst_remaining_.store(
      kVideoPacketsPerAudioCredit - 1, std::memory_order_release);
#endif
  return true;
}
#endif

void VideoRtpSession::queue_access_unit_(
    const EncodedVideoAccessUnit &access_unit) {
  if (!this->running_.load(std::memory_order_acquire) ||
      !this->sender_running_.load(std::memory_order_acquire) ||
      !this->send_enabled_ ||
      access_unit.data == nullptr || access_unit.size == 0) {
    return;
  }
  if (access_unit.size > kMaxAccessUnitBytes) {
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
    {
      this->tx_resync_needed_.store(true, std::memory_order_release);
      if (this->sender_task_handle_ != nullptr)
        xTaskNotifyGive(this->sender_task_handle_);
    }
#endif
    this->dropped_access_units_.fetch_add(1, std::memory_order_acq_rel);
    return;
  }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
  if (this->tx_resync_needed_.load(std::memory_order_acquire) &&
      !access_unit.key_frame) {
    this->dropped_access_units_.fetch_add(1, std::memory_order_acq_rel);
    return;
  }
#endif
  auto &slot = this->tx_access_units_[this->tx_write_slot_];
  if (slot.data == nullptr) return;
  uint8_t expected = 0;
  if (!slot.state.compare_exchange_strong(
          expected, 2, std::memory_order_acq_rel)) {
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
    this->tx_resync_needed_.store(true, std::memory_order_release);
#endif
    this->dropped_access_units_.fetch_add(1, std::memory_order_acq_rel);
    return;
  }
  memcpy(slot.data, access_unit.data, access_unit.size);
  slot.size = access_unit.size;
  slot.timestamp = access_unit.timestamp_90khz;
  slot.key_frame = access_unit.key_frame;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
  if (access_unit.key_frame)
    this->tx_resync_needed_.store(false, std::memory_order_release);
#endif
  slot.state.store(1, std::memory_order_release);
  this->tx_write_slot_ =
      static_cast<uint8_t>((this->tx_write_slot_ + 1) % kTxAccessUnitSlots);
  if (this->sender_task_handle_ != nullptr)
    xTaskNotifyGive(this->sender_task_handle_);
}

bool VideoRtpSession::start_sender_task_() {
  if (!this->reap_sender_task_()) return false;
  for (auto &slot : this->tx_access_units_)
    slot.state.store(0, std::memory_order_release);
  this->tx_write_slot_ = 0;
  this->tx_read_slot_ = 0;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_HOSTED_AUDIO_PACING
  this->reset_audio_pacing_burst_();
#endif
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
  this->tx_resync_needed_.store(false, std::memory_order_release);
#endif
  this->sender_running_.store(true, std::memory_order_release);
  xSemaphoreTake(this->sender_task_done_, 0);
  this->sender_task_done_observed_ = false;
  if (voip_audio_core::start_managed_pinned_task(
          VideoRtpSession::sender_task_trampoline_, "voip_video_tx",
          kSenderTaskStackBytes, this, kSenderTaskPriority, kSenderTaskCore,
          this->task_stack_in_psram_, TAG, &this->sender_task_handle_,
          &this->sender_task_tcb_, &this->sender_task_stack_,
          &this->sender_task_with_caps_)) {
    return true;
  }
  this->sender_running_.store(false, std::memory_order_release);
  return false;
}

bool VideoRtpSession::reap_sender_task_() {
  if (this->sender_task_handle_ == nullptr) {
    this->sender_task_done_observed_ = false;
    return true;
  }
  if (!this->sender_task_done_observed_) {
    if (this->sender_task_done_ == nullptr ||
        xSemaphoreTake(this->sender_task_done_, 0) != pdTRUE) {
      return false;
    }
    this->sender_task_done_observed_ = true;
  }
  voip_audio_core::cleanup_managed_pinned_task(
      &this->sender_task_handle_, &this->sender_task_stack_,
      kSenderTaskStackBytes, this->sender_task_with_caps_);
  if (this->sender_task_handle_ == nullptr) {
    this->sender_task_done_observed_ = false;
    return true;
  }
  return false;
}

bool VideoRtpSession::stop_sender_task_(TickType_t stop_started,
                                        TickType_t stop_budget) {
  const bool was_running =
      this->sender_running_.exchange(false, std::memory_order_acq_rel);
  if (was_running && this->sender_task_handle_ != nullptr)
    xTaskNotifyGive(this->sender_task_handle_);
  const TickType_t elapsed = xTaskGetTickCount() - stop_started;
  const TickType_t remaining =
      elapsed < stop_budget ? stop_budget - elapsed : 0;
  if (!this->reap_sender_task_() && !this->sender_task_done_observed_ &&
      this->sender_task_done_ != nullptr &&
      remaining > 0 &&
      xSemaphoreTake(this->sender_task_done_, remaining) == pdTRUE) {
    this->sender_task_done_observed_ = true;
    this->reap_sender_task_();
  }
  const bool stopped = this->sender_task_handle_ == nullptr;
  if (stopped) {
    for (auto &slot : this->tx_access_units_)
      slot.state.store(0, std::memory_order_release);
  }
  return stopped;
}

void VideoRtpSession::quiesce_tasks_() {
  this->sender_running_.store(false, std::memory_order_release);
  this->terminate_.store(true, std::memory_order_release);
  if (this->sender_task_handle_ != nullptr)
    xTaskNotifyGive(this->sender_task_handle_);
  this->wake_task_();

  if (this->sender_task_handle_ != nullptr &&
      !this->sender_task_done_observed_ &&
      this->sender_task_done_ != nullptr) {
    xSemaphoreTake(this->sender_task_done_, portMAX_DELAY);
    this->sender_task_done_observed_ = true;
  }
  while (this->sender_task_handle_ != nullptr) {
    this->reap_sender_task_();
    if (this->sender_task_handle_ != nullptr) taskYIELD();
  }

  if (this->task_handle_ != nullptr && !this->task_done_observed_ &&
      this->task_done_ != nullptr) {
    xSemaphoreTake(this->task_done_, portMAX_DELAY);
    this->task_done_observed_ = true;
  }
  while (this->task_handle_ != nullptr) {
    this->reap_receive_task_();
    if (this->task_handle_ != nullptr) taskYIELD();
  }
}

void VideoRtpSession::sender_task_trampoline_(void *ctx) {
  static_cast<VideoRtpSession *>(ctx)->sender_task_();
}

void VideoRtpSession::sender_task_() {
  while (this->sender_running_.load(std::memory_order_acquire)) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (!this->sender_running_.load(std::memory_order_acquire)) break;
    while (this->sender_running_.load(std::memory_order_acquire)) {
      auto &slot = this->tx_access_units_[this->tx_read_slot_];
      uint8_t expected = 1;
      if (!slot.state.compare_exchange_strong(
              expected, 2, std::memory_order_acq_rel)) {
        break;
      }
      const EncodedVideoAccessUnit access_unit{
          slot.data, slot.size, slot.timestamp, slot.key_frame};
      this->send_access_unit_(access_unit);
      slot.state.store(0, std::memory_order_release);
      this->tx_read_slot_ =
          static_cast<uint8_t>((this->tx_read_slot_ + 1) %
                               kTxAccessUnitSlots);
    }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
    if (this->tx_resync_needed_.load(std::memory_order_acquire) &&
        this->source_ != nullptr) {
      LockGuard source_lock(this->source_control_mutex_);
      this->source_->request_key_frame();
    }
#endif
  }
  voip_audio_core::finish_managed_pinned_task(this->sender_task_done_);
}

void VideoRtpSession::send_access_unit_(
    const EncodedVideoAccessUnit &access_unit) {
  if (!this->running_.load(std::memory_order_acquire) || !this->send_enabled_ ||
      access_unit.data == nullptr || access_unit.size == 0) {
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_HOSTED_AUDIO_PACING
    this->reset_audio_pacing_burst_();
#endif
    return;
  }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_DEBUG
  const uint32_t started_ms = millis();
#endif
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  this->send_jpeg_access_unit_(access_unit);
#else
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_HOSTED_AUDIO_PACING
  if (!this->wait_for_audio_pacing_()) return;
#endif
  this->send_h264_access_unit_(access_unit);
#endif
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_DEBUG
  const uint32_t elapsed_ms = millis() - started_ms;
  this->tx_max_access_unit_ms_ =
      std::max(this->tx_max_access_unit_ms_, elapsed_ms);
  const uint32_t now = millis();
  if (now - this->tx_last_debug_log_ms_ >= 5000U) {
    this->tx_last_debug_log_ms_ = now;
    ESP_LOGI(TAG,
             "Video TX: au=%u packets=%u dropped=%u backpressure=%u "
             "send_fail=%u slow_send=%u max_send_us=%u "
             "last_ms=%u max_ms=%u",
             (unsigned) this->tx_access_units_ok_.load(
                 std::memory_order_acquire),
             (unsigned) this->tx_packets_.load(std::memory_order_acquire),
             (unsigned) this->dropped_access_units_.load(
                 std::memory_order_acquire),
             (unsigned) this->tx_backpressure_events_,
             (unsigned) this->tx_send_failures_,
             (unsigned) this->tx_slow_send_calls_,
             (unsigned) this->tx_max_send_us_, (unsigned) elapsed_ms,
             (unsigned) this->tx_max_access_unit_ms_);
  }
#endif
}

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
void VideoRtpSession::send_h264_access_unit_(
    const EncodedVideoAccessUnit &access_unit) {
  const auto fail_access_unit = [this]() {
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_HOSTED_AUDIO_PACING
    this->reset_audio_pacing_burst_();
#endif
    this->tx_resync_needed_.store(true, std::memory_order_release);
    this->dropped_access_units_.fetch_add(1, std::memory_order_acq_rel);
  };
  size_t cursor = 0;
  const uint8_t *nal = nullptr;
  size_t nal_size = 0;
  bool sent_any = false;
  while (next_annex_b_nal(access_unit.data, access_unit.size, &cursor, &nal,
                          &nal_size)) {
    const size_t next_cursor = cursor;
    const uint8_t *next_nal = nullptr;
    size_t next_size = 0;
    const bool last = !next_annex_b_nal(access_unit.data, access_unit.size,
                                        &cursor, &next_nal, &next_size);
    cursor = next_cursor;
    if (nal_size <= this->max_payload_) {
      if (!this->send_rtp_payload_(nal, nal_size, last,
                                   access_unit.timestamp_90khz)) {
        fail_access_unit();
        return;
      }
      sent_any = true;
      continue;
    }
    if (nal_size < 2 || this->max_payload_ <= 2) {
      fail_access_unit();
      return;
    }
    const uint8_t fu_indicator = static_cast<uint8_t>((nal[0] & 0xE0) | 28);
    const uint8_t nal_type = nal[0] & 0x1F;
    size_t offset = 1;
    bool first = true;
    while (offset < nal_size) {
      const size_t chunk =
          std::min(this->max_payload_ - 2, nal_size - offset);
      uint8_t packet[kMaxRtpPacketBytes];
      packet[0] = fu_indicator;
      packet[1] = static_cast<uint8_t>(nal_type | (first ? 0x80 : 0) |
                                       (offset + chunk == nal_size ? 0x40 : 0));
      memcpy(packet + 2, nal + offset, chunk);
      const bool marker = last && offset + chunk == nal_size;
      if (!this->send_rtp_payload_(packet, chunk + 2, marker,
                                   access_unit.timestamp_90khz)) {
        fail_access_unit();
        return;
      }
      sent_any = true;
      first = false;
      offset += chunk;
    }
  }
  if (!sent_any) {
    fail_access_unit();
  } else {
    this->tx_access_units_ok_.fetch_add(1, std::memory_order_acq_rel);
  }
}
#endif

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
void VideoRtpSession::send_jpeg_access_unit_(
    const EncodedVideoAccessUnit &access_unit) {
  RtpJpegFrameView frame;
  RtpJpegParseError parse_error = RtpJpegParseError::NONE;
  if (!parse_jpeg_for_rtp(access_unit.data, access_unit.size, &frame,
                          &parse_error)) {
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_HOSTED_AUDIO_PACING
    this->reset_audio_pacing_burst_();
#endif
    this->dropped_access_units_.fetch_add(1, std::memory_order_acq_rel);
    if (!this->jpeg_parse_warning_emitted_) {
      this->jpeg_parse_warning_emitted_ = true;
      ESP_LOGW(TAG,
               "Dropping JPEG outside RFC 2435 baseline subset: %s",
               rtp_jpeg_parse_error_name(parse_error));
    }
    return;
  }
  size_t scan_offset = 0;
  while (scan_offset < frame.scan_size) {
    uint8_t payload[kMaxRtpPacketBytes];
    const size_t header_size = build_rtp_jpeg_fragment_header(
        frame, scan_offset, payload,
        std::min(sizeof(payload), this->max_payload_));
    if (header_size == 0 || header_size >= this->max_payload_) {
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_HOSTED_AUDIO_PACING
      this->reset_audio_pacing_burst_();
#endif
      this->dropped_access_units_.fetch_add(1, std::memory_order_acq_rel);
      return;
    }
    const size_t fragment_size =
        std::min(this->max_payload_ - header_size,
                 frame.scan_size - scan_offset);
    memcpy(payload + header_size, frame.scan + scan_offset, fragment_size);
    const bool marker = scan_offset + fragment_size == frame.scan_size;
    if (!this->send_rtp_payload_(payload, header_size + fragment_size, marker,
                                 access_unit.timestamp_90khz)) {
      this->dropped_access_units_.fetch_add(1, std::memory_order_acq_rel);
      return;
    }
    scan_offset += fragment_size;
  }
  this->tx_access_units_ok_.fetch_add(1, std::memory_order_acq_rel);
}
#endif

bool VideoRtpSession::send_rtp_payload_(const uint8_t *payload, size_t size,
                                        bool marker, uint32_t timestamp) {
  const auto abort_payload = [this]() -> bool {
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_HOSTED_AUDIO_PACING
    this->reset_audio_pacing_burst_();
#endif
    return false;
  };
  if (payload == nullptr || size == 0 || size > this->max_payload_ ||
      !this->send_enabled_.load(std::memory_order_acquire)) {
    return abort_payload();
  }
#if defined(USE_ESPHOME_VOIP_STACK_VIDEO_HOSTED_AUDIO_PACING) && \
    defined(USE_ESPHOME_VOIP_STACK_VIDEO_JPEG)
  if (!this->wait_for_audio_pacing_()) return abort_payload();
#endif
  if (!this->send_enabled_.load(std::memory_order_acquire))
    return abort_payload();
  uint8_t packet[kMaxRtpPacketBytes];
  if (size + 12 > sizeof(packet)) return abort_payload();
  packet[0] = 0x80;
  packet[1] = static_cast<uint8_t>(this->capability_.payload_type |
                                   (marker ? 0x80 : 0));
  const uint16_t sequence =
      this->tx_sequence_.fetch_add(1, std::memory_order_acq_rel);
  write_be16(packet + 2, sequence);
  const uint32_t wire_timestamp = timestamp + this->tx_timestamp_offset_;
  write_be32(packet + 4, wire_timestamp);
  write_be32(packet + 8, this->tx_ssrc_);
  memcpy(packet + 12, payload, size);
  const uint32_t ip = this->remote_ip_v4_.load(std::memory_order_acquire);
  const uint16_t latched_port =
      this->latched_remote_rtp_port_.load(std::memory_order_acquire);
  const uint16_t port =
      latched_port != 0
          ? latched_port
          : this->remote_rtp_port_.load(std::memory_order_acquire);
  struct sockaddr_in remote{};
  remote.sin_family = AF_INET;
  remote.sin_addr.s_addr = htonl(ip);
  remote.sin_port = htons(port);
  int sent = -1;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_DEBUG
  const uint32_t send_started_us = micros();
#endif
  {
    LockGuard lock(this->socket_mutex_);
    if (!this->running_.load(std::memory_order_acquire) ||
        !this->send_enabled_.load(std::memory_order_acquire) ||
        this->rtp_socket_ < 0) {
      return abort_payload();
    }
    for (uint8_t attempt = 0; attempt < 2; attempt++) {
      sent = sendto(this->rtp_socket_, packet, size + 12, 0,
                    reinterpret_cast<struct sockaddr *>(&remote),
                    sizeof(remote));
      if (sent > 0) break;
      const int error = errno;
      if (error != EAGAIN && error != EWOULDBLOCK && error != ENOBUFS &&
          error != ENOMEM) {
        break;
      }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_DEBUG
      this->tx_backpressure_events_++;
#endif
      if (attempt + 1 >= 2) break;
      // Wait for writability only while an active access unit still has
      // realtime value. Idle calls stay blocked on the sender notification.
      fd_set writefds;
      FD_ZERO(&writefds);
      FD_SET(this->rtp_socket_, &writefds);
      struct timeval timeout{
          0, static_cast<suseconds_t>(kTxBackpressureWaitMs * 1000U)};
      const int ready =
          select(this->rtp_socket_ + 1, nullptr, &writefds, nullptr, &timeout);
      if (ready < 0 && errno != EINTR) break;
    }
  }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_DEBUG
  const uint32_t send_elapsed_us = micros() - send_started_us;
  this->tx_max_send_us_ =
      std::max(this->tx_max_send_us_, send_elapsed_us);
  if (send_elapsed_us >= 5000U) this->tx_slow_send_calls_++;
#endif
  if (sent <= 0) {
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_DEBUG
    this->tx_send_failures_++;
#endif
    return abort_payload();
  }
  this->tx_packets_.fetch_add(1, std::memory_order_acq_rel);
  this->tx_octets_.fetch_add(size, std::memory_order_acq_rel);
  this->last_tx_rtp_timestamp_.store(wire_timestamp,
                                     std::memory_order_release);
  this->last_tx_rtp_ms_.store(millis(), std::memory_order_release);
  return true;
}

void VideoRtpSession::task_trampoline_(void *ctx) {
  static_cast<VideoRtpSession *>(ctx)->task_();
}

void VideoRtpSession::task_() {
  uint8_t packet[kMaxRtpPacketBytes];
  bool worker_failed = false;
  while (!this->terminate_.load(std::memory_order_acquire)) {
    fd_set readfds;
    FD_ZERO(&readfds);
    int maxfd = -1;
    if (this->rtp_socket_ >= 0) {
      FD_SET(this->rtp_socket_, &readfds);
      maxfd = this->rtp_socket_;
    }
    if (this->rtcp_socket_ >= 0) {
      FD_SET(this->rtcp_socket_, &readfds);
      maxfd = std::max(maxfd, this->rtcp_socket_);
    }
    if (this->wake_socket_ >= 0) {
      FD_SET(this->wake_socket_, &readfds);
      maxfd = std::max(maxfd, this->wake_socket_);
    }
    // RTCP reports are the only periodic work. RTP arrival wakes select()
    // immediately; an idle call does not poll ten times per second.
    const uint32_t elapsed = millis() - this->last_rtcp_ms_;
    const uint32_t wait_ms = elapsed >= 5000 ? 0 : 5000 - elapsed;
    struct timeval timeout{static_cast<time_t>(wait_ms / 1000),
                           static_cast<suseconds_t>((wait_ms % 1000) * 1000)};
    if (maxfd < 0) {
      worker_failed = true;
      break;
    }
    const int ready =
        select(maxfd + 1, &readfds, nullptr, nullptr, &timeout);
    if (ready < 0) {
      if (errno == EINTR) continue;
      ESP_LOGW(TAG, "Video RTP select failed: %d", errno);
      worker_failed = true;
      break;
    }
    if (ready > 0 && this->wake_socket_ >= 0 &&
        FD_ISSET(this->wake_socket_, &readfds)) {
      uint8_t wake_bytes[16];
      while (recv(this->wake_socket_, wake_bytes, sizeof(wake_bytes), 0) > 0) {
      }
    }
    if (!this->running_.load(std::memory_order_acquire) ||
        this->terminate_.load(std::memory_order_acquire)) {
      break;
    }
    if (this->rx_reset_requested_.exchange(false,
                                           std::memory_order_acq_rel)) {
      this->reset_reassembly_();
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
      this->jpeg_depacketizer_.reset_session();
#endif
      this->sequence_valid_ = false;
    }
    if (ready > 0 && this->rtp_socket_ >= 0 &&
        FD_ISSET(this->rtp_socket_, &readfds)) {
      int received = 0;
      size_t batch = 0;
      while (batch++ < kMaxReceiveBatchPackets) {
        struct sockaddr_in source{};
        socklen_t source_size = sizeof(source);
        received = recvfrom(
            this->rtp_socket_, packet, sizeof(packet), 0,
            reinterpret_cast<struct sockaddr *>(&source), &source_size);
        if (received <= 0) break;
        if (received < 12 || (packet[0] >> 6) != 2 ||
            (packet[1] & 0x7F) != this->capability_.payload_type) {
          continue;
        }
        const uint32_t source_ip = ntohl(source.sin_addr.s_addr);
        const uint16_t source_port = ntohs(source.sin_port);
        if (source_ip !=
            this->remote_ip_v4_.load(std::memory_order_acquire)) {
          continue;
        }
        const uint32_t source_ssrc = read_be32(packet + 8);
        const bool ssrc_latched =
            this->remote_ssrc_latched_.load(std::memory_order_acquire);
        bool source_changed = false;
        if (ssrc_latched &&
            source_ssrc !=
                this->remote_ssrc_.load(std::memory_order_acquire)) {
          // A browser/media process restart may replace its SSRC without
          // renegotiating SDP. Only the already-latched RTP tuple may do so.
          if (source_port !=
              this->latched_remote_rtp_port_.load(
                  std::memory_order_acquire)) {
            continue;
          }
          this->reset_reassembly_();
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
          this->jpeg_depacketizer_.reset_session();
#endif
          this->sequence_valid_ = false;
          source_changed = true;
        }
        if (!this->handle_rtp_packet_(packet,
                                      static_cast<size_t>(received))) {
          continue;
        }
        if (!ssrc_latched || source_changed) {
          const uint32_t previous_ssrc =
              this->remote_ssrc_.load(std::memory_order_acquire);
          this->remote_ssrc_.store(source_ssrc, std::memory_order_release);
          this->remote_ssrc_latched_.store(true, std::memory_order_release);
          if (source_changed) {
            ESP_LOGI(TAG,
                     "Video RTP source changed SSRC %08" PRIx32
                     " -> %08" PRIx32,
                     previous_ssrc, source_ssrc);
            if (this->capability_.is_h264()) this->send_rtcp_pli_();
          }
        }
        // Mirror the audio RTP symmetric-latch policy: the negotiated address
        // remains authoritative, while a valid same-SSRC NAT port rebind is
        // followed for subsequent outgoing RTP.
        this->latched_remote_rtp_port_.store(source_port,
                                             std::memory_order_release);
      }
    }
    if (ready > 0 && this->rtcp_socket_ >= 0 &&
        FD_ISSET(this->rtcp_socket_, &readfds)) {
      struct sockaddr_in source{};
      socklen_t source_size = sizeof(source);
      const int received = recvfrom(
          this->rtcp_socket_, packet, sizeof(packet), 0,
          reinterpret_cast<struct sockaddr *>(&source), &source_size);
      const uint32_t source_ip = ntohl(source.sin_addr.s_addr);
      bool request_key_frame = false;
      if (received > 0 &&
          source_ip ==
              this->remote_rtcp_ip_v4_.load(std::memory_order_acquire) &&
          this->handle_rtcp_packet_(
              packet, static_cast<size_t>(received), &request_key_frame)) {
        this->latched_remote_rtcp_port_.store(
            ntohs(source.sin_port), std::memory_order_release);
        if (request_key_frame && this->source_ != nullptr) {
          LockGuard source_lock(this->source_control_mutex_);
          this->source_->request_key_frame();
        }
      }
    }
    if (millis() - this->last_rtcp_ms_ >= 5000) {
      this->send_rtcp_report_();
      this->last_rtcp_ms_ = millis();
    }
  }
  if (worker_failed &&
      !this->terminate_.load(std::memory_order_acquire)) {
    ESP_LOGE(TAG, "Video RTP worker failed; quiescing video media");
    if (this->running_.exchange(false, std::memory_order_acq_rel)) {
      this->stop_had_active_session_.store(true,
                                           std::memory_order_release);
      this->rtcp_bye_requested_.store(true, std::memory_order_release);
    }
  }
  // The receive worker quiesces media callbacks before publishing completion.
  // The lifecycle owner joins this worker and only then closes the child
  // renderer task. Keeping that nested join out of this worker avoids a
  // timeout inversion when decoder teardown legitimately takes longer than
  // the RTP-worker join deadline. This matches Espressif's media teardown
  // order: stop workers first, then close decoder/render resources.
  {
    LockGuard direction_lock(this->direction_mutex_);
    this->send_enabled_.store(false, std::memory_order_release);
    this->receive_enabled_.store(false, std::memory_order_release);
    this->sender_running_.store(false, std::memory_order_release);
    if (this->sender_task_handle_ != nullptr)
      xTaskNotifyGive(this->sender_task_handle_);
    if (this->rtcp_bye_requested_.exchange(false,
                                           std::memory_order_acq_rel)) {
      this->send_rtcp_bye_();
    }
    if (this->source_started_.exchange(false,
                                       std::memory_order_acq_rel) &&
        this->source_ != nullptr) {
      LockGuard source_lock(this->source_control_mutex_);
      this->source_->stop_video();
    }
    if (this->sink_started_.load(std::memory_order_acquire) &&
        this->sink_ != nullptr) {
      this->sink_->set_video_active(false);
      // A spontaneous socket/select failure has no lifecycle caller waiting
      // to finish subordinate media, so this worker remains the fallback
      // cleanup owner for that exceptional path only.
      if (worker_failed &&
          this->sink_started_.exchange(false, std::memory_order_acq_rel)) {
        this->sink_->stop_video();
      }
    }
    this->send_prepared_.store(false, std::memory_order_release);
    this->receive_prepared_.store(false, std::memory_order_release);
  }
  this->close_sockets_();
  voip_audio_core::finish_managed_pinned_task(this->task_done_);
}

bool VideoRtpSession::handle_rtp_packet_(const uint8_t *packet, size_t size) {
  if (!this->receive_enabled_ || packet == nullptr || size < 12 ||
      (packet[0] >> 6) != 2) {
    return false;
  }
  const bool padding = (packet[0] & 0x20) != 0;
  if (padding) {
    const size_t padding_bytes = packet[size - 1];
    if (padding_bytes == 0 || padding_bytes > size - 12) return false;
    size -= padding_bytes;
  }
  const size_t csrc_bytes = (packet[0] & 0x0F) * 4;
  const bool extension = (packet[0] & 0x10) != 0;
  size_t offset = 12 + csrc_bytes;
  if (offset > size) return false;
  if (extension) {
    if (offset + 4 > size) return false;
    const size_t words =
        (static_cast<size_t>(packet[offset + 2]) << 8) | packet[offset + 3];
    offset += 4 + words * 4;
    if (offset > size) return false;
  }
  const uint8_t payload_type = packet[1] & 0x7F;
  if (payload_type != this->capability_.payload_type || offset >= size)
    return false;
  const bool marker = (packet[1] & 0x80) != 0;
  const uint16_t sequence =
      static_cast<uint16_t>((packet[2] << 8) | packet[3]);
  const uint32_t timestamp = read_be32(packet + 4);
  const bool sequence_gap =
      this->sequence_valid_ && sequence != this->expected_sequence_;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  if (sequence_gap) {
    this->reassembly_damaged_ = true;
  }
#endif

  const uint8_t *payload = packet + offset;
  const size_t payload_size = size - offset;
  bool payload_accepted = false;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  payload_accepted =
      this->handle_jpeg_payload_(payload, payload_size, marker, timestamp);
#else
  payload_accepted =
      this->handle_h264_payload_(payload, payload_size, marker, timestamp,
                                 sequence_gap);
#endif
  if (!payload_accepted) {
    // A packet with only a valid RTP envelope is not enough to establish the
    // symmetric RTP peer. In particular, a malformed first packet must not
    // latch an attacker-controlled SSRC/port and exclude the real stream.
    this->reset_reassembly_();
    return false;
  }
  this->expected_sequence_ = static_cast<uint16_t>(sequence + 1);
  this->sequence_valid_ = true;
  this->rx_packets_.fetch_add(1, std::memory_order_acq_rel);
  return true;
}

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
bool VideoRtpSession::handle_h264_payload_(const uint8_t *payload,
                                           size_t payload_size, bool marker,
                                           uint32_t timestamp,
                                           bool sequence_gap) {
  if (payload == nullptr || payload_size == 0) return false;
  const uint8_t nal_type = payload[0] & 0x1F;
  if (nal_type == 24) {
    // Validate the complete STAP-A before mutating reassembly state. A
    // truncated aggregate is not a valid media packet and cannot latch the
    // symmetric RTP source.
    size_t pos = 1;
    bool saw_nal = false;
    while (pos + 2 <= payload_size) {
      const size_t nal_size =
          (static_cast<size_t>(payload[pos]) << 8) | payload[pos + 1];
      pos += 2;
      if (nal_size == 0 || pos + nal_size > payload_size) return false;
      const uint8_t aggregated_type = payload[pos] & 0x1F;
      if (aggregated_type == 0 || aggregated_type > 23) return false;
      saw_nal = true;
      pos += nal_size;
    }
    if (!saw_nal || pos != payload_size) return false;
  } else if (nal_type == 28) {
    // RFC 6184 permits an empty FU payload, and receivers must ignore the
    // reserved bit even though conforming senders set it to zero.
    if (payload_size < 2) return false;
    const bool start = (payload[1] & 0x80) != 0;
    const bool end = (payload[1] & 0x40) != 0;
    const uint8_t fragment_type = payload[1] & 0x1F;
    if ((start && end) || fragment_type == 0 || fragment_type > 23)
      return false;
    if (!start &&
        (!this->reassembly_active_ ||
         timestamp != this->reassembly_timestamp_ || !this->fu_active_ ||
         this->fu_nal_type_ != fragment_type)) {
      return false;
    }
  } else if (nal_type == 0 || nal_type > 23) {
    return false;
  }

  if (!this->reassembly_active_ || timestamp != this->reassembly_timestamp_) {
    if (this->reassembly_active_) this->reset_reassembly_();
    this->reassembly_active_ = true;
    this->reassembly_timestamp_ = timestamp;
    this->reassembly_damaged_ = sequence_gap;
  } else if (sequence_gap) {
    this->reassembly_damaged_ = true;
  }

  if (nal_type >= 1 && nal_type <= 23) {
    if (this->fu_active_) this->reassembly_damaged_ = true;
    if (!this->append_annex_b_nal_(payload, payload_size))
      this->reassembly_damaged_ = true;
  } else if (nal_type == 24) {
    if (this->fu_active_) this->reassembly_damaged_ = true;
    size_t pos = 1;
    while (pos + 2 <= payload_size) {
      const size_t nal_size =
          (static_cast<size_t>(payload[pos]) << 8) | payload[pos + 1];
      pos += 2;
      if (nal_size == 0 || pos + nal_size > payload_size ||
          !this->append_annex_b_nal_(payload + pos, nal_size)) {
        this->reassembly_damaged_ = true;
        break;
      }
      pos += nal_size;
    }
    if (pos != payload_size) this->reassembly_damaged_ = true;
  } else if (nal_type == 28) {
    const bool start = (payload[1] & 0x80) != 0;
    const bool end = (payload[1] & 0x40) != 0;
    const uint8_t fragment_type = payload[1] & 0x1F;
    const uint8_t reconstructed =
        static_cast<uint8_t>((payload[0] & 0xE0) | fragment_type);
    if (start) {
      if (this->fu_active_) this->reassembly_damaged_ = true;
      this->fu_active_ = true;
      this->fu_nal_type_ = fragment_type;
      uint8_t prefix_and_header[5]{0, 0, 0, 1, reconstructed};
      if (this->reassembly_size_ + sizeof(prefix_and_header) >
          kMaxAccessUnitBytes) {
        this->reassembly_damaged_ = true;
      } else {
        memcpy(this->reassembly_ + this->reassembly_size_, prefix_and_header,
               sizeof(prefix_and_header));
        this->reassembly_size_ += sizeof(prefix_and_header);
      }
    }
    if (!this->reassembly_damaged_) {
      const size_t fragment = payload_size - 2;
      if (this->reassembly_size_ + fragment > kMaxAccessUnitBytes) {
        this->reassembly_damaged_ = true;
      } else {
        memcpy(this->reassembly_ + this->reassembly_size_, payload + 2,
               fragment);
        this->reassembly_size_ += fragment;
      }
    }
    if (end) {
      if (!this->fu_active_) this->reassembly_damaged_ = true;
      this->fu_active_ = false;
      this->fu_nal_type_ = 0;
    }
  }
  bool accepted = true;
  if (marker) {
    if (this->fu_active_) {
      this->reassembly_damaged_ = true;
      accepted = false;
    }
    this->finish_access_unit_(timestamp);
  }
  return accepted;
}
#endif

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
bool VideoRtpSession::handle_jpeg_payload_(const uint8_t *payload,
                                           size_t payload_size, bool marker,
                                           uint32_t timestamp) {
  if (payload_size >= 8 && payload[1] == 0 && payload[2] == 0 &&
      payload[3] == 0 &&
      payload[5] >= RtpJpegDepacketizer::kFirstCachedQuality &&
      payload[5] <= RtpJpegDepacketizer::kLastCachedQuality &&
      !this->ensure_jpeg_quantization_cache_()) {
    this->jpeg_depacketizer_.reset();
    this->reassembly_damaged_ = false;
    this->dropped_access_units_.fetch_add(1, std::memory_order_acq_rel);
    return false;
  }
  if (payload_size >= 4 && payload[1] == 0 && payload[2] == 0 &&
      payload[3] == 0) {
    // Every JPEG is independently decodable. A sequence gap before a fresh
    // fragment-offset zero must not poison the new frame.
    this->reassembly_damaged_ = false;
  }
  size_t access_unit_size = 0;
  const RtpJpegPushResult result = this->jpeg_depacketizer_.push(
      payload, payload_size, marker, timestamp, this->reassembly_,
      kMaxAccessUnitBytes, &access_unit_size);
  if (result == RtpJpegPushResult::DROPPED) {
    this->dropped_access_units_.fetch_add(1, std::memory_order_acq_rel);
    this->reassembly_damaged_ = false;
    return false;
  }
  if (result == RtpJpegPushResult::INCOMPLETE) return true;
  if (this->reassembly_damaged_ || access_unit_size == 0) {
    this->dropped_access_units_.fetch_add(1, std::memory_order_acq_rel);
    this->reassembly_damaged_ = false;
    return true;
  }
  if (this->sink_ != nullptr) {
    const EncodedVideoAccessUnit access_unit{
        this->reassembly_, access_unit_size, timestamp, true};
    if (!this->sink_->consume_video_access_unit(access_unit)) {
      this->dropped_access_units_.fetch_add(1, std::memory_order_acq_rel);
    } else {
      this->rx_access_units_ok_.fetch_add(1, std::memory_order_acq_rel);
    }
  }
  this->reassembly_damaged_ = false;
  return true;
}

bool VideoRtpSession::ensure_jpeg_quantization_cache_() {
  if (this->jpeg_quantization_cache_ != nullptr) return true;
  if (this->jpeg_quantization_cache_allocation_failed_) return false;
  this->jpeg_quantization_cache_ = static_cast<uint8_t *>(heap_caps_malloc(
      RtpJpegDepacketizer::kQuantizationCacheBytes,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->jpeg_quantization_cache_ == nullptr) {
    this->jpeg_quantization_cache_allocation_failed_ = true;
    ESP_LOGE(TAG, "JPEG quantization cache PSRAM allocation failed");
    return false;
  }
  this->jpeg_depacketizer_.set_quantization_cache(
      this->jpeg_quantization_cache_,
      RtpJpegDepacketizer::kQuantizationCacheBytes);
  return true;
}
#endif

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
bool VideoRtpSession::append_annex_b_nal_(const uint8_t *nal, size_t size) {
  static constexpr uint8_t START_CODE[4]{0, 0, 0, 1};
  if (nal == nullptr || size == 0 ||
      this->reassembly_size_ + sizeof(START_CODE) + size >
          kMaxAccessUnitBytes) {
    return false;
  }
  memcpy(this->reassembly_ + this->reassembly_size_, START_CODE,
         sizeof(START_CODE));
  this->reassembly_size_ += sizeof(START_CODE);
  memcpy(this->reassembly_ + this->reassembly_size_, nal, size);
  this->reassembly_size_ += size;
  return true;
}

void VideoRtpSession::finish_access_unit_(uint32_t timestamp) {
  if (!this->reassembly_damaged_ && this->reassembly_size_ > 0 &&
      this->sink_ != nullptr) {
    bool key_frame = false;
    bool has_sps = false;
    bool has_pps = false;
    size_t cursor = 0;
    const uint8_t *nal = nullptr;
    size_t nal_size = 0;
    while (next_annex_b_nal(this->reassembly_, this->reassembly_size_, &cursor,
                            &nal, &nal_size)) {
      const uint8_t nal_type = nal[0] & 0x1F;
      key_frame = key_frame || nal_type == 5;
      if (nal_type == 7) {
        has_sps = true;
        if (nal_size <= kH264ParameterSetBytes) {
          memcpy(this->h264_sps_, nal, nal_size);
          this->h264_sps_size_ = nal_size;
        }
      } else if (nal_type == 8) {
        has_pps = true;
        if (nal_size <= kH264ParameterSetBytes) {
          memcpy(this->h264_pps_, nal, nal_size);
          this->h264_pps_size_ = nal_size;
        }
      }
    }
    const size_t sps_prefix =
        key_frame && !has_sps && this->h264_sps_size_ != 0
            ? 4 + this->h264_sps_size_
            : 0;
    const size_t pps_prefix =
        key_frame && !has_pps && this->h264_pps_size_ != 0
            ? 4 + this->h264_pps_size_
            : 0;
    const size_t parameter_prefix = sps_prefix + pps_prefix;
    if (parameter_prefix != 0) {
      if (this->reassembly_size_ + parameter_prefix >
          kMaxAccessUnitBytes) {
        this->dropped_access_units_.fetch_add(
            1, std::memory_order_acq_rel);
        this->send_rtcp_pli_();
        this->reset_reassembly_();
        return;
      }
      memmove(this->reassembly_ + parameter_prefix, this->reassembly_,
              this->reassembly_size_);
      size_t prefix_offset = 0;
      static constexpr uint8_t START_CODE[4]{0, 0, 0, 1};
      if (sps_prefix != 0) {
        memcpy(this->reassembly_ + prefix_offset, START_CODE,
               sizeof(START_CODE));
        prefix_offset += sizeof(START_CODE);
        memcpy(this->reassembly_ + prefix_offset, this->h264_sps_,
               this->h264_sps_size_);
        prefix_offset += this->h264_sps_size_;
      }
      if (pps_prefix != 0) {
        memcpy(this->reassembly_ + prefix_offset, START_CODE,
               sizeof(START_CODE));
        prefix_offset += sizeof(START_CODE);
        memcpy(this->reassembly_ + prefix_offset, this->h264_pps_,
               this->h264_pps_size_);
      }
      this->reassembly_size_ += parameter_prefix;
    }
    EncodedVideoAccessUnit access_unit{this->reassembly_,
                                       this->reassembly_size_, timestamp,
                                       key_frame};
    if (!this->sink_->consume_video_access_unit(access_unit)) {
      this->dropped_access_units_.fetch_add(1, std::memory_order_acq_rel);
      this->send_rtcp_pli_();
    } else {
      this->rx_access_units_ok_.fetch_add(1, std::memory_order_acq_rel);
    }
  } else {
    this->dropped_access_units_.fetch_add(1, std::memory_order_acq_rel);
    this->send_rtcp_pli_();
  }
  this->reset_reassembly_();
}

void VideoRtpSession::reset_h264_parameter_sets_() {
  this->h264_sps_size_ = 0;
  this->h264_pps_size_ = 0;
}
#endif

void VideoRtpSession::reset_reassembly_() {
  this->reassembly_size_ = 0;
  this->reassembly_timestamp_ = 0;
  this->reassembly_active_ = false;
  this->reassembly_damaged_ = false;
  this->fu_active_ = false;
  this->fu_nal_type_ = 0;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  this->jpeg_depacketizer_.reset();
#endif
}

bool VideoRtpSession::handle_rtcp_packet_(const uint8_t *packet, size_t size,
                                          bool *request_key_frame) {
  if (packet == nullptr || request_key_frame == nullptr || size < 8)
    return false;
  *request_key_frame = false;
  bool saw_report = false;
  bool saw_sdes = false;
  size_t offset = 0;
  while (offset < size) {
    if (size - offset < 4 || (packet[offset] >> 6) != 2) return false;
    const bool padding = (packet[offset] & 0x20) != 0;
    const uint8_t count = packet[offset] & 0x1F;
    const uint8_t type = packet[offset + 1];
    const size_t subpacket_size =
        (static_cast<size_t>(
             (static_cast<uint16_t>(packet[offset + 2]) << 8) |
             packet[offset + 3]) +
         1) *
        4;
    if (subpacket_size < 4 || subpacket_size > size - offset) return false;
    const bool final = offset + subpacket_size == size;
    if (padding && !final) return false;
    size_t content_size = subpacket_size;
    if (padding) {
      const size_t padding_size = packet[offset + subpacket_size - 1];
      if (padding_size == 0 || padding_size > subpacket_size - 4)
        return false;
      content_size -= padding_size;
    }
    const uint8_t *subpacket = packet + offset;
    if (offset == 0 && type != 200 && type != 201) return false;
    if (type == 200) {
      if (content_size < 28 + static_cast<size_t>(count) * 24)
        return false;
      saw_report = true;
    } else if (type == 201) {
      if (content_size < 8 + static_cast<size_t>(count) * 24)
        return false;
      saw_report = true;
    } else if (type == 202) {
      if (content_size < 8) return false;
      saw_sdes = true;
    } else if (type == 203) {
      if (content_size < 4 + static_cast<size_t>(count) * 4)
        return false;
    } else if (type == 206) {
      if (content_size < 12) return false;
      // Feedback is meaningful only when the SDP explicitly negotiated it.
      // RTP/AVP without rtcp-fb relies on periodic key frames instead.
      if ((count == 1 && this->accept_remote_pli_) ||
          (count == 4 && this->accept_remote_fir_)) {
        bool targets_local_stream =
            count == 1 && read_be32(subpacket + 8) == this->tx_ssrc_;
        if (count == 4) {
          for (size_t fir = 12; fir + 8 <= content_size; fir += 8) {
            if (read_be32(subpacket + fir) == this->tx_ssrc_) {
              targets_local_stream = true;
              break;
            }
          }
        }
        *request_key_frame |= targets_local_stream;
      }
    }
    offset += subpacket_size;
  }
  return offset == size && saw_report && saw_sdes;
}

size_t VideoRtpSession::build_rtcp_report_sdes_(uint8_t *packet,
                                                size_t capacity) const {
  if (packet == nullptr || capacity < 64) return 0;
  memset(packet, 0, capacity);
  const bool sender = this->tx_packets_.load(std::memory_order_acquire) > 0;
  packet[0] = 0x80;
  packet[1] = sender ? 200 : 201;
  write_be16(packet + 2, sender ? 6 : 1);
  write_be32(packet + 4, this->tx_ssrc_);
  size_t size = 8;
  if (sender) {
    struct timeval now{};
    gettimeofday(&now, nullptr);
    const uint64_t unix_seconds =
        now.tv_sec > 0 ? static_cast<uint64_t>(now.tv_sec) : 0;
    const uint64_t ntp_seconds = unix_seconds + 2208988800ULL;
    const uint64_t ntp_fraction =
        (static_cast<uint64_t>(now.tv_usec) << 32) / 1000000ULL;
    write_be32(packet + 8, static_cast<uint32_t>(ntp_seconds));
    write_be32(packet + 12, static_cast<uint32_t>(ntp_fraction));
    const uint32_t last_timestamp =
        this->last_tx_rtp_timestamp_.load(std::memory_order_acquire);
    const uint32_t last_timestamp_ms =
        this->last_tx_rtp_ms_.load(std::memory_order_acquire);
    const uint32_t elapsed_ms =
        last_timestamp_ms == 0 ? 0 : millis() - last_timestamp_ms;
    write_be32(packet + 16, last_timestamp + elapsed_ms * 90U);
    write_be32(packet + 20, this->tx_packets_.load());
    write_be32(packet + 24, this->tx_octets_.load());
    size = 28;
  }
  // RFC 3550 compound reports carry one SDES CNAME so the SSRC remains
  // identifiable even when the video child is receive-only.
  char cname[24];
  const int cname_length =
      snprintf(cname, sizeof(cname), "esp-video-%08x",
               static_cast<unsigned>(this->tx_ssrc_));
  if (cname_length <= 0 ||
      static_cast<size_t>(cname_length) >= sizeof(cname))
    return 0;
  const size_t sdes = size;
  packet[sdes] = 0x81;
  packet[sdes + 1] = 202;
  write_be32(packet + sdes + 4, this->tx_ssrc_);
  packet[sdes + 8] = 1;
  packet[sdes + 9] = static_cast<uint8_t>(cname_length);
  memcpy(packet + sdes + 10, cname, static_cast<size_t>(cname_length));
  packet[sdes + 10 + cname_length] = 0;
  size_t sdes_size = 11 + static_cast<size_t>(cname_length);
  while ((sdes_size & 3U) != 0) packet[sdes + sdes_size++] = 0;
  write_be16(packet + sdes + 2,
             static_cast<uint16_t>(sdes_size / 4 - 1));
  size += sdes_size;
  return size;
}

void VideoRtpSession::send_rtcp_packet_(const uint8_t *packet, size_t size) {
  if (packet == nullptr || size == 0) return;
  const uint32_t ip =
      this->remote_rtcp_ip_v4_.load(std::memory_order_acquire);
  const uint16_t latched_port =
      this->latched_remote_rtcp_port_.load(std::memory_order_acquire);
  const uint16_t port =
      latched_port != 0
          ? latched_port
          : this->remote_rtcp_port_.load(std::memory_order_acquire);
  if (ip == 0 || port == 0) return;
  struct sockaddr_in remote{};
  remote.sin_family = AF_INET;
  remote.sin_addr.s_addr = htonl(ip);
  remote.sin_port = htons(port);
  LockGuard lock(this->socket_mutex_);
  if (this->rtcp_socket_ >= 0)
    sendto(this->rtcp_socket_, packet, size, 0,
           reinterpret_cast<struct sockaddr *>(&remote), sizeof(remote));
}

void VideoRtpSession::send_rtcp_report_() {
  uint8_t packet[96]{};
  const size_t size =
      this->build_rtcp_report_sdes_(packet, sizeof(packet));
  this->send_rtcp_packet_(packet, size);
}

void VideoRtpSession::send_rtcp_pli_() {
  if (!this->send_remote_pli_ || !this->capability_.is_h264()) return;
  const uint32_t now = millis();
  const uint32_t media_ssrc =
      this->remote_ssrc_.load(std::memory_order_acquire);
  if (media_ssrc == 0 || now - this->last_pli_ms_ < 500) return;
  this->last_pli_ms_ = now;
  uint8_t packet[112]{};
  size_t size = this->build_rtcp_report_sdes_(packet, sizeof(packet));
  if (size == 0 || size + 12 > sizeof(packet)) return;
  packet[size] = 0x81;
  packet[size + 1] = 206;
  write_be16(packet + size + 2, 2);
  write_be32(packet + size + 4, this->tx_ssrc_);
  write_be32(packet + size + 8, media_ssrc);
  size += 12;
  this->send_rtcp_packet_(packet, size);
}

void VideoRtpSession::send_rtcp_bye_() {
  uint8_t packet[104]{};
  size_t size = this->build_rtcp_report_sdes_(packet, sizeof(packet));
  if (size == 0 || size + 8 > sizeof(packet)) return;
  packet[size] = 0x81;
  packet[size + 1] = 203;
  write_be16(packet + size + 2, 1);
  write_be32(packet + size + 4, this->tx_ssrc_);
  size += 8;
  this->send_rtcp_packet_(packet, size);
}

void VideoRtpSession::wake_task_() {
  // select() cannot observe a FreeRTOS task notification. Use the private
  // loopback socket as a self-pipe, matching SipTransport's proven wake path.
  // No synthetic packet enters the RTP socket and idle media never polls.
  LockGuard lock(this->socket_mutex_);
  if (this->wake_socket_ < 0 || this->wake_port_ == 0) return;
  constexpr uint8_t WAKE_BYTE = 1;
  struct sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  local.sin_port = htons(this->wake_port_);
  if (sendto(this->wake_socket_, &WAKE_BYTE, sizeof(WAKE_BYTE), 0,
             reinterpret_cast<struct sockaddr *>(&local), sizeof(local)) < 0) {
    // A failed self-pipe write must not leave teardown blocked in select().
    // The lifecycle owner closes and recreates this per-session socket.
    shutdown(this->wake_socket_, SHUT_RDWR);
  }
}

}  // namespace voip_stack
}  // namespace esphome

#endif  // USE_ESP32 && USE_ESPHOME_VOIP_STACK_VIDEO
