#!/usr/bin/env python3
"""Static contract checks for the ESP VoIP C++ endpoint.

These tests do not replace hardware/audio validation. They guard the core
invariants that caused real regressions: no timer-paced media TX, explicit RTP
source latching, no zombie calls when media disappears, and minimal SIP
transaction behavior for UDP.
"""

from __future__ import annotations

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
VOIP = ROOT / "esphome" / "components" / "voip_stack"


def read(name: str) -> str:
    return (VOIP / name).read_text(encoding="utf-8")


def test_audio_path_is_not_timer_paced_or_sink_callback_paced() -> None:
    audio = read("voip_audio.cpp")
    stack = read("voip_stack.cpp")
    header = read("voip_stack.h")

    combined = "\n".join([audio, stack, header])
    assert "vTaskDelayUntil" not in combined
    assert "add_audio_output_callback" not in combined
    assert "handle_speaker_output_" not in combined
    assert "kTxPrebufferFrames" not in combined
    assert "kTxQueuedFrames" not in combined

    assert "Capture-clocked TX" in audio
    assert "TickType_t wait_budget = ticks_to_wait" in audio
    assert "speaker_->play(pcm + offset, bytes - offset, wait_budget)" in audio
    assert "wait_budget = 0" in audio
    assert "offset += written" in audio
    assert "written == 0" in audio
    assert "media_tx_queue_drops_" in audio
    assert "media_rx_queue_drops_" in audio
    assert "tx_task_priority" not in combined
    assert "rx_task_priority" not in combined


def test_audio_stack_tx_queue_is_optimized_without_changing_native_mic_path() -> None:
    init_py = read("__init__.py")
    audio = read("voip_audio.cpp")
    stack = read("voip_stack.cpp")
    header = read("voip_stack.h")

    assert "def _tx_uses_esp_audio_stack" in init_py
    assert 'CONF_TX_USES_ESP_AUDIO_STACK = "_tx_uses_esp_audio_stack"' in init_py
    assert 'mic_config.get("platform") == "esp_audio_stack"' in init_py
    assert "_esp_audio_stack_parent_config(mic_config) is not None" in init_py
    assert "config[CONF_TX_USES_ESP_AUDIO_STACK] = _tx_uses_esp_audio_stack(config)" in init_py
    assert 'cg.add_define("USE_ESPHOME_VOIP_STACK_AUDIO_STACK_MIC")' in init_py
    assert "set_microphone_source_is_audio_stack" not in header
    assert "microphone_source_is_audio_stack_" not in header

    assert "size_t VoipStack::tx_audio_buffer_bytes_() const" in audio
    assert "#ifdef USE_ESPHOME_VOIP_STACK_AUDIO_STACK_MIC" in audio
    assert "frame_bytes * 6" in audio
    assert "frame_bytes + 1024" in audio
    assert "frame_bytes * 16" in audio
    assert "frame_bytes + 4096" in audio
    assert "const size_t tx_buffer_bytes = this->tx_audio_buffer_bytes_();" in stack
    assert "std::max<size_t>(tx_frame_bytes * 16, tx_frame_bytes + 4096)" not in stack


def test_mic_enqueue_drop_accounting_is_single_primitive() -> None:
    audio = read("voip_audio.cpp")
    header = read("voip_stack.h")

    assert "bool write_mic_buffer_(const uint8_t *data, size_t len);" in header
    assert "bool VoipStack::write_mic_buffer_(const uint8_t *data, size_t len)" in audio
    assert "media_tx_queue_drops_.fetch_add(frames_for_bytes(dropped, frame_bytes)" in audio
    assert "xTaskNotifyGive(this->tx_task_handle_)" in audio
    assert "this->write_mic_buffer_(reinterpret_cast<const uint8_t *>(mic_converted), bytes);" in audio
    assert "this->write_mic_buffer_(data, len);" in audio
    assert audio.count("media_tx_queue_drops_.fetch_add(frames_for_bytes(dropped, frame_bytes)") == 1


