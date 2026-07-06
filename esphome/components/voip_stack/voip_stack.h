#pragma once

#ifdef USE_ESP32

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#include "audio_core_audio_utils.h"
#include "audio_core_ring_buffer_caps.h"

#ifdef USE_ESPHOME_VOIP_STACK_MIC
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/microphone/microphone_source.h"
#endif
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
#include "esphome/components/speaker/speaker.h"
#endif

#include "esphome/components/switch/switch.h"
#include "esphome/components/button/button.h"
#include "esphome/components/number/number.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include "voip_fsm.h"
#include "sip_types.h"
#include "phonebook.h"
#include "rtp_jitter_buffer.h"
#include "transport.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_event.h>

#include <atomic>
#include <algorithm>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace esphome {
namespace voip_stack {

// SIP signaling transport. Media is always RTP/UDP.
enum class TransportType : uint8_t {
  TCP,
  UDP,
};

// Connection state, derived from the SIP transport and call state.
enum class ConnectionState : uint8_t {
  DISCONNECTED,
  CONNECTED,
  IN_CALL,
};

/// SIP phone exposed through the historical `voip_stack` component name.
///
/// The phone speaks SIP signaling, SDP offer/answer and RTP PCM media.
/// Proprietary intercom transports are not selected by this class anymore.
///
/// The recv task is owned by the transport. TX is drained by a network task
/// when a microphone is configured; RX is played by a small playout task when
/// a speaker is configured.
/// See README.md.
class VoipStack : public Component {
 public:
  // FreeRTOS task stack sizes in bytes. Kept at class level so xTaskCreate
  // sites stay free of magic numbers. The transport task declares its own
  // stack size inside the transport class.
  static constexpr uint32_t kTxTaskStackBytes = 12288;
  static constexpr uint32_t kRxTaskStackBytes = 12288;
  static constexpr size_t kRxQueuedFrames = 16;
  static constexpr uint32_t kRxPrebufferFrames = 4;
  static constexpr uint32_t kRxSilenceAfterMs = 60;

  enum SchedulerId : uint32_t {
    SCHED_PUBLISH_INITIAL_STATE = 3,
    SCHED_SAVE_SETTINGS = 4,
  };

  void setup() override;
  void loop() override;
  void dump_config() override;
  // AFTER_CONNECTION: bind sockets and publish initial state only after
  // the HA API is up (AFTER_WIFI would race the API connection).
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  // Call from YAML api: on_client_connected: to publish restored entity states to HA
  void publish_entity_states();

  // Configuration
#ifdef USE_ESPHOME_VOIP_STACK_MIC
  void set_microphone(microphone::Microphone *mic) { this->microphone_ = mic; }
  void set_microphone_source(microphone::MicrophoneSource *source) { this->microphone_source_ = source; }
#endif
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
  void set_speaker(speaker::Speaker *spk) { this->speaker_ = spk; }
#endif

  void set_dc_offset_removal(bool enabled) { this->dc_offset_removal_ = enabled; }
  void set_task_stacks_in_psram(bool enabled) { this->task_stacks_in_psram_ = enabled; }
  void set_buffers_in_psram(bool enabled) { this->buffers_in_psram_ = enabled; }
  void set_device_name(const std::string &name) { this->device_name_ = name; }
  void set_extension(const std::string &extension) { this->extension_ = extension; }
  // Stable routing key (yaml `name:` slug, e.g. "spotpear-ball-v2").
  // Matches the slug HA uses for the esphome.{slug}_start_call action.
  void set_device_route_id(const std::string &id) { this->device_route_id_ = id; }
  // CALLING / RINGING timeouts (0 disables). They send CANCEL or a SIP
  // timeout final response and tear down locally.
  void set_calling_timeout(uint32_t ms) { this->calling_timeout_ms_ = ms; }
  void set_ringing_timeout(uint32_t ms) { this->ringing_timeout_ms_ = ms; }
  template<typename F> void add_on_state_callback(F &&callback) {
    this->state_callback_.add(std::forward<F>(callback));
  }

  // Transport configuration (set by codegen from YAML before setup()).
  void set_protocol(TransportType type) {
    this->protocol_ = type;
    if (this->transport_ != nullptr) {
      this->transport_->set_sip_signaling_transport(type == TransportType::TCP);
    }
  }
  void set_udp_max_payload(size_t bytes) { this->udp_max_payload_ = bytes; }
  void set_sip_port(uint16_t port) { this->sip_port_ = port; }
  void set_rtp_port(uint16_t port) { this->rtp_port_ = port; }
  std::string get_endpoint() const { return this->build_endpoint_string_(); }
  const char *get_audio_capability() const { return this->audio_capability_(); }

