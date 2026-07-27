#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_ESPHOME_VOIP_STACK_VIDEO)

#include "audio_core_task_utils.h"
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
#include "rtp_jpeg.h"
#endif
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
  static constexpr uint8_t kReceiveTaskPriority = 7;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_HOSTED_AUDIO_PACING
  // ESP-Hosted funnels every network packet through the same blocking SDIO
  // queue. Keep the video producer below the priority-9 audio RTP worker and
  // the priority-24 audio TX task. One binary audio event releases a bounded
  // local burst; no counting backlog or recurring timer can build up.
  static constexpr uint8_t kSenderTaskPriority = 8;
  static constexpr BaseType_t kSenderTaskCore = 1;
#else
  static constexpr uint8_t kSenderTaskPriority = 7;
  static constexpr BaseType_t kSenderTaskCore = 0;
#endif
  static constexpr size_t kMaxAccessUnitBytes = 512 * 1024;
  static constexpr size_t kMaxRtpPacketBytes = 1500;
  static constexpr size_t kMaxReceiveBatchPackets = 32;
  // Before the first audio packet arrives, keep video startup bounded. Once
  // audio is flowing, ESP-Hosted video is strictly released by successful
  // audio sends and has no periodic pacing wakeup.
  static constexpr uint32_t kAudioPacingStartupWaitMs = 40;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_HOSTED_AUDIO_PACING
  static constexpr uint8_t kVideoPacketsPerAudioCredit = 4;
#endif
  static constexpr uint32_t kTxBackpressureWaitMs = 3;
  static constexpr uint32_t kWorkerStopBudgetMs = 1000;

  explicit VideoRtpSession(bool task_stack_in_psram);
  ~VideoRtpSession();

  void set_source(EncodedVideoSource *source) { this->source_ = source; }
  void set_sink(EncodedVideoSink *sink) { this->sink_ = sink; }
  void set_local_port(uint16_t port) { this->local_rtp_port_ = port; }
  void set_max_payload(size_t bytes) { this->max_payload_ = bytes; }
  bool set_negotiated(const VideoCapability &capability,
                      const VideoCapability &send_capability,
                      const VideoCapability &receive_capability,
                      uint32_t remote_ip_v4, uint16_t remote_rtp_port,
                      uint32_t remote_rtcp_ip_v4, uint16_t remote_rtcp_port,
                      bool send_enabled, bool receive_enabled);

  /// Prepare the negotiated RTP/codec resources. When activate is false no
  /// source media is produced and no receive frame is presented until the
  /// offer/answer transaction commits through request_media_direction().
  bool start(bool activate = true);
  /// Change only the producer side of an established video session. The
  /// request is consumed by the existing RTP worker; callers never join media
  /// tasks from the SIP signaling task.
  bool request_send_direction(bool enabled);
  bool can_request_media_direction(bool send_enabled,
                                   bool receive_enabled) const;
  bool request_media_direction(bool send_enabled, bool receive_enabled);
  bool negotiation_matches(const VideoCapability &capability,
                           const VideoCapability &send_capability,
                           const VideoCapability &receive_capability,
                           uint32_t remote_ip_v4,
                           uint16_t remote_rtp_port,
                           uint32_t remote_rtcp_ip_v4,
                           uint16_t remote_rtcp_port) const;
  bool send_direction_enabled() const {
    return this->send_enabled_.load(std::memory_order_acquire);
  }
  /// Gate callbacks and wake both workers without waiting for their joins.
  /// The owning media lifecycle worker follows with stop().
  void request_stop();
  void stop();
  bool is_running() const { return this->running_.load(std::memory_order_acquire); }

  uint32_t tx_packets() const { return this->tx_packets_.load(std::memory_order_acquire); }
  uint32_t rx_packets() const { return this->rx_packets_.load(std::memory_order_acquire); }
  uint32_t tx_access_units() const {
    return this->tx_access_units_ok_.load(std::memory_order_acquire);
  }
  uint32_t rx_access_units() const {
    return this->rx_access_units_ok_.load(std::memory_order_acquire);
  }
  uint32_t dropped_access_units() const {
    return this->dropped_access_units_.load(std::memory_order_acquire);
  }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_HOSTED_AUDIO_PACING
  void notify_audio_packet_sent();
