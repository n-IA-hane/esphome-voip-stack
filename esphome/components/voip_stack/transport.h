#pragma once

#ifdef USE_ESP32

#include <cstddef>
#include <cstdint>
#include <string>

#include "sip_types.h"

namespace esphome {
namespace voip_stack {

struct TransportAudioFrame {
  const uint8_t *pcm{nullptr};
  size_t bytes{0};
  uint16_t sequence{0};
  uint32_t timestamp{0};
  bool has_rtp_metadata{false};
};

using TransportAudioCallback = void (*)(void *ctx, const TransportAudioFrame &frame);
using TransportSipSignalCallback = void (*)(void *ctx, const SipSignal &signal);
using TransportConnectionCallback = void (*)(void *ctx, bool connected);
using TransportAcceptCallback = bool (*)(void *ctx);
using TransportDialogActiveCallback = bool (*)(void *ctx);

struct SipTransportSnapshot {
  bool running{false};
  bool rtp_running{false};
  bool call_active{false};
  bool pending_invite{false};
  bool sip_tcp{false};
  uint16_t remote_sip_port{0};
  uint16_t remote_rtp_port{0};
  AudioFormat selected_tx_format{DEFAULT_AUDIO_FORMAT};
  AudioFormat selected_rx_format{DEFAULT_AUDIO_FORMAT};
  uint32_t rtp_tx_packets{0};
  uint32_t rtp_rx_packets{0};
  uint32_t rtp_tx_bytes{0};
  uint32_t rtp_rx_bytes{0};
  uint16_t last_sip_status_code{0};
  const char *last_sip_event{""};
};

/// Abstract SIP phone transport. VoipStack composes one and never touches
/// sockets directly.
///
/// Threading contract:
///   - Implementations may spawn FreeRTOS tasks.
///   - send_audio_frame must be safe from a Core 0 audio-priority task.
///   - Callbacks fire from the transport's own task; handlers must
///     marshal work via ring buffers / atomics and never block.
class SipPhoneTransport {
 public:
  virtual ~SipPhoneTransport() = default;

  /// Idempotent.
  virtual bool start() = 0;
  virtual void stop() = 0;

  /// Drop the current peer session without tearing the transport down.
  /// TCP closes the accepted client; UDP no-op (no per-session state).
  virtual void disconnect() {}

  /// TCP: a client is accepted. UDP: mirrors start()/stop().
  virtual bool is_connected() const = 0;

  /// Best-effort send. Payload is one PCM frame in the negotiated RTP format.
  /// Safe from a high-priority audio task; may drop on backpressure.
  virtual void send_audio_frame(const uint8_t *pcm, size_t bytes) = 0;

  /// SIP dialog commands. Return true when the message was committed to the wire.
  virtual bool send_invite(const std::string &call_id,
                           const std::string &caller_route,
                           const std::string &caller_name,
                           const std::string &dest_route,
                           const std::string &dest_name) = 0;
  virtual bool send_ringing(const std::string &call_id) = 0;
  virtual bool send_answer(const std::string &call_id,
                           const AudioFormat &caller_to_dest_format,
                           const AudioFormat &dest_to_caller_format) = 0;
  virtual bool send_cancel(const std::string &call_id) = 0;
  virtual bool send_bye(const std::string &call_id) = 0;
  virtual bool send_final_response(const std::string &call_id,
                                   uint16_t status,
                                   const std::string &reason) = 0;

  /// Used in dump_config / ESP_LOGCONFIG ("tcp", "udp", ...).
  virtual const char *transport_name() const = 0;

  /// Retarget the selected SIP peer. `port` is SIP signaling; `rtp_port`
  /// carries RTP media port until the internal naming is fully collapsed.
  virtual void set_remote(const std::string &ip, uint16_t port, uint16_t rtp_port = 0) {}

  /// SIP-only: select TCP or UDP for SIP signaling. Audio remains RTP/UDP.
  /// Other transports ignore this because their protocol is fixed by type.
  virtual void set_sip_signaling_transport(bool tcp) { (void) tcp; }