  // Direct SIP URIs are dialed peer-to-peer. Logical names without host/port
  // are routed through HA as a SIP B2BUA bridge.
  void set_use_ha_as_first_contact(bool enabled) { this->use_ha_as_first_contact_ = enabled; }
  void set_audio_debug(bool enabled) { this->audio_debug_ = enabled; }
  void set_tx_audio_format(uint32_t sample_rate, uint8_t pcm_format, uint8_t channels, uint16_t frame_ms) {
    this->tx_audio_format_ = AudioFormat{sample_rate, static_cast<PcmFormat>(pcm_format), channels, frame_ms};
    this->tx_audio_formats_.formats[0] = this->tx_audio_format_;
    this->tx_audio_formats_.count = 1;
    this->set_current_tx_audio_format_(this->tx_audio_format_);
  }
  void set_rx_audio_format(uint32_t sample_rate, uint8_t pcm_format, uint8_t channels, uint16_t frame_ms) {
    this->rx_audio_format_ = AudioFormat{sample_rate, static_cast<PcmFormat>(pcm_format), channels, frame_ms};
    this->rx_audio_formats_.formats[0] = this->rx_audio_format_;
    this->rx_audio_formats_.count = 1;
  }
  void add_supported_tx_audio_format(uint32_t sample_rate, uint8_t pcm_format, uint8_t channels, uint16_t frame_ms) {
    this->append_audio_format_(&this->tx_audio_formats_,
                               AudioFormat{sample_rate, static_cast<PcmFormat>(pcm_format), channels, frame_ms});
  }
  void add_supported_rx_audio_format(uint32_t sample_rate, uint8_t pcm_format, uint8_t channels, uint16_t frame_ms) {
    this->append_audio_format_(&this->rx_audio_formats_,
                               AudioFormat{sample_rate, static_cast<PcmFormat>(pcm_format), channels, frame_ms});
  }
  /// Update the selected SIP peer endpoint at runtime. `port` is SIP signaling;
  /// `rtp_port` is the peer RTP media port.
  void set_remote_endpoint(const std::string &ip, uint16_t port, uint16_t rtp_port = 0);
  void set_remote_sip_transport_tcp(bool tcp);
  const char *configured_sip_transport_name() const { return this->protocol_ == TransportType::TCP ? "tcp" : "udp"; }

  // Runtime control
  void start();
  void stop();
  bool is_active() const {
    CallState cs = this->call_state_.load(std::memory_order_acquire);
    return cs == CallState::IN_CALL || cs == CallState::CALLING ||
           cs == CallState::REMOTE_RINGING || cs == CallState::RINGING ||
           cs == CallState::CONNECTING;
  }
  bool is_connected() const {
    return this->transport_ != nullptr && this->transport_->is_connected();
  }

  // Volume control
  void set_volume(float volume);
  float get_volume() const { return this->volume_.load(std::memory_order_relaxed); }

  // Auto-answer control (for incoming calls)
  void set_auto_answer(bool enabled);
  bool is_auto_answer() const { return this->auto_answer_; }
  void set_do_not_disturb(bool enabled);
  bool is_do_not_disturb() const { return this->do_not_disturb_; }

  // Manual answer for incoming call (when auto_answer is OFF)
  void answer_call();
  void decline_call(const std::string &reason = "");
  bool is_ringing() const { return this->call_state_.load(std::memory_order_acquire) == CallState::RINGING; }
  bool is_calling() const {
    const CallState state = this->call_state_.load(std::memory_order_acquire);
    return state == CallState::CALLING || state == CallState::REMOTE_RINGING;
  }
  bool is_idle() const { return this->call_state_.load(std::memory_order_acquire) == CallState::IDLE; }
  bool is_in_call() const { return this->call_state_.load(std::memory_order_acquire) == CallState::IN_CALL; }
  // True when the selected contact name matches the configured HA peer.
  // Empty ha_peer_name_ disables the check (treated as "no HA configured").
  bool is_ha_destination() const {
    return !this->ha_peer_name_.empty() &&
           this->phonebook_.current_name() == this->ha_peer_name_;
  }
  void set_ha_peer_name(const std::string &name) { this->ha_peer_name_ = name; }

  // Smart call toggle: ringing → answer, active → hangup, idle → start
  void call_toggle();

