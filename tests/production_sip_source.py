"""Read the existing SipTransport owner across its production translation units.

Legacy narrow fixtures still select member definitions; their platform/class
adapters are unchanged. Native fixtures compile every actual unit separately.
"""
from pathlib import Path

TRANSPORT_PARTS = (
    "sip_transport_media.cpp", "sip_flow_io.cpp", "sip_dialog_transactions.cpp",
    "sip_dialog_requests.cpp", "sip_dialog_dispatch.cpp", "sip_dialog_video.cpp",
    "sip_transport_tasks.cpp",
)


# Keep legacy range-based fixtures in the former source order. Each fragment
# is read from its current production unit, without copying method bodies.
SOURCE_RANGES = (
    ("sip_transport.cpp", "void *SipTransport::operator new("),
    ("sip_transport_media.cpp", "void SipTransport::set_media_config_("),
    ("sip_transport.cpp", "bool SipTransport::parse_remote_("),
    ("sip_flow_io.cpp", "bool SipTransport::bind_udp_("),
    ("sip_transport.cpp", "bool SipTransport::start()"),
    ("sip_flow_io.cpp", "void SipTransport::request_tcp_client_close_("),
    ("sip_transport.cpp", "void SipTransport::stop()"),
    ("sip_transport_media.cpp", "bool SipTransport::start_audio_path()"),
    ("sip_transport.cpp", "bool SipTransport::originate("),
    ("sip_dialog_transactions.cpp", "void SipTransport::clear_invite_transaction_("),
    ("sip_flow_io.cpp", "bool SipTransport::local_ip_for_peer_("),
    ("sip_dialog_video.cpp", "#ifdef USE_ESPHOME_VOIP_STACK_VIDEO\nstd::string SipTransport::build_video_direction_offer_("),
    ("sip_dialog_requests.cpp", "bool SipTransport::send_invite("),
    ("sip_transport_media.cpp", "TransportAudioTxContext SipTransport::audio_tx_frame_context("),
    ("sip_dialog_requests.cpp", "bool SipTransport::send_ringing("),
    ("sip_dialog_dispatch.cpp", "#ifdef USE_ESPHOME_VOIP_PAGING\nvoid SipTransport::set_paging_auto_answer_peers("),
    ("sip_dialog_video.cpp", "#ifdef USE_ESPHOME_VOIP_STACK_VIDEO\nbool SipTransport::handle_video_direction_response_("),
    ("sip_dialog_dispatch.cpp", "void SipTransport::handle_sip_datagram_("),
    ("sip_flow_io.cpp", "void SipTransport::handle_sip_stream_("),
    ("sip_transport_tasks.cpp", "void SipTransport::sip_task_trampoline_("),
)


def read_production_source(path, **kwargs):
    path = Path(path)
    source = path.read_text(**kwargs)
    if path.name != "sip_transport.cpp" or not path.with_name("sip_transport_internal.h").exists():
        return source
    units = {name: path.with_name(name).read_text(**kwargs) for name in ("sip_transport.cpp", *TRANSPORT_PARTS)}
    fragments = []
    for index, (name, marker) in enumerate(SOURCE_RANGES):
        start = units[name].index(marker)
        next_marker = next((value for later, value in SOURCE_RANGES[index + 1:] if later == name), None)
        end = units[name].index(next_marker, start + len(marker)) if next_marker else units[name].rindex("}  // namespace voip_stack")
        fragments.append(units[name][start:end])
    header = path.with_name("sip_transport_internal.h").read_text(**kwargs)
    footer = header[header.index("}  // namespace voip_stack"):]
    return header[:header.index("}  // namespace voip_stack")] + "".join(fragments) + footer


def transport_units(component):
    return [component / "sip_transport.cpp", *(component / name for name in TRANSPORT_PARTS
                                               if (component / name).exists())]
