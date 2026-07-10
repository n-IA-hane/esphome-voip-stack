#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_ESPHOME_VOIP_SIP_TRANSPORT)

#include "transport.h"

#include "esphome/core/helpers.h"

#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/sockets.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace esphome {
namespace voip_stack {

class SipTransport : public SipPhoneTransport {
 public:
  static constexpr uint32_t kSipTaskStackBytes = 8192;
  static constexpr uint32_t kRtpTaskStackBytes = 8192;
  static constexpr uint8_t kSipTaskPriority = 4;
  static constexpr uint8_t kRtpTaskPriority = 9;
  static constexpr int kRtpSocketRxBufferBytes = 65536;

  SipTransport(uint16_t sip_port, uint16_t rtp_port, size_t udp_max_payload, const std::string &remote_host,
               bool task_stacks_in_psram);
  ~SipTransport() override;

  bool start() override;
  void stop() override;
  void disconnect() override;
  bool is_connected() const override;
  void send_audio_frame(const uint8_t *pcm, size_t bytes) override;
  bool send_invite(const std::string &call_id,
                   const std::string &caller_route,
                   const std::string &caller_name,
                   const std::string &dest_route,
                   const std::string &dest_name) override;
  bool send_ringing(const std::string &call_id) override;
  bool send_answer(const std::string &call_id,
                   const AudioFormat &caller_to_dest_format,
                   const AudioFormat &dest_to_caller_format) override;
  bool send_cancel(const std::string &call_id) override;
  bool send_bye(const std::string &call_id) override;
  bool send_final_response(const std::string &call_id,
                           uint16_t status,
                           const std::string &reason) override;
  const char *transport_name() const override { return "sip"; }
  bool start_audio_path() override;
  void stop_audio_path() override;
  bool originate(const std::string &host, uint16_t port) override;
  void set_remote(const std::string &ip, uint16_t port, uint16_t rtp_port = 0) override;
  void set_sip_signaling_transport(bool tcp) override;
  void set_audio_formats(const AudioFormatList &tx, const AudioFormatList &rx) override;
  SipTransportSnapshot snapshot() const override;

 protected:
  enum class SipEvent : uint8_t {
    NONE = 0,
    INVITE,
    ACK,
    CANCEL,
    BYE,
    OPTIONS,
    RESPONSE,
  };

  struct SipRequestOptions {
    uint32_t cseq_number{0};
    std::string cseq_method;
    std::string branch_override;
  };

  static void sip_task_trampoline_(void *param);
  static void rtp_task_trampoline_(void *param);
  void sip_task_();
  void rtp_task_();
  bool bind_udp_(int *fd, uint16_t port, const char *label);
  bool bind_tcp_(int *fd, uint16_t port, const char *label);
  bool parse_remote_(const std::string &host);
  bool send_sip_(const std::string &message, uint32_t ip_v4, uint16_t port);
  bool send_sip_tcp_(const std::string &message);
  bool send_sip_tcp_record_(const std::string &message, int socket);
  bool send_request_(const std::string &method, const std::string &body = "");
  bool send_request_(const std::string &method, const std::string &body,
                     const SipRequestOptions &options);
  bool send_cancel_unlocked_(const std::string &call_id);
  bool send_bye_unlocked_(const std::string &call_id);
  bool send_invite_error_ack_();
  bool send_response_(uint16_t status, const char *reason, const std::string &body = "",
                      const std::string &app_reason = "");
  bool send_stateless_response_(const std::string &request, const sockaddr_in &src,
                                uint16_t status, const char *reason,
                                const std::string &app_reason = "",
                                bool cache_transaction = false);
  std::string format_response_(uint16_t status, const char *reason,
                               const std::string &via, const std::string &from,
                               const std::string &to, const std::string &call_id,
                               const std::string &cseq, const std::string &app_reason,
                               const std::string &body, bool add_contact_ua,
                               bool add_to_tag, bool stateless);
  void handle_sip_datagram_(const char *data, size_t len, const sockaddr_in &src);
  void handle_sip_stream_(int socket, const sockaddr_in &src);
  bool handle_invite_(const std::string &message, const sockaddr_in &src);
  bool handle_response_(const std::string &message, const sockaddr_in &src);
  std::string build_sdp_offer_() const;
  std::string build_sdp_answer_() const;
  std::string wrap_sdp_envelope_(const std::string &local_ip, const std::string &payloads,
                                 const std::string &maps, const std::string &flows,
                                 uint8_t ptime) const;
  bool learn_remote_rtp_from_sdp_(const std::string &sdp, uint32_t default_ip);
  bool local_ip_for_peer_(uint32_t peer_ip_v4, std::string *out) const;
  void clear_udp_transactions_();
  void remember_udp_transaction_(const std::string &method, const std::string &message,
                                 uint32_t ip_v4, uint16_t port);
  void pump_udp_retransmits_();
  void clear_invite_transaction_();
  void clear_bye_transaction_();
  void reset_rtp_latch_();
  void open_media_session_();
  void close_media_session_();
  void request_tcp_client_close_();
  void close_tcp_client_from_sip_task_();
  void handle_tcp_peer_loss_();
  void wake_sip_task_();
  void wake_rtp_task_();
  void reset_dialog_();
  bool replay_completed_response_(const std::string &request, const sockaddr_in &src,
                                  const std::string &method);
  void remember_completed_response_(const std::string &request, uint32_t peer_ip_v4,
                                    uint16_t peer_port, const std::string &method,
                                    const std::string &response);
  uint16_t acknowledge_completed_invite_(const std::string &request,
                                         const sockaddr_in &src);
  bool replay_completed_invite_ack_(const std::string &response, const sockaddr_in &src);
  void remember_completed_invite_ack_(const std::string &request, uint32_t target_ip_v4,
                                      uint16_t target_port);
  bool reject_if_stale_dialog_(const std::string &request, const sockaddr_in &src,
                               const char *method_name);
  void mark_sip_event_(SipEvent event, uint16_t status = 0);
  static SipEvent sip_event_from_method_(const std::string &method);
  static const char *sip_event_name_(SipEvent event);
  void set_media_config_(const AudioFormat &tx, const AudioFormat &rx,
                         uint8_t tx_payload_type, uint8_t rx_payload_type);
  void get_media_config_(AudioFormat *tx, AudioFormat *rx,
                         uint8_t *tx_payload_type, uint8_t *rx_payload_type) const;