  // Mic gain control (dB scale: -20 to +20)
  void set_mic_gain_db(float db);
  float get_mic_gain() const { return this->mic_gain_.load(std::memory_order_relaxed); }

  // ConnectionState is derived: DISCONNECTED if no transport / peer,
  // IN_CALL when audio is flowing, CONNECTED otherwise.
  ConnectionState get_state() const {
    if (this->transport_ == nullptr || !this->transport_->is_connected())
      return ConnectionState::DISCONNECTED;
    return this->call_state_.load(std::memory_order_acquire) == CallState::IN_CALL
               ? ConnectionState::IN_CALL
               : ConnectionState::CONNECTED;
  }
  const char *get_state_str() const;

  // Sensor registration
  void set_state_sensor(text_sensor::TextSensor *sensor) { this->state_sensor_ = sensor; }
  void set_destination_sensor(text_sensor::TextSensor *sensor) { this->destination_sensor_ = sensor; }
  void set_caller_sensor(text_sensor::TextSensor *sensor) { this->caller_sensor_ = sensor; }
  void set_contacts_sensor(text_sensor::TextSensor *sensor) { this->contacts_sensor_ = sensor; }
  void set_transport_sensor(text_sensor::TextSensor *sensor) { this->transport_sensor_ = sensor; }
  void set_endpoint_sensor(text_sensor::TextSensor *sensor) { this->endpoint_sensor_ = sensor; }
  void set_last_reason_sensor(text_sensor::TextSensor *sensor) { this->last_reason_sensor_ = sensor; }
  void set_sip_snapshot_sensor(text_sensor::TextSensor *sensor) { this->sip_snapshot_sensor_ = sensor; }
  // Phonebook source from HA: shipped YAMLs wire a homeassistant text_sensor
  // through ha_phonebook_text_sensor_id. Current firmware consumes the unified
  // SIP roster sensor.voip_phonebook and normalizes it locally.
  void set_ha_phonebook_sensor(text_sensor::TextSensor *sensor) { this->ha_phonebook_sensor_ = sensor; }
  // Prune threshold (1..10). 0 disables pruning entirely.
  void set_prune_threshold(uint8_t t) { this->prune_threshold_ = t; }

  // Entity registration (for state sync after boot)
  void register_auto_answer_switch(switch_::Switch *sw) { this->auto_answer_switch_ = sw; }
  void register_dnd_switch(switch_::Switch *sw) { this->dnd_switch_ = sw; }
  void register_volume_number(number::Number *num) { this->volume_number_ = num; }
  void register_mic_gain_number(number::Number *num) { this->mic_gain_number_ = num; }
  // SIP dial-plan contact management. Public YAML should prefer the
  // structured add_contact action. add_contact(std::string) remains the compact
  // internal transport-row path used by HA roster sync and generated helpers.
  // add_contact and set_contacts run the same idempotent merge: same shape
  // = no-op, missing endpoint upgraded in place, mismatched endpoint
  // replaced. Slot order is stable; only flush_contacts() trims.
  void add_contact(const std::string &entry);
  void remove_contact(const std::string &name);
  void set_contacts(const std::string &contacts_csv);  // batch wrapper, same merge rules per entry
  void flush_contacts();
  // Open a phonebook update cycle: commit any previously-open cycle (counter
  // advance + prune), then read the HA sensor (if configured + non-empty).
  // on_phonebook_update fires only after a real phonebook mutation.
  // The cycle remains open until the next update_contacts() call or
  // CYCLE_TIMEOUT_MS elapses (loop() safety net).
  void update_contacts();
  bool set_contact(const std::string &name);
  void call_contact(const std::string &name);
  void next_contact();
  void prev_contact();
  const std::string &get_current_destination() const;
  // Endpoint of the selected SIP contact. Empty/zero makes start() fail with
  // transport_unreachable; there is no implicit default peer.
  const std::string &get_current_contact_ip() const;
  uint16_t get_current_contact_port() const;
  uint16_t get_current_contact_rtp_port() const;
  bool get_current_contact_sip_transport_tcp() const;
  std::string get_caller() const { return this->caller_sensor_ ? this->caller_sensor_->state : ""; }
  std::string get_contacts_csv() const;