#endif

 protected:
  static void source_callback_(void *ctx, const EncodedVideoAccessUnit &access_unit);
  static void task_trampoline_(void *ctx);
  static void sender_task_trampoline_(void *ctx);
  void task_();
  void sender_task_();
  bool start_sender_task_();
  bool stop_sender_task_(TickType_t stop_started,
                         TickType_t stop_budget);
  bool stop_receive_task_(TickType_t stop_started,
                          TickType_t stop_budget);
  bool reap_sender_task_();
  bool reap_receive_task_();
  void quiesce_tasks_();
  void close_sockets_();
  void queue_access_unit_(const EncodedVideoAccessUnit &access_unit);
  void send_access_unit_(const EncodedVideoAccessUnit &access_unit);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
  void send_h264_access_unit_(const EncodedVideoAccessUnit &access_unit);
#endif
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  void send_jpeg_access_unit_(const EncodedVideoAccessUnit &access_unit);
#endif
  bool send_rtp_payload_(const uint8_t *payload, size_t size, bool marker,
                         uint32_t timestamp);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_HOSTED_AUDIO_PACING
  bool wait_for_audio_pacing_();
  void reset_audio_pacing_burst_();
#endif
  bool handle_rtp_packet_(const uint8_t *packet, size_t size);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
  static constexpr size_t kH264ParameterSetBytes = 1024;
  bool handle_h264_payload_(const uint8_t *payload, size_t payload_size,
                            bool marker, uint32_t timestamp,
                            bool sequence_gap);
#endif
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  bool handle_jpeg_payload_(const uint8_t *payload, size_t payload_size,
                            bool marker, uint32_t timestamp);
  bool ensure_jpeg_quantization_cache_();
#endif
  void reset_reassembly_();
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
  bool append_annex_b_nal_(const uint8_t *nal, size_t size);
  void finish_access_unit_(uint32_t timestamp);
  void reset_h264_parameter_sets_();
#endif
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
  // SDP selects one RTP codec/PT contract, while local encoder and decoder
  // may legitimately have different frame envelopes. Keep those directional
  // contracts separate (for example P4 JPEG TX 400x400 and RX 640x480).
  VideoCapability capability_{};
  VideoCapability send_capability_{};
  VideoCapability receive_capability_{};
  uint16_t local_rtp_port_{40002};
  size_t max_payload_{1200};
  std::atomic<uint32_t> remote_ip_v4_{0};
  std::atomic<uint16_t> remote_rtp_port_{0};
  std::atomic<uint32_t> remote_rtcp_ip_v4_{0};
  std::atomic<uint16_t> remote_rtcp_port_{0};
  std::atomic<bool> send_enabled_{false};
  std::atomic<bool> receive_enabled_{false};
  std::atomic<bool> source_started_{false};
  std::atomic<bool> sink_started_{false};
  std::atomic<bool> send_prepared_{false};
  std::atomic<bool> receive_prepared_{false};
  // Reassembly is worker-owned. Signaling only posts this command and wakes
  // select(); it never mutates the plain depacketizer state concurrently.
  std::atomic<bool> rx_reset_requested_{false};

  int rtp_socket_{-1};
  int rtcp_socket_{-1};
  // Private loopback datagram socket included in the receive select set.
  // This is the same self-pipe pattern used by SipTransport: control changes
  // wake immediately without polling or injecting a fake RTP packet.
  int wake_socket_{-1};
  uint16_t wake_port_{0};
  // Source control can be requested by the signaling owner while RTCP or the
  // sender asks for a key frame. Serialize those rare control operations;
  // frame delivery itself remains lock-free through the bounded AU slot.
  Mutex source_control_mutex_;
  // A committed SDP direction change and terminal worker cleanup must never
  // operate the camera/renderer concurrently. request_stop() remains an
  // atomic gate+wake and therefore never blocks the SIP task.
  Mutex direction_mutex_;
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
  std::atomic<bool> rtcp_bye_requested_{false};
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_HOSTED_AUDIO_PACING
  SemaphoreHandle_t audio_pacing_{nullptr};
  StaticSemaphore_t audio_pacing_storage_{};
  std::atomic<bool> audio_pacing_started_{false};
  std::atomic<bool> audio_pacing_startup_fallback_used_{false};
  std::atomic<uint8_t> audio_pacing_burst_remaining_{0};
