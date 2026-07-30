#!/usr/bin/env python3
"""Contract and host-side behavior checks for the ESP VoIP C++ endpoint.

These tests do not replace hardware/audio validation. They guard the core
invariants that caused real regressions: no timer-paced media TX, explicit RTP
source latching, bounded RFC media parsing, no zombie calls when media
disappears, and minimal SIP transaction behavior for UDP.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path
import re
import subprocess
import textwrap

import esphome.config_validation as cv
import pytest


ROOT = Path(__file__).resolve().parents[1]
VOIP = ROOT / "esphome" / "components" / "voip_stack"


def read(name: str) -> str:
    return (VOIP / name).read_text(encoding="utf-8")


def cpp_method(source: str, qualified_name_pattern: str) -> str:
    """Return one balanced C++ method definition matched by a name regex."""

    signature = re.search(
        rf"\b(?:bool|void)\s+{qualified_name_pattern}\s*\([^;{{}}]*\)"
        r"(?:\s+const)?\s*\{",
        source,
        re.DOTALL,
    )
    assert signature is not None, f"C++ method not found: {qualified_name_pattern}"
    opening = source.index("{", signature.start())
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[signature.start() : index + 1]
    raise AssertionError(f"Unbalanced C++ method: {qualified_name_pattern}")


def cpp_methods(source: str, name_pattern: str) -> str:
    """Join C++ method definitions whose unqualified names match a regex."""

    names = re.findall(
        rf"\b(?:bool|void)\s+SipTransport::(?P<name>{name_pattern})\s*\(",
        source,
    )
    return "\n".join(
        cpp_method(source, rf"SipTransport::{re.escape(name)}")
        for name in dict.fromkeys(names)
    )


def video_reinvite_state(header: str) -> tuple[str, str]:
    """Return the dedicated local re-INVITE declaration and instance name."""

    struct = re.search(
        r"struct\s+(?P<type>(?:\w*[Rr]e[Ii]nvite\w*|"
        r"\w*[Vv]ideo\w*[Dd]irection\w*[Ii]nvite\w*))"
        r"\s*\{(?P<body>.*?)\n\s*\};",
        header,
        re.DOTALL,
    )
    assert struct is not None
    assert all(token in struct.group("body").lower() for token in ("cseq", "branch"))
    instance = re.search(
        rf"\b{re.escape(struct.group('type'))}\s+(?P<name>\w+)"
        r"\s*(?:\{\})?\s*;",
        header,
    )
    assert instance is not None
    return struct.group(0), instance.group("name")


def braced_block_after(source: str, marker: str) -> str:
    start = source.index(marker)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"Unbalanced block after: {marker}")


def test_full_duplex_examples_fit_the_default_rtp_payload_budget() -> None:
    example = (ROOT / "examples" / "native-full-duplex.yaml").read_text(
        encoding="utf-8"
    )
    root_readme = (ROOT / "README.md").read_text(encoding="utf-8")
    component_readme = read("README.md")

    # 48 kHz mono s16le at 10 ms is 960 bytes. The previous 20 ms example
    # produced 1920-byte RTP payloads and was rejected by the documented
    # 1200-byte default safety budget.
    assert example.count("frame_ms: 10") == 2
    assert "frame_ms: 20" not in example
    assert root_readme.count(
        "sample_rate: 48000, pcm_format: s16le, channels: 1, frame_ms: 10"
    ) >= 2
    assert component_readme.count("frame_ms: 10") >= 2


def test_phonebook_exposes_allocation_free_contact_count() -> None:
    header = read("voip_stack.h")

    accessor = re.search(
        r"size_t\s+get_contact_count\(\)\s+const\s*\{(?P<body>.*?)\}",
        header,
        re.DOTALL,
    )
    assert accessor is not None
    assert "return this->phonebook_.size();" in accessor.group("body")
    assert "std::string get_contacts_csv() const;" in header


def test_video_rtp_burst_capacity_is_compile_time_gated() -> None:
    init_py = read("__init__.py")
    video_rtp = read("video_rtp.cpp")

    video_codegen = init_py[
        init_py.index("    if CONF_VIDEO in config:", init_py.index("async def _add_core_settings")) :
        init_py.index("    cg.add(var.set_extension", init_py.index("async def _add_core_settings"))
    ]
    assert 'cg.add_define("USE_ESPHOME_VOIP_STACK_VIDEO")' in video_codegen
    assert (
        'esp32.add_idf_sdkconfig_option("CONFIG_LWIP_UDP_RECVMBOX_SIZE", 64)'
        in video_codegen
    )
    assert "this->sequence_valid_ = false;" in video_rtp
    assert "batch++ < kMaxReceiveBatchPackets" in video_rtp
    assert re.search(r"recvfrom\(\s*this->rtp_socket_", video_rtp)


def test_video_codec_schema_and_codegen_are_one_codec_per_build() -> None:
    init_path = VOIP / "__init__.py"
    spec = importlib.util.spec_from_file_location("voip_stack_codec_init", init_path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    jpeg = module._validate_video_config(
        {
            module.CONF_CODEC: module.VIDEO_CODEC_JPEG,
            module.CONF_WIDTH: 640,
            module.CONF_HEIGHT: 480,
        }
    )
    assert jpeg[module.CONF_OFFER_PAYLOAD_TYPE] == 26
    with pytest.raises(cv.Invalid, match="static RTP/JPEG"):
        module._validate_video_config(
            {
                module.CONF_CODEC: module.VIDEO_CODEC_JPEG,
                module.CONF_WIDTH: 640,
                module.CONF_HEIGHT: 480,
                module.CONF_OFFER_PAYLOAD_TYPE: 103,
            }
        )

    h264 = module._validate_video_config(
        {
            module.CONF_CODEC: module.VIDEO_CODEC_H264,
            module.CONF_WIDTH: 640,
            module.CONF_HEIGHT: 480,
        }
    )
    assert h264[module.CONF_OFFER_PAYLOAD_TYPE] == 103
    with pytest.raises(cv.Invalid, match="requires an encoded source"):
        module._validate_video_config(
            {
                module.CONF_CODEC: module.VIDEO_CODEC_H264,
                module.CONF_CAMERA_ID: object(),
                module.CONF_WIDTH: 640,
                module.CONF_HEIGHT: 480,
            }
        )
    with pytest.raises(cv.Invalid, match="dynamic offer_payload_type"):
        module._validate_video_config(
            {
                module.CONF_CODEC: module.VIDEO_CODEC_H264,
                module.CONF_WIDTH: 640,
                module.CONF_HEIGHT: 480,
                module.CONF_OFFER_PAYLOAD_TYPE: 26,
            }
        )

    init_py = read("__init__.py")
    assert "cv.Required(CONF_CODEC)" in init_py
    assert 'cg.add_define("USE_ESPHOME_VOIP_STACK_VIDEO_JPEG")' in init_py
    assert 'cg.add_define("USE_ESPHOME_VOIP_STACK_VIDEO_H264")' in init_py
    assert "cg.add(var.set_video_codec(codec_enum))" in init_py
    assert "defined(USE_ESPHOME_VOIP_STACK_VIDEO_JPEG)" in read(
        "rtp_jpeg.cpp"
    )
    assert "defined(USE_ESPHOME_VOIP_STACK_VIDEO_JPEG)" in read(
        "camera_video_source.cpp"
    )
    video_header = read("video_rtp.h")
    assert "#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG" in video_header
    assert "#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264" in video_header
    capability_header = read("video.h")
    assert (
        '#error "voip_stack video builds require exactly one codec backend"'
        in capability_header
    )

    stack = read("voip_stack.cpp")
    setup_transport = cpp_method(stack, r"VoipStack::setup_transport_")
    assert "this->video_source_ != nullptr" in setup_transport
    assert "this->video_sink_ != nullptr" in setup_transport
    assert "endpoint_matches_codec" in setup_transport
    assert "capability.valid() &&" in setup_transport
    assert "return false;" in setup_transport
    assert (
        setup_transport.index("endpoint_matches_codec")
        < setup_transport.index("std::make_unique<SipTransport>")
    )


def test_realtime_audio_task_stack_policy_defaults_to_internal_ram() -> None:
    init_py = read("__init__.py")
    header = read("voip_stack.h")
    source = read("voip_stack.cpp")

    assert (
        "cv.Optional(CONF_AUDIO_TASK_STACKS_IN_PSRAM, default=False): cv.boolean"
        in init_py
    )
    assert (
        "config[CONF_AUDIO_TASK_STACKS_IN_PSRAM]"
        in init_py
    )
    assert "set_audio_task_stacks_in_psram" in header
    assert "bool audio_task_stacks_in_psram_{false};" in header
    assert source.count("this->audio_task_stacks_in_psram_, TAG") == 2
    assert "this->task_stacks_in_psram_, TAG" not in source[
        source.index("bool VoipStack::start_runtime_tasks_()") :
        source.index("\nvoid VoipStack::", source.index("bool VoipStack::start_runtime_tasks_()"))
    ]


def test_video_rtcp_feedback_and_snapshot_are_directional_and_compile_gated() -> None:
    capability = read("video.h")
    rtp_header = read("video_rtp.h")
    rtp = read("video_rtp.cpp")
    transport = read("sip_transport.cpp")
    snapshot = read("transport.h")
    stack = read("voip_stack.cpp")

    assert "bool rtcp_feedback_pli{false};" in capability
    assert "bool rtcp_feedback_fir{false};" in capability
    assert "bool accept_remote_pli_{false};" in rtp_header
    assert "bool accept_remote_fir_{false};" in rtp_header
    assert "bool send_remote_pli_{false};" in rtp_header
    assert "rtcp_feedback_enabled_" not in rtp_header
    assert "count == 1 && this->accept_remote_pli_" in rtp
    assert "count == 4 && this->accept_remote_fir_" in rtp
    assert "!this->send_remote_pli_" in rtp
    assert '" nack pli\\r\\n"' in transport
    assert '" ccm fir\\r\\n"' in transport
    assert '"b=TIAS:"' in transport
    assert "parse_video_rtcp_feedback" in transport

    for field in (
        "video_tx_packets",
        "video_rx_packets",
        "video_tx_access_units",
        "video_rx_access_units",
        "media_lifecycle_phase",
    ):
        assert field in snapshot
    assert "std::atomic<uint32_t> tx_access_units_ok_{0};" in rtp_header
    assert "std::atomic<uint32_t> rx_access_units_ok_{0};" in rtp_header
    assert '"; vt=%u; vr=%u; vat=%u; var=%u; mlp=%u"' in stack


def test_video_debug_gates_hosted_and_media_send_timing() -> None:
    init_py = read("__init__.py")
    video_header = read("video_rtp.h")
    video_rtp = read("video_rtp.cpp")
    sip_header = read("sip_transport.h")
    sip_cpp = read("sip_transport.cpp")

    debug_codegen = init_py[
        init_py.index("        if config[CONF_VIDEO_DEBUG]:") :
        init_py.index("    cg.add(var.set_extension")
    ]
    assert 'if "esp32_hosted" in (CORE.config or {}):' in debug_codegen
    assert '"CONFIG_ESP_HOSTED_PKT_STATS", True' in debug_codegen
    assert '"CONFIG_ESP_HOSTED_PKT_STATS_INTERVAL_SEC", 5' in debug_codegen
    assert "tx_max_send_us_" in video_header
    assert "send_elapsed_us >= 5000U" in video_rtp
    assert "audio_tx_max_send_us_" in sip_header
    assert "send_elapsed_us >= 5000U" in sip_cpp
    assert (
        sip_cpp.index("#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_DEBUG")
        < sip_cpp.index("const uint32_t send_started_us = micros();")
    )


def test_same_tuple_rtp_source_restart_resets_audio_and_video_state() -> None:
    transport = read("transport.h")
    audio = read("sip_transport.cpp")
    playout = read("voip_audio.cpp")
    video = read("video_rtp.cpp")

    assert "bool source_changed{false};" in transport
    assert "latched_rtp_port_" in audio
    assert "source_changed = true;" in audio
    assert audio.index("rtp_payload_to_pcm(") < audio.index(
        "RTP source changed SSRC"
    )
    assert "frame.source_changed" in playout
    assert "this->rx_jitter_buffer_->reset();" in playout

    assert "latched_remote_rtp_port_" in video
    assert "Video RTP source changed SSRC" in video
    assert "this->reset_reassembly_();" in video
    assert "this->jpeg_depacketizer_.reset_session();" in video
    assert "this->sequence_valid_ = false;" in video


def test_video_media_tasks_are_event_driven_and_bounded() -> None:
    header = read("video_rtp.h")
    video = read("video.h")
    video_rtp = read("video_rtp.cpp")
    camera_source = read("camera_video_source.cpp")

    assert "queue_access_unit_(access_unit);" in video_rtp
    assert "source_callback_" in video_rtp
    assert "send_access_unit_(access_unit);" in video_rtp
    assert "ulTaskNotifyTake(pdTRUE, portMAX_DELAY);" in video_rtp
    assert "select(maxfd + 1" in video_rtp
    assert "kMaxReceiveBatchPackets" in header
    assert "pdMS_TO_TICKS(100)" not in video_rtp
    assert "virtual bool consume_video_access_unit(" in video
    assert "rx_drop_current_timestamp_" not in header
    assert "image->was_requested_by(camera::WEB_REQUESTER)" in camera_source
    assert "start_stream(camera::WEB_REQUESTER)" not in camera_source


def test_video_rtp_latches_only_codec_valid_media_packets() -> None:
    header = read("video_rtp.h")
    source = read("video_rtp.cpp")

    assert "bool handle_h264_payload_(" in header
    assert "bool handle_jpeg_payload_(" in header
    packet_handler = source[
        source.index("bool VideoRtpSession::handle_rtp_packet_(") :
        source.index("bool VideoRtpSession::handle_h264_payload_(")
    ]
    rejected = packet_handler.index("if (!payload_accepted)")
    assert packet_handler.index("handle_jpeg_payload_(") < rejected
    assert packet_handler.index("handle_h264_payload_(") < rejected
    assert rejected < packet_handler.index("this->expected_sequence_ =")
    assert rejected < packet_handler.index("this->rx_packets_.fetch_add(")

    h264_handler = source[
        source.index("bool VideoRtpSession::handle_h264_payload_(") :
        source.index("bool VideoRtpSession::handle_jpeg_payload_(")
    ]
    assert "payload_size < 2" in h264_handler
    assert "(payload[1] & 0x20)" not in h264_handler
    assert "timestamp != this->reassembly_timestamp_" in h264_handler
    assert "fragment_type == 0 || fragment_type > 23" in h264_handler
    negotiation = source[
        source.index("bool VideoRtpSession::set_negotiated(") :
        source.index("bool VideoRtpSession::bind_socket_(")
    ]
    assert negotiation.index("this->reset_reassembly_();") < negotiation.index(
        "this->jpeg_depacketizer_.reset_session();"
    )


def test_video_rtp_consumes_only_latched_rfc6263_keepalive_sequences() -> None:
    header = read("video_rtp.h")
    source = read("video_rtp.cpp")

    assert "bool handle_rtp_keepalive_(" in header
    receiver = cpp_method(source, r"VideoRtpSession::task_")
    assert receiver.index("source_ip !=") < receiver.index(
        "this->handle_rtp_keepalive_("
    )
    assert receiver.index("this->handle_rtp_keepalive_(") < receiver.index(
        "this->handle_rtp_packet_("
    )

    keepalive = cpp_method(source, r"VideoRtpSession::handle_rtp_keepalive_")
    assert "payload_type < 96" in keepalive
    assert "(packet[1] & 0x80) != 0" in keepalive
    assert "remote_ssrc_latched_" in keepalive
    assert "source_ssrc != this->remote_ssrc_" in keepalive
    assert "source_port !=" in keepalive
    assert "latched_remote_rtp_port_" in keepalive
    assert "payload_size != 0" in keepalive
    assert "!this->sequence_valid_" in keepalive
    assert "sequence != this->expected_sequence_" in keepalive
    assert keepalive.index("sequence != this->expected_sequence_") < (
        keepalive.index("this->expected_sequence_ =")
    )
    assert "rx_packets_.fetch_add" not in keepalive


def test_video_rtp_keeps_directional_endpoint_capabilities() -> None:
    header = read("video_rtp.h")
    source = read("video_rtp.cpp")
    transport = read("sip_transport.cpp")

    assert "VideoCapability send_capability_{};" in header
    assert "VideoCapability receive_capability_{};" in header
    assert "prepare_video(this->send_capability_)" in source
    assert "start_video(this->receive_capability_)" in source
    assert "this->send_capability_);" in source

    helper = transport[
        transport.index(
            "VideoCapability SipTransport::local_video_direction_capability_"
        ) :
        transport.index("bool SipTransport::prepare_video_session_locked_")
    ]
    assert "send ? this->local_video_send_capability_()" in helper
    assert ": this->local_video_receive_capability_();" in helper
    assert "capability.payload_type = negotiated.payload_type;" in helper
    assert "capability.width = negotiated.width;" not in helper
    assert "capability.height = negotiated.height;" not in helper
    assert "capability.profile_level_id = negotiated.profile_level_id;" not in helper

    initial = transport[
        transport.index("bool SipTransport::prepare_video_session_locked_") :
        transport.index("void SipTransport::reset_video_negotiation_")
    ]
    assert "this->negotiated_video_capability_, true" in initial
    assert "this->negotiated_video_capability_, false" in initial

    reinvite = transport[
        transport.index("bool SipTransport::handle_reinvite_(") :
        transport.index(
            "bool SipTransport::handle_video_direction_response_"
        )
    ]
    add_video_start = reinvite.index(
        "if (!old_video_negotiated && new_video_negotiated)"
    )
    add_video = reinvite[
        add_video_start :
        reinvite.index("} else if ((replace_video_direction", add_video_start)
    ]
    assert "new_video_capability, true" in add_video
    assert "new_video_capability, false" in add_video
    wire_identity = reinvite[
        reinvite.index("const bool same_video_media =") :
        reinvite.index("const bool same_video_direction =")
    ]
    assert ".width ==" not in wire_identity
    assert ".height ==" not in wire_identity
    assert ".max_fps ==" in wire_identity
    assert ".level_asymmetry_allowed ==" in wire_identity

    direction_answer = transport[
        transport.index("bool SipTransport::apply_video_direction_answer_") :
        transport.index("bool SipTransport::replay_completed_video_direction_ack_")
    ]
    assert "new_capability.max_fps == old_capability.max_fps" in direction_answer
    assert (
        "new_capability.level_asymmetry_allowed ==\n"
        "          old_capability.level_asymmetry_allowed"
    ) in direction_answer
    assert "new_capability.width == old_capability.width" not in direction_answer
    assert "new_capability.height == old_capability.height" not in direction_answer


def test_rtp_jpeg_dimension_limit_matches_rfc2435_encoding() -> None:
    init_path = VOIP / "__init__.py"
    spec = importlib.util.spec_from_file_location("voip_stack_local_init", init_path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    jpeg = {
        module.CONF_CODEC: module.VIDEO_CODEC_JPEG,
        module.CONF_OFFER_PAYLOAD_TYPE: 26,
        module.CONF_WIDTH: 2040,
        module.CONF_HEIGHT: 2040,
    }
    assert module._validate_video_config(dict(jpeg))[
        module.CONF_OFFER_PAYLOAD_TYPE
    ] == 26
    with pytest.raises(cv.Invalid, match="cannot exceed 2040x2040"):
        module._validate_video_config({**jpeg, module.CONF_WIDTH: 2048})

    # The schema keeps the wider generic bound available to custom H.264
    # sources; only RFC 2435's static JPEG format uses the 8-bit block fields.
    h264 = {
        module.CONF_CODEC: module.VIDEO_CODEC_H264,
        module.CONF_OFFER_PAYLOAD_TYPE: 103,
        module.CONF_WIDTH: 2048,
        module.CONF_HEIGHT: 2048,
    }
    assert module._validate_video_config(dict(h264))[
        module.CONF_OFFER_PAYLOAD_TYPE
    ] == 103


def test_rtp_jpeg_host_behavioral_contract(tmp_path: Path) -> None:
    """Compile the real allocation-free depacketizer and exercise RFC 2435."""

    stub = tmp_path / "esphome" / "core"
    stub.mkdir(parents=True)
    (stub / "defines.h").write_text("#pragma once\n", encoding="utf-8")
    probe = tmp_path / "rtp_jpeg_behavior.cpp"
    probe.write_text(
        textwrap.dedent(
            r"""
            #include "esphome/components/voip_stack/rtp_jpeg.h"

            #include <array>
            #include <cstdint>
            #include <cstring>
            #include <vector>

            using esphome::voip_stack::RtpJpegDepacketizer;
            using esphome::voip_stack::RtpJpegFrameView;
            using esphome::voip_stack::RtpJpegPushResult;
            using esphome::voip_stack::build_rtp_jpeg_fragment_header;
            using esphome::voip_stack::parse_jpeg_for_rtp;

            namespace {

            std::array<uint8_t, 64> make_table(uint8_t seed) {
              std::array<uint8_t, 64> table{};
              for (size_t index = 0; index < table.size(); index++) {
                table[index] =
                    static_cast<uint8_t>(1 + ((seed + index) % 255));
              }
              return table;
            }

            std::array<uint8_t, 128> make_table_pair(uint8_t first_seed,
                                                     uint8_t second_seed) {
              const auto first = make_table(first_seed);
              const auto second = make_table(second_seed);
              std::array<uint8_t, 128> tables{};
              std::memcpy(tables.data(), first.data(), first.size());
              std::memcpy(tables.data() + first.size(), second.data(),
                          second.size());
              return tables;
            }

            std::vector<uint8_t> make_payload(
                uint8_t quality, const uint8_t *quantizers,
                size_t quantizer_size) {
              static constexpr uint8_t SCAN[]{0x12, 0x34, 0x56, 0x78};
              std::vector<uint8_t> payload(8 + 4 + quantizer_size +
                                           sizeof(SCAN));
              payload[4] = 0;   // RFC 2435 type 0 (4:2:2).
              payload[5] = quality;
              payload[6] = 40;  // 320 pixels.
              payload[7] = 23;  // 184 pixels.
              payload[8] = 0;   // MBZ.
              payload[9] = 0;   // 8-bit precision.
              payload[10] = static_cast<uint8_t>(quantizer_size >> 8);
              payload[11] = static_cast<uint8_t>(quantizer_size);
              if (quantizer_size != 0) {
                std::memcpy(payload.data() + 12, quantizers, quantizer_size);
              }
              std::memcpy(payload.data() + 12 + quantizer_size, SCAN,
                          sizeof(SCAN));
              return payload;
            }

            RtpJpegPushResult push_frame(RtpJpegDepacketizer &depacketizer,
                                         const std::vector<uint8_t> &payload,
                                         uint32_t timestamp,
                                         std::array<uint8_t, 4096> &output,
                                         size_t *output_size,
                                         bool marker = true) {
              return depacketizer.push(
                  payload.data(), payload.size(), marker, timestamp,
                  output.data(), output.size(), output_size);
            }

            bool output_has_tables(const uint8_t *jpeg, size_t size,
                                   const uint8_t *first,
                                   const uint8_t *second) {
              for (size_t index = 0; index + 134 <= size; index++) {
                if (jpeg[index] != 0xFF || jpeg[index + 1] != 0xDB) continue;
                if (jpeg[index + 2] != 0 || jpeg[index + 3] != 132 ||
                    jpeg[index + 4] != 0 || jpeg[index + 69] != 1) {
                  return false;
                }
                return std::memcmp(jpeg + index + 5, first, 64) == 0 &&
                       std::memcmp(jpeg + index + 70, second, 64) == 0;
              }
              return false;
            }

            }  // namespace

            int main() {
              std::array<uint8_t,
                         RtpJpegDepacketizer::kQuantizationCacheBytes>
                  cache{};
              RtpJpegDepacketizer depacketizer;
              depacketizer.set_quantization_cache(cache.data(), cache.size());
              std::array<uint8_t, 4096> output{};
              size_t output_size = 0;
              uint32_t timestamp = 90000;

              // FFmpeg commonly emits one 64-byte table for PT 26. RX expands
              // it to both JPEG components; the rebuilt frame remains valid.
              const auto single = make_table(3);
              auto payload =
                  make_payload(255, single.data(), single.size());
              payload.resize(payload.size() - 2);
              if (push_frame(depacketizer, payload, timestamp, output,
                             &output_size, false) !=
                  RtpJpegPushResult::INCOMPLETE) {
                return 1;
              }
              std::vector<uint8_t> final_fragment{
                  0, 0, 0, 2, 0, 255, 40, 23, 0x56, 0x78};
              if (push_frame(depacketizer, final_fragment, timestamp++, output,
                             &output_size) != RtpJpegPushResult::COMPLETE) {
                return 2;
              }
              if (!output_has_tables(output.data(), output_size, single.data(),
                                     single.data())) {
                return 3;
              }
              RtpJpegFrameView rebuilt{};
              if (!parse_jpeg_for_rtp(output.data(), output_size, &rebuilt) ||
                  std::memcmp(rebuilt.quantizers.data(), single.data(), 64) !=
                      0 ||
                  std::memcmp(rebuilt.quantizers.data() + 64, single.data(),
                              64) != 0) {
                return 4;
              }

              // TX stays strict RFC 2435: two explicit tables, Length=128.
              const auto pair_a = make_table_pair(7, 41);
              RtpJpegFrameView tx{};
              static constexpr uint8_t TX_SCAN[]{1, 2, 3};
              tx.scan = TX_SCAN;
              tx.scan_size = sizeof(TX_SCAN);
              tx.quantizers = pair_a;
              tx.width = 320;
              tx.height = 184;
              tx.type = 0;
              std::array<uint8_t, 160> tx_header{};
              const size_t tx_header_size = build_rtp_jpeg_fragment_header(
                  tx, 0, tx_header.data(), tx_header.size());
              if (tx_header_size != 140 || tx_header[5] != 255 ||
                  tx_header[10] != 0 || tx_header[11] != 128 ||
                  std::memcmp(tx_header.data() + 12, pair_a.data(),
                              pair_a.size()) != 0) {
                return 5;
              }

              // A zero in either an abbreviated or full table is invalid and
              // must never reach the decoder.
              auto invalid_single = single;
              invalid_single[17] = 0;
              payload =
                  make_payload(255, invalid_single.data(), invalid_single.size());
              if (push_frame(depacketizer, payload, timestamp++, output,
                             &output_size) != RtpJpegPushResult::DROPPED) {
                return 6;
              }
              auto invalid_pair = pair_a;
              invalid_pair[97] = 0;
              payload =
                  make_payload(255, invalid_pair.data(), invalid_pair.size());
              if (push_frame(depacketizer, payload, timestamp++, output,
                             &output_size) != RtpJpegPushResult::DROPPED) {
                return 7;
              }

              // Q=128..254 mappings are independent and immutable for the RTP
              // session. Interleaving another Q must not evict the first.
              payload = make_payload(128, pair_a.data(), pair_a.size());
              if (push_frame(depacketizer, payload, timestamp++, output,
                             &output_size) != RtpJpegPushResult::COMPLETE) {
                return 8;
              }
              const auto pair_b = make_table_pair(19, 83);
              payload = make_payload(129, pair_b.data(), pair_b.size());
              if (push_frame(depacketizer, payload, timestamp++, output,
                             &output_size) != RtpJpegPushResult::COMPLETE) {
                return 9;
              }
              payload = make_payload(128, nullptr, 0);
              if (push_frame(depacketizer, payload, timestamp++, output,
                             &output_size) != RtpJpegPushResult::COMPLETE ||
                  !output_has_tables(output.data(), output_size, pair_a.data(),
                                     pair_a.data() + 64)) {
                return 10;
              }
              payload = make_payload(128, pair_a.data(), pair_a.size());
              if (push_frame(depacketizer, payload, timestamp++, output,
                             &output_size) != RtpJpegPushResult::COMPLETE) {
                return 11;
              }
              payload = make_payload(128, pair_b.data(), pair_b.size());
              if (push_frame(depacketizer, payload, timestamp++, output,
                             &output_size) != RtpJpegPushResult::DROPPED) {
                return 12;
              }
              payload = make_payload(128, nullptr, 0);
              if (push_frame(depacketizer, payload, timestamp++, output,
                             &output_size) != RtpJpegPushResult::COMPLETE ||
                  !output_has_tables(output.data(), output_size, pair_a.data(),
                                     pair_a.data() + 64)) {
                return 13;
              }

              // A frame reset retains session mappings; an RTP-session reset
              // explicitly invalidates every cached Q value.
              depacketizer.reset();
              if (push_frame(depacketizer, payload, timestamp++, output,
                             &output_size) != RtpJpegPushResult::COMPLETE) {
                return 14;
              }
              depacketizer.reset_session();
              if (push_frame(depacketizer, payload, timestamp++, output,
                             &output_size) != RtpJpegPushResult::DROPPED) {
                return 15;
              }

              const auto single_last = make_table(29);
              payload =
                  make_payload(254, single_last.data(), single_last.size());
              if (push_frame(depacketizer, payload, timestamp++, output,
                             &output_size) != RtpJpegPushResult::COMPLETE) {
                return 16;
              }
              payload = make_payload(254, nullptr, 0);
              if (push_frame(depacketizer, payload, timestamp++, output,
                             &output_size) != RtpJpegPushResult::COMPLETE ||
                  !output_has_tables(output.data(), output_size,
                                     single_last.data(), single_last.data())) {
                return 17;
              }

              // Q=255 is explicitly non-cacheable.
              payload = make_payload(255, nullptr, 0);
              if (push_frame(depacketizer, payload, timestamp++, output,
                             &output_size) != RtpJpegPushResult::DROPPED) {
                return 18;
              }
              return 0;
            }
            """
        ),
        encoding="utf-8",
    )
    executable = tmp_path / "rtp_jpeg_behavior"
    subprocess.run(
        [
            "g++",
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-DUSE_ESPHOME_VOIP_STACK_VIDEO",
            "-DUSE_ESPHOME_VOIP_STACK_VIDEO_JPEG",
            f"-I{tmp_path}",
            f"-I{ROOT}",
            str(probe),
            str(VOIP / "rtp_jpeg.cpp"),
            "-o",
            str(executable),
        ],
        check=True,
        cwd=ROOT,
    )
    subprocess.run([str(executable)], check=True, cwd=ROOT)
    video_rtp = read("video_rtp.cpp")
    negotiation = video_rtp[
        video_rtp.index("bool VideoRtpSession::set_negotiated") :
        video_rtp.index(
            "\nbool VideoRtpSession::bind_socket_",
            video_rtp.index("bool VideoRtpSession::set_negotiated"),
        )
    ]
    assert "this->jpeg_depacketizer_.reset_session();" in negotiation


def test_rejected_video_answer_preserves_the_first_offered_payload_type() -> None:
    sip_cpp = read("sip_transport.cpp")
    learn = sip_cpp[
        sip_cpp.index("bool SipTransport::learn_remote_video_from_sdp_") :
        sip_cpp.index(
            "\nbool SipTransport::send_request_",
            sip_cpp.index("bool SipTransport::learn_remote_video_from_sdp_"),
        )
    ]

    preserve = learn.index(
        "this->negotiated_video_capability_.payload_type ="
    )
    disabled = learn.index("if (media_port == 0) return true;")
    rejected_rtcp = learn.index("if (rtcp_mux_only")
    assert preserve < disabled < rejected_rtcp
    assert "candidate_order[0]" in learn[preserve : preserve + 180]


def test_video_reinvite_admits_resources_before_success_and_rolls_back() -> None:
    sip_cpp = read("sip_transport.cpp")
    reinvite = cpp_method(sip_cpp, r"SipTransport::handle_reinvite_")

    prestart = reinvite.index("this->video_session_->start(false)")
    success = reinvite.index('this->send_response_(200, "OK", answer)')
    activate = reinvite.index("this->video_session_->request_media_direction(")
    assert prestart < success < activate
    assert '"video_resources_unavailable"' in reinvite
    assert '"video_renegotiation_unsupported"' in reinvite
    assert "this->video_session_->request_media_direction(" in reinvite
    assert "old_video_send" in reinvite
    assert "old_video_receive" in reinvite


def test_reinvite_waits_for_prior_invite_ack_without_mutating_dialog() -> None:
    sip_cpp = read("sip_transport.cpp")
    reinvite = cpp_method(sip_cpp, r"SipTransport::handle_reinvite_")

    pending = reinvite.index("this->completed_invite_.awaiting_ack")
    parse_media = reinvite.index("this->learn_remote_rtp_from_sdp_")
    mutate_dialog = reinvite.index(
        'this->last_invite_via_ = header_values(message, "Via");'
    )
    assert pending < parse_media < mutate_dialog
    pending_response = reinvite[pending:parse_media]
    assert 'message, src, 500, "Server Internal Error"' in pending_response
    assert "esp_random() % 11U" in pending_response
    assert "false" in pending_response


def test_failed_reinvite_response_restores_dialog_and_is_not_cached() -> None:
    sip_cpp = read("sip_transport.cpp")
    response = sip_cpp[
        sip_cpp.index("bool SipTransport::send_response_") :
        sip_cpp.index("\nbool SipTransport::send_stateless_response_")
    ]
    assert response.index("this->send_sip_(msg, ip, port)") < response.index(
        "this->last_invite_response_ = msg;"
    )
    assert response.index("this->send_sip_(msg, ip, port)") < response.index(
        "this->remember_completed_response_"
    )

    stateless_start = sip_cpp.index(
        "bool SipTransport::send_stateless_response_"
    )
    stateless = sip_cpp[
        stateless_start :
        sip_cpp.index("\nbool SipTransport::send_invite(", stateless_start)
    ]
    assert stateless.index("this->send_sip_(msg, ip, port)") < stateless.index(
        "this->remember_completed_response_"
    )

    reinvite = cpp_method(sip_cpp, r"SipTransport::handle_reinvite_")
    failure = reinvite[
        reinvite.index('if (!this->send_response_(200, "OK", answer))') :
        reinvite.index(
            "\n  this->set_media_config_",
            reinvite.index('if (!this->send_response_(200, "OK", answer))'),
        )
    ]
    assert "restore_old_media();" in failure
    assert "restore_old_dialog_metadata();" in failure
    restore = reinvite[
        reinvite.index("const auto restore_old_dialog_metadata") :
        reinvite.index(
            "\n  };",
            reinvite.index("const auto restore_old_dialog_metadata"),
        )
    ]
    for field in (
        "last_invite_via",
        "last_invite_from",
        "last_invite_to",
        "last_invite_cseq",
        "last_invite_response",
        "last_invite_cseq_number",
        "remote_target_uri",
    ):
        assert f"this->{field}_ = old_{field};" in restore


def test_video_send_action_is_templatable_and_compile_time_gated() -> None:
    init_py = read("__init__.py")
    actions = read("actions.h")

    registration = init_py.index('"voip_stack.set_video_send"')
    registration_contract = init_py[registration : registration + 700]
    assert "cv.boolean" in registration_contract
    assert (
        "_register_templated_action" in init_py[registration - 80 : registration]
        or "cg.templatable" in registration_contract
    )

    action_match = re.search(
        r"class\s+(?P<name>\w*[Vv]ideo\w*[Ss]end\w*Action)"
        r".*?\{(?P<body>.*?)\n\};",
        actions,
        re.DOTALL,
    )
    assert action_match is not None
    action_body = action_match.group("body")
    assert re.search(r"TEMPLATABLE_VALUE\s*\(\s*bool\s*,", action_body)
    assert re.search(
        r"parent_->set_video_send(?:_enabled)?\s*\(", action_body
    )

    action_pos = action_match.start()
    guard_pos = actions.rfind(
        "#ifdef USE_ESPHOME_VOIP_STACK_VIDEO", 0, action_pos
    )
    assert guard_pos >= 0
    assert guard_pos > actions.rfind("#endif", 0, action_pos)
    assert actions.find("#endif", action_pos) >= 0


def test_video_send_switch_is_compile_gated_and_transport_confirmed() -> None:
    schema = read("switch.py")
    header = read("voip_stack.h")
    source = read("voip_stack.cpp")
    transport = read("transport.h")

    assert 'CONF_VIDEO_SEND = "video_send"' in schema
    assert "voip_stack video_send switch requires voip_stack.video." in schema
    assert "register_video_send_switch" in schema
    assert "class VoipStackVideoSendSwitch" in header
    assert "if (!this->parent_->set_video_send(state))" in header
    assert "set_video_send_state_callback" in transport
    assert "transport_video_send_state_callback_" in source
    loop = cpp_method(source, r"VoipStack::loop")
    assert "video_send_state_event_.exchange" in loop
    assert "video_send_switch_->publish_state" in loop


def test_video_lifecycle_automations_follow_transport_edges() -> None:
    init_py = read("__init__.py")
    header = read("voip_stack.h")
    source = read("voip_stack.cpp")
    transport = read("transport.h")
    sip = read("sip_transport.cpp")

    assert 'CONF_ON_VIDEO_START = "on_video_start"' in init_py
    assert 'CONF_ON_VIDEO_END = "on_video_end"' in init_py
    assert "var.get_video_start_trigger()" in init_py
    assert "var.get_video_end_trigger()" in init_py
    assert "get_video_start_trigger()" in header
    assert "get_video_end_trigger()" in header
    assert "set_video_active_state_callback" in transport
    assert "transport_video_active_state_callback_" in source
    callback = cpp_method(
        source, r"VoipStack::transport_video_active_state_callback_"
    )
    assert "defer" in callback
    edge_handler = cpp_method(source, r"VoipStack::on_video_active_state_")
    assert "video_start_trigger_.trigger()" in edge_handler
    assert "video_end_trigger_.trigger()" in edge_handler
    assert sip.count("emit_video_active_state_") >= 4
    loop = cpp_method(source, r"VoipStack::loop")
    assert "is_video_active()" not in loop


def test_video_send_control_has_a_dedicated_transport_api() -> None:
    transport = read("transport.h")
    sip_header = read("sip_transport.h")
    component_header = read("voip_stack.h")
    component_source = read("voip_stack.cpp")

    transport_api = r"(?:request|set)_video_send(?:_enabled)?"
    transport_match = re.search(
        rf"virtual\s+bool\s+(?P<name>{transport_api})\s*\(\s*bool\b",
        transport,
    )
    assert transport_match is not None
    transport_name = transport_match.group("name")
    assert re.search(
        rf"\bbool\s+{re.escape(transport_name)}\s*\(\s*bool\b"
        rf"[^;]*\boverride\s*;",
        sip_header,
    )
    component_match = re.search(
        r"\bbool\s+(?P<name>set_video_send(?:_enabled)?)\s*\(\s*bool\b",
        component_header,
    )
    assert component_match is not None
    component_name = component_match.group("name")

    component_method = cpp_method(
        component_source, rf"VoipStack::{re.escape(component_name)}"
    )
    assert f"transport_->{transport_name}(" in component_method
    assert "call(" not in component_method
    assert "call_toggle(" not in component_method

    api_pos = transport_match.start()
    guard_pos = transport.rfind(
        "#ifdef USE_ESPHOME_VOIP_STACK_VIDEO", 0, api_pos
    )
    assert guard_pos > transport.rfind("#endif", 0, api_pos)


def test_local_video_reinvite_is_an_event_driven_dialog_transaction() -> None:
    header = read("sip_transport.h")
    source = read("sip_transport.cpp")
    transaction, instance = video_reinvite_state(header)
    local_flow = cpp_methods(
        source,
        r"\w*(?:[Rr]e[Ii]nvite|[Vv]ideo_[Ss]end|"
        r"[Vv]ideo_[Dd]irection)\w*",
    )

    assert f"this->{instance}" in local_flow
    assert re.search(r'send_request_\s*\(\s*"INVITE"', local_flow)
    assert "wake_sip_task_();" in local_flow
    sender = cpp_methods(
        source, r"\w*[Vv]ideo_[Dd]irection_[Rr]e[Ii]nvite\w*"
    )
    assert re.search(rf"\bpending\s*=\s*this->{re.escape(instance)}", sender)
    assert "pending.cseq = cseq;" in sender
    assert "pending.branch = branch;" in sender
    assert not re.search(
        r"this->(?:invite_cseq_|branch_)\s*(?:=|\+\+|--)", local_flow
    )

    deadline = re.search(
        r"\b\w+\s+(?P<name>\w*deadline\w*)"
        r"\s*(?:\{[^}]*\})?\s*;",
        transaction,
        re.I,
    )
    assert deadline is not None
    deadline_name = deadline.group("name")
    sip_task = cpp_method(source, r"SipTransport::sip_task_")
    assert instance in sip_task
    assert deadline_name in sip_task
    assert "select(max_fd + 1" in sip_task
    assert "vTaskDelay" not in sip_task
    assert "pending.transaction.udp" in sip_task

    incoming = cpp_method(source, r"SipTransport::handle_reinvite_")
    pending = incoming.index(f"this->{instance}")
    parse_media = incoming.index("this->learn_remote_rtp_from_sdp_")
    assert pending < parse_media
    assert "491" in incoming[pending:parse_media]
    assert "Request Pending" in incoming[pending:parse_media]

    response = cpp_methods(
        source,
        r"(?:\w*[Rr]e[Ii]nvite\w*|"
        r"\w*[Vv]ideo_[Dd]irection\w*)[Rr]esponse\w*",
    )
    assert response
    assert re.search(r"\bACK\b", response)
    glare = braced_block_after(response, "if (status == 491")
    assert "pending.waiting_retry = true;" in glare
    assert "reset_dialog_" not in glare
    ordinary_reject = response[
        response.index("pending.clear();", response.index(glare))
        : response.index("if (status == 408")
    ]
    assert "video_send_requested_.store(previous_send" in ordinary_reject
    assert "established media retained" in ordinary_reject
    assert "reset_dialog_" not in ordinary_reject
    assert "status == 408 || status == 481" in response


def test_in_dialog_reinvite_dispatch_works_for_outbound_dialogs() -> None:
    source = read("sip_transport.cpp")
    invite = cpp_method(source, r"SipTransport::handle_invite_")
    dispatch = invite[
        invite.index("const bool in_dialog_invite") :
        invite.index("LockGuard media_lock", invite.index("const bool in_dialog_invite"))
    ]

    assert "incoming_call_id == this->call_id_" in dispatch
    assert "this->media_active_.load(std::memory_order_acquire)" in dispatch
    assert "tag_from_header(incoming_from) == this->remote_tag_" in dispatch
    assert "tag_from_header(incoming_to) == this->local_tag_" in dispatch
    assert "return this->handle_reinvite_(message, src);" in dispatch
    assert "last_invite_cseq_number_ != 0" not in dispatch


def test_in_dialog_merged_invite_is_rejected_before_reinvite_dispatch() -> None:
    source = read("sip_transport.cpp")
    invite = cpp_method(source, r"SipTransport::handle_invite_")
    merged = invite.index("SIP merged in-dialog INVITE rejected")
    dispatch = invite.index("return this->handle_reinvite_(message, src);")

    assert merged < dispatch
    guard = invite[invite.rfind("if (", 0, merged) : merged]
    assert "incoming_cseq_number == this->last_invite_cseq_number_" in guard
    assert "via_branch(incoming_via) != via_branch(this->last_invite_via_)" in guard
    assert "482" in invite[merged : dispatch]


def test_video_direction_commit_uses_prepared_media_without_polling() -> None:
    header = read("video_rtp.h")
    source = read("video_rtp.cpp")
    request = cpp_method(source, r"VideoRtpSession::request_media_direction")
    start = cpp_method(source, r"VideoRtpSession::start")

    assert "send_prepared_" in header
    assert "receive_prepared_" in header
    admission = cpp_method(
        source, r"VideoRtpSession::can_request_media_direction"
    )
    assert "send_prepared_" in admission
    assert "receive_prepared_" in admission
    assert "can_request_media_direction" in request
    assert "start_video" in request
    assert "rx_reset_requested_.store" in request
    assert "wake_task_();" in request
    assert "vTaskDelay" not in request
    assert "heap_caps_malloc" not in request
    assert "start_sender_task_" not in request
    assert "stop_sender_task_" not in request
    assert "heap_caps_malloc" in start
    assert "start_sender_task_" in start


def test_video_direction_response_uses_transaction_target_and_caches_ack() -> None:
    source = read("sip_transport.cpp")
    response = cpp_method(
        source, r"SipTransport::handle_video_direction_response_"
    )

    assert "pending.transaction.ip_v4" in response
    assert "expected_response_ip" in response
    assert "if (!ack.empty()" in response
    assert "if (ack_sent &&" not in response
    assert "completed.ack = ack;" in response


def test_local_video_send_direction_changes_do_not_stop_video_rx() -> None:
    video_header = read("video_rtp.h")
    video_source = read("video_rtp.cpp")
    sip_source = read("sip_transport.cpp")

    declaration = re.search(
        r"\bbool\s+(?P<name>\w*[Ss]end\w*)\s*\(\s*bool\b[^;]*\)\s*;",
        video_header,
    )
    assert declaration is not None
    method_name = declaration.group("name")
    direction_method = cpp_method(
        video_source,
        rf"VideoRtpSession::{re.escape(method_name)}",
    )
    for receive_teardown in (
        "this->stop()",
        "request_stop()",
        "reap_receive_task_",
        "receive_task_handle_",
        "sink_->",
        "close(",
        "shutdown(",
    ):
        assert receive_teardown not in direction_method

    local_apply = cpp_methods(
        sip_source,
        r"(?:\w*[Rr]e[Ii]nvite\w*|"
        r"\w*[Vv]ideo_[Dd]irection\w*)[Rr]esponse\w*",
    )
    assert "apply_video_direction_answer_(" in local_apply
    assert "video_session_->stop();" not in local_apply
    answer_apply = cpp_method(
        sip_source, r"SipTransport::apply_video_direction_answer_"
    )
    assert f"video_session_->{method_name}(" in answer_apply
    assert "video_session_->stop();" not in answer_apply


def test_h264_tx_failure_aborts_the_au_and_requests_resynchronization() -> None:
    video = read("video_rtp.cpp")
    packetizer = video[
        video.index("void VideoRtpSession::send_h264_access_unit_") :
        video.index("\nvoid VideoRtpSession::send_jpeg_access_unit_")
    ]

    assert "const auto fail_access_unit" in packetizer
    assert "this->tx_resync_needed_.store(true" in packetizer
    assert packetizer.count("fail_access_unit();") >= 4
    assert "sent_any |= this->send_rtp_payload_" not in packetizer
    assert re.search(
        r"if \(!this->send_rtp_payload_\([^;]+\)\) \{\s*"
        r"fail_access_unit\(\);\s*return;",
        packetizer,
        re.DOTALL,
    )


def test_video_sender_completes_owned_au_and_bounds_udp_backpressure() -> None:
    header = read("video_rtp.h")
    video = read("video_rtp.cpp")
    sip = read("sip_transport.cpp")
    codegen = read("__init__.py")
    sender = video[
        video.index("void VideoRtpSession::send_access_unit_") :
        video.index("\nvoid VideoRtpSession::task_trampoline_")
    ]
    payload = sender[
        sender.index("bool VideoRtpSession::send_rtp_payload_") :
    ]

    # Once transmission starts, finish the AU: deliberately aborting it after
    # spending bandwidth on most fragments guarantees an undecodable frame.
    # A bounded hosted queue lets the encoder finish several small H.264 frames
    # while the sender starts, and each individual UDP retry remains bounded.
    assert "tx_access_unit_deadline_ms_" not in header
    assert "tx_access_unit_deadline_expired_" not in payload
    assert "kTxPacketPacingMs" not in header
    assert (
        "#if defined(USE_ESPHOME_VOIP_STACK_VIDEO_HOSTED_AUDIO_PACING) && \\\n"
        "    defined(USE_ESPHOME_VOIP_STACK_VIDEO_JPEG)\n"
        "  if (!this->wait_for_audio_pacing_()) return abort_payload();"
        in payload
    )
    assert "if (!this->wait_for_audio_pacing_()) return;" in sender
    assert "vTaskDelay" not in payload
    for transient_error in ("EAGAIN", "EWOULDBLOCK", "ENOBUFS", "ENOMEM"):
        assert transient_error in payload
    assert "attempt < 2" in payload
    assert "FD_SET(this->rtp_socket_, &writefds)" in payload
    assert "select(this->rtp_socket_ + 1" in payload
    assert "pdMS_TO_TICKS(20)" not in payload
    assert "DSCP AF41" in video

    # ESP-Hosted has one blocking SDIO queue for both flows. Its compile-time
    # gated scheduler collapses audio completions into one binary event. H.264
    # consumes one event per complete dependency unit, while JPEG retains a
    # bounded packet burst; native-WiFi builds carry none of it.
    assert "USE_ESPHOME_VOIP_STACK_VIDEO_HOSTED_AUDIO_PACING" in codegen
    assert '"esp32_hosted" in (CORE.config or {})' in codegen
    assert "xSemaphoreCreateBinaryStatic(&this->audio_pacing_storage_)" in video
    assert "xSemaphoreCreateCounting" not in video
    assert "xSemaphoreTake(this->audio_pacing_, portMAX_DELAY)" in video
    assert "xSemaphoreGive(this->audio_pacing_)" in video
    assert "kVideoPacketsPerAudioCredit = 16" in header
    assert "kMaxAccessUnitBytes = 128 * 1024" in header
    assert "kMaxAccessUnitBytes = 512 * 1024" in header
    assert header.count("kTxAccessUnitSlots = 2") == 2
    assert "TxAccessUnitSlot tx_access_units_[kTxAccessUnitSlots]" in header
    assert "kMaxReceiveBatchPackets = 64" in header
    assert "kVideoPacketsPerAudioCredit - 1" in video
    assert "audio_pacing_burst_remaining_" in header
    assert "kAudioPacingStartupWaitMs" not in header
    assert "audio_pacing_startup_fallback" not in video
    assert "pdMS_TO_TICKS(kAudioPacingStartupWaitMs)" not in video
    assert "pdMS_TO_TICKS(100)" not in video
    assert "this->video_session_->notify_audio_packet_sent();" in sip

    voip_header = read("voip_stack.h")
    voip_source = read("voip_stack.cpp")
    assert "static constexpr uint8_t kTxTaskPriority = 24;" in voip_header
    assert "static constexpr uint8_t kTxTaskPriority = 15;" in voip_header
    assert "static constexpr uint8_t kRxTaskPriority = 15;" in voip_header
    assert "static constexpr uint8_t kSenderTaskPriority = 8;" in header
    assert "static constexpr uint8_t kSenderTaskPriority = 7;" in header
    assert "static constexpr BaseType_t kSenderTaskCore = 1;" in header
    assert "static constexpr BaseType_t kSenderTaskCore = 0;" in header
    assert "static constexpr uint8_t kReceiveTaskPriority = 7;" in header
    assert "VoipStack::kTxTaskPriority" in voip_source
    assert "VoipStack::kRxTaskPriority" in voip_source
    assert (
        "kSenderTaskStackBytes, this, kSenderTaskPriority, kSenderTaskCore"
        in video
    )
    assert "this, kReceiveTaskPriority" in video


def test_video_offer_does_not_invent_a_missing_endpoint_capability() -> None:
    sip_cpp = read("sip_transport.cpp")
    capabilities = sip_cpp[
        sip_cpp.index("VideoCapability SipTransport::local_video_send_capability_") :
        sip_cpp.index("\nvoid SipTransport::reset_video_negotiation_")
    ]

    assert "if (this->video_source_ == nullptr)" in capabilities
    assert "if (this->video_sink_ == nullptr)" in capabilities
    assert capabilities.count("capability.max_fps = 0;") == 2
    assert "else if (this->video_source_ != nullptr)" not in capabilities

    append = sip_cpp[
        sip_cpp.index("std::string SipTransport::append_video_sdp_") :
        sip_cpp.index("\nstd::string SipTransport::build_sdp_offer_")
    ]
    assert "const bool offer_send =" in append
    assert "const bool offer_receive =" in append
    assert (
        "answer ? this->negotiated_video_capability_\n"
        "             : offer_receive ? local_receive : local_send"
        in append
    )
    assert append.index("const VideoCapability local_receive") < append.index(
        "VideoCapability capability ="
    )
    assert append.index(": offer_receive ? local_receive : local_send") < append.index(
        'out += "a=fmtp:"'
    )
    assert "RFC 6184 profile-level-id" in append
    assert 'const char *direction =' in append
    assert '"sendrecv" : send ? "sendonly" : "recvonly"' in append


def test_media_lifecycle_is_serialized_across_fsm_and_sip_tasks() -> None:
    header = read("sip_transport.h")
    source = read("sip_transport.cpp")

    assert "mutable Mutex media_lifecycle_mutex_;" in header
    start = source[
        source.index("bool SipTransport::start_audio_path()") :
        source.index("\nvoid SipTransport::stop_audio_path()")
    ]
    stop = source[
        source.index("void SipTransport::stop_audio_path()") :
        source.index("\nbool SipTransport::originate")
    ]
    reinvite = source[
        source.index("bool SipTransport::handle_reinvite_") :
        source.index("\nbool SipTransport::handle_response_")
    ]
    lock = "LockGuard media_lock(this->media_lifecycle_mutex_);"
    assert lock in start
    assert lock in stop
    assert lock in reinvite
    assert reinvite.index(lock) < reinvite.index("this->video_session_->start(false)")
    reset = source[
        source.index("void SipTransport::reset_dialog_()") :
        source.index("\nvoid SipTransport::remember_udp_transaction_")
    ]
    assert lock in reset
    assert "this->request_audio_path_stop_locked_();" in reset
    assert reset.index(lock) < reset.index("this->reset_video_negotiation_();")
    disconnect = source[
        source.index("void SipTransport::disconnect()") :
        source.index("\nbool SipTransport::start_audio_path()")
    ]
    assert "this->stop_audio_path();" not in disconnect
    for begin, end, operation in (
        ("bool SipTransport::send_invite(", "\nvoid SipTransport::send_audio_frame", "this->build_sdp_offer_()"),
        ("bool SipTransport::send_answer(", "\nbool SipTransport::send_cancel", "this->build_sdp_answer_()"),
        ("bool SipTransport::handle_invite_(", "\nbool SipTransport::handle_reinvite_", "this->learn_remote_rtp_from_sdp_(body, src_ip)"),
        ("bool SipTransport::handle_response_(", "\nvoid SipTransport::handle_sip_datagram_", "this->learn_remote_rtp_from_sdp_"),
    ):
        section = source[source.index(begin) : source.index(end)]
        assert lock in section
        assert section.index(lock) < section.index(operation)


def test_incoming_answer_prepares_before_200_and_commits_after_send() -> None:
    transport = read("transport.h")
    header = read("sip_transport.h")
    source = read("sip_transport.cpp")
    fsm = read("voip_fsm.cpp")

    for method in (
        "prepare_media_path()",
        "commit_media_path()",
        "abort_media_path()",
    ):
        assert method in transport
        assert method in header
    assert "PREPARED = 4" in header

    prepare = cpp_method(source, r"SipTransport::prepare_media_path_locked_")
    commit_public = cpp_method(source, r"SipTransport::commit_media_path")
    commit = cpp_method(source, r"SipTransport::commit_media_path_locked_")
    commit_failure = cpp_method(
        source, r"SipTransport::terminate_dialog_after_media_commit_failure_"
    )
    abort = cpp_method(source, r"SipTransport::abort_media_path")
    assert "this->bind_udp_(&this->rtp_socket_" in prepare
    assert "this->prepare_video_session_locked_()" in prepare
    assert "MediaLifecyclePhase::PREPARED" in prepare
    assert "this->rtp_running_.store(true" not in prepare
    assert "xTaskNotifyGive(this->rtp_task_handle_)" not in prepare
    assert "request_media_direction(" not in prepare

    assert "MediaLifecyclePhase::ACTIVE" in commit
    assert "this->rtp_running_.store(true" in commit
    assert "xTaskNotifyGive(this->rtp_task_handle_)" in commit
    assert "request_media_direction(" in commit
    assert commit.index("MediaLifecyclePhase::ACTIVE") < commit.index(
        "MediaLifecyclePhase::PREPARED"
    )
    assert "terminate_dialog_after_media_commit_failure_();" in commit_public
    assert "this->completed_invite_.awaiting_ack" in commit_failure
    assert "this->terminate_after_invite_ack_ = true;" in commit_failure
    assert "this->wake_sip_task_();" in commit_failure
    assert "this->send_bye_unlocked_(this->call_id_)" in commit_failure
    assert "this->reset_dialog_();" not in commit_failure
    acknowledge = source[
        source.index("uint16_t SipTransport::acknowledge_completed_invite_") :
        source.index("\nbool SipTransport::replay_completed_invite_ack_")
    ]
    datagram = cpp_method(source, r"SipTransport::handle_sip_datagram_")
    assert "this->terminate_after_invite_ack_" in acknowledge
    assert "if (terminate_after_ack)" in datagram
    assert "this->send_bye_unlocked_(call_id)" in datagram
    assert "this->stop_audio_path();" in abort

    manual = cpp_method(fsm, r"VoipStack::answer_call")
    assert manual.index("prepare_media_path()") < manual.index(
        "set_audio_devices_active_(true)"
    ) < manual.index("send_sip_answer_") < manual.index(
        "commit_media_path()"
    )
    assert manual.count("abort_media_path()") == 2

    auto_start = fsm.index("      if (this->auto_answer_)")
    auto_end = fsm.index("      } else {", auto_start)
    auto_answer = fsm[auto_start:auto_end]
    assert auto_answer.index("prepare_media_path()") < auto_answer.index(
        "set_audio_devices_active_(true)"
    ) < auto_answer.index("send_sip_answer_") < auto_answer.index(
        "commit_media_path()"
    )
    assert auto_answer.count("abort_media_path()") == 2

    outbound = fsm[
        fsm.index("case SipSignalType::STATUS_200_OK") :
        fsm.index("case SipSignalType::CANCEL")
    ]
    assert "this->transport_->start_audio_path()" in outbound


def test_video_workers_share_one_bounded_stop_deadline_without_forced_delete() -> None:
    header = read("video_rtp.h")
    video = read("video_rtp.cpp")
    stop = cpp_method(video, r"VideoRtpSession::stop")
    sender_stop = cpp_method(video, r"VideoRtpSession::stop_sender_task_")
    receiver_stop = cpp_method(video, r"VideoRtpSession::stop_receive_task_")
    destructor = video[
        video.index("VideoRtpSession::~VideoRtpSession()") :
        video.index("\nbool VideoRtpSession::set_negotiated")
    ]

    assert "kWorkerStopBudgetMs = 1000" in header
    assert "const TickType_t stop_started = xTaskGetTickCount();" in stop
    assert "const TickType_t stop_budget" in stop
    assert "stop_sender_task_(stop_started, stop_budget)" in stop
    assert "stop_receive_task_(stop_started, stop_budget)" in stop
    for worker_stop in (sender_stop, receiver_stop):
        assert "xTaskGetTickCount() - stop_started" in worker_stop
        assert "elapsed < stop_budget ? stop_budget - elapsed : 0" in worker_stop
        assert "pdMS_TO_TICKS(1000)" not in worker_stop
    assert "force" not in stop.lower()
    assert "this->quiesce_tasks_();" in destructor


def test_call_teardown_is_deferred_to_the_event_driven_rtp_worker() -> None:
    header = read("sip_transport.h")
    source = read("sip_transport.cpp")
    video = read("video_rtp.cpp")

    assert "enum class MediaLifecyclePhase" in header
    assert "MediaLifecyclePhase::CLEANING" in source
    assert "SemaphoreHandle_t rtp_cleanup_done_{nullptr};" in header
    assert "SemaphoreHandle_t rtp_task_done_{nullptr};" in header
    public_stop = source[
        source.index("void SipTransport::stop_audio_path()") :
        source.index("\nvoid SipTransport::request_audio_path_stop_locked_")
    ]
    request_stop = source[
        source.index("void SipTransport::request_audio_path_stop_locked_") :
        source.index("\nvoid SipTransport::finish_audio_path_stop_")
    ]
    worker = source[
        source.index("void SipTransport::rtp_task_()") :
        source.index("\n}  // namespace voip_stack")
    ]

    assert "this->request_audio_path_stop_locked_();" in public_stop
    assert "xSemaphoreTake" not in public_stop
    assert "this->video_session_->request_stop();" in request_stop
    assert "MediaLifecyclePhase::CLEANING" in request_stop
    assert "this->finish_audio_path_stop_();" in worker
    assert "ulTaskNotifyTake(pdTRUE, portMAX_DELAY);" in worker
    assert "xSemaphoreGive(this->rtp_cleanup_done_);" in worker
    assert "xSemaphoreGive(this->rtp_task_done_);" in worker

    video_request = video[
        video.index("void VideoRtpSession::request_stop()") :
        video.index("\nvoid VideoRtpSession::stop()")
    ]
    video_stop = video[
        video.index("void VideoRtpSession::stop()") :
        video.index("\nbool VideoRtpSession::reap_receive_task_")
    ]
    assert "source_->stop_video()" not in video_request
    assert "send_rtcp_bye_()" not in video_request
    assert "rtcp_bye_requested_" in video_request
    assert "source_->stop_video()" in video_stop
    assert "sink_->stop_video()" in video_stop
    video_worker = cpp_method(video, r"VideoRtpSession::task_")
    assert "rtcp_bye_requested_.exchange" in video_worker
    assert "send_rtcp_bye_()" in video_worker
    assert "sink_->set_video_active(false)" not in video_worker
    assert video_worker.count("sink_->stop_video()") == 1
    assert "FD_SET(this->wake_socket_, &readfds)" in video_worker
    assert "FD_ISSET(this->wake_socket_, &readfds)" in video_worker
    assert re.search(
        r"if \(worker_failed &&\s+"
        r"this->sink_started_\.exchange\(false, std::memory_order_acq_rel\) &&\s+"
        r"this->sink_ != nullptr\)",
        video_worker,
    )
    video_wake = video[
        video.index("void VideoRtpSession::wake_task_()") :
        video.index("\n}  // namespace voip_stack")
    ]
    assert "this->wake_socket_" in video_wake
    assert "this->wake_port_" in video_wake
    assert "this->rtp_socket_" not in video_wake
    assert 'socket.consume_sockets(3, "voip_stack_video"' in read("__init__.py")

    shutdown = source[
        source.index("void SipTransport::stop()") :
        source.index("\nbool SipTransport::is_connected()")
    ]
    assert "xSemaphoreTake(this->rtp_cleanup_done_, portMAX_DELAY);" in shutdown
    assert "xSemaphoreTake(this->rtp_task_done_, portMAX_DELAY);" in shutdown
    assert "pdMS_TO_TICKS" not in shutdown

    invite = source[
        source.index("bool SipTransport::handle_invite_(") :
        source.index("\nbool SipTransport::handle_reinvite_(")
    ]
    reinvite = source[
        source.index("bool SipTransport::handle_reinvite_(") :
        source.index("\nbool SipTransport::handle_response_(")
    ]
    assert 'message, src, 503, "Service Unavailable", "media_cleanup"' in invite
    assert "MediaLifecyclePhase::CLEANING" in reinvite
    assert 'message, src, 491, "Request Pending"' in reinvite


def test_endpoint_requires_at_least_one_audio_direction() -> None:
    init_py = read("__init__.py")
    header = read("voip_stack.h")

    assert "voip_stack requires at least one audio direction" in init_py
    assert "CONF_MICROPHONE not in config" in init_py
    assert "CONF_MICROPHONE_SOURCE not in config" in init_py
    assert "CONF_SPEAKER not in config" in init_py
    assert 'return "control_only"' not in header


def test_optional_entity_platforms_are_feature_gated_not_autoloaded() -> None:
    init_py = read("__init__.py")
    header = read("voip_stack.h")
    autoload = init_py.split("def AUTO_LOAD", 1)[1].split("\n\n", 1)[0]
    for platform in ("button", "number", "switch", "text", "text_sensor"):
        assert f'"{platform}"' not in autoload
    for flag, component in (
        ("USE_BUTTON", "button"),
        ("USE_NUMBER", "number"),
        ("USE_SWITCH", "switch"),
        ("USE_TEXT", "text"),
        ("USE_TEXT_SENSOR", "text_sensor"),
    ):
        assert (
            f'#ifdef {flag}\n#include "esphome/components/{component}/'
        ) in header


def test_yaml_lifecycle_callbacks_receive_stable_peer_identity() -> None:
    init_py = read("__init__.py")
    fsm = read("voip_fsm.cpp")
    header = read("voip_stack.h")

    for trigger in ("ringing", "in_call", "calling", "dest_ringing"):
        assert f"Trigger<std::string> *get_{trigger}_trigger()" in header
        assert f'var.get_{trigger}_trigger(), [(cg.std_string, "peer")]' in init_py

    assert "Trigger<std::string, std::string> *get_hangup_trigger()" in header
    assert "Trigger<std::string, std::string> *get_call_failed_trigger()" in header
    assert '[(cg.std_string, "peer"), (cg.std_string, "reason")]' in init_py
    assert "Trigger<std::string> *get_destination_changed_trigger()" in header
    assert "Trigger<std::string> *get_phonebook_update_trigger()" in header
    assert 'var.get_destination_changed_trigger(), [(cg.std_string, "destination")]' in init_py
    assert 'var.get_phonebook_update_trigger(), [(cg.std_string, "destination")]' in init_py
    assert "const CallSnapshot trigger_call = this->snapshot_call_identity_();" in fsm
    assert "const CallSnapshot call = this->snapshot_call_identity_();" in fsm
    assert "this->clear_call_identity_();" in fsm
    assert fsm.index("const CallSnapshot call = this->snapshot_call_identity_();") < fsm.index(
        "this->clear_call_identity_();"
    )


def test_remote_ringing_transition_emits_one_callback() -> None:
    fsm = read("voip_fsm.cpp")
    assert fsm.count("dest_ringing_trigger_.trigger(") == 1


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
    assert "if (len > capacity)" in audio
    assert "data += skipped;" in audio
    assert "const size_t dropped = skipped + replaced" in audio
    assert "return skipped == 0 && written == len;" in audio
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

    assert "MEDIA_TIMEOUT" in fsm_h
    assert 'kReasonMediaTimeout = "media_timeout"' in fsm_h
    assert "last_peer_audio_ms_" in stack_h
    assert "MEDIA_TIMEOUT_MS" in stack_h
    assert "CallEndReason::MEDIA_TIMEOUT" in stack_cpp
    assert "last_peer_audio_ms_.store(millis()" in fsm_cpp
    assert "first_audio_received_" in stack_cpp
    watchdog = fsm_cpp[
        fsm_cpp.index("void VoipStack::reset_peer_audio_watchdog_") :
        fsm_cpp.index("\nvoid VoipStack::set_in_call_")
    ]
    assert "watchdog_start = seed_from_transport ? millis() : 0" in watchdog
    rtp_rx = read("sip_transport.cpp")
    assert "out_len != rx_format.nominal_frame_bytes()" in rtp_rx


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
    assert "SIP_TRANSACTION_TIMEOUT_MS = 64 * SIP_T1_MS" in sip_cpp
    assert "txn->deadline_ms = now + SIP_TRANSACTION_TIMEOUT_MS" in sip_cpp
    assert "txn.retries++;" in sip_cpp
    assert "if (sent)" in sip_cpp
    response = sip_cpp[
        sip_cpp.index("bool SipTransport::handle_response_") :
        sip_cpp.index("\nvoid SipTransport::handle_sip_datagram_")
    ]
    assert "if (status < 200 && !this->pending_invite_.empty())" in response
    assert "this->pending_invite_.completed = true;" in response
    assert "this->pending_invite_.next_ms = this->pending_invite_.deadline_ms;" in response


def test_completed_sip_server_transactions_are_bounded_and_replayed() -> None:
    sip_h = read("sip_transport.h")
    sip_cpp = read("sip_transport.cpp")

    assert "CompletedServerTransaction completed_invite_;" in sip_h
    assert "CompletedServerTransaction completed_control_;" in sip_h
    assert "std::vector<CompletedServerTransaction>" not in sip_h
    assert "replay_completed_response_" in sip_h
    assert "remember_completed_response_" in sip_h
    datagram = sip_cpp[
        sip_cpp.index("void SipTransport::handle_sip_datagram_") :
        sip_cpp.index("\nbool SipTransport::reject_if_stale_dialog_")
    ]
    replay = datagram.index("this->replay_completed_response_")
    stale_bye = datagram.index('this->reject_if_stale_dialog_(msg, src, "BYE")')
    assert replay < stale_bye
    assert 'this->send_stateless_response_(msg, src, 200, "OK", "", true);' in datagram
    assert 'this->replay_completed_response_(message, src, "INVITE")' in sip_cpp


def test_retransmitted_invite_final_replays_the_cached_ack() -> None:
    sip_h = read("sip_transport.h")
    sip_cpp = read("sip_transport.cpp")

    assert "CompletedInviteClientTransaction completed_invite_client_;" in sip_h
    assert "replay_completed_invite_ack_" in sip_h
    assert "remember_completed_invite_ack_" in sip_h
    request = sip_cpp[
        sip_cpp.index("bool SipTransport::send_request_(const std::string &method, const std::string &body,") :
        sip_cpp.index("\nbool SipTransport::send_invite_error_ack_")
    ]
    assert request.index("remember_completed_invite_ack_") < request.index("const bool sent")
    response = sip_cpp[
        sip_cpp.index("bool SipTransport::handle_response_") :
        sip_cpp.index("\nvoid SipTransport::handle_sip_datagram_")
    ]
    assert response.index("replay_completed_invite_ack_") < response.index("response_call_id.empty()")


def test_sip_responses_preserve_the_full_via_chain() -> None:
    sip_cpp = read("sip_transport.cpp")

    assert "std::string header_values(" in sip_cpp
    assert 'out += "\\r\\nVia: ";' in sip_cpp
    assert 'const std::string incoming_via = header_values(message, "Via");' in sip_cpp
    assert 'const std::string via = header_values(request, "Via");' in sip_cpp
    assert 'via.find("\\r\\nVia: ")' in sip_cpp


def test_sip_header_parsing_stops_before_sdp_and_validates_datagram_framing() -> None:
    sip_cpp = read("sip_transport.cpp")

    header_parser = sip_cpp[
        sip_cpp.index("std::string header_value") :
        sip_cpp.index("\nstd::string header_values")
    ]
    assert "if (line.empty()) break;" in header_parser
    datagram = sip_cpp[
        sip_cpp.index("void SipTransport::handle_sip_datagram_") :
        sip_cpp.index("\nbool SipTransport::reject_if_stale_dialog_")
    ]
    assert "const bool invalid_framing = !sip_content_length" in datagram
    assert "declared_body_len != msg.size() - body_separator - 4" in datagram
    assert 'this->send_stateless_response_(msg, src, 400, "Bad Request")' in datagram


def test_phonebook_capacity_and_transport_updates_are_centralized() -> None:
    phonebook = read("phonebook.h")
    settings = read("voip_settings.cpp")

    merge = phonebook[
        phonebook.index("AddResult merge_") :
        phonebook.index("\n  static bool same_entry_")
    ]
    assert "if (!valid_entry_(incoming))" in merge
    assert "this->entries_.size() >= MAX_CONTACTS" in merge
    assert "existing.sip_transport_tcp == incoming.sip_transport_tcp" in merge
    add_batch = phonebook[
        phonebook.index("bool add_batch") :
        phonebook.index("\n  AddResult add_entry")
    ]
    assert "processed < MAX_CONTACTS" in add_batch
    assert "this->entries_.size() < MAX_CONTACTS" not in add_batch
    assert "MAX_ROSTER_JSON_BYTES = 32768" in settings
    assert "roster_json.size() > MAX_ROSTER_JSON_BYTES" in settings


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
    assert "SIP TCP invalid or ambiguous Content-Length" in stream
    assert stream.count("this->handle_tcp_peer_loss_();") >= 3
    assert 'normalized == "content-length" || normalized == "l"' in sip_cpp

    send = sip_cpp[
        sip_cpp.index("bool SipTransport::send_sip_tcp_") :
        sip_cpp.index("\nstd::string SipTransport::wrap_sdp_envelope_")
    ]
    assert "const bool replacing_session" in send
    assert "sip_tcp_client_close_requested_" in send
    assert "tcp_connect_requested_" in send
    assert "if (socket < 0 || replacing_session)" in send
    assert "send_sip_tcp_record_(message, socket)" in send
    assert "this->wake_sip_task_();" in send

    promote = sip_cpp[sip_cpp.index("auto promote_tcp_connect") : sip_cpp.index("while (this->running_")]
    assert "LockGuard send_lock(this->tcp_send_mutex_);" in promote
    assert "pending.swap(this->tcp_tx_pending_);" in promote
    assert "send_sip_tcp_record_(pending, promoted_fd)" in promote

    accept = sip_cpp[sip_cpp.index("int client = accept(") : sip_cpp.index("if (this->sip_socket_ >= 0", sip_cpp.index("int client = accept("))]
    assert "this->dialog_active_()" in accept
    assert "active_ip_v4 != accepted_ip_v4" in accept
    assert "signaling_owned" in accept
    assert "signaling_uses_tcp" in accept
    assert "terminal_transaction_pending_locked_()" in accept
    assert "replaces_owned_transport" in accept
    assert "SIP TCP accept rejected: signaling transport owned" in accept

    peer_loss = cpp_method(sip_cpp, r"SipTransport::handle_tcp_peer_loss_")
    assert "this->terminal_transaction_pending_locked_()" in peer_loss
    assert "!this->completed_invite_.udp" in peer_loss
    assert "this->completed_invite_.clear();" in peer_loss
    assert "!this->completed_control_.udp" in peer_loss
    assert "this->completed_control_.clear();" in peer_loss
    assert "!this->completed_invite_client_.udp" in peer_loss
    assert "this->completed_invite_client_.clear();" in peer_loss
    assert "this->terminate_after_invite_ack_ = false;" in peer_loss
    assert peer_loss.index("this->completed_invite_.clear();") < peer_loss.index(
        "this->remote_sip_tcp_.store(false"
    )


def test_endpoint_group_membership_is_optional_and_forward_compatible() -> None:
    init_py = read("__init__.py")
    header = read("voip_stack.h")
    stack_cpp = read("voip_stack.cpp")
    text_py = read("text.py")
    text_sensor_py = read("text_sensor.py")
    switch_py = read("switch.py")

    assert 'CONF_CONFERENCE_GROUPS = "conference_groups"' in init_py
    assert 'CONF_CONFERENCE_RING = "conference_ring"' in init_py
    assert 'CONF_RING_GROUPS = "ring_groups"' in init_py
    assert "_validate_endpoint_label" in init_py
    assert 'len(value.encode("utf-8")) > 32' in init_py
    assert 'len(group.encode("utf-8")) > 32' in init_py
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
    assert '"%s | %s | %u | %u | %s | %s | %s | %s | %s%s"' in endpoint
    assert 'video_extra = " | video=jpeg"' in endpoint
    assert 'video_extra = " | video=h264"' in endpoint
    assert 'video_extra = ""' in endpoint
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

    settings = read("voip_settings.cpp")
    roster_parser = settings[
        settings.index("bool parse_json_roster_slot") : settings.index("\n}  // namespace")
    ]
    assert "if (!valid_name(name)) name = id;" in roster_parser


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


def test_sip_task_self_terminates_before_its_stack_is_released() -> None:
    sip_h = read("sip_transport.h")
    sip_cpp = read("sip_transport.cpp")
    task_utils = read("audio_core_task_utils.h")

    assert "SemaphoreHandle_t sip_task_done_{nullptr};" in sip_h
    assert "StaticSemaphore_t sip_task_done_storage_{};" in sip_h
    stop = sip_cpp[sip_cpp.index("void SipTransport::stop()") : sip_cpp.index("\nbool SipTransport::is_connected")]
    assert "this->wake_sip_task_();" in stop
    assert "xSemaphoreTake(this->sip_task_done_" in stop
    assert "cleanup_pinned_task(&this->sip_task_handle_" in stop
    assert "force_delete_pinned_task(&this->sip_task_handle_" not in stop
    assert stop.index("xSemaphoreTake(this->sip_task_done_") < stop.index("close(this->sip_socket_)")
    task = sip_cpp[sip_cpp.index("void SipTransport::sip_task_()") : sip_cpp.index("\nvoid SipTransport::rtp_task_()")]
    assert "xSemaphoreGive(this->sip_task_done_)" in task
    assert "taskYIELD();" in task_utils
    assert "retaining stack to avoid UAF" in task_utils
    assert "vTaskDelay(" not in task_utils


def test_rtp_task_is_preallocated_and_parked_between_calls() -> None:
    sip_h = read("sip_transport.h")
    sip_cpp = read("sip_transport.cpp")

    start = sip_cpp[sip_cpp.index("bool SipTransport::start()") : sip_cpp.index("\nvoid SipTransport::request_tcp_client_close_")]
    media_start = sip_cpp[sip_cpp.index("bool SipTransport::start_audio_path()") : sip_cpp.index("\nvoid SipTransport::stop_audio_path()")]
    media_stop = sip_cpp[sip_cpp.index("void SipTransport::stop_audio_path()") : sip_cpp.index("\nbool SipTransport::originate")]
    task = sip_cpp[sip_cpp.index("void SipTransport::rtp_task_()") :]

    assert 'start_pinned_task(SipTransport::rtp_task_trampoline_, "voip_rtp"' in start
    assert "start_pinned_task" not in media_start
    assert "xSemaphoreCreateBinaryStatic(&this->rtp_task_done_storage_)" in start
    assert "StaticSemaphore_t rtp_task_done_storage_{};" in sip_h
    assert "std::atomic<bool> rtp_task_quiesced_{true};" in sip_h
    assert "std::atomic<bool> rtp_task_terminate_{false};" in sip_h
    assert "ulTaskNotifyTake(pdTRUE, portMAX_DELAY);" in task
    assert "rtp_task_quiesced_.store(true" in task
    assert "cleanup_pinned_task" not in media_stop
    assert "vTaskDelay(" not in task


def test_active_rtp_wake_does_not_leave_a_stale_task_notification() -> None:
    sip_cpp = read("sip_transport.cpp")

    wake = sip_cpp[sip_cpp.index("void SipTransport::wake_rtp_task_()") : sip_cpp.index("\nvoid SipTransport::stop()")]
    socket_branch = wake[wake.index("if (socket >= 0)") : wake.index("else if")]
    parked_branch = wake[wake.index("else if") :]
    assert "sendto(socket" in socket_branch
    assert "xTaskNotifyGive" not in socket_branch
    assert "xTaskNotifyGive" in parked_branch


def test_rtp_receive_hot_path_uses_atomic_state_not_call_id_strings() -> None:
    fsm = read("voip_fsm.cpp")

    receive = fsm[fsm.index("void VoipStack::on_audio_received_") : fsm.index("\nvoid VoipStack::on_sip_signal_received_")]
    assert "CallState::CONNECTING" in receive
    assert "CallState::IN_CALL" in receive
    assert "get_current_call_id_" not in receive
    assert "recent_terminal_call_" not in receive
    assert "set_in_call_" not in receive

    ok_response = fsm[fsm.index("case SipSignalType::STATUS_200_OK") : fsm.index("case SipSignalType::CANCEL")]
    assert ok_response.index("set_call_state_(CallState::CONNECTING)") < ok_response.index("set_in_call_(true)")


def test_rx_gap_playout_has_only_one_blocking_frame_budget() -> None:
    audio = read("voip_audio.cpp")

    rx_task = audio[audio.index("void VoipStack::rx_task_()") : audio.index("\nvoid VoipStack::reset_rx_audio_()")]
    assert "ulTaskNotifyTake(pdTRUE, frame_ticks);" in rx_task
    assert "play_silence_frame_(SilenceReason::NETWORK_GAP, 0);" in rx_task
    assert "play_silence_frame_(SilenceReason::NETWORK_GAP, frame_ticks);" not in rx_task
    assert "const TickType_t wait_started = xTaskGetTickCount();" in rx_task
    assert "remaining = frame_ticks - elapsed;" in rx_task
    assert "vTaskDelay(" not in rx_task


def test_decline_keeps_an_outgoing_cancel_transaction_alive() -> None:
    fsm = read("voip_fsm.cpp")

    decline = fsm[
        fsm.index("void VoipStack::decline_call") :
        fsm.index("\nvoid VoipStack::call_toggle")
    ]
    assert "const bool cancelling_outgoing = this->is_calling();" in decline
    assert "waiting_for_terminal_response = cancelling_outgoing && sent;" in decline
    assert "!waiting_for_terminal_response" in decline


def test_sip_control_callbacks_cross_to_the_esphome_loop() -> None:
    stack = read("voip_stack.cpp")

    sip_callback = stack[
        stack.index("void VoipStack::transport_sip_signal_callback_") :
        stack.index("\nvoid VoipStack::transport_connection_callback_")
    ]
    connection_callback = stack[
        stack.index("void VoipStack::transport_connection_callback_") :
        stack.index("\nbool VoipStack::transport_accept_callback_")
    ]
    assert "self->defer(" in sip_callback
    assert "on_sip_signal_received_" in sip_callback
    assert "enable_loop_soon_any_context" in sip_callback
    assert "self->defer(" in connection_callback
    assert "on_connection_change_" in connection_callback
    assert "enable_loop_soon_any_context" in connection_callback


def test_rtp_socket_close_is_serialized_with_the_final_tx_send() -> None:
    sip_h = read("sip_transport.h")
    sip_cpp = read("sip_transport.cpp")

    assert "mutable Mutex rtp_socket_mutex_;" in sip_h
    send = sip_cpp[sip_cpp.index("void SipTransport::send_audio_frame") : sip_cpp.index("\nbool SipTransport::send_ringing")]
    stop = sip_cpp[sip_cpp.index("void SipTransport::stop_audio_path") : sip_cpp.index("\nbool SipTransport::originate")]
    assert "LockGuard socket_lock(this->rtp_socket_mutex_);" in send
    assert "LockGuard socket_lock(this->rtp_socket_mutex_);" in stop
    assert send.index("LockGuard socket_lock") < send.index("sendto(this->rtp_socket_")
    assert send.index("pcm_to_rtp_payload") < send.index("LockGuard socket_lock")
    assert send.count("rtp_running_.load") >= 3
    assert stop.index("LockGuard socket_lock") < stop.index("close(this->rtp_socket_)")
    media_start = sip_cpp[
        sip_cpp.index("bool SipTransport::start_audio_path") :
        sip_cpp.index("\nvoid SipTransport::stop_audio_path")
    ]
    assert "rtp_sequence_.store(static_cast<uint16_t>(esp_random())" in media_start
    assert "rtp_timestamp_.store(esp_random()" in media_start
    assert "this->rtp_ssrc_ = esp_random();" in media_start


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
    assert "3261 section 17.1.1.3" in sip_cpp
    assert 'return this->send_request_("ACK", "", options);' in sip_cpp
    assert "this->send_invite_error_ack_();" in sip_cpp


def test_tcp_connect_queue_never_overwrites_the_pending_invite() -> None:
    sip_cpp = read("sip_transport.cpp")

    send_tcp = sip_cpp[
        sip_cpp.index("bool SipTransport::send_sip_tcp_") :
        sip_cpp.index("\nstd::string SipTransport::wrap_sdp_envelope_", sip_cpp.index("bool SipTransport::send_sip_tcp_"))
    ]
    assert "if (!this->tcp_tx_pending_.empty())" in send_tcp
    assert "return false;" in send_tcp
    reset = sip_cpp[
        sip_cpp.index("void SipTransport::reset_dialog_()") :
        sip_cpp.index("\nvoid SipTransport::remember_udp_transaction_", sip_cpp.index("void SipTransport::reset_dialog_()"))
    ]
    assert "this->tcp_tx_pending_.clear();" in reset


def test_all_meaningful_invite_progress_responses_stop_local_calling_state() -> None:
    sip_cpp = read("sip_transport.cpp")

    assert 'if (status > 100 && status < 200 && method == "INVITE")' in sip_cpp
    assert "signal.status_code = static_cast<uint16_t>(status);" in sip_cpp


def test_reinvite_and_rtp_latch_are_explicit() -> None:
    sip_h = read("sip_transport.h")
    sip_cpp = read("sip_transport.cpp")

    assert "last_invite_cseq_number_" in sip_h
    assert "cseq_number(" in sip_cpp
    assert "bool handle_reinvite_" in sip_h
    reinvite = cpp_method(sip_cpp, r"SipTransport::handle_reinvite_")
    assert "RFC 3261 section 14" in reinvite
    assert 'signal.reason = "video_activation_failed";' in reinvite
    assert "this->terminate_after_invite_ack_ = true;" in reinvite
    assert "signal.terminal_transaction_pending = true;" in reinvite
    assert "this->video_session_->request_media_direction(" in reinvite
    assert "this->video_session_->stop();" not in reinvite
    assert 'this->send_response_(200, "OK", answer)' in reinvite
    assert '"media_incompatible"' in reinvite
    assert "latched_rtp_ip_v4_" in sip_h
    assert "latched_rtp_port_" in sip_h
    assert "latched_rtp_ssrc_" in sip_h
    assert "rtp_ssrc_latched_" in sip_h
    assert "latched_rtp_port_.store" in sip_cpp
    assert "latched_rtp_ssrc_.load" in sip_cpp
    assert "remote_rtp_port_.store(src_port" in sip_cpp
    assert "uint8_t pcm[2048];" in sip_cpp


def test_simultaneous_invites_preserve_local_transaction_and_reject_glare() -> None:
    sip_cpp = read("sip_transport.cpp")
    fsm = read("voip_fsm.cpp")
    inbound = sip_cpp[
        sip_cpp.index("bool SipTransport::handle_invite_") :
        sip_cpp.index("\nbool SipTransport::handle_response_")
    ]

    assert "const bool glare = this->outgoing_invite_pending_" in inbound
    assert "active_peer_ip == src_ip" in inbound
    assert "incoming_caller_name == this->dest_name_" in inbound
    glare = inbound[
        inbound.index("// One transport owns one dialog") :
        inbound.index("\n  const uint32_t active_peer_ip", inbound.index("// One transport owns one dialog"))
    ]
    assert 'message, src, 491, "Request Pending", "glare", true' in glare
    assert "static_cast<int>(esp_random() % 3U)" in glare
    assert "send_cancel_unlocked_" not in glare
    assert "reset_dialog_" not in glare

    invite_signal = fsm[
        fsm.index("case SipSignalType::INVITE:") :
        fsm.index("\n    case SipSignalType::BYE:")
    ]
    non_idle = invite_signal[
        invite_signal.index("if (state != CallState::IDLE)") :
        invite_signal.index("std::string cached_cid")
    ]
    assert "SipTransport owns transaction collisions" in non_idle
    assert "goto " not in non_idle
    assert "we_win" not in non_idle
    assert "set_call_state_(CallState::IDLE)" not in non_idle
    assert "send_sip_final_response_" not in non_idle


def test_in_progress_invite_retransmission_replays_without_refiring_fsm() -> None:
    sip_h = read("sip_transport.h")
    sip_cpp = read("sip_transport.cpp")
    assert "std::string last_invite_response_;" in sip_h
    response = sip_cpp[
        sip_cpp.index("bool SipTransport::send_response_") :
        sip_cpp.index("\nbool SipTransport::send_stateless_response_")
    ]
    assert "this->last_invite_response_ = msg;" in response
    inbound = sip_cpp[
        sip_cpp.index("bool SipTransport::handle_invite_") :
        sip_cpp.index("\nbool SipTransport::handle_response_")
    ]
    replay = inbound.index("if (!this->last_invite_response_.empty())")
    emit = inbound.index("this->emit_sip_signal_(signal)")
    assert replay < emit


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


def test_rtp_clock_advances_without_consuming_sequence_for_a_local_payload_drop() -> None:
    sip_cpp = read("sip_transport.cpp")
    audio = sip_cpp[
        sip_cpp.index("void SipTransport::send_audio_frame") :
        sip_cpp.index("\nbool SipTransport::send_ringing", sip_cpp.index("void SipTransport::send_audio_frame"))
    ]

    drop = audio.index("if (bytes == 0 || bytes > this->udp_max_payload_)")
    sequence = audio.index("this->rtp_sequence_.fetch_add")
    assert drop < sequence
    assert "this->rtp_timestamp_.fetch_add(samples" in audio[drop:sequence]


def test_jitter_prebuffer_handles_initial_reordering_and_preserves_metadata() -> None:
    jitter = read("rtp_jitter_buffer.h")

    assert "if (this->prebuffer_ == 0)" in jitter
    assert "this->prebuffer_ = 1;" in jitter
    assert "if (this->prebuffer_ > this->slot_count_)" in jitter
    assert "this->prebuffer_ = this->slot_count_;" in jitter
    assert "if (this->buffering_ && delta > -static_cast<int16_t>(this->slot_count_))" in jitter
    assert "this->next_sequence_ = sequence;" in jitter
    assert "slot.has_metadata = frame.has_metadata;" in jitter
    assert "*has_metadata = slot.has_metadata;" in jitter
    assert "if (slot.bytes != expected_bytes)" in jitter
    assert "return ReadResult::MISSING;" in jitter


def test_jitter_large_jump_anchors_at_a_real_frame() -> None:
    jitter = read("rtp_jitter_buffer.h")

    realign = jitter[
        jitter.index("if (realigned) {") :
        jitter.index("} else if (this->buffering_", jitter.index("if (realigned) {"))
    ]
    assert "candidate_slot.valid && candidate_slot.sequence == candidate" in realign
    assert "this->next_sequence_ = candidate;" in realign
    assert "this->buffering_ = this->valid_count_ < this->prebuffer_;" in realign


def test_rx_rebuffering_clocks_network_gap_silence_without_task_delays() -> None:
    audio = read("voip_audio.cpp")
    header = read("voip_stack.h")

    assert "std::atomic<uint32_t> rx_underrun_start_ms_{0};" in header
    assert "read_result == RtpJitterBuffer::ReadResult::BUFFERING &&" in audio
    assert "!this->first_audio_received_.load(std::memory_order_acquire)" in audio
    assert "SilenceReason::NETWORK_GAP" in audio
    assert "vTaskDelay(" not in audio


def test_call_identity_formats_survive_teardown_and_invalid_route_is_terminal() -> None:
    fsm = read("voip_fsm.cpp")

    clear_identity = fsm[
        fsm.index("void VoipStack::clear_call_identity_()") :
        fsm.index("\nVoipStack::CallSnapshot", fsm.index("void VoipStack::clear_call_identity_()"))
    ]
    assert "current_media_formats_" not in clear_identity

    start = fsm[fsm.index("void VoipStack::start()") : fsm.index("\nvoid VoipStack::stop()")]
    invalid_route = start[start.index("if (dial_ip.empty() || dial_port == 0)") : start.index("\n  this->clear_terminal_call_snapshot_", start.index("if (dial_ip.empty() || dial_port == 0)"))]
    assert "this->set_call_identity_(" in invalid_route
    assert "this->set_call_state_(CallState::CALLING);" in invalid_route
    assert "this->end_call_(CallEndReason::TRANSPORT_UNREACHABLE);" in invalid_route


def test_sip_response_validation_precedes_retarget_and_bad_sdp_closes_dialog() -> None:
    sip_h = read("sip_transport.h")
    sip_cpp = read("sip_transport.cpp")
    response = cpp_method(sip_cpp, r"SipTransport::handle_response_")

    call_id_check = response.index("response_call_id != this->call_id_")
    cseq_check = response.index("response_cseq_number != this->invite_cseq_")
    retarget = response.index("this->remote_ip_v4_.store(src_ip")
    assert call_id_check < cseq_check < retarget
    incompatible = response[
        response.index("if (!media_ok || !video_prepared)")
        : response.index("this->open_media_session_()")
    ]
    assert 'this->send_request_("ACK", "", options);' in response
    assert "this->send_bye_unlocked_(this->call_id_)" in incompatible
    assert "signal.type = SipSignalType::MEDIA_INCOMPATIBLE;" in incompatible
    assert "signal.terminal_transaction_pending = bye_pending;" in incompatible
    assert "this->reset_dialog_();" in incompatible
    assert "std::atomic<uint32_t> remote_rtp_ip_v4_{0};" in sip_h
    learn = sip_cpp[sip_cpp.index("bool SipTransport::learn_remote_rtp_from_sdp_") : sip_cpp.index("\nbool SipTransport::send_request_")]
    assert "remote_rtp_ip_v4_.store(media_ip" in learn
    assert "remote_ip_v4_.store(media_ip" not in learn
    request = sip_cpp[sip_cpp.index("bool SipTransport::send_request_(const std::string &method, const std::string &body,") : sip_cpp.index("\nbool SipTransport::send_invite_error_ack_")]
    assert "remote_ip_v4_.load" in request
    assert "remote_rtp_ip_v4_.load" not in request


def test_udp_listener_cannot_flip_an_active_tcp_dialog() -> None:
    sip_cpp = read("sip_transport.cpp")
    udp_receive = sip_cpp[
        sip_cpp.index("if (this->sip_socket_ >= 0 && FD_ISSET") :
        sip_cpp.index("\n    const int active_tcp_client", sip_cpp.index("if (this->sip_socket_ >= 0 && FD_ISSET"))
    ]

    active_guard = udp_receive.index("if (tcp_session_active)")
    udp_mode_store = udp_receive.index("this->remote_sip_tcp_.store(false")
    assert active_guard < udp_mode_store
    assert "sip_tcp_client_socket_" in udp_receive
    assert "connecting_fd >= 0" in udp_receive
    assert "tcp_connect_requested_" in udp_receive
    assert "bool tcp_call_active" in udp_receive
    assert "!this->call_id_.empty()" in udp_receive
    assert "terminal_transaction_pending_locked_()" in udp_receive
    assert "close_tcp_client_from_sip_task_" in udp_receive


def test_tcp_invite_can_be_cancelled_atomically_before_connect_flush() -> None:
    sip_cpp = read("sip_transport.cpp")
    cancel = cpp_method(sip_cpp, r"SipTransport::send_cancel_unlocked_")
    promote = sip_cpp[
        sip_cpp.index("auto promote_tcp_connect") :
        sip_cpp.index("while (this->running_")
    ]
    reset = cpp_method(sip_cpp, r"SipTransport::reset_dialog_media_locked_")

    assert "LockGuard send_lock(this->tcp_send_mutex_);" in cancel
    assert "LockGuard pending_lock(this->tcp_tx_pending_mutex_);" in cancel
    assert 'this->tcp_tx_pending_.rfind("INVITE ", 0) == 0' in cancel
    assert 'header_value(this->tcp_tx_pending_, "Call-ID") == this->call_id_' in cancel
    assert "this->tcp_tx_pending_.clear();" in cancel
    assert "this->tcp_connect_requested_.store(false" in cancel
    assert "this->sip_tcp_client_close_requested_.store(" in cancel
    assert "if (cancelled_before_flush)" in cancel
    assert "this->reset_dialog_();" in cancel
    assert "return true;" in cancel

    # Promotion and cancellation share the same outer lock: either the queued
    # INVITE is retracted, or promotion owns and sends it before CANCEL.
    assert "LockGuard send_lock(this->tcp_send_mutex_);" in promote
    assert "pending.swap(this->tcp_tx_pending_);" in promote
    assert "LockGuard send_lock(this->tcp_send_mutex_);" in reset
    assert "abort_queued_tcp_record" in reset


def test_schema_matches_rtp_implementation_limits() -> None:
    init_py = read("__init__.py")
    sip_types = read("sip_types.h")

    assert "UDP_IMPLEMENTATION_MAX_PAYLOAD_BYTES = 1488" in init_py
    assert "max=UDP_IMPLEMENTATION_MAX_PAYLOAD_BYTES" in init_py
    assert 'if fmt[CONF_PCM_FORMAT] == "s32le":' in init_py
    assert "fmt.channels != 1" not in sip_types
    assert "cv.Optional(CONF_IP): cv.ipv4address" in init_py
    assert "voip_stack task stacks in PSRAM require the psram component" in init_py
    assert "esp32.get_esp32_variant() == esp32.VARIANT_ESP32" in init_py


def test_l24_wire_negotiation_accepts_packed_and_s32_container_variants() -> None:
    sip_types = read("sip_types.h")
    match = sip_types[
        sip_types.index("inline bool audio_format_list_match_udp_safe") :
        sip_types.index("\n}\n\n}  // namespace voip_stack", sip_types.index("inline bool audio_format_list_match_udp_safe"))
    ]

    assert "same_wire_format" in match
    assert "audio_format_rtp_encoding(candidate" in match
    assert "audio_format_rtp_encoding(remote" in match
    assert "candidate == remote" not in match


def test_sdp_only_negotiates_payloads_from_the_selected_audio_media() -> None:
    sip_cpp = read("sip_transport.cpp")
    parser = sip_cpp[
        sip_cpp.index("bool parse_audio_media_line") :
        sip_cpp.index("\nsize_t pcm_to_rtp_payload")
    ]
    learn = sip_cpp[
        sip_cpp.index("bool SipTransport::learn_remote_rtp_from_sdp_") :
        sip_cpp.index("\nbool SipTransport::send_request_")
    ]

    assert 'media.substr(protocol_start, protocol_end - protocol_start) != "RTP/AVP"' in parser
    assert "payload_types[payload_type] = true;" in parser
    assert "selected_audio_line" in learn
    assert "offered_payload[pt]" in learn
    assert "pos == selected_audio_line" in learn
    assert 'line == "a=sendonly"' in learn
    assert 'line == "a=recvonly"' in learn
    assert 'line == "a=inactive"' in learn
    assert "uint8_t session_flow = 0x03;" in learn
    assert "media_flow = session_flow;" in learn
    assert "(!seen_any_media || in_audio)" in learn


def test_sip_compact_headers_and_tcp_close_are_centralized() -> None:
    sip_cpp = read("sip_transport.cpp")

    header_parser = sip_cpp[sip_cpp.index("std::string header_value") : sip_cpp.index("\nstd::string message_body")]
    assert 'canonical == "via"' in header_parser
    assert 'canonical == "call-id"' in header_parser
    assert 'canonical == "content-length"' in header_parser

    close = sip_cpp[
        sip_cpp.index("void SipTransport::close_tcp_client_from_sip_task_") :
        sip_cpp.index("\nvoid SipTransport::wake_sip_task_")
    ]
    assert "LockGuard send_lock(this->tcp_send_mutex_);" in close
    peer_loss = sip_cpp[
        sip_cpp.index("void SipTransport::handle_tcp_peer_loss_") :
        sip_cpp.index("\nvoid SipTransport::wake_sip_task_")
    ]
    assert "LockGuard lock(this->dialog_mutex_);" in peer_loss
    assert "this->reset_dialog_();" in peer_loss
    assert "this->emit_connection_change_(false);" in peer_loss


def test_sip_worker_uses_a_dedicated_event_driven_wake_socket() -> None:
    init_py = read("__init__.py")
    sip_h = read("sip_transport.h")
    sip_cpp = read("sip_transport.cpp")

    assert 'socket.consume_sockets(3, "voip_stack_sip", socket.SocketType.UDP)' in init_py
    assert "int sip_wake_socket_{-1};" in sip_h
    assert "uint16_t sip_wake_port_{0};" in sip_h

    start = sip_cpp[
        sip_cpp.index("bool SipTransport::start()") :
        sip_cpp.index("\nvoid SipTransport::request_tcp_client_close_")
    ]
    assert 'this->bind_udp_(&this->sip_wake_socket_, 0, "SIP wake")' in start
    assert "getsockname(this->sip_wake_socket_" in start

    wake = sip_cpp[
        sip_cpp.index("void SipTransport::wake_sip_task_()") :
        sip_cpp.index("\nvoid SipTransport::wake_rtp_task_()")
    ]
    assert "this->sip_wake_socket_" in wake
    assert "this->sip_wake_port_" in wake
    assert "this->sip_socket_" not in wake

    sip_task = sip_cpp[
        sip_cpp.index("void SipTransport::sip_task_()") :
        sip_cpp.index("\nvoid SipTransport::rtp_task_()")
    ]
    assert "FD_SET(this->sip_wake_socket_, &readfds)" in sip_task
    assert "recv(this->sip_wake_socket_" in sip_task
    assert "select(max_fd + 1, &readfds, &writefds, nullptr, timeout_ptr)" in sip_task


def test_cancel_transactions_are_serialized_and_handle_crossed_200() -> None:
    sip_h = read("sip_transport.h")
    sip_cpp = read("sip_transport.cpp")
    fsm = read("voip_fsm.cpp")
    component = read("voip_stack.cpp")

    assert "UdpTransaction pending_cancel_;" in sip_h
    assert "std::atomic<bool> cancel_requested_{false};" in sip_h
    retransmit = sip_cpp[sip_cpp.index("void SipTransport::pump_udp_retransmits_()") : sip_cpp.index("\nbool SipTransport::local_ip_for_peer_")]
    assert "LockGuard lock(this->dialog_mutex_);" in retransmit
    sip_task = sip_cpp[sip_cpp.index("void SipTransport::sip_task_()") : sip_cpp.index("\nvoid SipTransport::rtp_task_()")]
    assert "include_txn(this->pending_cancel_);" in sip_task
    cancel = sip_cpp[sip_cpp.index("bool SipTransport::send_cancel(") : sip_cpp.index("\nbool SipTransport::send_bye(")]
    early_cancel = cancel[
        cancel.index("if (cancelled_before_flush)") :
        cancel.index("\n  SipRequestOptions options;")
    ]
    wire_cancel = cancel[cancel.index("\n  SipRequestOptions options;") :]
    assert "this->reset_dialog_();" in early_cancel
    assert "this->reset_dialog_();" not in wire_cancel
    assert "this->clear_invite_transaction_();" in cancel
    response = sip_cpp[sip_cpp.index("bool SipTransport::handle_response_(") : sip_cpp.index("\nvoid SipTransport::handle_sip_datagram_")]
    assert "CANCEL crossed the final 2xx" in response
    assert "this->send_bye_unlocked_(this->call_id_)" in response
    datagram = sip_cpp[sip_cpp.index("void SipTransport::handle_sip_datagram_") : sip_cpp.index("\nbool SipTransport::reject_if_stale_dialog_")]
    assert "incoming_cseq_number != this->last_invite_cseq_number_" in datagram
    assert "incoming_branch == invite_branch" in datagram
    assert "!same_transaction_via" in datagram
    assert "this->media_active_.load(std::memory_order_acquire)" in datagram
    assert "waiting_for_terminal_response = this->send_sip_cancel_(call_id);" in fsm
    timeout = component[
        component.index("void VoipStack::fire_timeout_decline_()") :
        component.index("\nvoid VoipStack::dump_config()")
    ]
    assert "waiting_for_terminal_response = this->send_sip_cancel_(call_id);" in timeout
    assert "!waiting_for_terminal_response" in timeout


def test_unanswered_invite_timeout_does_not_send_cancel() -> None:
    component = read("voip_stack.cpp")

    timeout_dispatch = component[
        component.index("void VoipStack::handle_call_timeouts_") :
        component.index("\nvoid VoipStack::loop()")
    ]
    no_response = timeout_dispatch[
        timeout_dispatch.index("if (!saw_sip_response)") :
        timeout_dispatch.index(
            "\n  if (calling_timeout_ms > 0",
            timeout_dispatch.index("if (!saw_sip_response)"),
        )
    ]
    assert "fire_unanswered_invite_timeout_();" in no_response
    assert "fire_timeout_decline_();" not in no_response

    teardown = component[
        component.index("void VoipStack::fire_unanswered_invite_timeout_()") :
        component.index("\nvoid VoipStack::fire_timeout_decline_()")
    ]
    assert "send_sip_cancel_" not in teardown
    assert "CallEndReason::TIMEOUT" in teardown
    assert "this->transport_->disconnect();" in teardown


def test_udp_invite_server_final_retransmits_until_matching_ack() -> None:
    sip_h = read("sip_transport.h")
    sip_cpp = read("sip_transport.cpp")

    assert "struct CompletedServerTransaction" in sip_h
    for field in (
        "next_retransmit_ms",
        "deadline_ms",
        "retransmit_interval_ms",
        "awaiting_ack",
        "peer_port",
        "from_tag",
        "to_tag",
    ):
        assert field in sip_h

    remember = sip_cpp[
        sip_cpp.index("void SipTransport::remember_completed_response_(") :
        sip_cpp.index("\nuint16_t SipTransport::acknowledge_completed_invite_")
    ]
    assert "now + SIP_T1_MS" in remember
    assert "now + SIP_TRANSACTION_TIMEOUT_MS" in remember
    assert 'completed->awaiting_ack = method == "INVITE";' in remember
    assert "completed->call_id == this->call_id_" in remember

    retransmit = sip_cpp[
        sip_cpp.index("void SipTransport::pump_udp_retransmits_()") :
        sip_cpp.index("\nbool SipTransport::local_ip_for_peer_")
    ]
    assert "completed_invite_.response" in retransmit
    assert "completed_invite_.peer_port" in retransmit
    assert "completed_invite_.retransmit_interval_ms * 2" in retransmit
    assert "SIP_T2_MS" in retransmit
    assert 'signal.reason = "ack_timeout";' in retransmit
    assert "active_2xx_dialog" in retransmit
    assert "this->send_bye_unlocked_(timed_out_call_id)" in retransmit

    acknowledge = sip_cpp[
        sip_cpp.index("uint16_t SipTransport::acknowledge_completed_invite_(") :
        sip_cpp.index("\nbool SipTransport::replay_completed_invite_ack_")
    ]
    assert 'cseq_method(cseq) == "ACK"' in acknowledge
    assert "peer_ip == this->completed_invite_.peer_ip_v4" in acknowledge
    assert 'header_value(request, "From")' in acknowledge
    assert 'header_value(request, "To")' in acknowledge
    assert "this->completed_invite_.status < 300" in acknowledge
    assert "branch == this->completed_invite_.branch" in acknowledge
    assert "this->completed_invite_.awaiting_ack = false;" in acknowledge

    datagram = sip_cpp[
        sip_cpp.index("void SipTransport::handle_sip_datagram_") :
        sip_cpp.index("\nbool SipTransport::reject_if_stale_dialog_")
    ]
    assert "acknowledge_completed_invite_(msg, src," in datagram
    assert "if (terminate_after_ack)" in datagram
    assert "this->send_bye_unlocked_(call_id)" in datagram
    assert "if (completed_status >= 300) return;" in datagram
    assert "completed_status >= 200 && completed_status < 300" in datagram

    sip_task = sip_cpp[
        sip_cpp.index("void SipTransport::sip_task_()") :
        sip_cpp.index("\nvoid SipTransport::rtp_task_()")
    ]
    assert "LockGuard lock(this->dialog_mutex_);" in sip_task
    assert "this->completed_invite_.next_retransmit_ms" in sip_task
    assert "this->completed_invite_.deadline_ms" in sip_task


def test_sip_transactions_keep_deadlines_on_tcp_and_ack_replay_is_transport_safe() -> None:
    sip_h = read("sip_transport.h")
    sip_cpp = read("sip_transport.cpp")

    assert "bool udp{true};" in sip_h
    remember = cpp_method(sip_cpp, r"SipTransport::remember_udp_transaction_")
    assert "txn->udp = !this->remote_sip_tcp_" in remember
    assert "remote_sip_tcp_" not in remember[: remember.index("UdpTransaction *txn")]

    pump = cpp_method(sip_cpp, r"SipTransport::pump_udp_retransmits_")
    assert "if (!txn.udp)" in pump
    assert "txn.deadline_ms" in pump
    assert "this->completed_invite_.status < 300" in pump
    assert "this->send_bye_unlocked_(timed_out_call_id)" in pump

    sip_task = cpp_method(sip_cpp, r"SipTransport::sip_task_")
    assert "txn.udp && !txn.completed ? txn.next_ms" in sip_task
    assert ": txn.deadline_ms" in sip_task

    replay = cpp_method(
        sip_cpp, r"SipTransport::replay_completed_invite_ack_"
    )
    assert "completed_invite_client_.udp" in replay
    assert "transport_matches" in replay
    assert "if (this->remote_sip_tcp_" not in replay

    video_replay = cpp_method(
        sip_cpp, r"SipTransport::replay_completed_video_direction_ack_"
    )
    assert "completed.udp" in video_replay
    assert "transport_matches" in video_replay
    assert "if (completed.empty()) return false;" in video_replay


def test_reinvite_proposal_cannot_leak_temporary_audio_format_to_rtp() -> None:
    sip_h = read("sip_transport.h")
    sip_cpp = read("sip_transport.cpp")

    assert "std::atomic<uint32_t> media_proposal_epoch_{0};" in sip_h
    assert "class ScopedMediaProposal" in sip_cpp
    send = cpp_method(sip_cpp, r"SipTransport::send_audio_frame")
    receive = cpp_method(sip_cpp, r"SipTransport::rtp_task_")
    assert send.count("media_proposal_epoch_.load") >= 3
    assert "proposal_epoch & 1U" in send
    assert receive.count("media_proposal_epoch_.load") >= 2
    assert "proposal_epoch & 1U" in receive

    apply_answer = cpp_method(
        sip_cpp, r"SipTransport::apply_video_direction_answer_"
    )
    reinvite = cpp_method(sip_cpp, r"SipTransport::handle_reinvite_")
    assert "ScopedMediaProposal proposal_scope" in apply_answer
    assert "ScopedMediaProposal proposal_scope" in reinvite
    assert '"audio_renegotiation_unsupported"' in reinvite


def test_video_direction_changes_are_worker_owned_and_stop_mid_au_promptly() -> None:
    video = read("video_rtp.cpp")
    direction = cpp_method(
        video, r"VideoRtpSession::request_media_direction"
    )
    worker = cpp_method(video, r"VideoRtpSession::task_")
    sender = cpp_method(video, r"VideoRtpSession::sender_task_")
    payload = cpp_method(video, r"VideoRtpSession::send_rtp_payload_")

    assert "rx_reset_requested_.store(true" in direction
    assert "reset_reassembly_()" not in direction
    assert "rx_reset_requested_.exchange(false" in worker
    assert "reset_reassembly_()" in worker
    assert "slot.state.store(0" not in direction
    assert "slot.state.store(0" in sender
    assert payload.count("send_enabled_.load") >= 3
    assert "xSemaphoreGive(this->audio_pacing_)" in direction
    assert "LockGuard source_lock(this->source_control_mutex_)" in direction
    assert "LockGuard direction_lock(this->direction_mutex_)" in direction
    assert "LockGuard direction_lock(this->direction_mutex_)" in worker
    assert "this->terminate_.load(std::memory_order_acquire)" in direction


def test_reinvite_recovery_does_not_reenter_the_media_mutex() -> None:
    sip_cpp = read("sip_transport.cpp")
    reinvite = cpp_method(sip_cpp, r"SipTransport::handle_reinvite_")
    failure = reinvite[
        reinvite.index('"Prepared video direction failed after 200')
        :
    ]

    assert "this->request_audio_path_stop_locked_();" in failure
    assert "this->terminate_after_invite_ack_ = true;" in failure
    assert "signal.terminal_transaction_pending = true;" in failure
    assert "this->send_bye_unlocked_" not in failure
    assert 'this->send_request_("BYE")' not in failure
    assert "this->reset_dialog_();" not in failure


def test_reinvite_retry_after_content_type_and_dialog_loss_are_standardized() -> None:
    sip_cpp = read("sip_transport.cpp")
    response = sip_cpp[
        sip_cpp.index("std::string SipTransport::format_response_")
        : sip_cpp.index("\nbool SipTransport::send_response_")
    ]
    reinvite = cpp_method(sip_cpp, r"SipTransport::handle_reinvite_")
    video_response = cpp_method(
        sip_cpp, r"SipTransport::handle_video_direction_response_"
    )

    assert '"Retry-After: "' in response
    assert "std::tolower" in reinvite
    assert 'media_type != "application/sdp"' in reinvite
    assert 'message, src, 500, "Server Internal Error", "request_pending"' in reinvite
    assert 'message, src, 491, "Request Pending"' in reinvite
    assert reinvite.count("esp_random() % 11U") >= 2
    terminal = video_response[video_response.index("if (status == 408 || status == 481)") :]
    assert "this->send_bye_unlocked_(call_id)" in terminal
    assert "if (!bye_pending) this->reset_dialog_();" in terminal


def test_terminal_sip_transaction_remains_owned_until_peer_completion() -> None:
    sip_types = read("sip_types.h")
    transport_h = read("transport.h")
    sip_cpp = read("sip_transport.cpp")
    fsm = read("voip_fsm.cpp")

    assert "bool terminal_transaction_pending{false};" in sip_types
    assert "bool terminal_transaction_pending{false};" in transport_h

    pending = cpp_method(
        sip_cpp, r"SipTransport::terminal_transaction_pending_locked_"
    )
    for token in (
        "pending_cancel_",
        "pending_bye_",
        "completed_invite_.awaiting_ack",
        "terminate_after_invite_ack_",
    ):
        assert token in pending

    invite = cpp_method(sip_cpp, r"SipTransport::send_invite")
    assert (
        invite.index("terminal_transaction_pending_locked_()")
        < invite.index("this->reset_dialog_();")
    )
    disconnect = cpp_method(sip_cpp, r"SipTransport::disconnect")
    assert "terminal_transaction_pending_locked_()" in disconnect
    assert disconnect.index("return;") < disconnect.index("this->reset_dialog_();")

    start = cpp_method(fsm, r"VoipStack::start")
    assert "snapshot().terminal_transaction_pending" in start
    terminal_signal = cpp_method(fsm, r"VoipStack::on_sip_signal_received_")
    assert "!msg.terminal_transaction_pending" in terminal_signal

    inbound = cpp_method(sip_cpp, r"SipTransport::handle_invite_")
    gate = inbound[
        inbound.index("terminal_transaction_pending_locked_()")
        : inbound.index("std::string incoming_caller_name")
    ]
    assert 'same_dialog ? 500 : 503' in gate
    assert '"transaction_pending", false, 1' in gate

    datagram = cpp_method(sip_cpp, r"SipTransport::handle_sip_datagram_")
    bye = datagram[datagram.index('} else if (method == "BYE")') :]
    assert "const bool local_bye_pending = !this->pending_bye_.empty();" in bye
    assert "!local_bye_pending" in bye
    assert "signal.terminal_transaction_pending = local_bye_pending;" in bye
    assert "if (!local_bye_pending) this->reset_dialog_();" in bye

    worker = cpp_method(sip_cpp, r"SipTransport::rtp_task_")
    worker_failure = worker[worker.index('signal.reason = "rtp_worker_failed"') :]
    assert "this->send_bye_unlocked_(signal.call_id)" in worker_failure
    assert "signal.terminal_transaction_pending" in worker_failure


def test_dialog_strings_are_serialized_off_the_media_hot_path() -> None:
    sip_h = read("sip_transport.h")
    sip_cpp = read("sip_transport.cpp")

    assert "Mutex dialog_mutex_;" in sip_h
    invite = sip_cpp[sip_cpp.index("bool SipTransport::send_invite(") : sip_cpp.index("\nvoid SipTransport::send_audio_frame")]
    datagram = sip_cpp[sip_cpp.index("void SipTransport::handle_sip_datagram_") : sip_cpp.index("\nbool SipTransport::reject_if_stale_dialog_")]
    audio = sip_cpp[sip_cpp.index("void SipTransport::send_audio_frame") : sip_cpp.index("\nbool SipTransport::send_ringing")]
    assert "LockGuard lock(this->dialog_mutex_);" in invite
    assert "LockGuard lock(this->dialog_mutex_);" in datagram
    assert "dialog_mutex_" not in audio


def test_dialog_remote_target_and_in_dialog_requests_are_kept_separate() -> None:
    sip_h = read("sip_transport.h")
    sip_cpp = read("sip_transport.cpp")

    assert "std::string remote_uri_;" in sip_h
    assert "std::string remote_target_uri_;" in sip_h
    request = sip_cpp[sip_cpp.index("bool SipTransport::send_request_(") : sip_cpp.index("\nbool SipTransport::send_invite_error_ack_")]
    assert "this->remote_target_uri_" in request
    assert 'msg += "To: " + this->remote_uri_;' in request
    inbound = sip_cpp[sip_cpp.index("bool SipTransport::handle_invite_(") : sip_cpp.index("\nbool SipTransport::handle_response_")]
    assert 'strip_angle_uri(header_value(message, "Contact"))' in inbound
    assert 'this->remote_uri_ = remote_identity_uri.empty() ? "" : "<" + remote_identity_uri + ">";' in inbound
    response = sip_cpp[sip_cpp.index("bool SipTransport::handle_response_(") : sip_cpp.index("\nvoid SipTransport::handle_sip_datagram_")]
    assert 'const std::string contact_target = strip_angle_uri(header_value(message, "Contact"));' in response
    assert "this->remote_target_uri_ = contact_target;" in response


def test_ack_bye_and_cancel_validate_their_dialog_or_transaction() -> None:
    sip_cpp = read("sip_transport.cpp")

    response = sip_cpp[sip_cpp.index("bool SipTransport::handle_response_(") : sip_cpp.index("\nvoid SipTransport::handle_sip_datagram_")]
    assert 'method != "INVITE" && method != "CANCEL" && method != "BYE"' in response
    assert "this->pending_bye_.empty()" in response
    assert "mismatched BYE transaction" in response
    datagram = sip_cpp[sip_cpp.index("void SipTransport::handle_sip_datagram_") : sip_cpp.index("\nbool SipTransport::reject_if_stale_dialog_")]
    assert "request_cseq == this->last_invite_cseq_number_" in datagram
    assert 'cseq_method(header_value(msg, "CSeq")) == "ACK"' in datagram
    assert 'tag_from_header(header_value(msg, "From")) == this->remote_tag_' in datagram
    assert 'tag_from_header(header_value(msg, "To")) == this->local_tag_' in datagram
    assert "incoming_from_tag != this->remote_tag_" in datagram
    assert "completed_status >= 200 && completed_status < 300" in datagram

    response = sip_cpp[
        sip_cpp.index("bool SipTransport::handle_response_(") :
        sip_cpp.index("\nvoid SipTransport::handle_sip_datagram_")
    ]
    assert "response_from_tag != this->local_tag_" in response
    assert "response_to_tag != this->remote_tag_" in response
    cancel_failure = response[
        response.index('} else if (method == "CANCEL")', response.index("if (status >= 300)")) :
        response.index("\n      }", response.index('} else if (method == "CANCEL")', response.index("if (status >= 300)")))
    ]
    assert "this->reset_dialog_();" not in cancel_failure


def test_trying_response_does_not_create_an_early_dialog_tag() -> None:
    sip_cpp = read("sip_transport.cpp")
    response = sip_cpp[
        sip_cpp.index("std::string SipTransport::format_response_(") :
        sip_cpp.index("\nbool SipTransport::send_response_")
    ]

    assert "add_to_tag && status != 100" in response


def test_current_media_formats_are_atomic_on_realtime_readers() -> None:
    header = read("voip_stack.h")
    audio = read("voip_audio.cpp")
    fsm = read("voip_fsm.cpp")
    sip_types = read("sip_types.h")

    assert "pack_audio_format" in sip_types
    assert "unpack_audio_format" in sip_types
    assert "std::atomic<uint32_t> current_tx_audio_format_packed_" in header
    assert "std::atomic<uint32_t> current_rx_audio_format_packed_" in header
    assert "CurrentMediaFormats snapshot_current_media_formats_() const" in header
    assert "const AudioFormat rx_format = this->get_current_rx_audio_format_();" in audio
    assert "this->set_current_media_formats_(" in fsm
    assert "current_rx_audio_format_." not in audio


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
    assert '"%s | %s | %u | %u | %s | %s | %s | %s | %s%s"' in stack
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
    assert "contact_transport_tcp == local_sip_transport_tcp" not in settings
    assert "entry.sip_transport_tcp = contact_transport_tcp;" in settings
    assert '} else if (has_ha) {' in settings


def test_yaml_calling_conditions_cover_remote_ringing() -> None:
    actions = read("actions.h")
    init = read("__init__.py")

    assert "VoipIsCallingCondition" in actions
    assert "return this->parent_->is_calling();" in actions
    assert "VoipIsRemoteRingingCondition" in actions
    assert "CallState::REMOTE_RINGING" in actions
    assert '"voip_stack.is_remote_ringing"' in init