  // Call state triggers (exposed to YAML)
  Trigger<> *get_ringing_trigger() { return &this->ringing_trigger_; }
  Trigger<> *get_in_call_trigger() { return &this->in_call_trigger_; }
  Trigger<> *get_idle_trigger() { return &this->idle_trigger_; }
  Trigger<> *get_calling_trigger() { return &this->calling_trigger_; }
  Trigger<> *get_dest_ringing_trigger() { return &this->dest_ringing_trigger_; }
  Trigger<std::string, std::string, std::string, std::string> *get_incoming_call_trigger() {
    return &this->incoming_call_trigger_;
  }
  Trigger<std::string, std::string, std::string, std::string> *get_outgoing_call_trigger() {
    return &this->outgoing_call_trigger_;
  }
  Trigger<std::string, std::string, std::string, std::string> *get_bridge_request_trigger() {
    return &this->bridge_request_trigger_;
  }
  Trigger<std::string> *get_hangup_trigger() { return &this->hangup_trigger_; }
  Trigger<std::string> *get_call_failed_trigger() { return &this->call_failed_trigger_; }
  Trigger<> *get_destination_changed_trigger() { return &this->destination_changed_trigger_; }
  // Fires after every real phonebook mutation: HA roster push, manual
  // add/remove/set/flush, or stale-contact pruning.
  Trigger<> *get_phonebook_update_trigger() { return &this->phonebook_update_trigger_; }

  // Call state getter
  CallState get_call_state() const { return this->call_state_.load(std::memory_order_acquire); }
  const char *get_call_state_str() const { return call_state_to_str(this->call_state_.load(std::memory_order_acquire)); }

 protected:
  // Phonebook cycle helpers (see voip_settings.cpp).
  void track_csv_(const std::string &csv);  // populate seen_in_cycle_ with CSV names
  void commit_cycle_();                      // advance counters + prune via Phonebook::commit_cycle
  std::string normalize_phonebook_for_transport_(const std::string &contacts_csv);
  bool apply_roster_json_contacts_(const std::string &roster_json);
  bool maybe_auto_select_ha_first_();
  void publish_phonebook_changed_();

  bool has_microphone_() const {
#ifdef USE_ESPHOME_VOIP_STACK_MIC
    return this->microphone_ != nullptr || this->microphone_source_ != nullptr;
#else
    return false;
#endif
  }
  bool has_speaker_() const {
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
    return this->speaker_ != nullptr;
#else
    return false;
#endif
  }
  const char *audio_capability_() const {
    const bool mic = this->has_microphone_();
    const bool spk = this->has_speaker_();
    if (mic && spk) return "full_duplex";
    if (mic) return "mic_only";
    if (spk) return "speaker_only";
    return "control_only";
  }

  // setup() phases. Kept separate so setup() reads as a transaction and the
  // failure cleanup stays in one place.
  void cleanup_partial_setup_();
  bool allocate_setup_buffers_();
  bool setup_audio_helpers_();
  bool setup_transport_();
  bool start_runtime_tasks_();
  void append_audio_format_(AudioFormatList *list, const AudioFormat &format);
  void publish_initial_state_later_();
  void fail_setup_();
  void handle_call_timeouts_(uint32_t now_ms, uint32_t calling_timeout_ms);

#ifdef USE_ESPHOME_VOIP_STACK_MIC
  // TX task: mic capture + send (Core 0). Created only when a microphone is
  // configured; recv/control live in the transport task.
  static void tx_task(void *param);
  void tx_task_();

  bool is_tx_stream_ready_() const;
  void send_chunk_(const uint8_t *data, size_t length);
  void process_tx_chunk_(const uint8_t *audio_chunk);
#endif
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
  void debug_log_pcm_level_(const char *label, const uint8_t *pcm, size_t bytes,
                            const AudioFormat &format,
                            uint32_t &last_log_ms, uint32_t &frame_count);
#endif

  // Transport callbacks (registered in setup()).
  static void transport_audio_callback_(void *ctx, const TransportAudioFrame &frame);
  static void transport_sip_signal_callback_(void *ctx, const SipSignal &signal);
  static void transport_connection_callback_(void *ctx, bool connected);
  static bool transport_accept_callback_(void *ctx);
  static bool transport_dialog_active_callback_(void *ctx);
  void on_audio_received_(const TransportAudioFrame &frame);
  void on_sip_signal_received_(const SipSignal &signal);
  bool ignore_if_idle_or_stale_(const char *message_name, const std::string &call_id) const;
  void on_connection_change_(bool connected);
  bool can_accept_session_() const;

#ifdef USE_ESPHOME_VOIP_STACK_MIC
  void on_microphone_data_(const uint8_t *data, size_t len);
#endif
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
  static void rx_task(void *param);
  void rx_task_();
  void enqueue_rx_frame_(const TransportAudioFrame &frame);
  enum class SilenceReason : uint8_t {
    NONE,
    NETWORK_GAP,
    MUTED_SINK,
  };
  void play_rx_frame_(const uint8_t *pcm, size_t bytes, SilenceReason silence_reason, TickType_t ticks_to_wait);
  void play_silence_frame_(SilenceReason reason, TickType_t ticks_to_wait);
  void reset_rx_audio_();
#endif

