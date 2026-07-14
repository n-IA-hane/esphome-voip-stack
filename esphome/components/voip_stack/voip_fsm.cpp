#include "voip_stack.h"

#ifdef USE_ESP32

#include <cstring>

#include <esp_random.h>
#include <esp_system.h>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace voip_stack {

static const char *const TAG = "voip_stack.fsm";

#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
static audio::AudioStreamInfo audio_stream_info_from_format(const AudioFormat &format) {
  return audio::AudioStreamInfo(audio_format_bits_per_sample(format), format.channels, format.sample_rate);
}
#endif

// === Call-identity helpers ===
// Mutex-guarded so the recv task and the main loop never observe a
// half-written std::string assignment to the per-call fields.

void VoipStack::set_call_identity_(const std::string &call_id,
                                      const std::string &caller_route,
                                      const std::string &caller_name,
                                      const std::string &dest_route,
                                      const std::string &dest_name) {
  LockGuard l(this->call_state_mutex_);
  this->current_call_id_ = call_id;
  this->current_caller_route_id_ = caller_route;
  this->current_caller_name_ = caller_name;
  this->current_dest_route_id_ = dest_route;
  this->current_dest_name_ = dest_name;
}

void VoipStack::clear_call_identity_() {
  LockGuard l(this->call_state_mutex_);
  this->current_call_id_.clear();
  this->current_caller_route_id_.clear();
  this->current_caller_name_.clear();
  this->current_dest_route_id_.clear();
  this->current_dest_name_.clear();
}

VoipStack::CallSnapshot VoipStack::snapshot_call_identity_() const {
  LockGuard l(this->call_state_mutex_);
  return CallSnapshot{this->current_call_id_,
                      this->current_caller_route_id_, this->current_caller_name_,
                      this->current_dest_route_id_, this->current_dest_name_};
}

std::string VoipStack::get_current_call_id_() const {
  LockGuard l(this->call_state_mutex_);
  return this->current_call_id_;
}

void VoipStack::set_terminal_response_(const std::string &call_id, const std::string &reason) {
  LockGuard l(this->call_state_mutex_);
  this->last_terminal_response_call_id_ = call_id;
  this->last_terminal_response_reason_ = reason;
  this->last_terminal_response_ms_ = millis();
}

void VoipStack::clear_terminal_response_() {
  LockGuard l(this->call_state_mutex_);
  this->last_terminal_response_call_id_.clear();
  this->last_terminal_response_reason_.clear();
  this->last_terminal_response_ms_ = 0;
}

void VoipStack::snapshot_terminal_response_(std::string *call_id, std::string *reason, uint32_t *age_ms) const {
  LockGuard l(this->call_state_mutex_);
  if (call_id) *call_id = this->last_terminal_response_call_id_;
  if (reason) *reason = this->last_terminal_response_reason_;
  if (age_ms) *age_ms = this->last_terminal_response_ms_ == 0
      ? UINT32_MAX
      : millis() - this->last_terminal_response_ms_;
}

bool VoipStack::recent_terminal_call_(const std::string &call_id, std::string *reason) const {
  if (call_id.empty()) {
    return false;
  }
  std::string cached_call_id;
  std::string cached_reason;
  uint32_t cached_age_ms = UINT32_MAX;
  this->snapshot_terminal_response_(&cached_call_id, &cached_reason, &cached_age_ms);
  if (cached_call_id != call_id || cached_age_ms > TERMINAL_RESPONSE_REPLAY_MS) {
    return false;
  }
  if (reason != nullptr) {
    *reason = cached_reason;
  }
  return true;
}

bool VoipStack::send_sip_ringing_(const std::string &call_id) {
  return this->transport_ != nullptr && this->transport_->send_ringing(call_id);
}

bool VoipStack::send_sip_bye_(const std::string &call_id) {
  return this->transport_ != nullptr && this->transport_->send_bye(call_id);
}

bool VoipStack::send_sip_cancel_(const std::string &call_id) {
  return this->transport_ != nullptr && this->transport_->send_cancel(call_id);
}

bool VoipStack::send_sip_final_response_(const std::string &call_id, const std::string &reason) {
  if (this->transport_ == nullptr) return false;
  uint16_t status = 486;
  if (reason == kReasonMediaIncompatible) status = 488;
  else if (reason == kReasonCancelled) status = 487;
  else if (reason == kReasonDeclined) status = 603;
  return this->transport_->send_final_response(call_id, status, reason);
}

bool VoipStack::send_sip_answer_(const std::string &call_id) {
  const CurrentMediaFormats formats = this->snapshot_current_media_formats_();
  return this->transport_ != nullptr &&
         this->transport_->send_answer(call_id, formats.caller_to_dest, formats.dest_to_caller);
}

bool VoipStack::send_sip_invite_(const std::string &call_id,
                                   const std::string &caller_route,
                                   const std::string &caller_name,
                                   const std::string &dest_route,
                                   const std::string &dest_name) {
  return this->transport_ != nullptr &&
         this->transport_->send_invite(call_id, caller_route, caller_name, dest_route, dest_name);
}

bool VoipStack::ignore_if_idle_or_stale_(const char *message_name,
                                           const std::string &call_id) const {
  if (this->call_state_.load(std::memory_order_acquire) == CallState::IDLE) {
    ESP_LOGD(TAG, "ignoring %s while idle (call_id=%s)", message_name, call_id.c_str());
    return true;
  }
  if (!call_id.empty()) {
    const std::string current_cid = this->get_current_call_id_();
    if (!current_cid.empty() && call_id != current_cid) {
      ESP_LOGD(TAG, "ignoring stale %s for call_id=%s (current=%s)",
               message_name, call_id.c_str(), current_cid.c_str());
      return true;
    }
  }
  return false;
}