  struct UdpTransaction {
    std::string request;
    uint32_t ip_v4{0};
    uint16_t port{0};
    uint32_t next_ms{0};
    uint32_t deadline_ms{0};
    uint16_t interval_ms{500};
    uint8_t retries{0};
    bool completed{false};
    void clear() {
      this->request.clear();
      this->ip_v4 = 0;
      this->port = 0;
      this->next_ms = 0;
      this->deadline_ms = 0;
      this->interval_ms = 500;
      this->retries = 0;
      this->completed = false;
    }
    bool empty() const { return this->request.empty(); }
  };

  struct CompletedServerTransaction {
    std::string method;
    std::string call_id;
    std::string branch;
    std::string from_tag;
    std::string to_tag;
    std::string response;
    uint32_t cseq{0};
    uint32_t peer_ip_v4{0};
    uint16_t peer_port{0};
    uint16_t status{0};
    uint32_t completed_ms{0};
    uint32_t next_retransmit_ms{0};
    uint32_t deadline_ms{0};
    uint16_t retransmit_interval_ms{500};
    uint8_t retransmits{0};
    bool udp{false};
    bool awaiting_ack{false};
    void clear() {
      this->method.clear();
      this->call_id.clear();
      this->branch.clear();
      this->from_tag.clear();
      this->to_tag.clear();
      this->response.clear();
      this->cseq = 0;
      this->peer_ip_v4 = 0;
      this->peer_port = 0;
      this->status = 0;
      this->completed_ms = 0;
      this->next_retransmit_ms = 0;
      this->deadline_ms = 0;
      this->retransmit_interval_ms = 500;
      this->retransmits = 0;
      this->udp = false;
      this->awaiting_ack = false;
    }
    bool empty() const { return this->response.empty(); }
  };

  struct CompletedInviteClientTransaction {
    std::string call_id;
    std::string branch;
    std::string ack;
    uint32_t cseq{0};
    uint32_t response_ip_v4{0};
    uint32_t ack_ip_v4{0};
    uint16_t ack_port{0};
    uint32_t completed_ms{0};
    void clear() {
      this->call_id.clear();
      this->branch.clear();
      this->ack.clear();
      this->cseq = 0;
      this->response_ip_v4 = 0;
      this->ack_ip_v4 = 0;
      this->ack_port = 0;
      this->completed_ms = 0;
    }
    bool empty() const { return this->ack.empty(); }
  };