  void set_audio_devices_active_(bool on);
  void set_in_call_(bool on);
  void start_speaker_for_current_rx_();
  void reset_peer_audio_watchdog_(bool seed_from_transport);
  void notify_audio_tasks_();

  void publish_state_();
  void publish_last_reason_(const std::string &reason);
  void publish_destination_();
  void publish_caller_(const std::string &caller_name);
  void publish_contacts_();
  void publish_transport_();
  void publish_endpoint_();
  void publish_sip_snapshot_();
  void clear_terminal_call_snapshot_();
  void request_endpoint_publish_();
  static void ip_event_handler_(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data);
  std::string local_ip_string_() const;
  std::string build_endpoint_string_() const;
  std::string build_sip_snapshot_string_() const;
  static std::string audio_format_token_(const AudioFormat &fmt);

  void set_call_state_(CallState new_state);
  void end_call_(CallEndReason reason, const std::string &detail = "");
  // Shared teardown for calling/ringing timeouts: send SIP timeout,
  // cache it as terminal, end locally.
  void fire_timeout_decline_();

  bool send_sip_ringing_(const std::string &call_id);
  bool send_sip_bye_(const std::string &call_id);
  bool send_sip_cancel_(const std::string &call_id);
  bool send_sip_final_response_(const std::string &call_id, const std::string &reason);
  bool send_sip_answer_(const std::string &call_id);
  bool send_sip_invite_(const std::string &call_id,
                       const std::string &caller_route, const std::string &caller_name,
                       const std::string &dest_route, const std::string &dest_name);

  // Components
#ifdef USE_ESPHOME_VOIP_STACK_MIC
  microphone::Microphone *microphone_{nullptr};
  microphone::MicrophoneSource *microphone_source_{nullptr};
#endif
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
  speaker::Speaker *speaker_{nullptr};
#endif

  // Call state is the public FSM source of truth. is_active() is true for
  // ringing/calling too; audio_devices_active_ only gates mic/speaker hardware
  // and is intentionally false while a call is merely ringing.
  std::atomic<bool> audio_devices_active_{false};
  std::atomic<CallState> call_state_{CallState::IDLE};  // FSM source of truth

  // Transport configuration (set from YAML before setup()). Defaults:
  TransportType protocol_{TransportType::UDP};
  size_t udp_max_payload_{UDP_SAFE_AUDIO_PAYLOAD_BYTES};
  uint16_t sip_port_{5060};
  uint16_t rtp_port_{40000};

  // Active SIP phone transport (created in setup() based on protocol_).
  std::unique_ptr<SipPhoneTransport> transport_;