// === Lifecycle / user actions ===

void VoipStack::start() {
  if (this->call_state_.load(std::memory_order_acquire) != CallState::IDLE) {
    ESP_LOGW(TAG, "Cannot start call: already %s", this->get_call_state_str());
    return;
  }

  std::string dial_ip = this->get_current_contact_ip();
  uint16_t dial_port = this->get_current_contact_port();
  uint16_t dial_rtp_port = this->get_current_contact_rtp_port();
  bool dial_sip_tcp = this->get_current_contact_sip_transport_tcp();

  const std::string selected_dest = this->get_current_destination();
  const bool route_via_ha = !this->ha_peer_name_.empty() && selected_dest == this->ha_peer_name_;
  const std::string dest_name = !this->pending_dialplan_target_.empty()
      ? this->pending_dialplan_target_
      : selected_dest.empty()
      ? (this->ha_peer_name_.empty() ? std::string("(unknown)") : this->ha_peer_name_)
      : selected_dest;
  const std::string caller_route =
      this->device_route_id_.empty() ? this->device_name_ : this->device_route_id_;
  const std::string &dest_route = dest_name;
  const std::string call_id = caller_route + "-" + std::to_string(millis()) + "-" +
                              std::to_string(static_cast<unsigned>(esp_random())) +
                              "@" + (this->device_route_id_.empty() ? std::string("esp") : this->device_route_id_);
  if (dial_ip.empty() || dial_port == 0) {
    ESP_LOGE(TAG, "%s: SIP outgoing needs a SIP contact with host+port for '%s'", this->device_name_.c_str(),
             dest_name.c_str());
    // end_call_ intentionally ignores IDLE. Publish a short-lived CALLING
    // attempt with identity so invalid dial-plan routes still produce a
    // terminal reason and on_call_failed event instead of failing silently.
    this->clear_terminal_call_snapshot_();
    this->set_call_identity_(call_id, caller_route, this->device_name_, dest_route, dest_name);
    this->clear_terminal_response_();
    this->set_current_media_formats_(this->tx_audio_format_, this->rx_audio_format_, this->tx_audio_format_,
                                     this->rx_audio_format_);
    this->set_call_state_(CallState::CALLING);
    this->end_call_(CallEndReason::TRANSPORT_UNREACHABLE);
    return;
  }

  this->clear_terminal_call_snapshot_();
  this->set_remote_sip_transport_tcp(dial_sip_tcp);
  this->set_remote_endpoint(dial_ip, dial_port, dial_rtp_port);
  this->set_call_identity_(call_id, caller_route, this->device_name_,
                            dest_route, dest_name);
  this->clear_terminal_response_();

  const std::string remote_uri = "sip:" + dest_route + "@" + dial_ip + ":" + std::to_string(dial_port);
  this->defer([this, call_id, caller = this->device_name_, dest_name, remote_uri, route_via_ha]() {
    this->outgoing_call_trigger_.trigger(call_id, caller, dest_name, remote_uri);
    if (route_via_ha) {
      this->bridge_request_trigger_.trigger(call_id, caller, dest_name, remote_uri);
    }
  });

  ESP_LOGI(TAG, "%s -> %s: calling... (call_id=%s)",
           this->device_name_.c_str(), dest_name.c_str(), call_id.c_str());
  this->set_current_media_formats_(this->tx_audio_format_, this->rx_audio_format_, this->tx_audio_format_,
                                   this->rx_audio_format_);
  this->set_audio_devices_active_(true);
  this->set_call_state_(CallState::CALLING);
  this->calling_start_time_ = millis();

  if (this->transport_ == nullptr || !this->transport_->originate(dial_ip, dial_port)) {
    ESP_LOGE(TAG, "%s: SIP originate to %s:%u failed",
             this->device_name_.c_str(), dial_ip.c_str(), (unsigned) dial_port);
    this->pending_dialplan_target_.clear();
    this->end_call_(CallEndReason::TRANSPORT_UNREACHABLE);
    this->set_audio_devices_active_(false);
    if (this->transport_) this->transport_->disconnect();
    return;
  }

  if (!this->send_sip_invite_(call_id, caller_route, this->device_name_,
                              dest_route, dest_name)) {
    ESP_LOGE(TAG, "SIP INVITE send failed");
    this->pending_dialplan_target_.clear();
    this->end_call_(CallEndReason::PROTOCOL_ERROR);
    this->set_audio_devices_active_(false);
    if (this->transport_) this->transport_->disconnect();
    return;
  }
  this->pending_dialplan_target_.clear();
}

void VoipStack::stop() {
  // Terminal states already own their reason and deferred IDLE transition. A
  // repeated button/API stop must not replace that snapshot with local_hangup.
  if (!this->is_active()) {
    if (this->audio_devices_active_.load(std::memory_order_acquire)) {
      this->set_audio_devices_active_(false);
    }
    return;
  }

  const std::string call_id = this->get_current_call_id_();
  const CallState state = this->call_state_.load(std::memory_order_acquire);
  ESP_LOGI(TAG, "%s: hanging up (state=%s call_id=%s)",
           this->device_name_.c_str(),
           call_state_to_str(state),
           call_id.c_str());

  if (!call_id.empty()) {
    this->set_terminal_response_(call_id, "");
  }
  this->end_call_(CallEndReason::LOCAL_HANGUP);
  bool waiting_for_terminal_response = false;
  if (this->transport_ && this->transport_->is_connected() && !call_id.empty()) {
    if (state == CallState::IN_CALL) {
      waiting_for_terminal_response = this->send_sip_bye_(call_id);
    } else if (state == CallState::RINGING) {
      this->send_sip_final_response_(call_id, kReasonDeclined);
    } else {
      waiting_for_terminal_response = this->send_sip_cancel_(call_id);
    }
  }
  this->set_audio_devices_active_(false);
  this->set_in_call_(false);
  if (this->transport_ && !waiting_for_terminal_response) this->transport_->disconnect();
}

