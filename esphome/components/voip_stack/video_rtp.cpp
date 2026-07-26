#include "video_rtp.h"

#if defined(USE_ESP32) && defined(USE_ESPHOME_VOIP_STACK_VIDEO)

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>

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

}  // namespace

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

VideoRtpSession::VideoRtpSession(bool task_stack_in_psram)
    : task_stack_in_psram_(task_stack_in_psram) {
  this->task_done_ = xSemaphoreCreateBinaryStatic(&this->task_done_storage_);
  this->sender_task_done_ =
      xSemaphoreCreateBinaryStatic(&this->sender_task_done_storage_);
}

VideoRtpSession::~VideoRtpSession() {
  this->stop();
  if (this->reassembly_ != nullptr) {
    heap_caps_free(this->reassembly_);
    this->reassembly_ = nullptr;
  }
  if (this->tx_access_unit_ != nullptr) {
    heap_caps_free(this->tx_access_unit_);
    this->tx_access_unit_ = nullptr;
  }
}

void VideoRtpSession::set_negotiated(const VideoCapability &capability,
                                     uint32_t remote_ip_v4,
                                     uint16_t remote_rtp_port,
                                     bool send_enabled,
                                     bool receive_enabled) {
  this->capability_ = capability;
  this->remote_ip_v4_.store(remote_ip_v4, std::memory_order_release);
  this->remote_rtp_port_.store(remote_rtp_port, std::memory_order_release);
  this->send_enabled_ = send_enabled;
  this->receive_enabled_ = receive_enabled;
}