  // Sensors
  text_sensor::TextSensor *state_sensor_{nullptr};
  text_sensor::TextSensor *destination_sensor_{nullptr};  // full: selected contact
  text_sensor::TextSensor *caller_sensor_{nullptr};       // full: who is calling
  text_sensor::TextSensor *contacts_sensor_{nullptr};     // full: contact count (e.g. "3 contacts")
  text_sensor::TextSensor *transport_sensor_{nullptr};    // SIP signaling transport: "udp" or "tcp"
  text_sensor::TextSensor *endpoint_sensor_{nullptr};     // HA roster row with SIP route and audio capabilities.
  text_sensor::TextSensor *last_reason_sensor_{nullptr};  // terminal reason for HA/card mirroring
  text_sensor::TextSensor *sip_snapshot_sensor_{nullptr};  // diagnostic SipPhoneState JSON
  std::string last_reason_;
  std::string last_endpoint_;
  std::string last_sip_snapshot_;
  uint32_t last_sip_snapshot_refresh_ms_{0};
  std::atomic<bool> endpoint_publish_requested_{false};
  std::string last_terminal_call_id_;
  std::string last_terminal_direction_;
  std::string last_terminal_caller_name_;
  std::string last_terminal_dest_name_;
  AudioFormat last_terminal_tx_audio_format_{DEFAULT_AUDIO_FORMAT};
  AudioFormat last_terminal_rx_audio_format_{DEFAULT_AUDIO_FORMAT};
  // Registered entities (for state sync after boot)
  switch_::Switch *auto_answer_switch_{nullptr};
  switch_::Switch *dnd_switch_{nullptr};
  bool entity_restore_applied_{false};
  number::Number *volume_number_{nullptr};
  number::Number *mic_gain_number_{nullptr};
  // Contacts management. Empty at boot; fed by the optional HA text_sensor
  // subscription plus any YAML sources wired on the on_phonebook_update trigger.
  // Slot order is stable:
  // re-add keeps the slot, only the endpoint may upgrade or replace. The
  // cycle counter on each ContactEntry advances/resets in commit_cycle()
  // and prunes once delete_contact_missing_from.updates_number is hit.
  Phonebook phonebook_;
  // Phonebook update cycle state. update_contacts() opens a cycle, set_contacts()
  // calls within the open cycle add their CSV names to seen_in_cycle_, and the
  // next update_contacts() (or the loop() timeout safeguard) commits the cycle.
  text_sensor::TextSensor *ha_phonebook_sensor_{nullptr};
  std::unordered_set<std::string> seen_in_cycle_;
  uint32_t cycle_started_at_{0};
  bool cycle_active_{false};
  uint8_t prune_threshold_{0};  // 0 = pruning disabled (default)
  static constexpr uint32_t CYCLE_TIMEOUT_MS = 10000;  // safety net for stuck cycles
  // Empty by default. HA-facing YAML should set this to hass.config.location_name
  // through voip_stack.set_ha_peer_name for default dial-plan selection.
  // phonebook entry. Direct P2P usage can leave it empty.
  std::string ha_peer_name_;
  bool use_ha_as_first_contact_{false};
  bool first_contacts_batch_committed_{false};
  std::string device_name_;  // This device's friendly name (to exclude from contacts)
  std::string extension_;  // Optional internal dial-plan alias published to HA.
  std::string last_published_destination_;
  std::string device_route_id_;  // routing key (yaml node name slug)

#ifdef USE_ESPHOME_VOIP_STACK_MIC
  // Audio buffers
  audio_core::RingBufferPtr mic_buffer_;

  // Per-iteration drain buffers, heap-allocated at setup() so the audio
  // tasks don't carry 4 KB VLAs on top of an 8 KB stack.
  size_t tx_audio_chunk_bytes_() const {
    return this->current_tx_audio_frame_bytes_.load(std::memory_order_acquire);
  }
  size_t tx_audio_max_chunk_bytes_() const {
    size_t bytes = this->tx_audio_format_.nominal_frame_bytes();
    for (uint8_t i = 0; i < this->tx_audio_formats_.count; i++) {
      bytes = std::max(bytes, this->tx_audio_formats_.formats[i].nominal_frame_bytes());
    }
    return bytes;
  }
  size_t mic_processing_samples_() const { return this->tx_audio_max_chunk_bytes_() / sizeof(int16_t); }
#endif
#ifdef USE_ESPHOME_VOIP_STACK_MIC
  uint8_t *tx_audio_chunk_{nullptr};
  size_t tx_audio_chunk_alloc_bytes_{0};
  bool read_tx_chunk_(uint8_t *audio_chunk);

  // task_stacks_in_psram_: true puts task stacks in PSRAM (saves internal
  // heap on S3/P4 with heavy AFE/MWW/LVGL load); false keeps them in
  // internal RAM (the only option on plain ESP32). Honoured by the
  // transport's own task too.
  TaskHandle_t tx_task_handle_{nullptr};
  StaticTask_t tx_task_tcb_{};
  StackType_t *tx_task_stack_{nullptr};
#endif
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
  uint8_t *rx_audio_chunk_{nullptr};
  uint8_t *rx_jitter_pcm_storage_{nullptr};
  uint8_t *rx_silence_chunk_{nullptr};
  size_t rx_audio_chunk_alloc_bytes_{0};
  std::unique_ptr<RtpJitterBuffer> rx_jitter_buffer_;
  TaskHandle_t rx_task_handle_{nullptr};
  StaticTask_t rx_task_tcb_{};
  StackType_t *rx_task_stack_{nullptr};
#endif
  bool task_stacks_in_psram_{false};
  static constexpr uint8_t kMediaTaskPriority = 5;