void VoipStack::answer_call() {
  if (!this->is_ringing()) {
    ESP_LOGW(TAG, "Cannot answer: not ringing (state=%s)", this->get_call_state_str());
    return;
  }

  if (!this->is_connected()) {
    ESP_LOGW(TAG, "Cannot answer: no connection");
    return;
  }

  const std::string call_id = this->get_current_call_id_();
  ESP_LOGI(TAG, "%s: answering call (call_id=%s)",
           this->device_name_.c_str(), call_id.c_str());
  this->set_audio_devices_active_(true);
  if (this->transport_ && !this->transport_->start_audio_path()) {
    ESP_LOGE(TAG, "%s: RTP start failed while answering call", this->device_name_.c_str());
    this->end_call_(CallEndReason::TRANSPORT_UNREACHABLE);
    this->set_audio_devices_active_(false);
    this->transport_->disconnect();
    return;
  }
  this->set_call_state_(CallState::CONNECTING);
  if (this->transport_ && !call_id.empty()) {
    if (!this->send_sip_answer_(call_id)) {
      ESP_LOGE(TAG, "%s: failed to send SIP answer", this->device_name_.c_str());
      this->end_call_(CallEndReason::PROTOCOL_ERROR);
      this->set_audio_devices_active_(false);
      this->transport_->disconnect();
      return;
    }
  }
  this->set_in_call_(true);  // also publishes IN_CALL state
}

void VoipStack::decline_call(const std::string &reason) {
  // A SIP rejection is a pre-call final response. Mid-call termination uses
  // BYE, so a YAML decline_call() during IN_CALL falls back to stop() to avoid
  // a misleading "declined" reaching the peer after we'd already accepted.
  if (this->call_state_.load(std::memory_order_acquire) == CallState::IN_CALL) {
    ESP_LOGD(TAG, "decline_call() during IN_CALL -> falling back to stop()");
    this->stop();
    return;
  }
  const bool cancelling_outgoing = this->is_calling();
  if (this->is_ringing()) {
    ESP_LOGI(TAG, "%s: declining incoming call", this->device_name_.c_str());
  } else if (this->is_calling()) {
    ESP_LOGI(TAG, "%s: cancelling SIP INVITE to %s",
             this->device_name_.c_str(), this->get_current_destination().c_str());
  } else {
    ESP_LOGW(TAG, "Cannot decline: no call in progress (state=%s)",
             this->get_call_state_str());
    return;
  }

  // Empty reason => peer treats as remote_hangup; non-empty surfaces as
  // user-visible "Call ended: X".
  const std::string call_id = this->get_current_call_id_();
  // Cached so a retransmitted INVITE with this Call-ID replays the same
  // final response instead of being seen as a fresh ring.
  this->set_terminal_response_(call_id, reason);

  this->end_call_(
      reason.empty() ? CallEndReason::LOCAL_HANGUP : CallEndReason::DECLINED,
      reason);
  bool waiting_for_terminal_response = false;
  if (this->transport_ && this->transport_->is_connected() && !call_id.empty()) {
    const bool sent = this->send_sip_final_response_(call_id, reason);
    // For an outgoing INVITE send_final_response() is a CANCEL. Keep the SIP
    // dialog alive for its 200/487 exchange and bounded retransmission timer.
    waiting_for_terminal_response = cancelling_outgoing && sent;
  }
  this->set_audio_devices_active_(false);

  // Disconnect after end_call_ so on_connection_change_(false) sees IDLE
  // and skips the REMOTE_HANGUP path that would mask our trigger.
  if (this->transport_ && this->transport_->is_connected() && !waiting_for_terminal_response) {
    this->transport_->disconnect();
  }
}

void VoipStack::call_toggle() {
  if (this->is_ringing()) {
    this->answer_call();
  } else if (this->is_active()) {
    this->stop();
  } else {
    this->start();
  }
}

// === State helpers ===

const char *VoipStack::get_state_str() const {
  return call_state_to_str(this->call_state_.load(std::memory_order_acquire));
}

void VoipStack::publish_state_() {
#ifdef USE_TEXT_SENSOR
  if (this->state_sensor_ != nullptr) {
    this->state_sensor_->publish_state(this->get_state_str());
  }
#endif
  this->publish_sip_snapshot_();
}

void VoipStack::clear_terminal_call_snapshot_() {
  this->last_terminal_call_id_.clear();
  this->last_terminal_direction_.clear();
  this->last_terminal_caller_name_.clear();
  this->last_terminal_dest_name_.clear();
  this->last_terminal_tx_audio_format_ = DEFAULT_AUDIO_FORMAT;
  this->last_terminal_rx_audio_format_ = DEFAULT_AUDIO_FORMAT;
}

void VoipStack::publish_last_reason_(const std::string &reason) {
  if (this->last_reason_ == reason) {
    return;
  }
  this->last_reason_ = reason;
#ifdef USE_TEXT_SENSOR
  if (this->last_reason_sensor_ != nullptr) {
    this->last_reason_sensor_->publish_state(reason);
  }
#endif
  this->publish_sip_snapshot_();
}

void VoipStack::set_audio_devices_active_(bool on) {
  bool was = this->audio_devices_active_.exchange(on, std::memory_order_acq_rel);
  if (was == on) return;
  this->notify_audio_tasks_();

  if (on) {
    this->reset_peer_audio_watchdog_(true);

#ifdef USE_ESPHOME_VOIP_STACK_MIC
    if (this->microphone_) {
      this->microphone_->start();
    }
    if (this->microphone_source_) {
      this->microphone_source_->start();
    }
#endif
  } else {
    this->reset_peer_audio_watchdog_(false);

#ifdef USE_ESPHOME_VOIP_STACK_MIC
    if (this->microphone_) {
      this->microphone_->stop();
    }
    if (this->microphone_source_) {
      this->microphone_source_->stop();
    }
#endif
  }
}