bool VideoRtpSession::bind_socket_(int *fd, uint16_t port, const char *label) {
  *fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (*fd < 0) {
    ESP_LOGE(TAG, "%s socket failed: %d", label, errno);
    return false;
  }
  const int flags = fcntl(*fd, F_GETFL, 0);
  if (flags >= 0) fcntl(*fd, F_SETFL, flags | O_NONBLOCK);
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

bool VideoRtpSession::start() {
  if (this->running_.load(std::memory_order_acquire)) return true;
  if (!this->capability_.valid() || this->remote_ip_v4_.load() == 0 ||
      this->remote_rtp_port_.load() == 0) {
    ESP_LOGW(TAG, "Video media not started: incomplete negotiation");
    return false;
  }
  if (this->send_enabled_ && this->source_ == nullptr) return false;
  if (this->receive_enabled_ && this->sink_ == nullptr) return false;

  if (this->reassembly_ == nullptr && this->receive_enabled_) {
    this->reassembly_ = static_cast<uint8_t *>(
        heap_caps_malloc(kMaxAccessUnitBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (this->reassembly_ == nullptr) {
      ESP_LOGE(TAG, "Video reassembly PSRAM allocation failed");
      return false;
    }
  }
  if (this->tx_access_unit_ == nullptr && this->send_enabled_) {
    this->tx_access_unit_ = static_cast<uint8_t *>(
        heap_caps_malloc(kMaxAccessUnitBytes,
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (this->tx_access_unit_ == nullptr) {
      ESP_LOGE(TAG, "Video TX access-unit PSRAM allocation failed");
      return false;
    }
  }
  if (!this->bind_socket_(&this->rtp_socket_, this->local_rtp_port_, "video RTP") ||
      !this->bind_socket_(&this->rtcp_socket_, this->local_rtp_port_ + 1, "video RTCP")) {
    this->stop();
    return false;
  }
  this->tx_sequence_.store(static_cast<uint16_t>(esp_random()), std::memory_order_release);
  this->tx_ssrc_ = esp_random();
  this->sequence_valid_ = false;
  this->terminate_.store(false, std::memory_order_release);
  this->last_rtcp_ms_ = millis();
  this->running_.store(true, std::memory_order_release);
  xSemaphoreTake(this->task_done_, 0);
  if (!voip_audio_core::start_pinned_task(
          VideoRtpSession::task_trampoline_, "voip_video_rtp", kTaskStackBytes,
          this, kTaskPriority, 0, this->task_stack_in_psram_, TAG,
          &this->task_handle_, &this->task_tcb_, &this->task_stack_)) {
    this->running_.store(false, std::memory_order_release);
    this->stop();
    return false;
  }
  if (this->send_enabled_ && !this->start_sender_task_()) {
    this->stop();
    return false;
  }
  if (this->receive_enabled_ && !this->sink_->start_video(this->capability_)) {
    this->stop();
    return false;
  }
  if (this->send_enabled_ &&
      !this->source_->start_video(VideoRtpSession::source_callback_, this)) {
    this->stop();
    return false;
  }
  ESP_LOGI(TAG, "Video RTP started local=%u remote=%u PT=%u profile=%s dir=%s%s",
           (unsigned) this->local_rtp_port_,
           (unsigned) this->remote_rtp_port_.load(),
           (unsigned) this->capability_.payload_type,
           this->capability_.profile_level_id.c_str(),
           this->send_enabled_ ? "send" : "",
           this->receive_enabled_ ? "recv" : "");
  return true;
}

void VideoRtpSession::stop() {
  LockGuard stop_lock(this->stop_mutex_);
  const bool was_running = this->running_.exchange(false, std::memory_order_acq_rel);
  if (was_running && this->send_enabled_ && this->source_ != nullptr) {
    this->source_->stop_video();
  }
  this->stop_sender_task_();
  if (was_running) this->send_rtcp_bye_();
  this->terminate_.store(true, std::memory_order_release);
  this->wake_task_();
  if (this->task_handle_ != nullptr && this->task_done_ != nullptr &&
      xSemaphoreTake(this->task_done_, pdMS_TO_TICKS(1000)) == pdTRUE) {
    voip_audio_core::cleanup_pinned_task(&this->task_handle_, &this->task_stack_,
                                          kTaskStackBytes);
  }
  {
    LockGuard lock(this->socket_mutex_);
    if (this->rtp_socket_ >= 0) close(this->rtp_socket_);
    if (this->rtcp_socket_ >= 0) close(this->rtcp_socket_);
    this->rtp_socket_ = -1;
    this->rtcp_socket_ = -1;
  }
  if (was_running && this->receive_enabled_ && this->sink_ != nullptr) {
    this->sink_->stop_video();
  }
  this->reset_reassembly_();
}

void VideoRtpSession::source_callback_(void *ctx,
                                       const EncodedVideoAccessUnit &access_unit) {
  static_cast<VideoRtpSession *>(ctx)->queue_access_unit_(access_unit);
}

void VideoRtpSession::queue_access_unit_(
    const EncodedVideoAccessUnit &access_unit) {
  if (!this->running_.load(std::memory_order_acquire) ||
      !this->sender_running_.load(std::memory_order_acquire) ||
      !this->send_enabled_ || this->tx_access_unit_ == nullptr ||
      access_unit.data == nullptr || access_unit.size == 0 ||
      access_unit.size > kMaxAccessUnitBytes) {
    return;
  }
  if (this->tx_resync_needed_.load(std::memory_order_acquire) &&
      !access_unit.key_frame) {
    this->dropped_access_units_.fetch_add(1, std::memory_order_acq_rel);
    return;
  }
  uint8_t expected = 0;
  if (!this->tx_access_unit_state_.compare_exchange_strong(
          expected, 2, std::memory_order_acq_rel)) {
    this->tx_resync_needed_.store(true, std::memory_order_release);
    this->dropped_access_units_.fetch_add(1, std::memory_order_acq_rel);
    return;
  }
  memcpy(this->tx_access_unit_, access_unit.data, access_unit.size);
  this->tx_access_unit_size_ = access_unit.size;
  this->tx_access_unit_timestamp_ = access_unit.timestamp_90khz;
  this->tx_access_unit_key_frame_ = access_unit.key_frame;
  if (access_unit.key_frame)
    this->tx_resync_needed_.store(false, std::memory_order_release);
  this->tx_access_unit_state_.store(1, std::memory_order_release);
  if (this->sender_task_handle_ != nullptr)
    xTaskNotifyGive(this->sender_task_handle_);
}

bool VideoRtpSession::start_sender_task_() {
  this->tx_access_unit_state_.store(0, std::memory_order_release);
  this->tx_resync_needed_.store(false, std::memory_order_release);
  this->sender_running_.store(true, std::memory_order_release);
  xSemaphoreTake(this->sender_task_done_, 0);
  if (voip_audio_core::start_pinned_task(
          VideoRtpSession::sender_task_trampoline_, "voip_video_tx",
          kSenderTaskStackBytes, this, kTaskPriority, 0,
          this->task_stack_in_psram_, TAG, &this->sender_task_handle_,
          &this->sender_task_tcb_, &this->sender_task_stack_)) {
    return true;
  }
  this->sender_running_.store(false, std::memory_order_release);
  return false;
}

void VideoRtpSession::stop_sender_task_() {
  const bool was_running =
      this->sender_running_.exchange(false, std::memory_order_acq_rel);
  if (was_running && this->sender_task_handle_ != nullptr)
    xTaskNotifyGive(this->sender_task_handle_);
  if (this->sender_task_handle_ != nullptr &&
      this->sender_task_done_ != nullptr &&
      xSemaphoreTake(this->sender_task_done_, pdMS_TO_TICKS(1000)) ==
          pdTRUE) {
    voip_audio_core::cleanup_pinned_task(
        &this->sender_task_handle_, &this->sender_task_stack_,
        kSenderTaskStackBytes);
  }
  this->tx_access_unit_state_.store(0, std::memory_order_release);
}

void VideoRtpSession::sender_task_trampoline_(void *ctx) {
  static_cast<VideoRtpSession *>(ctx)->sender_task_();
}

void VideoRtpSession::sender_task_() {
  while (this->sender_running_.load(std::memory_order_acquire)) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (!this->sender_running_.load(std::memory_order_acquire)) break;
    uint8_t expected = 1;
    if (!this->tx_access_unit_state_.compare_exchange_strong(
            expected, 2, std::memory_order_acq_rel)) {
      continue;
    }
    const EncodedVideoAccessUnit access_unit{
        this->tx_access_unit_, this->tx_access_unit_size_,
        this->tx_access_unit_timestamp_, this->tx_access_unit_key_frame_};
    this->send_access_unit_(access_unit);
    this->tx_access_unit_state_.store(0, std::memory_order_release);
    if (this->tx_resync_needed_.load(std::memory_order_acquire) &&
        this->source_ != nullptr) {
      this->source_->request_key_frame();
    }
  }
  xSemaphoreGive(this->sender_task_done_);
  vTaskDelete(nullptr);
}

void VideoRtpSession::send_access_unit_(
    const EncodedVideoAccessUnit &access_unit) {
  if (!this->running_.load(std::memory_order_acquire) || !this->send_enabled_ ||
      access_unit.data == nullptr || access_unit.size == 0) {
    return;
  }
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
      sent_any |= this->send_rtp_payload_(nal, nal_size, last,
                                          access_unit.timestamp_90khz);
      continue;
    }
    if (nal_size < 2 || this->max_payload_ <= 2) {
      this->dropped_access_units_.fetch_add(1, std::memory_order_acq_rel);
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
        this->dropped_access_units_.fetch_add(1, std::memory_order_acq_rel);
        return;
      }
      sent_any = true;
      first = false;
      offset += chunk;
    }
  }
  if (!sent_any)
    this->dropped_access_units_.fetch_add(1, std::memory_order_acq_rel);
}

bool VideoRtpSession::send_rtp_payload_(const uint8_t *payload, size_t size,
                                        bool marker, uint32_t timestamp) {
  if (payload == nullptr || size == 0 || size > this->max_payload_) return false;
  uint8_t packet[kMaxRtpPacketBytes];
  if (size + 12 > sizeof(packet)) return false;
  packet[0] = 0x80;
  packet[1] = static_cast<uint8_t>(this->capability_.payload_type |
                                   (marker ? 0x80 : 0));
  const uint16_t sequence =
      this->tx_sequence_.fetch_add(1, std::memory_order_acq_rel);
  write_be16(packet + 2, sequence);
  write_be32(packet + 4, timestamp);
  write_be32(packet + 8, this->tx_ssrc_);
  memcpy(packet + 12, payload, size);
  const uint32_t ip = this->remote_ip_v4_.load(std::memory_order_acquire);
  const uint16_t port = this->remote_rtp_port_.load(std::memory_order_acquire);
  struct sockaddr_in remote{};
  remote.sin_family = AF_INET;
  remote.sin_addr.s_addr = htonl(ip);
  remote.sin_port = htons(port);
  LockGuard lock(this->socket_mutex_);
  if (!this->running_.load(std::memory_order_acquire) ||
      this->rtp_socket_ < 0) {
    return false;
  }
  const int sent =
      sendto(this->rtp_socket_, packet, size + 12, 0,
             reinterpret_cast<struct sockaddr *>(&remote), sizeof(remote));
  if (sent <= 0) return false;
  this->tx_packets_.fetch_add(1, std::memory_order_acq_rel);
  this->tx_octets_.fetch_add(size, std::memory_order_acq_rel);
  return true;
}

void VideoRtpSession::task_trampoline_(void *ctx) {
  static_cast<VideoRtpSession *>(ctx)->task_();
}

void VideoRtpSession::task_() {
  uint8_t packet[kMaxRtpPacketBytes];
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
    // RTCP reports are the only periodic work. RTP arrival wakes select()
    // immediately; an idle call does not poll ten times per second.
    const uint32_t elapsed = millis() - this->last_rtcp_ms_;
    const uint32_t wait_ms = elapsed >= 5000 ? 0 : 5000 - elapsed;
    struct timeval timeout{static_cast<time_t>(wait_ms / 1000),
                           static_cast<suseconds_t>((wait_ms % 1000) * 1000)};
    const int ready = maxfd >= 0
                          ? select(maxfd + 1, &readfds, nullptr, nullptr, &timeout)
                          : 0;
    if (ready > 0 && this->rtp_socket_ >= 0 &&
        FD_ISSET(this->rtp_socket_, &readfds)) {
      int received = 0;
      size_t batch = 0;
      while (batch++ < kMaxReceiveBatchPackets &&
             (received =
                  recv(this->rtp_socket_, packet, sizeof(packet), 0)) > 0) {
        this->handle_rtp_packet_(packet, static_cast<size_t>(received));
      }
    }
    if (ready > 0 && this->rtcp_socket_ >= 0 &&
        FD_ISSET(this->rtcp_socket_, &readfds)) {
      const int received = recv(this->rtcp_socket_, packet, sizeof(packet), 0);
      if (received >= 12 && packet[1] == 206) {
        const uint8_t feedback_type = packet[0] & 0x1F;
        if ((feedback_type == 1 || feedback_type == 4) &&
            this->source_ != nullptr) {
          this->source_->request_key_frame();
        }
      }
    }
    if (millis() - this->last_rtcp_ms_ >= 5000) {
      this->send_rtcp_report_();
      this->last_rtcp_ms_ = millis();
    }
  }
  xSemaphoreGive(this->task_done_);
  vTaskDelete(nullptr);
}