def test_voip_helper_namespace_does_not_collide_with_audio_stack() -> None:
    ring_caps = read("audio_core_ring_buffer_caps.h")
    task_utils = read("audio_core_task_utils.h")
    stack_h = read("voip_stack.h")
    stack_cpp = read("voip_stack.cpp")
    sip_cpp = read("sip_transport.cpp")

    helpers = "\n".join([ring_caps, task_utils])
    combined = "\n".join([helpers, stack_h, stack_cpp, sip_cpp])
    assert "namespace audio_core {" not in helpers
    assert "namespace voip_audio_core {" in helpers
    assert re.search(r"(?<!voip_)audio_core::", combined) is None
    assert "voip_audio_core::" in combined


def test_media_timeout_is_a_terminal_phone_reason() -> None:
    fsm_h = read("voip_fsm.h")
    fsm_cpp = read("voip_fsm.cpp")
    stack_h = read("voip_stack.h")
    stack_cpp = read("voip_stack.cpp")
    settings_cpp = read("voip_settings.cpp")

    assert "MEDIA_TIMEOUT" in fsm_h
    assert 'kReasonMediaTimeout = "media_timeout"' in fsm_h
    assert "last_peer_audio_ms_" in stack_h
    assert "MEDIA_TIMEOUT_MS" in stack_h
    assert "CallEndReason::MEDIA_TIMEOUT" in stack_cpp
    assert "last_peer_audio_ms_.store(millis()" in fsm_cpp
    assert "first_audio_received_" in stack_cpp


def test_sip_udp_transactions_are_minimal_and_explicit() -> None:
    sip_h = read("sip_transport.h")
    sip_cpp = read("sip_transport.cpp")

    assert "struct UdpTransaction" in sip_h
    assert "UdpTransaction pending_invite_" in sip_h
    assert "UdpTransaction pending_bye_" in sip_h
    assert "pending_invite_request_" not in sip_h
    assert "pending_bye_request_" not in sip_h
    assert "remember_udp_transaction_" in sip_h
    assert "pump_udp_retransmits_" in sip_h
    assert 'remember_udp_transaction_(method, msg, ip, port)' in sip_cpp
    assert "clear_invite_transaction_();" in sip_cpp
    assert "clear_bye_transaction_();" in sip_cpp
    assert "SIP UDP %s retransmit" in sip_cpp


def test_sip_tcp_originate_is_async() -> None:
    sip_cpp = read("sip_transport.cpp")
    start = sip_cpp.index("bool SipTransport::originate(")
    end = sip_cpp.index("\nvoid SipTransport::set_remote", start)
    originate = sip_cpp[start:end]

    assert "tcp_connect_requested_" in originate
    assert "tcp_tx_pending_" in sip_cpp
    assert "delay(" not in originate
    assert "select(" not in originate
    assert "socket(" not in originate
    assert "connect(" not in originate


def test_sip_tcp_rx_is_bounded_and_active_dialog_accept_is_guarded() -> None:
    sip_h = read("sip_transport.h")
    transport_h = read("transport.h")
    sip_cpp = read("sip_transport.cpp")
    stack_cpp = read("voip_stack.cpp")

    assert "MAX_SIP_BODY_BYTES = 4096" in sip_cpp
    assert "MAX_SIP_TCP_RX_BUFFER = 8192" in sip_cpp
    assert "sip_tcp_client_ip_v4_" in sip_h
    assert "TransportDialogActiveCallback" in transport_h
    assert "set_dialog_active_callback" in transport_h
    assert "transport_dialog_active_callback_" in stack_cpp

    stream = sip_cpp[sip_cpp.index("void SipTransport::handle_sip_stream_") : sip_cpp.index("\nvoid SipTransport::sip_task_trampoline_", sip_cpp.index("void SipTransport::handle_sip_stream_"))]
    assert "this->sip_tcp_rx_buffer_.size() > MAX_SIP_TCP_RX_BUFFER" in stream
    assert "body_len > MAX_SIP_BODY_BYTES" in stream
    assert "drop_tcp_stream(\"SIP TCP RX buffer overflow\")" in stream
    assert "drop_tcp_stream(\"SIP TCP Content-Length exceeds limit\")" in stream

    accept = sip_cpp[sip_cpp.index("int client = accept(") : sip_cpp.index("if (this->sip_socket_ >= 0", sip_cpp.index("int client = accept("))]
    assert "this->dialog_active_()" in accept
    assert "active_ip_v4 != accepted_ip_v4" in accept
    assert "SIP TCP accept rejected: dialog active with different peer" in accept