void VoipStack::start_speaker_for_current_rx_() {
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
  if (this->speaker_ == nullptr) return;
  this->speaker_->set_audio_stream_info(audio_stream_info_from_format(this->get_current_rx_audio_format_()));
  this->speaker_->start();
#endif
}

void VoipStack::reset_peer_audio_watchdog_(bool seed_from_transport) {
  uint32_t baseline_rx_packets = 0;
  if (seed_from_transport && this->transport_ != nullptr) {
    baseline_rx_packets = this->transport_->snapshot().rtp_rx_packets;
  }
  this->first_audio_received_.store(false, std::memory_order_release);
  // Seed on media start so a peer that never sends its first RTP packet is
  // covered by the same timeout as a stream that stops later.
  uint32_t watchdog_start = seed_from_transport ? millis() : 0;
  if (seed_from_transport && watchdog_start == 0) watchdog_start = 1;
  this->last_peer_audio_ms_.store(watchdog_start, std::memory_order_release);
  this->media_timeout_rtp_rx_packets_.store(baseline_rx_packets, std::memory_order_release);
}

void VoipStack::set_in_call_(bool on) {
  if (on) {
    if (this->call_state_.load(std::memory_order_acquire) == CallState::IN_CALL) return;
    const std::string call_id = this->get_current_call_id_();
    std::string terminal_reason;
    if (this->recent_terminal_call_(call_id, &terminal_reason)) {
      ESP_LOGD(TAG, "Refusing IN_CALL for terminal call_id=%s reason=%s",
               call_id.c_str(),
               terminal_reason.empty() ? "(none)" : terminal_reason.c_str());
      this->publish_state_();
      return;
    }
  }
  if (on) {
    // A new media leg starts here even if audio_devices_active_ was already
    // true because a previous call did not fully drain yet. Do not inherit
    // peer-audio liveness from the previous dialog, or the media watchdog can
    // fire early.
    this->reset_peer_audio_watchdog_(true);

    // Drop stale frames from the previous call before audio resumes.
#ifdef USE_ESPHOME_VOIP_STACK_MIC
    if (this->mic_buffer_) {
      this->mic_buffer_->reset();
    }
    this->media_tx_queue_depth_.store(0, std::memory_order_relaxed);
    this->media_tx_queue_drops_.store(0, std::memory_order_relaxed);
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
    this->media_tx_queue_drop_bytes_.store(0, std::memory_order_relaxed);
#endif
#endif
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
    this->reset_rx_audio_();
    this->start_speaker_for_current_rx_();
#endif
#ifdef USE_ESPHOME_VOIP_STACK_MIC
    this->dc_blocker_ = {};
#endif

    // The state flip is the media gate observed by the audio tasks.
    this->set_call_state_(CallState::IN_CALL);  // publishes state internally
  } else {
    if (this->transport_) this->transport_->stop_audio_path();
#ifdef USE_ESPHOME_VOIP_STACK_MIC
    if (this->mic_buffer_) this->mic_buffer_->reset();
    this->media_tx_queue_depth_.store(0, std::memory_order_relaxed);
    this->media_tx_queue_drops_.store(0, std::memory_order_relaxed);
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
    this->media_tx_queue_drop_bytes_.store(0, std::memory_order_relaxed);
#endif
#endif
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
    this->reset_rx_audio_();
    if (this->speaker_ != nullptr && this->speaker_->is_running()) {
      this->speaker_->stop();
    }
#endif
    this->publish_state_();
  }
}

void VoipStack::notify_audio_tasks_() {
#ifdef USE_ESPHOME_VOIP_STACK_MIC
  if (this->tx_task_handle_ != nullptr) {
    xTaskNotifyGive(this->tx_task_handle_);
  }
#endif
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
  if (this->rx_task_handle_ != nullptr) {
    xTaskNotifyGive(this->rx_task_handle_);
  }
#endif
}

void VoipStack::set_call_state_(CallState new_state) {
  if (new_state != CallState::IDLE) {
    this->enable_loop_soon_any_context();
  }
  // Atomic exchange so concurrent callers (SIP receive task vs main-loop
  // ringing_timeout) can't both fire the triggers; the loser sees
  // old==new and returns.
  CallState old_state = this->call_state_.exchange(new_state, std::memory_order_acq_rel);
  if (old_state == new_state) return;
  if ((old_state == CallState::IN_CALL) != (new_state == CallState::IN_CALL)) {
    this->notify_audio_tasks_();
  }

  if (new_state == CallState::IN_CALL && !this->current_caller_name_.empty()) {
    ESP_LOGI(TAG, "%s: %s -> %s with %s", this->device_name_.c_str(),
             call_state_to_str(old_state), call_state_to_str(new_state),
             this->current_caller_name_.c_str());
  } else {
    ESP_LOGI(TAG, "%s: %s -> %s", this->device_name_.c_str(),
             call_state_to_str(old_state), call_state_to_str(new_state));
  }

  // Defer trigger fires to the main loop: call-state changes come from
  // the voip_srv task on Core 1, but on_* actions often touch LVGL
  // which must run on the main loop. Running inline trips the WD.
  this->defer([this, new_state]() { this->state_callback_.call(new_state); });
  const CallSnapshot trigger_call = this->snapshot_call_identity_();
  std::string trigger_peer = trigger_call.caller_name == this->device_name_
                                 ? trigger_call.dest_name
                                 : trigger_call.caller_name;
  if (trigger_peer.empty()) trigger_peer = this->get_current_destination();
  switch (new_state) {
    case CallState::IDLE:
      this->defer([this]() { this->idle_trigger_.trigger(); });
      this->defer([this]() { this->publish_caller_(""); });
      break;
    case CallState::CALLING:
      this->publish_last_reason_("");
      this->defer([this, trigger_peer]() { this->calling_trigger_.trigger(trigger_peer); });
      break;
    case CallState::REMOTE_RINGING:
      this->publish_last_reason_("");
      this->defer([this, trigger_peer]() { this->dest_ringing_trigger_.trigger(trigger_peer); });
      break;
    case CallState::RINGING:
      this->publish_last_reason_("");
      this->defer([this, trigger_peer]() { this->ringing_trigger_.trigger(trigger_peer); });
      break;
    case CallState::CONNECTING:
      this->publish_last_reason_("");
      break;
    case CallState::IN_CALL:
      this->publish_last_reason_("");
      this->defer([this, trigger_peer]() { this->in_call_trigger_.trigger(trigger_peer); });
      break;
    case CallState::TERMINATING:
    case CallState::BUSY:
    case CallState::DECLINED:
    case CallState::CANCELLED:
    case CallState::MEDIA_INCOMPATIBLE:
    case CallState::TRANSPORT_UNREACHABLE:
    case CallState::AUTH_REQUIRED_UNSUPPORTED:
      break;
  }

  this->publish_state_();
}