void VideoRtpSession::handle_rtp_packet_(const uint8_t *packet, size_t size) {
  if (!this->receive_enabled_ || packet == nullptr || size < 12 ||
      (packet[0] >> 6) != 2) {
    return;
  }
  const bool padding = (packet[0] & 0x20) != 0;
  if (padding) {
    const size_t padding_bytes = packet[size - 1];
    if (padding_bytes == 0 || padding_bytes > size - 12) return;
    size -= padding_bytes;
  }
  const size_t csrc_bytes = (packet[0] & 0x0F) * 4;
  const bool extension = (packet[0] & 0x10) != 0;
  size_t offset = 12 + csrc_bytes;
  if (offset > size) return;
  if (extension) {
    if (offset + 4 > size) return;
    const size_t words =
        (static_cast<size_t>(packet[offset + 2]) << 8) | packet[offset + 3];
    offset += 4 + words * 4;
    if (offset > size) return;
  }
  const uint8_t payload_type = packet[1] & 0x7F;
  if (payload_type != this->capability_.payload_type || offset >= size) return;
  const bool marker = (packet[1] & 0x80) != 0;
  const uint16_t sequence =
      static_cast<uint16_t>((packet[2] << 8) | packet[3]);
  const uint32_t timestamp = read_be32(packet + 4);
  this->remote_ssrc_.store(read_be32(packet + 8), std::memory_order_release);
  if (this->sequence_valid_ && sequence != this->expected_sequence_) {
    this->reassembly_damaged_ = true;
  }
  this->expected_sequence_ = static_cast<uint16_t>(sequence + 1);
  this->sequence_valid_ = true;
  this->rx_packets_.fetch_add(1, std::memory_order_acq_rel);

  const uint8_t *payload = packet + offset;
  const size_t payload_size = size - offset;
  const uint8_t nal_type = payload[0] & 0x1F;
  if (!this->reassembly_active_ || timestamp != this->reassembly_timestamp_) {
    if (this->reassembly_active_) this->reset_reassembly_();
    this->reassembly_active_ = true;
    this->reassembly_timestamp_ = timestamp;
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
  } else if (nal_type == 28 && payload_size >= 2) {
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
    } else if (!this->fu_active_ || this->fu_nal_type_ != fragment_type) {
      this->reassembly_damaged_ = true;
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
  } else {
    this->reassembly_damaged_ = true;
  }
  if (marker) {
    if (this->fu_active_) this->reassembly_damaged_ = true;
    this->finish_access_unit_(timestamp);
  }
}

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
    size_t cursor = 0;
    const uint8_t *nal = nullptr;
    size_t nal_size = 0;
    while (next_annex_b_nal(this->reassembly_, this->reassembly_size_, &cursor,
                            &nal, &nal_size)) {
      if ((nal[0] & 0x1F) == 5) {
        key_frame = true;
        break;
      }
    }
    EncodedVideoAccessUnit access_unit{this->reassembly_,
                                       this->reassembly_size_, timestamp,
                                       key_frame};
    if (!this->sink_->consume_video_access_unit(access_unit)) {
      this->dropped_access_units_.fetch_add(1, std::memory_order_acq_rel);
      this->send_rtcp_pli_();
    }
  } else {
    this->dropped_access_units_.fetch_add(1, std::memory_order_acq_rel);
    this->send_rtcp_pli_();
  }
  this->reset_reassembly_();
}