  /// Open an outbound leg for an originating call. TCP connects to the
  /// peer; UDP no-op (control_socket_ is already bound, set_remote
  /// retargets sendto).
  virtual bool originate(const std::string &host, uint16_t port) { return true; }

  /// Publish local media capabilities for SIP/SDP offer/answer negotiation.
  virtual void set_audio_formats(const AudioFormatList &tx, const AudioFormatList &rx) {
    (void) tx;
    (void) rx;
  }

  /// Lazy audio path. UDP binds the audio socket and spawns recv_task
  /// only here so an idle device isn't a passive PCM listener. TCP no-op.
  virtual bool start_audio_path() { return true; }
  virtual void stop_audio_path() {}

  virtual SipTransportSnapshot snapshot() const { return {}; }

  // === Callbacks (set by VoipStack before start()) ===

  void set_audio_callback(TransportAudioCallback cb, void *ctx) {
    this->on_audio_frame_ = cb;
    this->on_audio_frame_ctx_ = ctx;
  }

  void set_sip_signal_callback(TransportSipSignalCallback cb, void *ctx) {
    this->on_sip_signal_ = cb;
    this->on_sip_signal_ctx_ = ctx;
  }

  void set_connection_callback(TransportConnectionCallback cb, void *ctx) {
    this->on_connection_change_ = cb;
    this->on_connection_change_ctx_ = ctx;
  }

  void set_accept_callback(TransportAcceptCallback cb, void *ctx) {
    this->should_accept_session_cb_ = cb;
    this->should_accept_session_ctx_ = ctx;
  }

  void set_dialog_active_callback(TransportDialogActiveCallback cb, void *ctx) {
    this->dialog_active_cb_ = cb;
    this->dialog_active_ctx_ = ctx;
  }

 protected:
  /// Buffer lifetime = callback duration only.
  void emit_audio_frame_(const uint8_t *pcm, size_t bytes) {
    TransportAudioFrame frame;
    frame.pcm = pcm;
    frame.bytes = bytes;
    if (this->on_audio_frame_ != nullptr) this->on_audio_frame_(this->on_audio_frame_ctx_, frame);
  }

  void emit_audio_frame_(const uint8_t *pcm, size_t bytes, uint16_t sequence, uint32_t timestamp) {
    TransportAudioFrame frame;
    frame.pcm = pcm;
    frame.bytes = bytes;
    frame.sequence = sequence;
    frame.timestamp = timestamp;
    frame.has_rtp_metadata = true;
    if (this->on_audio_frame_ != nullptr) this->on_audio_frame_(this->on_audio_frame_ctx_, frame);
  }

  void emit_sip_signal_(const SipSignal &signal) {
    if (this->on_sip_signal_ != nullptr) this->on_sip_signal_(this->on_sip_signal_ctx_, signal);
  }

  /// TCP: per accept/disconnect. UDP: once per start/stop.
  void emit_connection_change_(bool connected) {
    if (this->on_connection_change_ != nullptr) {
      this->on_connection_change_(this->on_connection_change_ctx_, connected);
    }
  }

  /// TCP gate before accept. UDP ignores (no per-session concept).
  bool should_accept_session_() const {
    return this->should_accept_session_cb_ == nullptr ||
           this->should_accept_session_cb_(this->should_accept_session_ctx_);
  }

  bool dialog_active_() const {
    return this->dialog_active_cb_ != nullptr && this->dialog_active_cb_(this->dialog_active_ctx_);
  }

 private:
  TransportAudioCallback on_audio_frame_{nullptr};
  void *on_audio_frame_ctx_{nullptr};
  TransportSipSignalCallback on_sip_signal_{nullptr};
  void *on_sip_signal_ctx_{nullptr};
  TransportConnectionCallback on_connection_change_{nullptr};
  void *on_connection_change_ctx_{nullptr};
  TransportAcceptCallback should_accept_session_cb_{nullptr};
  void *should_accept_session_ctx_{nullptr};
  TransportDialogActiveCallback dialog_active_cb_{nullptr};
  void *dialog_active_ctx_{nullptr};
};

}  // namespace voip_stack
}  // namespace esphome

#endif  // USE_ESP32