void VoipStack::end_call_(CallEndReason reason, const std::string &detail) {
  if (this->call_state_.load(std::memory_order_acquire) == CallState::IDLE) return;
  this->pending_dialplan_target_.clear();

  std::string reason_str = detail.empty() ? call_end_reason_to_str(reason) : detail;
  const CallSnapshot call = this->snapshot_call_identity_();
  std::string peer = call.caller_name == this->device_name_ ? call.dest_name : call.caller_name;
  if (peer.empty()) peer = this->get_current_destination();
  const std::string call_id = call.call_id;
  this->last_terminal_call_id_ = call.call_id;
  this->last_terminal_caller_name_ = call.caller_name;
  this->last_terminal_dest_name_ = call.dest_name;
  const CurrentMediaFormats formats = this->snapshot_current_media_formats_();
  this->last_terminal_tx_audio_format_ = formats.tx;
  this->last_terminal_rx_audio_format_ = formats.rx;
  if (!call.caller_name.empty() && call.caller_name == this->device_name_) {
    this->last_terminal_direction_ = "outgoing";
  } else if (!call.dest_name.empty() && call.dest_name == this->device_name_) {
    this->last_terminal_direction_ = "incoming";
  } else {
    this->last_terminal_direction_.clear();
  }
  ESP_LOGI(TAG, "%s: call ended (%s) call_id=%s",
           this->device_name_.c_str(), reason_str.c_str(),
           call_id.empty() ? "(none)" : call_id.c_str());
  this->publish_last_reason_(reason_str);
  CallState terminal_state = CallState::TERMINATING;
  switch (reason) {
    case CallEndReason::BUSY:
      terminal_state = CallState::BUSY;
      break;
    case CallEndReason::DECLINED:
      terminal_state = CallState::DECLINED;
      break;
    case CallEndReason::CANCELLED:
      terminal_state = CallState::CANCELLED;
      break;
    case CallEndReason::MEDIA_INCOMPATIBLE:
      terminal_state = CallState::MEDIA_INCOMPATIBLE;
      break;
    case CallEndReason::TRANSPORT_UNREACHABLE:
      terminal_state = CallState::TRANSPORT_UNREACHABLE;
      break;
    case CallEndReason::AUTH_REQUIRED_UNSUPPORTED:
    case CallEndReason::PROXY_AUTH_REQUIRED_UNSUPPORTED:
      terminal_state = CallState::AUTH_REQUIRED_UNSUPPORTED;
      break;
    default:
      terminal_state = CallState::TERMINATING;
      break;
  }
  this->set_call_state_(terminal_state);

  if (reason == CallEndReason::DECLINED ||
      reason == CallEndReason::TIMEOUT ||
      reason == CallEndReason::CANCELLED ||
      reason == CallEndReason::TRANSPORT_UNREACHABLE ||
      reason == CallEndReason::MEDIA_INCOMPATIBLE ||
      reason == CallEndReason::MEDIA_TIMEOUT ||
      reason == CallEndReason::AUTH_REQUIRED_UNSUPPORTED ||
      reason == CallEndReason::PROXY_AUTH_REQUIRED_UNSUPPORTED ||
      reason == CallEndReason::BUSY ||
      reason == CallEndReason::PROTOCOL_ERROR) {
    this->defer([this, peer, reason_str]() { this->call_failed_trigger_.trigger(peer, reason_str); });
  } else {
    this->defer([this, peer, reason_str]() { this->hangup_trigger_.trigger(peer, reason_str); });
  }
  // Clear per-call identity. Terminal response cache stays so a duplicate
  // INVITE can replay the same final response; cleared by the next accepted
  // INVITE.
  this->clear_call_identity_();

  this->defer([this]() {
    const CallState state = this->call_state_.load(std::memory_order_acquire);
    switch (state) {
      case CallState::TERMINATING:
      case CallState::BUSY:
      case CallState::DECLINED:
      case CallState::CANCELLED:
      case CallState::MEDIA_INCOMPATIBLE:
      case CallState::TRANSPORT_UNREACHABLE:
      case CallState::AUTH_REQUIRED_UNSUPPORTED:
        this->set_call_state_(CallState::IDLE);
        this->publish_destination_();
        break;
      default:
        break;
    }
  });
}

// === Transport callbacks ===