def test_endpoint_group_membership_is_optional_and_forward_compatible() -> None:
    init_py = read("__init__.py")
    header = read("voip_stack.h")
    stack_cpp = read("voip_stack.cpp")
    settings_cpp = read("voip_settings.cpp")
    text_py = read("text.py")
    text_sensor_py = read("text_sensor.py")
    switch_py = read("switch.py")

    assert 'CONF_CONFERENCE_GROUPS = "conference_groups"' in init_py
    assert 'CONF_CONFERENCE_RING = "conference_ring"' in init_py
    assert 'CONF_RING_GROUPS = "ring_groups"' in init_py
    assert "_validate_endpoint_label" in init_py
    assert "def _validate_group_list" in init_py
    assert 'value.split(",")' in init_py
    assert 'cv.Optional(CONF_EXTENSION, default=""): _validate_endpoint_label' in init_py
    assert 'cv.Optional(CONF_CONFERENCE_GROUPS, default=""): _validate_group_list' in init_py
    assert 'cv.Optional(CONF_RING_GROUPS, default=""): _validate_group_list' in init_py
    assert "set_conference_groups" in header
    assert "set_conference_ring" in header
    assert "set_ring_groups" in header
    assert "set_extension_text" in header
    assert "get_extension" in header
    assert "std::string conference_groups_" in header
    assert "bool conference_ring_{false}" in header
    assert "std::string ring_groups_" in header
    assert "text::Text *extension_text_{nullptr}" in header
    assert "var.set_conference_groups" in init_py
    assert "var.set_conference_ring" in init_py
    assert "var.set_ring_groups" in init_py
    assert 'TYPE_EXTENSION = "extension"' in text_py
    assert '"set_extension_text"' in text_py
    assert 'r"^[^|,;\\r\\n]*$"' in text_py
    assert 'TYPE_RING_GROUPS = "ring_groups"' in text_py
    assert 'TYPE_CONFERENCE_GROUPS = "conference_groups"' in text_py
    assert "VoipStackGroupsText" in text_py
    assert 'TYPE_ENDPOINT = "endpoint"' in text_sensor_py
    assert 'TYPE_LAST_REASON = "last_reason"' in text_sensor_py
    assert 'CONF_CONFERENCE_RING = "conference_ring"' in switch_py

    endpoint = stack_cpp[stack_cpp.index("std::string VoipStack::build_endpoint_string_"):]
    assert "char buf[640]" in endpoint
    assert '"%s | %s | %u | %u | %s | %s | %s | %s | %s"' in endpoint
    assert "this->conference_groups_.c_str()" not in endpoint
    assert "this->ring_groups_.c_str()" not in endpoint
    assert 'this->conference_ring_ ? "1" : "0"' not in endpoint
    assert "VoIP endpoint string truncated" in endpoint

    set_extension = stack_cpp[
        stack_cpp.index("void VoipStack::set_extension(") :
        stack_cpp.index("\nvoid VoipStack::set_ring_groups")
    ]
    assert "normalize_endpoint_label(extension)" in set_extension
    assert "this->extension_text_->publish_state(normalized)" in set_extension
    assert "this->request_endpoint_publish_()" in set_extension


def test_voip_media_tasks_are_not_idle_polling() -> None:
    audio = read("voip_audio.cpp")
    sip_cpp = read("sip_transport.cpp")

    tx_start = audio.index("void VoipStack::tx_task_()")
    tx_end = audio.index("\n// === Microphone Callback ===", tx_start)
    tx_task = audio[tx_start:tx_end]
    assert "pdMS_TO_TICKS(20)" not in tx_task
    assert "portMAX_DELAY" in tx_task
    assert "xTaskNotifyGive(this->tx_task_handle_)" in audio

    rtp_start = sip_cpp.index("void SipTransport::rtp_task_()")
    rtp_task = sip_cpp[rtp_start:]
    assert "select(socket + 1, &readfds" in rtp_task
    assert "delay(" not in sip_cpp
    assert "delay(5)" not in rtp_task
    assert "} else {\n      delay" not in rtp_task


