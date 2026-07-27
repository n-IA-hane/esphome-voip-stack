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
        'esp32.add_idf_sdkconfig_option("CONFIG_LWIP_UDP_RECVMBOX_SIZE", 32)'
        in video_codegen
    )
    assert "this->sequence_valid_ = false;" in video_rtp
    assert "batch++ < kMaxReceiveBatchPackets" in video_rtp
    assert re.search(r"recvfrom\(\s*this->rtp_socket_", video_rtp)


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


def test_rtp_jpeg_dimension_limit_matches_rfc2435_encoding() -> None:
    init_path = VOIP / "__init__.py"
    spec = importlib.util.spec_from_file_location("voip_stack_local_init", init_path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    jpeg = {
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
        sip_cpp.index("\n#endif", sip_cpp.index("bool SipTransport::learn_remote_video_from_sdp_"))
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
    reinvite = sip_cpp[
        sip_cpp.index("bool SipTransport::handle_reinvite_") :
        sip_cpp.index("\nbool SipTransport::handle_response_")
    ]

    prestart = reinvite.index("this->video_session_->start()")
    success = reinvite.index('this->send_response_(200, "OK", answer)')
    assert prestart < success
    assert '"video_resources_unavailable"' in reinvite
    assert '"video_renegotiation_unsupported"' in reinvite
    assert "Video direction re-INVITE rollback failed" in reinvite


def test_reinvite_waits_for_prior_invite_ack_without_mutating_dialog() -> None:
    sip_cpp = read("sip_transport.cpp")
    reinvite = sip_cpp[
        sip_cpp.index("bool SipTransport::handle_reinvite_") :
        sip_cpp.index("\nbool SipTransport::handle_response_")
    ]

    pending = reinvite.index("this->completed_invite_.awaiting_ack")
    parse_media = reinvite.index("this->learn_remote_rtp_from_sdp_")
    mutate_dialog = reinvite.index(
        'this->last_invite_via_ = header_values(message, "Via");'
    )
    assert pending < parse_media < mutate_dialog
    pending_response = reinvite[pending:parse_media]
    assert 'message, src, 491, "Request Pending"' in pending_response
    assert "cache_transaction" not in pending_response


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

    reinvite = sip_cpp[
        sip_cpp.index("bool SipTransport::handle_reinvite_") :
        sip_cpp.index("\nbool SipTransport::handle_response_")
    ]
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
    sender = video[
        video.index("void VideoRtpSession::send_access_unit_") :
        video.index("\nvoid VideoRtpSession::task_trampoline_")
    ]
    payload = sender[
        sender.index("bool VideoRtpSession::send_rtp_payload_") :
    ]

    # Once transmission starts, finish the AU: deliberately aborting it after
    # spending bandwidth on most fragments guarantees an undecodable frame.
    # The one-slot latest-wins queue drops newer source frames while the owned
    # AU is paced, and each individual UDP retry remains bounded.
    assert "tx_access_unit_deadline_ms_" not in header
    assert "tx_access_unit_deadline_expired_" not in payload
    assert "kTxPacketPacingMs" in header
    assert "pdMS_TO_TICKS(kTxPacketPacingMs)" in payload
    for transient_error in ("EAGAIN", "EWOULDBLOCK", "ENOBUFS", "ENOMEM"):
        assert transient_error in payload
    assert "attempt < 2" in payload
    assert "FD_SET(this->rtp_socket_, &writefds)" in payload
    assert "select(this->rtp_socket_ + 1" in payload
    assert "pdMS_TO_TICKS(20)" not in payload
    assert "DSCP AF41" in video


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
    assert "capability = this->local_video_receive_capability_();" in append
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
    assert reinvite.index(lock) < reinvite.index("this->video_session_->start()")
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
    assert "source_->stop_video()" in video_stop
    assert "send_rtcp_bye_()" in video_stop

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
    assert "SIP TCP accept rejected: dialog active with different peer" in accept


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
    reinvite = sip_cpp[
        sip_cpp.index("bool SipTransport::handle_reinvite_") :
        sip_cpp.index("\nbool SipTransport::handle_response_")
    ]
    assert "RFC 3261 section 14" in reinvite
    assert "this->emit_sip_signal_" not in reinvite
    assert "this->video_session_->stop();" in reinvite
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


def test_simultaneous_invites_use_one_deterministic_glare_winner() -> None:
    sip_cpp = read("sip_transport.cpp")
    inbound = sip_cpp[
        sip_cpp.index("bool SipTransport::handle_invite_") :
        sip_cpp.index("\nbool SipTransport::handle_response_")
    ]

    assert "const bool glare = this->outgoing_invite_pending_" in inbound
    assert "active_peer_ip == src_ip" in inbound
    assert "incoming_caller_name == this->dest_name_" in inbound
    assert "const bool local_invite_wins" in inbound
    assert "this->call_id_ < incoming_call_id" in inbound
    assert "this->send_cancel_unlocked_(this->call_id_);" in inbound


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
    response = sip_cpp[
        sip_cpp.index("bool SipTransport::handle_response_(") :
        sip_cpp.index("\nvoid SipTransport::", sip_cpp.index("bool SipTransport::handle_response_("))
    ]

    call_id_check = response.index("response_call_id != this->call_id_")
    cseq_check = response.index("response_cseq_number != this->invite_cseq_")
    retarget = response.index("this->remote_ip_v4_.store(src_ip")
    assert call_id_check < cseq_check < retarget
    incompatible = response[response.index("if (!media_ok)") : response.index("this->open_media_session_()")]
    assert 'this->send_request_("ACK", "", options);' in response
    assert 'this->send_request_("BYE");' in incompatible
    assert "signal.type = SipSignalType::MEDIA_INCOMPATIBLE;" in incompatible
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
    assert "const bool tcp_call_active" in udp_receive
    assert "close_tcp_client_from_sip_task_" in udp_receive


def test_schema_matches_rtp_implementation_limits() -> None:
    init_py = read("__init__.py")
    sip_types = read("sip_types.h")

    assert "UDP_IMPLEMENTATION_MAX_PAYLOAD_BYTES = 1488" in init_py
    assert "max=UDP_IMPLEMENTATION_MAX_PAYLOAD_BYTES" in init_py
    assert 'if fmt[CONF_PCM_FORMAT] == "s32le":' in init_py
    assert "fmt.channels != 1" not in sip_types
    assert "cv.Optional(CONF_IP): cv.ipv4address" in init_py
    assert "voip_stack.task_stacks_in_psram requires the psram component" in init_py
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
    assert "this->reset_dialog_();" not in cancel
    assert "this->clear_invite_transaction_();" in cancel
    response = sip_cpp[sip_cpp.index("bool SipTransport::handle_response_(") : sip_cpp.index("\nvoid SipTransport::handle_sip_datagram_")]
    assert "CANCEL crossed the final 2xx" in response
    assert 'this->send_request_("BYE")' in response
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
    assert 'method == "INVITE" && completed->udp' in remember
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
    assert "acknowledge_completed_invite_(msg, src)" in datagram
    assert "if (completed_status >= 300) return;" in datagram
    assert "completed_status >= 200 && completed_status < 300" in datagram

    sip_task = sip_cpp[
        sip_cpp.index("void SipTransport::sip_task_()") :
        sip_cpp.index("\nvoid SipTransport::rtp_task_()")
    ]
    assert "LockGuard lock(this->dialog_mutex_);" in sip_task
    assert "include_at(this->completed_invite_.next_retransmit_ms);" in sip_task


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