void VoipStack::on_audio_received_(const TransportAudioFrame &frame) {
  const CallState state = this->call_state_.load(std::memory_order_acquire);
  if (state != CallState::CONNECTING && state != CallState::IN_CALL) {
    return;
  }
  const AudioFormat rx_format = this->get_current_rx_audio_format_();
  const size_t expected = rx_format.nominal_frame_bytes();
  if (frame.bytes != expected) {
    ESP_LOGW(TAG,
             "Dropping VoIP audio frame with wrong size: got %u bytes, "
             "expected %u for rx format %u:%u:%u:%u",
             (unsigned) frame.bytes, (unsigned) expected, (unsigned) rx_format.sample_rate,
             (unsigned) rx_format.pcm_format, (unsigned) rx_format.channels, (unsigned) rx_format.frame_ms);
    return;
  }
#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG
  if (this->audio_debug_) {
    this->debug_log_pcm_level_("rx_network", frame.pcm, frame.bytes,
                               rx_format,
                               this->audio_debug_last_rx_log_ms_, this->audio_debug_rx_frames_);
  }
#endif

  // First inbound audio is the strongest "call established" signal; gates
  // the 200 OK echo loop in on_sip_signal_received_() and arms media timeout.
  this->first_audio_received_.store(true, std::memory_order_release);
  this->last_peer_audio_ms_.store(millis(), std::memory_order_release);
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
  if (this->speaker_) {
    this->enqueue_rx_frame_(frame);
  }
#endif

}