void VideoRtpSession::reset_reassembly_() {
  this->reassembly_size_ = 0;
  this->reassembly_timestamp_ = 0;
  this->reassembly_active_ = false;
  this->reassembly_damaged_ = false;
  this->fu_active_ = false;
  this->fu_nal_type_ = 0;
}

void VideoRtpSession::send_rtcp_report_() {
  uint8_t packet[64]{};
  const bool sender = this->tx_packets_.load(std::memory_order_acquire) > 0;
  packet[0] = 0x80;
  packet[1] = sender ? 200 : 201;
  write_be16(packet + 2, sender ? 6 : 1);
  write_be32(packet + 4, this->tx_ssrc_);
  size_t size = 8;
  if (sender) {
    const uint64_t unix_us = static_cast<uint64_t>(millis()) * 1000ULL;
    const uint64_t ntp_seconds = unix_us / 1000000ULL + 2208988800ULL;
    const uint64_t ntp_fraction =
        ((unix_us % 1000000ULL) << 32) / 1000000ULL;
    write_be32(packet + 8, static_cast<uint32_t>(ntp_seconds));
    write_be32(packet + 12, static_cast<uint32_t>(ntp_fraction));
    write_be32(packet + 16, 0);
    write_be32(packet + 20, this->tx_packets_.load());
    write_be32(packet + 24, this->tx_octets_.load());
    size = 28;
  }
  // RFC 3550 compound reports carry one SDES CNAME so the SSRC remains
  // identifiable even when the video child is receive-only.
  static constexpr char CNAME[] = "esp-video";
  const size_t sdes = size;
  packet[sdes] = 0x81;
  packet[sdes + 1] = 202;
  write_be32(packet + sdes + 4, this->tx_ssrc_);
  packet[sdes + 8] = 1;
  packet[sdes + 9] = sizeof(CNAME) - 1;
  memcpy(packet + sdes + 10, CNAME, sizeof(CNAME) - 1);
  packet[sdes + 10 + sizeof(CNAME) - 1] = 0;
  size_t sdes_size = 11 + sizeof(CNAME) - 1;
  while ((sdes_size & 3U) != 0) packet[sdes + sdes_size++] = 0;
  write_be16(packet + sdes + 2,
             static_cast<uint16_t>(sdes_size / 4 - 1));
  size += sdes_size;
  const uint32_t ip = this->remote_ip_v4_.load();
  const uint16_t port = this->remote_rtp_port_.load();
  if (ip == 0 || port == 0) return;
  struct sockaddr_in remote{};
  remote.sin_family = AF_INET;
  remote.sin_addr.s_addr = htonl(ip);
  remote.sin_port = htons(port + 1);
  LockGuard lock(this->socket_mutex_);
  if (this->rtcp_socket_ >= 0)
    sendto(this->rtcp_socket_, packet, size, 0,
           reinterpret_cast<struct sockaddr *>(&remote), sizeof(remote));
}

