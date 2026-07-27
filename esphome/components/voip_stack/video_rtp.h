#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_ESPHOME_VOIP_STACK_VIDEO)

#include "audio_core_task_utils.h"
#include "rtp_jpeg.h"
#include "video.h"

#include "esphome/core/helpers.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace esphome {
namespace voip_stack {

/// One RTP/RTCP video media child owned by the existing SIP dialog.
///
/// This object never changes call state and never sends SIP. The authoritative
/// SipTransport starts and stops it together with the negotiated media session.
class VideoRtpSession {
 public:
  static constexpr uint32_t kTaskStackBytes = 12288;
  static constexpr uint32_t kSenderTaskStackBytes = 8192;
  static constexpr uint8_t kTaskPriority = 7;
  static constexpr size_t kMaxAccessUnitBytes = 512 * 1024;
  static constexpr size_t kMaxRtpPacketBytes = 1500;
  static constexpr size_t kMaxReceiveBatchPackets = 32;
  // A frame packet burst must not monopolize a shallow embedded network
  // queue shared with realtime audio. This pacer runs only while a queued
  // access unit is actively being sent; the sender otherwise blocks forever
  // on its task notification.
  static constexpr uint32_t kTxPacketPacingMs = 3;
  static constexpr uint32_t kTxBackpressureWaitMs = 3;

  explicit VideoRtpSession(bool task_stack_in_psram);
  ~VideoRtpSession();

  void set_source(EncodedVideoSource *source) { this->source_ = source; }
  void set_sink(EncodedVideoSink *sink) { this->sink_ = sink; }
  void set_local_port(uint16_t port) { this->local_rtp_port_ = port; }
  void set_max_payload(size_t bytes) { this->max_payload_ = bytes; }
  bool set_negotiated(const VideoCapability &capability,
                      uint32_t remote_ip_v4, uint16_t remote_rtp_port,
                      uint32_t remote_rtcp_ip_v4, uint16_t remote_rtcp_port,
                      bool send_enabled, bool receive_enabled);

  bool start();
  /// Gate callbacks and wake both workers without waiting for their joins.
  /// The owning media lifecycle worker follows with stop().
  void request_stop();
  void stop();
  bool is_running() const { return this->running_.load(std::memory_order_acquire); }

  uint32_t tx_packets() const { return this->tx_packets_.load(std::memory_order_acquire); }
  uint32_t rx_packets() const { return this->rx_packets_.load(std::memory_order_acquire); }
  uint32_t dropped_access_units() const {
    return this->dropped_access_units_.load(std::memory_order_acquire);
  }

 protected:
  static void source_callback_(void *ctx, const EncodedVideoAccessUnit &access_unit);
  static void task_trampoline_(void *ctx);
  static void sender_task_trampoline_(void *ctx);
  void task_();
  void sender_task_();
  bool start_sender_task_();
  bool stop_sender_task_();
  bool stop_receive_task_();
  bool reap_sender_task_();
  bool reap_receive_task_();
  void quiesce_tasks_();
  void close_sockets_();
  void queue_access_unit_(const EncodedVideoAccessUnit &access_unit);
  void send_access_unit_(const EncodedVideoAccessUnit &access_unit);
  void send_h264_access_unit_(const EncodedVideoAccessUnit &access_unit);
  void send_jpeg_access_unit_(const EncodedVideoAccessUnit &access_unit);
  bool send_rtp_payload_(const uint8_t *payload, size_t size, bool marker,
                         uint32_t timestamp);
  bool handle_rtp_packet_(const uint8_t *packet, size_t size);
  bool handle_h264_payload_(const uint8_t *payload, size_t payload_size,
                            bool marker, uint32_t timestamp,
                            bool sequence_gap);
  bool handle_jpeg_payload_(const uint8_t *payload, size_t payload_size,
                            bool marker, uint32_t timestamp);
  bool ensure_jpeg_quantization_cache_();
  void reset_reassembly_();
  bool append_annex_b_nal_(const uint8_t *nal, size_t size);
  void finish_access_unit_(uint32_t timestamp);
  bool handle_rtcp_packet_(const uint8_t *packet, size_t size,
                           bool *request_key_frame);
  size_t build_rtcp_report_sdes_(uint8_t *packet, size_t capacity) const;
  void send_rtcp_packet_(const uint8_t *packet, size_t size);
  void send_rtcp_report_();
  void send_rtcp_pli_();
  void send_rtcp_bye_();
  bool bind_socket_(int *fd, uint16_t port, const char *label);
  void wake_task_();