  std::atomic<float> volume_{1.0f};
  bool audio_debug_{false};
  AudioFormat tx_audio_format_{DEFAULT_AUDIO_FORMAT};
  AudioFormat rx_audio_format_{DEFAULT_AUDIO_FORMAT};
  AudioFormatList tx_audio_formats_{};
  AudioFormatList rx_audio_formats_{};
  AudioFormat current_caller_to_dest_format_{DEFAULT_AUDIO_FORMAT};
  AudioFormat current_dest_to_caller_format_{DEFAULT_AUDIO_FORMAT};
  AudioFormat current_tx_audio_format_{DEFAULT_AUDIO_FORMAT};
  AudioFormat current_rx_audio_format_{DEFAULT_AUDIO_FORMAT};
  void set_current_tx_audio_format_(const AudioFormat &format) {
    this->current_tx_audio_format_ = format;
    this->current_tx_audio_frame_bytes_.store(format.nominal_frame_bytes(), std::memory_order_release);
    this->current_tx_audio_frame_ms_.store(format.frame_ms == 0 ? 1 : format.frame_ms, std::memory_order_release);
  }
  std::atomic<size_t> current_tx_audio_frame_bytes_{DEFAULT_AUDIO_FORMAT.nominal_frame_bytes()};
  std::atomic<uint16_t> current_tx_audio_frame_ms_{DEFAULT_AUDIO_FORMAT.frame_ms};
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
  uint32_t audio_debug_last_tx_log_ms_{0};
  uint32_t audio_debug_last_rx_log_ms_{0};
  uint32_t audio_debug_last_mic_log_ms_{0};
  uint32_t audio_debug_last_mic_callback_ms_{0};
  uint32_t audio_debug_tx_frames_{0};
  uint32_t audio_debug_rx_frames_{0};
  uint32_t audio_debug_mic_callbacks_{0};
#endif
  std::atomic<uint32_t> media_tx_queue_drops_{0};
  std::atomic<uint32_t> media_tx_queue_depth_{0};
  std::atomic<uint32_t> media_rx_queue_drops_{0};
  std::atomic<uint32_t> media_rx_queue_depth_{0};
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
  std::atomic<uint32_t> media_tx_queue_drop_bytes_{0};
  std::atomic<uint32_t> audio_debug_rx_late_frames_{0};
  std::atomic<uint32_t> audio_debug_rx_missing_frames_{0};
  std::atomic<uint32_t> audio_debug_rx_duplicate_frames_{0};
  std::atomic<uint32_t> audio_debug_rx_silence_frames_{0};
  std::atomic<uint32_t> audio_debug_speaker_short_writes_{0};
#endif
  uint32_t rx_underrun_start_ms_{0};

  // First peer audio frame closes the 200 OK echo loop.
  std::atomic<bool> first_audio_received_{false};
  std::atomic<uint32_t> last_peer_audio_ms_{0};
  std::atomic<uint32_t> media_timeout_rtp_rx_packets_{0};

  bool auto_answer_{true};
  bool do_not_disturb_{false};

  // SIP call timeouts (0 = disabled). Pending outbound calls send CANCEL.
  uint32_t calling_timeout_ms_{0};
  uint32_t ringing_timeout_ms_{0};
  uint32_t ringing_start_time_{0};
  uint32_t calling_start_time_{0};
  static constexpr uint32_t INVITE_NO_RESPONSE_TIMEOUT_MS = 5000;
  static constexpr uint32_t MEDIA_TIMEOUT_MS = 15000;

  // === SIP call state ===
  // SIP Call-ID, opaque and echoed by SIP dialogs.
  // All fields are accessed from main loop AND transport recv task; only
  // the helpers below (set/snapshot/clear) are allowed to touch them.
  mutable Mutex call_state_mutex_;
  std::string current_call_id_;
  std::string current_caller_route_id_;
  std::string current_caller_name_;
  std::string current_dest_route_id_;
  std::string current_dest_name_;
  // Last terminal SIP final response: replayed briefly when an INVITE with
  // the same Call-ID arrives again. The cache is time-limited so a later real
  // call between the same two devices cannot inherit the previous reason.
  static constexpr uint32_t TERMINAL_RESPONSE_REPLAY_MS = 1500;
  std::string last_terminal_response_call_id_;
  std::string last_terminal_response_reason_;
  uint32_t last_terminal_response_ms_{0};

  struct CallSnapshot {
    std::string call_id;
    std::string caller_route;
    std::string caller_name;
    std::string dest_route;
    std::string dest_name;
  };