void VideoRtpSession::send_rtcp_pli_() {
  const uint32_t now = millis();
  const uint32_t media_ssrc =
      this->remote_ssrc_.load(std::memory_order_acquire);
  if (media_ssrc == 0 || now - this->last_pli_ms_ < 500) return;
  this->last_pli_ms_ = now;
  uint8_t packet[12]{0x81, 206, 0, 2};
  write_be32(packet + 4, this->tx_ssrc_);
  write_be32(packet + 8, media_ssrc);
  const uint32_t ip = this->remote_ip_v4_.load(std::memory_order_acquire);
  const uint16_t port =
      this->remote_rtp_port_.load(std::memory_order_acquire);
  if (ip == 0 || port == 0) return;
  struct sockaddr_in remote{};
  remote.sin_family = AF_INET;
  remote.sin_addr.s_addr = htonl(ip);
  remote.sin_port = htons(port + 1);
  LockGuard lock(this->socket_mutex_);
  if (this->running_.load(std::memory_order_acquire) &&
      this->rtcp_socket_ >= 0) {
    sendto(this->rtcp_socket_, packet, sizeof(packet), 0,
           reinterpret_cast<struct sockaddr *>(&remote), sizeof(remote));
  }
}

void VideoRtpSession::send_rtcp_bye_() {
  uint8_t packet[8]{0x81, 203, 0, 1};
  write_be32(packet + 4, this->tx_ssrc_);
  const uint32_t ip = this->remote_ip_v4_.load();
  const uint16_t port = this->remote_rtp_port_.load();
  if (ip == 0 || port == 0) return;
  struct sockaddr_in remote{};
  remote.sin_family = AF_INET;
  remote.sin_addr.s_addr = htonl(ip);
  remote.sin_port = htons(port + 1);
  LockGuard lock(this->socket_mutex_);
  if (this->rtcp_socket_ >= 0)
    sendto(this->rtcp_socket_, packet, sizeof(packet), 0,
           reinterpret_cast<struct sockaddr *>(&remote), sizeof(remote));
}

void VideoRtpSession::wake_task_() {
  // select() cannot observe a FreeRTOS task notification. Send one harmless
  // loopback datagram through the already-open RTP socket so teardown wakes
  // immediately without a polling timeout or an extra permanent socket.
  LockGuard lock(this->socket_mutex_);
  if (this->rtp_socket_ < 0) return;
  const uint8_t byte = 0;
  struct sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  local.sin_port = htons(this->local_rtp_port_);
  sendto(this->rtp_socket_, &byte, sizeof(byte), 0,
         reinterpret_cast<struct sockaddr *>(&local), sizeof(local));
}

}  // namespace voip_stack
}  // namespace esphome

#endif  // USE_ESP32 && USE_ESPHOME_VOIP_STACK_VIDEO