  bool task_stack_in_psram_{false};
  EncodedVideoSource *source_{nullptr};
  EncodedVideoSink *sink_{nullptr};
  VideoCapability capability_{};
  uint16_t local_rtp_port_{40002};
  size_t max_payload_{1200};
  std::atomic<uint32_t> remote_ip_v4_{0};
  std::atomic<uint16_t> remote_rtp_port_{0};
  std::atomic<uint32_t> remote_rtcp_ip_v4_{0};
  std::atomic<uint16_t> remote_rtcp_port_{0};
  bool send_enabled_{false};
  bool receive_enabled_{false};
  std::atomic<bool> source_started_{false};
  std::atomic<bool> sink_started_{false};

  int rtp_socket_{-1};
  int rtcp_socket_{-1};
  // SIP teardown and the main-loop FSM can observe the same BYE nearly
  // simultaneously. Serialize the idempotent stop so only one caller consumes
  // the task completion semaphore; the follower then sees a fully stopped
  // child instead of blocking for the full timeout.
  Mutex stop_mutex_;
  mutable Mutex socket_mutex_;
  TaskHandle_t task_handle_{nullptr};
  StaticTask_t task_tcb_{};
  StackType_t *task_stack_{nullptr};
  bool task_with_caps_{false};
  SemaphoreHandle_t task_done_{nullptr};
  StaticSemaphore_t task_done_storage_{};
  bool task_done_observed_{false};
  TaskHandle_t sender_task_handle_{nullptr};
  StaticTask_t sender_task_tcb_{};
  StackType_t *sender_task_stack_{nullptr};
  bool sender_task_with_caps_{false};
  SemaphoreHandle_t sender_task_done_{nullptr};
  StaticSemaphore_t sender_task_done_storage_{};
  bool sender_task_done_observed_{false};
  std::atomic<bool> running_{false};
  std::atomic<bool> terminate_{false};
  std::atomic<bool> sender_running_{false};
  std::atomic<bool> stop_had_active_session_{false};

  // One bounded PSRAM access-unit slot decouples the hardware encoder from
  // UDP packetisation. A slow network drops a GOP and requests a fresh IDR;
  // it can never block camera capture, audio, SIP teardown or the main loop.
  uint8_t *tx_access_unit_{nullptr};
  size_t tx_access_unit_size_{0};
  uint32_t tx_access_unit_timestamp_{0};
  bool tx_access_unit_key_frame_{false};
  std::atomic<uint8_t> tx_access_unit_state_{0};  // 0=free, 1=ready, 2=owned
  std::atomic<bool> tx_resync_needed_{false};

  uint8_t *reassembly_{nullptr};
  size_t reassembly_size_{0};
  uint32_t reassembly_timestamp_{0};
  bool reassembly_active_{false};
  bool reassembly_damaged_{false};
  bool fu_active_{false};
  uint8_t fu_nal_type_{0};
  uint16_t expected_sequence_{0};
  bool sequence_valid_{false};
  uint8_t *jpeg_quantization_cache_{nullptr};
  bool jpeg_quantization_cache_allocation_failed_{false};
  RtpJpegDepacketizer jpeg_depacketizer_{};
  std::atomic<uint16_t> tx_sequence_{0};
  uint32_t tx_timestamp_offset_{0};
  uint32_t tx_ssrc_{0};
  std::atomic<uint32_t> remote_ssrc_{0};
  std::atomic<bool> remote_ssrc_latched_{false};
  std::atomic<uint16_t> latched_remote_rtp_port_{0};
  std::atomic<uint16_t> latched_remote_rtcp_port_{0};
  std::atomic<uint32_t> tx_packets_{0};
  std::atomic<uint32_t> rx_packets_{0};
  std::atomic<uint32_t> tx_octets_{0};
  std::atomic<uint32_t> last_tx_rtp_timestamp_{0};
  std::atomic<uint32_t> last_tx_rtp_ms_{0};
  std::atomic<uint32_t> dropped_access_units_{0};
  bool rtcp_feedback_enabled_{false};
  uint32_t last_rtcp_ms_{0};
  uint32_t last_pli_ms_{0};
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_DEBUG
  uint32_t tx_access_units_completed_{0};
  uint32_t tx_backpressure_events_{0};
  uint32_t tx_send_failures_{0};
  uint32_t tx_max_access_unit_ms_{0};
  uint32_t tx_last_debug_log_ms_{0};
#endif
};

}  // namespace voip_stack
}  // namespace esphome

#endif  // USE_ESP32 && USE_ESPHOME_VOIP_STACK_VIDEO