  // === Call-identity helpers (mutex-protected) ===
  void set_call_identity_(const std::string &call_id,
                          const std::string &caller_route, const std::string &caller_name,
                          const std::string &dest_route, const std::string &dest_name);
  void clear_call_identity_();
  CallSnapshot snapshot_call_identity_() const;
  std::string get_current_call_id_() const;
  void set_terminal_response_(const std::string &call_id, const std::string &reason);
  void clear_terminal_response_();
  void snapshot_terminal_response_(std::string *call_id, std::string *reason, uint32_t *age_ms) const;
  bool recent_terminal_call_(const std::string &call_id, std::string *reason = nullptr) const;
  bool ensure_mic_processing_buffer_();

  std::atomic<float> mic_gain_{1.0f};
  float mic_gain_db_{0.0f};  // UI-friendly dB value (persisted)

  // === Settings persistence (local flash) ===
  static constexpr uint8_t SETTINGS_VERSION = 1;

  struct StoredSettings {
    uint8_t version{SETTINGS_VERSION};
    uint8_t volume_pct{100};
    int8_t mic_gain_db{0};
    uint8_t reserved{0};
  };

  ESPPreferenceObject settings_pref_{};
  bool suppress_save_{false};
  bool save_scheduled_{false};

  void load_settings_();
  void schedule_save_settings_();
  void save_settings_();

  bool dc_offset_removal_{false};       // for mics with DC bias (SPH0645)
  bool buffers_in_psram_{false};
#ifdef USE_ESPHOME_VOIP_STACK_MIC
  DcBlockerState dc_blocker_;

  // Pre-allocated to avoid task-stack VLAs.
  std::atomic<int16_t *> mic_converted_{nullptr};  // 512 samples, lazy for optional mic processing
#endif

  Trigger<> ringing_trigger_;
  Trigger<> in_call_trigger_;
  Trigger<> idle_trigger_;
  Trigger<> calling_trigger_;
  Trigger<> dest_ringing_trigger_;
  Trigger<std::string, std::string, std::string, std::string> incoming_call_trigger_;
  Trigger<std::string, std::string, std::string, std::string> outgoing_call_trigger_;
  Trigger<std::string, std::string, std::string, std::string> bridge_request_trigger_;
  Trigger<std::string> hangup_trigger_;
  Trigger<std::string> call_failed_trigger_;
  Trigger<> destination_changed_trigger_;
  Trigger<> phonebook_update_trigger_;
  CallbackManager<void(CallState)> state_callback_{};
};

class VoipStackSwitch : public switch_::Switch, public Parented<VoipStack> {
 public:
  void write_state(bool state) override {
    if (state) {
      this->parent_->start();
    } else {
      this->parent_->stop();
    }
    this->publish_state(state);
  }
};

class VoipStackVolume : public number::Number, public Parented<VoipStack> {
 public:
  void control(float value) override {
    if (value != value) value = 0.0f;
    if (value < 0.0f) value = 0.0f;
    if (value > 100.0f) value = 100.0f;
    this->parent_->set_volume(value / 100.0f);
    this->publish_state(value);
  }
};

class VoipStackMicGain : public number::Number, public Parented<VoipStack> {
 public:
  void control(float value) override {
    if (value != value) value = 0.0f;
    if (value < -20.0f) value = -20.0f;
    if (value > 20.0f) value = 20.0f;
    this->parent_->set_mic_gain_db(value);
    this->publish_state(value);
  }
};

class VoipStackAutoAnswer : public switch_::Switch, public Parented<VoipStack> {
 public:
  void write_state(bool state) override {
    this->parent_->set_auto_answer(state);
    this->publish_state(state);
  }
};

class VoipStackDndSwitch : public switch_::Switch, public Parented<VoipStack> {
 public:
  void write_state(bool state) override {
    this->parent_->set_do_not_disturb(state);
    this->publish_state(state);
  }
};

class VoipCallButton : public button::Button, public Parented<VoipStack> {
 protected:
  void press_action() override { this->parent_->call_toggle(); }
};

class VoipNextContactButton : public button::Button, public Parented<VoipStack> {
 protected:
  void press_action() override { this->parent_->next_contact(); }
};

class VoipPreviousContactButton : public button::Button, public Parented<VoipStack> {
 protected:
  void press_action() override { this->parent_->prev_contact(); }
};

class VoipDeclineButton : public button::Button, public Parented<VoipStack> {
 protected:
  void press_action() override { this->parent_->decline_call(); }
};

}  // namespace voip_stack
}  // namespace esphome

// Action / Condition templates. Included after the namespace closes
// because actions.h re-opens it and needs the full VoipStack type.
#include "actions.h"

#endif  // USE_ESP32