def test_non_2xx_invite_final_response_sends_ack() -> None:
    sip_h = read("sip_transport.h")
    sip_cpp = read("sip_transport.cpp")

    assert "struct SipRequestOptions" in sip_h
    assert "std::string branch_override" in sip_h
    assert "std::string cseq_method" in sip_h
    assert 'bool send_request_(const std::string &method, const std::string &body = "");' in sip_h
    assert "bool send_request_(const std::string &method, const std::string &body,\n                     const SipRequestOptions &options);" in sip_h
    assert "SipRequestOptions &options =" not in sip_h
    assert "bool SipTransport::send_request_(const std::string &method, const std::string &body) {" in sip_cpp
    assert "send_invite_error_ack_" in sip_h
    assert "bool SipTransport::send_invite_error_ack_()" in sip_cpp
    assert "options.cseq_number = this->invite_cseq_" in sip_cpp
    assert 'options.cseq_method = "ACK"' in sip_cpp
    assert "options.branch_override = this->branch_" in sip_cpp
    assert "RFC 3261 section 17.1.1.3" in sip_cpp
    assert 'return this->send_request_("ACK", "", options);' in sip_cpp
    assert "this->send_invite_error_ack_();" in sip_cpp


def test_reinvite_and_rtp_latch_are_explicit() -> None:
    sip_h = read("sip_transport.h")
    sip_cpp = read("sip_transport.cpp")

    assert "last_invite_cseq_number_" in sip_h
    assert "cseq_number(" in sip_cpp
    assert "reinvite_unsupported" in sip_cpp
    assert "latched_rtp_ip_v4_" in sip_h
    assert "latched_rtp_port_" in sip_h
    assert "latched_rtp_ssrc_" in sip_h
    assert "rtp_ssrc_latched_" in sip_h
    assert "latched_rtp_port_.store" in sip_cpp
    assert "latched_rtp_ssrc_.load" in sip_cpp


def test_rtp_jitter_buffer_is_extracted_and_static() -> None:
    audio = read("voip_audio.cpp")
    header = read("voip_stack.h")
    jitter_h = read("rtp_jitter_buffer.h")

    assert "class RtpJitterBuffer" in jitter_h
    assert "std::unique_ptr<RtpJitterBuffer> rx_jitter_buffer_" in header
    assert "RxJitterSlot" not in header
    assert "rx_jitter_slots_" not in header
    assert "new Slot" not in jitter_h
    assert "delete[]" not in jitter_h
    assert "inline RtpJitterBuffer::ReadResult RtpJitterBuffer::read" in jitter_h
    assert "static constexpr uint8_t MAX_SLOTS" in jitter_h
    assert "RtpJitterBuffer::ReadResult::MISSING" in audio


def test_media_session_and_silence_policies_are_explicit() -> None:
    audio = read("voip_audio.cpp")
    header = read("voip_stack.h")
    sip_h = read("sip_transport.h")
    sip_cpp = read("sip_transport.cpp")

    assert "SilenceReason::NETWORK_GAP" in audio
    assert "SilenceReason::MUTED_SINK" in audio
    assert "play_silence_frame_(SilenceReason reason" in header
    assert "media_active_" in sip_h
    assert "call_active_" not in sip_h
    assert "open_media_session_" in sip_h
    assert "close_media_session_" in sip_h
    assert "if (!this->media_active_.load" in sip_cpp


def test_sip_snapshot_refresh_is_throttled_unless_audio_debug_is_enabled() -> None:
    stack = read("voip_stack.cpp")

    assert "const uint32_t snapshot_refresh_ms = this->audio_debug_ ? 500 : 2000;" in stack
    assert "now - this->last_sip_snapshot_refresh_ms_ >= snapshot_refresh_ms" in stack