void VoipStack::on_sip_signal_received_(const SipSignal &msg) {
  const std::string &in_call_id = msg.call_id;
  const std::string &in_caller_route = msg.caller_route;
  const std::string &in_caller_name = msg.caller_name;
  const std::string &in_dest_route = msg.dest_route;
  const std::string &in_dest_name = msg.dest_name;
  const std::string &in_reason = msg.reason;

  switch (msg.type) {
    case SipSignalType::INVITE: {
      const std::string &incoming_cid = in_call_id;
      const std::string active_cid = this->get_current_call_id_();
      const CallState state = this->call_state_.load(std::memory_order_acquire);

      if (state == CallState::RINGING) {
        if (incoming_cid == active_cid) {
          ESP_LOGD(TAG, "INVITE retransmit while RINGING - re-sending 180");
          this->send_sip_ringing_(active_cid);
        } else {
          ESP_LOGW(TAG, "%s: another caller while RINGING - 486 Busy Here",
                   this->device_name_.c_str());
          this->send_sip_final_response_(incoming_cid, kReasonBusy);
        }
        break;
      }
      if (state == CallState::IN_CALL) {
        if (incoming_cid == active_cid) {
          ESP_LOGD(TAG, "INVITE retransmit while IN_CALL - re-sending 200 OK");
          this->send_sip_answer_(active_cid);
        } else {
          ESP_LOGW(TAG, "%s: another caller while IN_CALL - 486 Busy Here",
                   this->device_name_.c_str());
          this->send_sip_final_response_(incoming_cid, kReasonBusy);
        }
        break;
      }
      if (state == CallState::CALLING) {
        if (incoming_cid == active_cid) {
          ESP_LOGD(TAG, "INVITE echo of our own call_id while CALLING - ignored");
          break;
        }
        // Glare: both ends dialed each other. Recognised when the
        // inbound caller_name matches our current dest. Anything else is
        // a third-party collision (486 Busy Here).
        const auto active = this->snapshot_call_identity_();
        const bool is_glare = !in_caller_name.empty() &&
                              in_caller_name == active.dest_name;
        if (!is_glare) {
          ESP_LOGW(TAG, "%s: collision (we are CALLING) - sending 486 Busy Here",
                   this->device_name_.c_str());
          this->send_sip_final_response_(incoming_cid, kReasonBusy);
          break;
        }
        // Tie-break: lexicographically lower device_name wins. Symmetric
        // on both sides so exactly one peer survives.
        const bool we_win = this->device_name_ < in_caller_name;
        if (we_win) {
          ESP_LOGW(TAG, "%s: glare with %s (we win), rejecting inbound INVITE",
                   this->device_name_.c_str(), in_caller_name.c_str());
          this->send_sip_final_response_(incoming_cid, "glare");
          break;
        }
        ESP_LOGW(TAG, "%s: glare with %s (we lose), aborting CALLING and accepting inbound",
                 this->device_name_.c_str(), in_caller_name.c_str());
        // Retract silently. The peer never sees our 200 OK, so its
        // outgoing leg dies on its own timeout; a final response here would
        // just race the peer's success.
        this->set_audio_devices_active_(false);
        this->set_call_state_(CallState::IDLE);
        goto handle_incoming_invite_in_idle;
      }
handle_incoming_invite_in_idle:
      std::string cached_cid, cached_reason;
      uint32_t cached_age_ms = UINT32_MAX;
      this->snapshot_terminal_response_(&cached_cid, &cached_reason, &cached_age_ms);
      if (!cached_cid.empty() && incoming_cid == cached_cid &&
          cached_age_ms <= TERMINAL_RESPONSE_REPLAY_MS) {
        ESP_LOGD(TAG, "Replaying cached SIP final response for %s", incoming_cid.c_str());
        this->send_sip_final_response_(incoming_cid, cached_reason);
        break;
      }

      if (this->do_not_disturb_) {
        ESP_LOGI(TAG, "%s: do-not-disturb active - 486 Busy Here inbound call from %s",
                 this->device_name_.c_str(),
                 in_caller_name.empty() ? "(unknown caller)" : in_caller_name.c_str());
        this->set_terminal_response_(incoming_cid, kReasonBusy);
        this->send_sip_final_response_(incoming_cid, kReasonBusy);
        if (this->transport_ != nullptr) {
          this->transport_->disconnect();
        }
        break;
      }

      AudioFormat caller_to_dest;
      AudioFormat dest_to_caller;
      if (!choose_common_audio_format(msg.caller_tx_formats, this->rx_audio_formats_, &caller_to_dest) ||
          !choose_common_audio_format(msg.caller_rx_formats, this->tx_audio_formats_, &dest_to_caller)) {
        ESP_LOGW(TAG,
                 "%s: inbound INVITE from %s has no compatible audio format - "
                 "488 media_incompatible",
                 this->device_name_.c_str(), in_caller_name.empty() ? "(unknown caller)" : in_caller_name.c_str());
        this->set_terminal_response_(incoming_cid, kReasonMediaIncompatible);
        this->send_sip_final_response_(incoming_cid, kReasonMediaIncompatible);
        if (this->transport_ != nullptr) {
          this->transport_->disconnect();
        }
        break;
      }
      this->set_current_media_formats_(caller_to_dest, dest_to_caller, dest_to_caller, caller_to_dest);

      this->clear_terminal_call_snapshot_();
      const std::string dest_route = in_dest_route.empty()
          ? (this->device_route_id_.empty() ? this->device_name_ : this->device_route_id_)
          : in_dest_route;
      const std::string dest_name = in_dest_name.empty() ? this->device_name_ : in_dest_name;
      this->set_call_identity_(incoming_cid, in_caller_route, in_caller_name,
                                dest_route, dest_name);
      this->clear_terminal_response_();

      const char *local = this->device_name_.c_str();
      const char *remote = in_caller_name.empty() ? "(unknown caller)" : in_caller_name.c_str();
      ESP_LOGI(TAG, "%s <- %s: incoming call (call_id=%s)",
               local, remote, incoming_cid.c_str());

      this->publish_caller_(in_caller_name);
      const std::string remote_uri = in_caller_route.empty()
          ? std::string("sip:") + in_caller_name
          : std::string("sip:") + in_caller_route;
      this->defer([this, incoming_cid, in_caller_name, dest_name, remote_uri]() {
        this->incoming_call_trigger_.trigger(incoming_cid, in_caller_name, dest_name, remote_uri);
      });

      if (this->auto_answer_) {
        this->set_audio_devices_active_(true);
        if (this->transport_ && !this->transport_->start_audio_path()) {
          ESP_LOGE(TAG, "%s: RTP start failed while auto-answering call", this->device_name_.c_str());
          this->end_call_(CallEndReason::TRANSPORT_UNREACHABLE);
          this->set_audio_devices_active_(false);
          if (this->transport_) this->transport_->disconnect();
          break;
        }
        this->set_call_state_(CallState::CONNECTING);
        if (!this->send_sip_answer_(incoming_cid)) {
          ESP_LOGE(TAG, "%s: failed to send automatic SIP answer", this->device_name_.c_str());
          this->end_call_(CallEndReason::PROTOCOL_ERROR);
          this->set_audio_devices_active_(false);
          if (this->transport_) this->transport_->disconnect();
          break;
        }
        this->set_in_call_(true);
      } else {
        if (!this->send_sip_ringing_(incoming_cid)) {
          ESP_LOGE(TAG, "%s: failed to send SIP ringing response", this->device_name_.c_str());
          this->end_call_(CallEndReason::PROTOCOL_ERROR);
          if (this->transport_) this->transport_->disconnect();
          break;
        }
        this->ringing_start_time_ = millis();
        this->set_call_state_(CallState::RINGING);
        ESP_LOGI(TAG, "%s: ringing (waiting for local answer)", this->device_name_.c_str());
      }
      break;
    }

    case SipSignalType::BYE: {
      if (this->ignore_if_idle_or_stale_("BYE", in_call_id)) break;
      ESP_LOGI(TAG, "%s: remote hung up (call_id=%s)",
               this->device_name_.c_str(),
               in_call_id.c_str());
      this->set_call_state_(CallState::TERMINATING);
      this->end_call_(CallEndReason::REMOTE_HANGUP);
      this->set_in_call_(false);
      this->set_audio_devices_active_(false);
      if (this->transport_) this->transport_->disconnect();
      break;
    }

    case SipSignalType::STATUS_200_OK: {
      if (!in_call_id.empty()) {
        std::string terminal_reason;
        if (this->recent_terminal_call_(in_call_id, &terminal_reason)) {
          ESP_LOGD(TAG, "ignoring late 200 OK for terminal call_id=%s reason=%s",
                   in_call_id.c_str(),
                   terminal_reason.empty() ? "(none)" : terminal_reason.c_str());
          break;
        }
        const std::string current_cid = this->get_current_call_id_();
        if (!current_cid.empty() && in_call_id != current_cid) {
          ESP_LOGD(TAG, "ignoring stale 200 OK for call_id=%s (current=%s)",
                   in_call_id.c_str(), current_cid.c_str());
          break;
        }
      }
      const CallState state = this->call_state_.load(std::memory_order_acquire);
      if (state == CallState::CALLING || state == CallState::REMOTE_RINGING) {
        if (!audio_format_list_contains(this->tx_audio_formats_, msg.selected_tx_format) ||
            !audio_format_list_contains(this->rx_audio_formats_, msg.selected_rx_format)) {
          ESP_LOGE(TAG,
                   "%s: 200 OK confirmed an incompatible audio format; ending call",
                   this->device_name_.c_str());
          this->end_call_(CallEndReason::MEDIA_INCOMPATIBLE);
          this->set_in_call_(false);
          this->set_audio_devices_active_(false);
          if (this->transport_) this->transport_->disconnect();
          break;
        }
        if (this->transport_ && !this->transport_->start_audio_path()) {
          ESP_LOGE(TAG, "%s: RTP start failed after SIP answer", this->device_name_.c_str());
          this->end_call_(CallEndReason::TRANSPORT_UNREACHABLE);
          this->set_audio_devices_active_(false);
          this->transport_->disconnect();
          break;
        }
        this->set_current_media_formats_(msg.selected_tx_format, msg.selected_rx_format, msg.selected_tx_format,
                                         msg.selected_rx_format);
        ESP_LOGI(TAG, "%s: destination answered, in_call (call_id=%s)",
                 this->device_name_.c_str(),
                 in_call_id.c_str());
        this->set_call_state_(CallState::CONNECTING);
        this->set_in_call_(true);
      } else if (state == CallState::RINGING) {
        ESP_LOGI(TAG, "%s: answered remotely (by HA)", this->device_name_.c_str());
        this->answer_call();
      } else if (state == CallState::IN_CALL &&
                 !this->first_audio_received_.load(std::memory_order_acquire)) {
        ESP_LOGD(TAG, "200 OK repeat in IN_CALL (no audio yet) - re-echoing ACK");
        this->send_sip_answer_(this->get_current_call_id_());
      } else {
        const bool audio_seen = this->first_audio_received_.load(std::memory_order_acquire);
        ESP_LOGD(TAG, "200 OK ignored in state %s (audio_seen=%d)",
                 call_state_to_str(state), audio_seen);
      }
      break;
    }

    case SipSignalType::CANCEL:
      if (this->ignore_if_idle_or_stale_("CANCEL", in_call_id)) break;
      ESP_LOGI(TAG, "%s: remote cancelled call (call_id=%s)",
               this->device_name_.c_str(), in_call_id.c_str());
      this->end_call_(CallEndReason::CANCELLED);
      this->set_audio_devices_active_(false);
      if (this->transport_) this->transport_->disconnect();
      break;

    case SipSignalType::FINAL_RESPONSE:
    case SipSignalType::MEDIA_INCOMPATIBLE:
    case SipSignalType::AUTH_REQUIRED:
    case SipSignalType::PROXY_AUTH_REQUIRED:
    case SipSignalType::PROTOCOL_ERROR: {
      if (this->ignore_if_idle_or_stale_(sip_signal_type_name(msg.type), in_call_id)) break;
      CallEndReason reason = CallEndReason::DECLINED;
      if (msg.status_code == 486 || in_reason == kReasonBusy) reason = CallEndReason::BUSY;
      else if (msg.status_code == 487 || in_reason == kReasonCancelled) reason = CallEndReason::CANCELLED;
      else if (msg.status_code == 488 || in_reason == kReasonMediaIncompatible) reason = CallEndReason::MEDIA_INCOMPATIBLE;
      else if (msg.status_code == 408 || in_reason == kReasonTimeout) reason = CallEndReason::TIMEOUT;
      else if (msg.status_code == 401 || in_reason == kReasonAuthRequiredUnsupported) reason = CallEndReason::AUTH_REQUIRED_UNSUPPORTED;
      else if (msg.status_code == 407 || in_reason == kReasonProxyAuthRequiredUnsupported) reason = CallEndReason::PROXY_AUTH_REQUIRED_UNSUPPORTED;
      else if (msg.type == SipSignalType::PROTOCOL_ERROR) reason = CallEndReason::PROTOCOL_ERROR;
      std::string detail;
      if (reason == CallEndReason::DECLINED && !in_reason.empty() && in_reason != kReasonDeclined) {
        detail = in_reason;
      }
      ESP_LOGI(TAG, "%s: SIP final status %u reason=%s call_id=%s",
               this->device_name_.c_str(), (unsigned) msg.status_code,
               detail.empty() ? call_end_reason_to_str(reason) : detail.c_str(),
               in_call_id.c_str());
      this->end_call_(reason, detail);
      this->set_audio_devices_active_(false);
      // A malformed/incompatible 2xx still creates a confirmed SIP dialog.
      // SipTransport has already ACKed it and owns a retransmitted BYE; do not
      // clear that transaction here. A non-2xx 488 was reset by the transport
      // before the signal was emitted, so leaving it alone is also safe.
      if (this->transport_ && msg.type != SipSignalType::MEDIA_INCOMPATIBLE) {
        this->transport_->disconnect();
      }
      break;
    }

    case SipSignalType::STATUS_180_RINGING:
      if (this->call_state_.load(std::memory_order_acquire) == CallState::CALLING) {
        ESP_LOGI(TAG, "%s: dest is ringing (call_id=%s)",
                 this->device_name_.c_str(),
                 in_call_id.c_str());
        this->set_call_state_(CallState::REMOTE_RINGING);
      } else {
        ESP_LOGD(TAG, "180 ignored in state %s", this->get_call_state_str());
      }
      break;

    case SipSignalType::OPTIONS:
      ESP_LOGD(TAG, "OPTIONS handled by SIP transport");
      break;

    default:
      ESP_LOGW(TAG, "Unknown SIP signal %s", sip_signal_type_name(msg.type));
      break;
  }
}

void VoipStack::on_connection_change_(bool connected) {
  if (connected) {
    // FSM stays IDLE until an INVITE arrives; can_accept_session_ has gated.
    return;
  }

  ESP_LOGI(TAG, "Transport disconnected");
  this->set_audio_devices_active_(false);
  if (this->call_state_.load(std::memory_order_acquire) != CallState::IDLE) {
    this->end_call_(CallEndReason::TRANSPORT_UNREACHABLE);
    this->set_in_call_(false);
  } else {
    this->publish_caller_("");
    this->publish_state_();
  }
}

bool VoipStack::can_accept_session_() const {
  // Accept new TCP sessions only when IDLE or CALLING. CALLING covers
  // the bridged-caller case where the dest connects back.
  CallState cs = this->call_state_.load(std::memory_order_acquire);
  return cs == CallState::IDLE || cs == CallState::CALLING;
}

}  // namespace voip_stack
}  // namespace esphome

#endif  // USE_ESP32