#endif

  // One bounded PSRAM access-unit slot decouples the hardware encoder from
  // UDP packetisation. A slow network drops a GOP and requests a fresh IDR;
  // it can never block camera capture, audio, SIP teardown or the main loop.
  uint8_t *tx_access_unit_{nullptr};
  size_t tx_access_unit_size_{0};
  uint32_t tx_access_unit_timestamp_{0};
  bool tx_access_unit_key_frame_{false};
  std::atomic<uint8_t> tx_access_unit_state_{0};  // 0=free, 1=ready, 2=owned
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
  std::atomic<bool> tx_resync_needed_{false};
#endif

  uint8_t *reassembly_{nullptr};
  size_t reassembly_size_{0};
  uint32_t reassembly_timestamp_{0};
  bool reassembly_active_{false};
  bool reassembly_damaged_{false};
  bool fu_active_{false};
  uint8_t fu_nal_type_{0};
  uint16_t expected_sequence_{0};
  bool sequence_valid_{false};
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
  // RFC 6184 senders may transmit SPS/PPS only at stream bootstrap. Keep the
  // last complete sets for this RTP session and prepend them to a later IDR
  // after local loss/reset, matching the browser-side depacketizer.
  uint8_t h264_sps_[kH264ParameterSetBytes]{};
  size_t h264_sps_size_{0};
  uint8_t h264_pps_[kH264ParameterSetBytes]{};
  size_t h264_pps_size_{0};
#endif
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  uint8_t *jpeg_quantization_cache_{nullptr};
  bool jpeg_quantization_cache_allocation_failed_{false};
  RtpJpegDepacketizer jpeg_depacketizer_{};
  bool jpeg_parse_warning_emitted_{false};
#endif
  std::atomic<uint16_t> tx_sequence_{0};
  uint32_t tx_timestamp_offset_{0};
  uint32_t tx_ssrc_{0};
  std::atomic<uint32_t> remote_ssrc_{0};
  std::atomic<bool> remote_ssrc_latched_{false};
  std::atomic<uint16_t> latched_remote_rtp_port_{0};
  std::atomic<uint16_t> latched_remote_rtcp_port_{0};
  std::atomic<uint32_t> tx_packets_{0};
  std::atomic<uint32_t> rx_packets_{0};
  std::atomic<uint32_t> tx_access_units_ok_{0};
  std::atomic<uint32_t> rx_access_units_ok_{0};
  std::atomic<uint32_t> tx_octets_{0};
  std::atomic<uint32_t> last_tx_rtp_timestamp_{0};
  std::atomic<uint32_t> last_tx_rtp_ms_{0};
  std::atomic<uint32_t> dropped_access_units_{0};
  bool accept_remote_pli_{false};
  bool accept_remote_fir_{false};
  bool send_remote_pli_{false};
  uint32_t last_rtcp_ms_{0};
  uint32_t last_pli_ms_{0};
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_DEBUG
  uint32_t tx_access_units_completed_{0};
  uint32_t tx_backpressure_events_{0};
  uint32_t tx_send_failures_{0};
  uint32_t tx_max_access_unit_ms_{0};
  uint32_t tx_slow_send_calls_{0};
  uint32_t tx_max_send_us_{0};
  uint32_t tx_last_debug_log_ms_{0};
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_HOSTED_AUDIO_PACING
  uint32_t tx_audio_pacing_startup_fallbacks_{0};
  uint32_t tx_audio_pacing_credit_waits_{0};
  uint32_t tx_audio_pacing_max_wait_us_{0};
#endif
#endif
};

}  // namespace voip_stack
}  // namespace esphome

#endif  // USE_ESP32 && USE_ESPHOME_VOIP_STACK_VIDEO