def test_long_diagnostic_text_sensors_have_wrapping_separators() -> None:
    stack = read("voip_stack.cpp")

    assert 'out += "; ";' in stack
    assert 'out += ";";' not in stack
    assert '"%s | %s | %u | %u | %s | %s | %s | %s | %s"' in stack
    assert '"st=%s; id=%s; dir=%s; from=%s; to=%s; ct=%s; tr=%s; sc=%u; "' in stack
    assert '"tx=%s; rx=%s; pt=%u; pr=%u; "' in stack
    assert '"tqd=%u; tqdrop=%u; rqd=%u; rqdrop=%u; rs=%s; ev=%s"' in stack


def test_ha_routed_contacts_use_local_esp_signaling_transport() -> None:
    settings = read("voip_settings.cpp")

    assert "const bool local_sip_transport_tcp = this->protocol_ == TransportType::TCP;" in settings
    assert "entry.sip_transport_tcp = local_sip_transport_tcp;" in settings
    assert "entry.sip_transport_tcp = ha_slot.sip_transport == \"tcp\";" not in settings


def test_call_action_is_universal_local_or_ha_dialplan() -> None:
    init_py = read("__init__.py")
    actions = read("actions.h")
    header = read("voip_stack.h")
    settings = read("voip_settings.cpp")
    fsm = read("voip_fsm.cpp")

    assert 'CallAction = voip_stack_ns.class_("CallAction", automation.Action)' in init_py
    assert '"voip_stack.call"' in init_py
    assert "CONF_TARGET" in init_py
    assert "var.set_target(value)" in init_py
    assert "voip_stack.call_contact" not in init_py
    assert "CallContactAction" not in actions
    assert "void call(const std::string &target);" in header
    assert "void call_contact" not in header

    call_body = settings[settings.index("void VoipStack::call(") : settings.index("\nvoid VoipStack::next_contact")]
    assert "this->phonebook_.select(target)" in call_body
    assert "this->pending_dialplan_target_.clear();" in call_body
    assert "this->phonebook_.select(this->ha_peer_name_)" in call_body
    assert "this->pending_dialplan_target_ = target;" in call_body
    assert "Routing target '%s' through HA peer" in call_body
    assert "this->start();" in call_body

    start_body = fsm[fsm.index("void VoipStack::start()") : fsm.index("\nvoid VoipStack::stop()")]
    assert "const bool route_via_ha" in start_body
    assert "!this->pending_dialplan_target_.empty()" in start_body
    assert "this->bridge_request_trigger_.trigger" in start_body
    assert "if (route_via_ha)" in start_body

    destination_body = settings[
        settings.index("void VoipStack::publish_destination_()") :
        settings.index("\nvoid VoipStack::publish_caller_")
    ]
    assert "this->pending_dialplan_target_" in destination_body
    assert "call.dest_name" in destination_body
    assert "call.caller_name == this->device_name_" in destination_body
    assert "this->last_terminal_direction_ == \"outgoing\"" in destination_body
    assert "this->last_terminal_dest_name_" in destination_body


def test_contact_cycler_dismisses_terminal_destination_snapshot() -> None:
    settings = read("voip_settings.cpp")
    next_body = settings[settings.index("void VoipStack::next_contact()") : settings.index("\nvoid VoipStack::prev_contact()")]
    prev_body = settings[settings.index("void VoipStack::prev_contact()") : settings.index("\nconst std::string &VoipStack::get_current_destination")]

    for body in (next_body, prev_body):
        assert "this->phonebook_" in body
        assert 'this->publish_last_reason_("")' in body
        assert "this->clear_terminal_call_snapshot_()" in body
        assert "this->publish_destination_()" in body


def test_roster_json_uses_address_direct_or_ha_route_without_kind_semantics() -> None:
    settings = read("voip_settings.cpp")

    assert 'json_metadata_bool(obj, "local_ha")' in settings
    assert 'slot->local_ha = json_metadata_bool(obj, "local_ha");' in settings
    assert 'std::string kind' not in settings
    assert 'softphone' not in settings
    assert 'registered' not in settings
    assert 'slot.address.empty()' in settings
    assert 'const bool direct_candidate = !slot.address.empty() && !slot.local_ha && !slot.ha_bridge' in settings
    assert 'contact_transport_tcp == local_sip_transport_tcp' in settings
    assert '} else if (has_ha) {' in settings