  uint16_t sip_port_{5060};
  uint16_t rtp_port_{40000};
  size_t udp_max_payload_{UDP_SAFE_AUDIO_PAYLOAD_BYTES};
  bool task_stacks_in_psram_{false};
  std::atomic<uint32_t> remote_ip_v4_{0};
  std::atomic<uint32_t> remote_rtp_ip_v4_{0};
  std::atomic<uint16_t> remote_sip_port_{5060};
  std::atomic<uint16_t> remote_rtp_port_{0};
  std::atomic<uint16_t> rtp_sequence_{0};
  std::atomic<uint32_t> rtp_timestamp_{0};
  uint32_t rtp_ssrc_{0x49434150};

  std::string call_id_;
  std::string local_tag_;
  std::string remote_tag_;
  std::string branch_;
  std::string local_uri_;
  std::string remote_uri_;
  std::string remote_target_uri_;
  std::string last_invite_via_;
  std::string last_invite_from_;
  std::string last_invite_to_;
  std::string last_invite_cseq_;
  std::string last_invite_response_;
  uint32_t last_invite_cseq_number_{0};
  std::string caller_route_;
  std::string caller_name_;
  std::string dest_route_;
  std::string dest_name_;
  CompletedServerTransaction completed_invite_;
  CompletedServerTransaction completed_control_;
  CompletedInviteClientTransaction completed_invite_client_;
  std::string sip_tcp_rx_buffer_;
  AudioFormatList offer_tx_formats_{};
  AudioFormatList offer_rx_formats_{};
  AudioFormat selected_tx_format_{DEFAULT_AUDIO_FORMAT};
  AudioFormat selected_rx_format_{DEFAULT_AUDIO_FORMAT};
  uint8_t rtp_tx_payload_type_{96};
  uint8_t rtp_rx_payload_type_{96};
  mutable portMUX_TYPE media_config_lock_ = portMUX_INITIALIZER_UNLOCKED;
  uint32_t cseq_{1};
  uint32_t invite_cseq_{1};
  UdpTransaction pending_invite_;
  UdpTransaction pending_cancel_;
  UdpTransaction pending_bye_;

  int sip_socket_{-1};
  int sip_tcp_listener_socket_{-1};
  std::atomic<int> sip_tcp_client_socket_{-1};
  std::atomic<uint32_t> sip_tcp_client_ip_v4_{0};
  std::atomic<uint32_t> tcp_connect_ip_v4_{0};
  std::atomic<uint16_t> tcp_connect_port_{0};
  std::atomic<bool> tcp_connect_requested_{false};
  mutable Mutex tcp_tx_pending_mutex_;
  std::string tcp_tx_pending_;
  mutable Mutex tcp_send_mutex_;
  mutable Mutex dialog_mutex_;
  mutable Mutex rtp_socket_mutex_;
  int rtp_socket_{-1};
  TaskHandle_t sip_task_handle_{nullptr};
  StaticTask_t sip_task_tcb_{};
  StackType_t *sip_task_stack_{nullptr};
  SemaphoreHandle_t sip_task_done_{nullptr};
  StaticSemaphore_t sip_task_done_storage_{};
  TaskHandle_t rtp_task_handle_{nullptr};
  StaticTask_t rtp_task_tcb_{};
  StackType_t *rtp_task_stack_{nullptr};
  SemaphoreHandle_t rtp_task_done_{nullptr};
  StaticSemaphore_t rtp_task_done_storage_{};
  std::atomic<bool> running_{false};
  std::atomic<bool> rtp_running_{false};
  std::atomic<bool> rtp_task_quiesced_{true};
  std::atomic<bool> rtp_task_terminate_{false};
  std::atomic<bool> media_active_{false};
  std::atomic<bool> outgoing_invite_pending_{false};
  std::atomic<bool> cancel_requested_{false};
  std::atomic<bool> remote_sip_tcp_{false};
  std::atomic<bool> sip_tcp_client_close_requested_{false};
  std::atomic<uint32_t> rtp_tx_packets_{0};
  std::atomic<uint32_t> rtp_rx_packets_{0};
  std::atomic<uint32_t> rtp_tx_bytes_{0};
  std::atomic<uint32_t> rtp_rx_bytes_{0};
  std::atomic<uint16_t> last_sip_status_code_{0};
  std::atomic<uint8_t> last_sip_event_{0};
  std::atomic<uint32_t> latched_rtp_ip_v4_{0};
  std::atomic<uint16_t> latched_rtp_port_{0};
  std::atomic<uint32_t> latched_rtp_ssrc_{0};
  std::atomic<bool> rtp_ssrc_latched_{false};
};

}  // namespace voip_stack
}  // namespace esphome

#endif  // USE_ESP32 && USE_ESPHOME_VOIP_SIP_TRANSPORT
