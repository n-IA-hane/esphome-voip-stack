import logging

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome import automation
from esphome.core import CORE, TimePeriod
from esphome.const import (
    CONF_ID,
    CONF_MICROPHONE,
    CONF_NUM_CHANNELS,
    CONF_SPEAKER,
    CONF_ICON,
    CONF_NAME,
    CONF_MODE,
    CONF_DISABLED_BY_DEFAULT,
)
from esphome.components import audio, microphone, speaker, text_sensor

CODEOWNERS = ["@n-IA-hane"]
DEPENDENCIES = ["esp32"]


def AUTO_LOAD(config):
    return ["button", "switch", "number", "text_sensor"]

_LOGGER = logging.getLogger(__name__)

CONF_VOIP_STACK_ID = "voip_stack_id"
CONF_DC_OFFSET_REMOVAL = "dc_offset_removal"
CONF_TASK_STACKS_IN_PSRAM = "task_stacks_in_psram"
CONF_BUFFERS_IN_PSRAM = "buffers_in_psram"
CONF_AUTO_ENTITIES = "auto_entities"
CONF_MICROPHONE_SOURCE = "microphone_source"

CONF_PROCESSOR_ID = "processor_id"
CONF_AEC_REF_DELAY_MS = "aec_reference_delay_ms"
CONF_RINGING_TIMEOUT = "ringing_timeout"
CONF_CALLING_TIMEOUT = "calling_timeout"
CONF_ON_DESTINATION_CHANGED = "on_destination_changed"
CONF_ON_PHONEBOOK_UPDATE = "on_phonebook_update"
CONF_DELETE_CONTACT_MISSING_FROM = "delete_contact_missing_from"
CONF_UPDATES_NUMBER = "updates_number"
CONF_HA_PHONEBOOK_TEXT_SENSOR_ID = "ha_phonebook_text_sensor_id"
# HA publishes one SIP roster at `sensor.voip_phonebook`. The shipped
# subscription package binds that HA text_sensor here; voip_stack then
# normalizes each contact into the local SIP/UDP or SIP/TCP dial plan.

CONF_ON_RINGING = "on_ringing"
CONF_ON_IN_CALL = "on_in_call"
CONF_ON_IDLE = "on_idle"
# FSM triggers
CONF_ON_CALLING = "on_calling"
CONF_ON_DEST_RINGING = "on_dest_ringing"
CONF_ON_INCOMING_CALL = "on_incoming_call"
CONF_ON_OUTGOING_CALL = "on_outgoing_call"
CONF_ON_BRIDGE_REQUEST = "on_bridge_request"
CONF_ON_HANGUP = "on_hangup"
CONF_ON_CALL_FAILED = "on_call_failed"
CONF_REASON = "reason"

# SIP signaling transport selection. RTP media is always UDP.
CONF_TRANSPORT = "transport"
CONF_SIP_PORT = "sip_port"
CONF_RTP_PORT = "rtp_port"
CONF_USE_HA_AS_FIRST_CONTACT = "use_ha_as_first_contact"
CONF_AUDIO_DEBUG = "audio_debug"
CONF_AUDIO = "audio"
CONF_TX = "tx"
CONF_RX = "rx"
CONF_TX_FORMATS = "tx_formats"
CONF_RX_FORMATS = "rx_formats"
CONF_SAMPLE_RATE = "sample_rate"
CONF_PCM_FORMAT = "pcm_format"
CONF_CHANNELS = "channels"
CONF_FRAME_MS = "frame_ms"
CONF_NETWORK_SOCKET_HEADROOM = "network_socket_headroom"
CONF_UDP_MAX_PAYLOAD = "udp_max_payload"
CONF_AUTO = "auto"
CONF_STATIC_CONTACTS = "static_contacts"
CONF_ENTRY = "entry"
CONF_CONTACT = "contact"
CONF_EXTENSION = "extension"
CONF_CONFERENCE_GROUP = "conference_group"
CONF_RING_GROUP = "ring_group"
CONF_IP = "ip"
CONF_PORT = "port"
CONF_RTP_PORT_ACTION = "rtp_port"

TRANSPORT_TCP = "tcp"
TRANSPORT_UDP = "udp"

voip_stack_ns = cg.esphome_ns.namespace("voip_stack")
VoipStack = voip_stack_ns.class_("VoipStack", cg.Component)
TransportType = voip_stack_ns.enum("TransportType", is_class=True)
PcmFormat = voip_stack_ns.enum("PcmFormat", is_class=True)

PCM_FORMAT_IDS = {
    "s16le": 1,
    "s24le": 2,
    "s24le_in_s32": 3,
    "s32le": 4,
}

SUPPORTED_PHONE_SAMPLE_RATES = (8000, 12000, 16000, 24000, 32000, 44100, 48000)
UDP_SAFE_PAYLOAD_BYTES = 1200


def _is_auto(value):
    return isinstance(value, str) and value.lower() == CONF_AUTO


def _validate_endpoint_label(value):
    value = cv.string(value)
    if len(value) > 32:
        raise cv.Invalid("must be at most 32 characters")
    if any(ch in value for ch in ("|", ",", ";", "\r", "\n")):
        raise cv.Invalid("must not contain |, comma, semicolon or newlines")
    return value


def _validate_voip_audio_format(value):
    if _is_auto(value):
        return CONF_AUTO
    if any(_is_auto(value[key]) for key in (CONF_SAMPLE_RATE, CONF_PCM_FORMAT, CONF_CHANNELS, CONF_FRAME_MS)):
        return value
    if (value[CONF_SAMPLE_RATE] * value[CONF_FRAME_MS]) % 1000 != 0:
        raise cv.Invalid(
            f"sample_rate {value[CONF_SAMPLE_RATE]} and frame_ms {value[CONF_FRAME_MS]} "
            "do not form whole PCM frames"
        )
    return value


def _validate_voip_audio_config(value):
    for primary, extra_key in (
        (CONF_TX, CONF_TX_FORMATS),
        (CONF_RX, CONF_RX_FORMATS),
    ):
        if 1 + len(value[extra_key]) > 8:
            raise cv.Invalid(
                f"voip_stack.audio.{extra_key} supports at most 7 extra formats "
                f"because audio.{primary} is always included first"
            )
    return value


def _has_auto_audio_field(fmt: dict) -> bool:
    return any(_is_auto(fmt[key]) for key in (CONF_SAMPLE_RATE, CONF_PCM_FORMAT, CONF_CHANNELS, CONF_FRAME_MS))


def _validate_tx_reframe_formats(tx_fmt: dict, tx_formats: list[dict]) -> None:
    for fmt in tx_formats:
        if _has_auto_audio_field(fmt):
            raise cv.Invalid(
                "voip_stack.audio.tx_formats entries must be explicit. "
                "Only audio.tx can derive fields automatically from the microphone."
            )
        for key in (CONF_SAMPLE_RATE, CONF_PCM_FORMAT, CONF_CHANNELS):
            if fmt[key] != tx_fmt[key]:
                raise cv.Invalid(
                    "voip_stack.audio.tx_formats can only declare packet-time reframes "
                    "of audio.tx. sample_rate, pcm_format and channels must match audio.tx."
                )


PHONE_AUDIO_FORMAT_SCHEMA = cv.All(cv.Any(cv.one_of(CONF_AUTO, lower=True), cv.Schema(
    {
        cv.Optional(CONF_SAMPLE_RATE, default=CONF_AUTO): cv.Any(
            cv.one_of(CONF_AUTO, lower=True), cv.one_of(*SUPPORTED_PHONE_SAMPLE_RATES, int=True)
        ),
        cv.Optional(CONF_PCM_FORMAT, default=CONF_AUTO): cv.Any(
            cv.one_of(CONF_AUTO, lower=True), cv.one_of(*PCM_FORMAT_IDS.keys(), lower=True)
        ),
        cv.Optional(CONF_CHANNELS, default=CONF_AUTO): cv.Any(
            cv.one_of(CONF_AUTO, lower=True), cv.one_of(1, 2, int=True)
        ),
        cv.Optional(CONF_FRAME_MS, default=CONF_AUTO): cv.Any(
            cv.one_of(CONF_AUTO, lower=True), cv.one_of(10, 16, 20, 32, int=True)
        ),
    }
)), _validate_voip_audio_format)


def _format_container_bits(fmt: dict) -> int:
    pcm = fmt[CONF_PCM_FORMAT]
    if pcm == "s16le":
        return 16
    if pcm == "s24le":
        return 24
    return 32


def _format_rtp_payload_bytes(fmt: dict) -> int:
    samples = (fmt[CONF_SAMPLE_RATE] * fmt[CONF_FRAME_MS]) // 1000
    sample_bytes = 3 if fmt[CONF_PCM_FORMAT] == "s24le_in_s32" else (_format_container_bits(fmt) // 8)
    return samples * fmt[CONF_CHANNELS] * sample_bytes


def _pcm_from_bits(bits: int) -> str:
    if bits == 16:
        return "s16le"
    if bits == 24:
        return "s24le"
    if bits == 32:
        # Most ESP I2S 24-bit microphone paths are transported in 32-bit slots.
        # Users with true 32-bit PCM can still declare pcm_format: s32le.
        return "s24le_in_s32"
    raise cv.Invalid(f"voip_stack audio auto cannot map {bits} bits per sample to a PCM format")


def _declared_config_for_id(id_value):
    fconf = fv.full_config.get()
    path = fconf.get_path_for_id(id_value)[:-1]
    return fconf.get_config_for_path(path)


def _single_stream_value(declaration: dict, key: str, min_key: str, max_key: str, *, context: str):
    if key in declaration:
        return declaration[key]
    low = declaration.get(min_key)
    high = declaration.get(max_key)
    if low is not None and high is not None and low == high:
        return low
    return None


def _esp_audio_stack_parent_config(declaration: dict):
    parent_id = declaration.get("esp_audio_stack_id")
    if parent_id is None:
        return None
    return _declared_config_for_id(parent_id)


def _derive_stream_format_from_device(device_config: dict, *, direction: str) -> dict | None:
    parent = _esp_audio_stack_parent_config(device_config)
    if parent is not None:
        if direction == CONF_TX:
            sample_rate = parent.get("output_sample_rate") or parent.get(CONF_SAMPLE_RATE)
            channels = 1
            bits = 16
        else:
            sample_rate = parent.get(CONF_SAMPLE_RATE)
            channels = device_config.get(CONF_NUM_CHANNELS) or parent.get("speaker_channels") or 1
            bits = 16
        if sample_rate:
            return {
                CONF_SAMPLE_RATE: sample_rate,
                CONF_PCM_FORMAT: _pcm_from_bits(bits),
                CONF_CHANNELS: channels,
                CONF_FRAME_MS: 16,
            }

    sample_rate = _single_stream_value(
        device_config,
        CONF_SAMPLE_RATE,
        audio.CONF_MIN_SAMPLE_RATE,
        audio.CONF_MAX_SAMPLE_RATE,
        context=direction,
    )
    bits = _single_stream_value(
        device_config,
        "bits_per_sample",
        audio.CONF_MIN_BITS_PER_SAMPLE,
        audio.CONF_MAX_BITS_PER_SAMPLE,
        context=direction,
    )
    channels = _single_stream_value(
        device_config,
        CONF_NUM_CHANNELS,
        audio.CONF_MIN_CHANNELS,
        audio.CONF_MAX_CHANNELS,
        context=direction,
    )
    if sample_rate is None or bits is None or channels is None:
        return None
    return {
        CONF_SAMPLE_RATE: sample_rate,
        CONF_PCM_FORMAT: _pcm_from_bits(int(bits)),
        CONF_CHANNELS: channels,
        CONF_FRAME_MS: 16,
    }


def _derive_tx_format(config: dict) -> dict | None:
    if CONF_MICROPHONE_SOURCE in config:
        source = config[CONF_MICROPHONE_SOURCE]
        mic_config = _declared_config_for_id(source[CONF_MICROPHONE])
        base = _derive_stream_format_from_device(mic_config, direction=CONF_TX)
        if base is None:
            return None
        bits = int(source.get("bits_per_sample", 16))
        channels = len(source.get(CONF_CHANNELS, [0]))
        base[CONF_PCM_FORMAT] = _pcm_from_bits(bits)
        base[CONF_CHANNELS] = channels
        return base
    if CONF_MICROPHONE in config:
        return _derive_stream_format_from_device(
            _declared_config_for_id(config[CONF_MICROPHONE]),
            direction=CONF_TX,
        )
    return None


def _derive_rx_format(config: dict) -> dict | None:
    if CONF_SPEAKER not in config:
        return None
    return _derive_stream_format_from_device(
        _declared_config_for_id(config[CONF_SPEAKER]),
        direction=CONF_RX,
    )


def _resolve_audio_format(config: dict, direction: str, value) -> dict:
    derive = _derive_tx_format if direction == CONF_TX else _derive_rx_format
    derived = derive(config)
    if _is_auto(value):
        if derived is not None:
            return derived
        if (direction == CONF_TX and CONF_MICROPHONE not in config and CONF_MICROPHONE_SOURCE not in config) or (
            direction == CONF_RX and CONF_SPEAKER not in config
        ):
            return {
                CONF_SAMPLE_RATE: 16000,
                CONF_PCM_FORMAT: "s16le",
                CONF_CHANNELS: 1,
                CONF_FRAME_MS: 16,
            }
        source = "microphone/source" if direction == CONF_TX else "speaker"
        raise cv.Invalid(
            f"voip_stack.audio.{direction}: auto could not derive the PCM format from the referenced "
            f"{source}. Declare audio.{direction}.sample_rate/pcm_format/channels/frame_ms manually."
        )

    resolved = dict(value)
    if any(_is_auto(resolved[key]) for key in (CONF_SAMPLE_RATE, CONF_PCM_FORMAT, CONF_CHANNELS, CONF_FRAME_MS)):
        if derived is None and all(
            _is_auto(resolved[key]) for key in (CONF_SAMPLE_RATE, CONF_PCM_FORMAT, CONF_CHANNELS, CONF_FRAME_MS)
        ) and (
            (direction == CONF_TX and CONF_MICROPHONE not in config and CONF_MICROPHONE_SOURCE not in config) or
            (direction == CONF_RX and CONF_SPEAKER not in config)
        ):
            return {
                CONF_SAMPLE_RATE: 16000,
                CONF_PCM_FORMAT: "s16le",
                CONF_CHANNELS: 1,
                CONF_FRAME_MS: 16,
            }
        if derived is None:
            source = "microphone/source" if direction == CONF_TX else "speaker"
            raise cv.Invalid(
                f"voip_stack.audio.{direction}: one or more fields are auto, but the referenced {source} "
                "does not expose a concrete stream format. Declare every audio field manually."
            )
        for key in (CONF_SAMPLE_RATE, CONF_PCM_FORMAT, CONF_CHANNELS, CONF_FRAME_MS):
            if _is_auto(resolved[key]):
                resolved[key] = derived[key]
    return _validate_voip_audio_format(resolved)

# === Action classes (for YAML: voip_stack.next_contact, etc.) ===
NextContactAction = voip_stack_ns.class_("NextContactAction", automation.Action)
PrevContactAction = voip_stack_ns.class_("PrevContactAction", automation.Action)
StartAction = voip_stack_ns.class_("StartAction", automation.Action)
StopAction = voip_stack_ns.class_("StopAction", automation.Action)
AnswerCallAction = voip_stack_ns.class_("AnswerCallAction", automation.Action)
DeclineCallAction = voip_stack_ns.class_("DeclineCallAction", automation.Action)
CallToggleAction = voip_stack_ns.class_("CallToggleAction", automation.Action)
PublishEntityStatesAction = voip_stack_ns.class_("PublishEntityStatesAction", automation.Action)

# Parameterized actions
SetVolumeAction = voip_stack_ns.class_("SetVolumeAction", automation.Action)
SetMicGainDbAction = voip_stack_ns.class_("SetMicGainDbAction", automation.Action)
SetContactsAction = voip_stack_ns.class_("SetContactsAction", automation.Action)
SetContactAction = voip_stack_ns.class_("SetContactAction", automation.Action)
CallContactAction = voip_stack_ns.class_("CallContactAction", automation.Action)
SetRemoteEndpointAction = voip_stack_ns.class_("SetRemoteEndpointAction", automation.Action)
AddContactAction = voip_stack_ns.class_("AddContactAction", automation.Action)
RemoveContactAction = voip_stack_ns.class_("RemoveContactAction", automation.Action)
FlushContactsAction = voip_stack_ns.class_("FlushContactsAction", automation.Action)
UpdateContactsAction = voip_stack_ns.class_("UpdateContactsAction", automation.Action)
SetHaPeerNameAction = voip_stack_ns.class_("SetHaPeerNameAction", automation.Action)

# === Condition classes (for YAML: voip_stack.is_idle, etc.) ===
VoipIsIdleCondition = voip_stack_ns.class_("VoipIsIdleCondition", automation.Condition)
VoipIsRingingCondition = voip_stack_ns.class_("VoipIsRingingCondition", automation.Condition)
VoipIsInCallCondition = voip_stack_ns.class_("VoipIsInCallCondition", automation.Condition)
VoipIsCallingCondition = voip_stack_ns.class_("VoipIsCallingCondition", automation.Condition)
VoipIsIncomingCondition = voip_stack_ns.class_("VoipIsIncomingCondition", automation.Condition)
VoipDestinationIsCondition = voip_stack_ns.class_("VoipDestinationIsCondition", automation.Condition)
VoipIsHaDestinationCondition = voip_stack_ns.class_("VoipIsHaDestinationCondition", automation.Condition)

# Auto-entity classes: declared here so to_code below can construct them even
# when YAML does not include explicit `switch:` / `number:` platform blocks.
# The explicit platforms reuse the same class names through the namespace.
from esphome.components import switch as _switch_ns, number as _number_ns
VoipStackAutoAnswerCls = voip_stack_ns.class_(
    "VoipStackAutoAnswer", _switch_ns.Switch, cg.Parented.template(VoipStack)
)
VoipStackDndSwitchCls = voip_stack_ns.class_(
    "VoipStackDndSwitch", _switch_ns.Switch, cg.Parented.template(VoipStack)
)
VoipStackVolumeCls = voip_stack_ns.class_(
    "VoipStackVolume", _number_ns.Number, cg.Parented.template(VoipStack)
)
VoipStackMicGainCls = voip_stack_ns.class_(
    "VoipStackMicGain", _number_ns.Number, cg.Parented.template(VoipStack)
)


PHONEBOOK_CONTACT_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_ENTRY): cv.string,
        cv.Optional(CONF_NAME): cv.string,
        cv.Optional(CONF_IP): cv.string,
        cv.Optional(CONF_PORT, default=5060): cv.port,
        cv.Optional(CONF_RTP_PORT_ACTION, default=40000): cv.port,
        cv.Optional(CONF_TRANSPORT): cv.one_of(TRANSPORT_UDP, TRANSPORT_TCP, lower=True),
    }
)


def _validate_static_contact(value):
    value = PHONEBOOK_CONTACT_SCHEMA(value)
    if CONF_ENTRY in value:
        return value
    if CONF_NAME not in value:
        raise cv.Invalid("static contact requires name")
    return value


def _static_contact_entry(contact, default_transport: str) -> str:
    if CONF_ENTRY in contact:
        return contact[CONF_ENTRY]
    name = contact[CONF_NAME]
    ip = contact.get(CONF_IP, "")
    if not ip:
        return name
    transport = contact.get(CONF_TRANSPORT) or default_transport
    transport = "sip_tcp" if transport == TRANSPORT_TCP else "sip_udp"
    return f"{name}|{ip}|{contact[CONF_PORT]}|{contact[CONF_RTP_PORT_ACTION]}|{transport}"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(VoipStack),
        # SIP signaling transport. Use transport: tcp for SIP/TCP or
        # transport: udp for SIP/UDP. Audio remains RTP/UDP.
        cv.Optional(CONF_TRANSPORT, default=TRANSPORT_UDP): cv.one_of(
            TRANSPORT_TCP, TRANSPORT_UDP, lower=True
        ),
        cv.Optional(CONF_UDP_MAX_PAYLOAD, default=UDP_SAFE_PAYLOAD_BYTES): cv.int_range(
            min=576, max=65507
        ),
        cv.Optional(CONF_SIP_PORT, default=5060): cv.port,
        cv.Optional(CONF_RTP_PORT, default=40000): cv.port,
        cv.Optional(CONF_STATIC_CONTACTS, default=[]): cv.ensure_list(
            _validate_static_contact
        ),
        cv.Optional(CONF_EXTENSION, default=""): cv.string,
        cv.Optional(CONF_CONFERENCE_GROUP, default=""): _validate_endpoint_label,
        cv.Optional(CONF_RING_GROUP, default=""): _validate_endpoint_label,
        # On the first post-boot phonebook population, select the HA peer row
        # as the current destination so a freshly booted ESP is tuned to HA
        # instead of whichever contact happens to be first in the roster order.
        cv.Optional(CONF_USE_HA_AS_FIRST_CONTACT, default=False): cv.boolean,
        # Targeted diagnostics: logs PCM peak/RMS on VoIP TX/RX.
        # Keep disabled by default; enable only on devices under audio-level test.
        cv.Optional(CONF_AUDIO_DEBUG, default=False): cv.boolean,
        # VoIP wire PCM contract. `tx` is microphone/source -> wire;
        # `rx` is wire -> speaker/sink. They are intentionally independent:
        # an AFE mic can publish 16 kHz while the speaker sink accepts 48 kHz.
        cv.Optional(CONF_AUDIO, default={}): cv.All(cv.Schema(
            {
                cv.Optional(CONF_TX, default={}): PHONE_AUDIO_FORMAT_SCHEMA,
                cv.Optional(CONF_RX, default={}): PHONE_AUDIO_FORMAT_SCHEMA,
                cv.Optional(CONF_TX_FORMATS, default=[]): cv.ensure_list(
                    PHONE_AUDIO_FORMAT_SCHEMA
                ),
                cv.Optional(CONF_RX_FORMATS, default=[]): cv.ensure_list(
                    PHONE_AUDIO_FORMAT_SCHEMA
                ),
            }
        ), _validate_voip_audio_config),
        # Preferred path: use the native ESPHome microphone directly. Maintained
        # esp_audio_stack profiles already expose 16 kHz / 16-bit / mono audio,
        # so MicrophoneSource would only add an avoidable copy/conversion pass.
        cv.Optional(CONF_MICROPHONE): cv.use_id(microphone.Microphone),
        # Compatibility/advanced path for raw microphones that need channel,
        # bit-depth, or integer gain conversion before voip_stack sees them.
        cv.Optional(CONF_MICROPHONE_SOURCE): microphone.microphone_source_schema(
            min_bits_per_sample=16,
            max_bits_per_sample=32,
            min_channels=1,
            max_channels=2,
        ),
        cv.Optional(CONF_SPEAKER): cv.use_id(speaker.Speaker),
        # DC offset removal for mics with significant DC bias (e.g., SPH0645)
        cv.Optional(CONF_DC_OFFSET_REMOVAL, default=False): cv.boolean,
        # Place VoIP network task stacks in PSRAM. The TX task exists only
        # when a microphone is configured; transport/control tasks are owned by
        # the selected transport. Default false keeps stacks in internal RAM,
        # required on plain ESP32 boards without PSRAM. Set true on heavy S3/P4
        # builds when PSRAM stacks are enabled in sdkconfig.
        cv.Optional(CONF_TASK_STACKS_IN_PSRAM, default=False): cv.boolean,
        # Auto-create the boilerplate switches/numbers (auto_answer, volume,
        # mic_gain). Default false for YAMLs that already declare them via
        # `switch:`/`number: - platform: voip_stack`.
        # Set to true on a minimal new yaml to skip that boilerplate.
        cv.Optional(CONF_AUTO_ENTITIES, default=False): cv.boolean,
        # Standalone VoIP DSP/AEC was removed. Use native ESPHome mic/speaker
        # directly, or put software AEC/AFE on esp_audio_stack and pass its
        # microphone/speaker facade here.
        cv.Optional(CONF_PROCESSOR_ID): cv.invalid(
            "voip_stack.processor_id was removed. Use native ESPHome "
            "microphone/speaker directly, or put processor_id on esp_audio_stack "
            "for software AEC/AFE."
        ),
        cv.Optional(CONF_AEC_REF_DELAY_MS): cv.invalid(
            "voip_stack.aec_reference_delay_ms was removed with standalone "
            "VoIP AEC. Configure AEC on esp_audio_stack instead."
        ),
        # Place VoIP staging buffers in PSRAM. This applies only to buffers
        # still owned by voip_stack: the optional mic ring, TX chunk, and mic
        # processing scratch. Software AEC/AFE buffers belong to esp_audio_stack.
        cv.Optional(CONF_BUFFERS_IN_PSRAM, default=False): cv.boolean,
        # Ringing timeout: auto-decline call if not answered within this time
        cv.Optional(CONF_RINGING_TIMEOUT): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_CALLING_TIMEOUT): cv.positive_time_period_milliseconds,
        # Trigger when incoming call (auto_answer OFF)
        cv.Optional(CONF_ON_RINGING): automation.validate_automation(single=True),
        # Trigger when in_call starts
        cv.Optional(CONF_ON_IN_CALL): automation.validate_automation(single=True),
        # Trigger when state returns to idle
        cv.Optional(CONF_ON_IDLE): automation.validate_automation(single=True),
        # FSM triggers
        cv.Optional(CONF_ON_CALLING): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_DEST_RINGING): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_INCOMING_CALL): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_OUTGOING_CALL): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_BRIDGE_REQUEST): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_HANGUP): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_CALL_FAILED): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_DESTINATION_CHANGED): automation.validate_automation(single=True),
        # Fires after every real phonebook mutation: HA roster push, manual
        # add/remove/set/flush, or stale-contact pruning.
        cv.Optional(CONF_ON_PHONEBOOK_UPDATE): automation.validate_automation(single=True),
        # Bind a YAML-declared `homeassistant` text_sensor (typically via
        # packages/voip/phonebook_subscribe.yaml) as the authoritative
        # HA-side source. Optional: when absent the HA path is skipped.
        cv.Optional(CONF_HA_PHONEBOOK_TEXT_SENSOR_ID): cv.use_id(
            text_sensor.TextSensor
        ),
        # Contact pruning: delete a contact after N consecutive update cycles
        # in which no source reported it. Optional - absent means pruning is
        # disabled (slots survive forever until explicit remove_contact /
        # flush_contacts). Range 1..10.
        cv.Optional(CONF_DELETE_CONTACT_MISSING_FROM): cv.Schema(
            {
                cv.Required(CONF_UPDATES_NUMBER): cv.int_range(min=1, max=10),
            }
        ),
        # Additional TCP socket headroom for full-experience firmware where HA
        # API/logging, media HTTP, TTS/announcement HTTP and VoIP calls can overlap.
        # Validation-only: no runtime code is generated by this option.
        cv.Optional(CONF_NETWORK_SOCKET_HEADROOM, default=0): cv.int_range(
            min=0, max=32
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


def _consume_voip_sockets(config):
    """Reserve lwIP sockets for voip_stack at validation time.

    TCP mode: one TCP listening socket + up to three concurrent TCP client
    sockets (mirrors the upstream api component pattern).
    UDP mode: a single datagram socket (no listen, no per-peer connection).
    """
    from esphome.components import socket

    # SIP signaling can be UDP or TCP; audio remains RTP/UDP.
    socket.consume_sockets(2, "voip_stack_sip", socket.SocketType.UDP)(config)
    socket.consume_sockets(2, "voip_stack_sip_tcp")(config)
    socket.consume_sockets(1, "voip_stack_sip", socket.SocketType.TCP_LISTEN)(config)
    extra = config.get(CONF_NETWORK_SOCKET_HEADROOM, 0)
    if extra:
        socket.consume_sockets(extra, "voip_stack_headroom")(config)
    return config


def _final_validate(config):
    """Cross-component validation + socket reservation."""
    protocol = config.get(CONF_TRANSPORT, TRANSPORT_UDP)
    if CONF_MICROPHONE in config and CONF_MICROPHONE_SOURCE in config:
        raise cv.Invalid(
            "Use only one of voip_stack.microphone or voip_stack.microphone_source."
        )

    audio_cfg = config[CONF_AUDIO]
    audio_cfg[CONF_TX] = _resolve_audio_format(config, CONF_TX, audio_cfg[CONF_TX])
    audio_cfg[CONF_RX] = _resolve_audio_format(config, CONF_RX, audio_cfg[CONF_RX])
    tx_fmt = audio_cfg[CONF_TX]
    rx_fmt = audio_cfg[CONF_RX]
    _validate_tx_reframe_formats(tx_fmt, audio_cfg[CONF_TX_FORMATS])

    if protocol in (TRANSPORT_UDP, TRANSPORT_TCP):
        audio_cfg = config[CONF_AUDIO]
        max_payload = config[CONF_UDP_MAX_PAYLOAD]
        checks = [
            (CONF_TX, audio_cfg[CONF_TX]),
            (CONF_RX, audio_cfg[CONF_RX]),
        ]
        checks.extend((CONF_TX_FORMATS, fmt) for fmt in audio_cfg[CONF_TX_FORMATS])
        checks.extend((CONF_RX_FORMATS, fmt) for fmt in audio_cfg[CONF_RX_FORMATS])
        for direction, fmt in checks:
            payload_bytes = _format_rtp_payload_bytes(fmt)
            if payload_bytes > max_payload:
                raise cv.Invalid(
                    f"voip_stack RTP audio.{direction} payload is {payload_bytes} bytes, "
                    f"above the configured RTP payload limit of {max_payload}. "
                    "ESPHome editor action required: lower audio sample_rate, channels, pcm_format, "
                    "or frame_ms for this SIP/RTP profile. Only raise udp_max_payload when this LAN is "
                    "intentionally configured for larger datagrams; the default limit is 1200 bytes."
                )

    if CONF_MICROPHONE in config:
        try:
            audio.final_validate_audio_schema(
                "voip_stack",
                audio_device=CONF_MICROPHONE,
                bits_per_sample=_format_container_bits(tx_fmt),
                channels=tx_fmt[CONF_CHANNELS],
                sample_rate=tx_fmt[CONF_SAMPLE_RATE],
                audio_device_issue=True,
            )(config)
        except AssertionError:
            _LOGGER.warning(
                "voip_stack could not validate the referenced microphone audio "
                "format because that microphone component does not publish ESPHome "
                "audio stream limits. Continuing with the explicitly declared "
                "voip_stack.audio.tx format."
            )

    if CONF_MICROPHONE_SOURCE in config:
        microphone.final_validate_microphone_source_schema(
            "voip_stack", sample_rate=tx_fmt[CONF_SAMPLE_RATE]
        )(config[CONF_MICROPHONE_SOURCE])

    if CONF_SPEAKER in config:
        try:
            audio.final_validate_audio_schema(
                "voip_stack",
                audio_device=CONF_SPEAKER,
                bits_per_sample=_format_container_bits(rx_fmt),
                channels=rx_fmt[CONF_CHANNELS],
                sample_rate=rx_fmt[CONF_SAMPLE_RATE],
                audio_device_issue=True,
            )(config)
        except AssertionError:
            _LOGGER.warning(
                "voip_stack speaker format was not declared by the referenced "
                "speaker component, so ESPHome cannot check it at compile time. "
                "Continuing with the explicitly declared voip_stack.audio.rx format."
            )

    # Check if esp_audio_stack is also configured
    audio_stack_configs = fv.full_config.get().get("esp_audio_stack", [])

    if audio_stack_configs:
        # Warn about DC offset double-filtering
        if config.get(CONF_DC_OFFSET_REMOVAL, False):
            for audio_stack in (audio_stack_configs if isinstance(audio_stack_configs, list) else [audio_stack_configs]):
                if isinstance(audio_stack, dict) and audio_stack.get("correct_dc_offset", False):
                    raise cv.Invalid(
                        "Both voip_stack.dc_offset_removal and esp_audio_stack.correct_dc_offset are enabled. "
                        "Double DC-block filtering causes instability. "
                        "Use correct_dc_offset on esp_audio_stack only."
                    )

    _consume_voip_sockets(config)
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def _add_core_settings(var, config):
    if CONF_MICROPHONE in config:
        cg.add_define("USE_ESPHOME_VOIP_STACK_MIC")
        mic = await cg.get_variable(config[CONF_MICROPHONE])
        cg.add(var.set_microphone(mic))

    if CONF_MICROPHONE_SOURCE in config:
        cg.add_define("USE_ESPHOME_VOIP_STACK_MIC")
        mic_source = await microphone.microphone_source_to_code(config[CONF_MICROPHONE_SOURCE])
        cg.add(var.set_microphone_source(mic_source))

    if CONF_SPEAKER in config:
        cg.add_define("USE_ESPHOME_VOIP_STACK_SPEAKER")
        spk = await cg.get_variable(config[CONF_SPEAKER])
        cg.add(var.set_speaker(spk))

    cg.add(var.set_dc_offset_removal(config[CONF_DC_OFFSET_REMOVAL]))
    cg.add(var.set_task_stacks_in_psram(config[CONF_TASK_STACKS_IN_PSRAM]))
    cg.add(var.set_buffers_in_psram(config[CONF_BUFFERS_IN_PSRAM]))
    cg.add(var.set_extension(config[CONF_EXTENSION]))
    cg.add(var.set_conference_group(config[CONF_CONFERENCE_GROUP]))
    cg.add(var.set_ring_group(config[CONF_RING_GROUP]))
    cg.add(var.set_use_ha_as_first_contact(config[CONF_USE_HA_AS_FIRST_CONTACT]))
    cg.add(var.set_audio_debug(config[CONF_AUDIO_DEBUG]))
    if config[CONF_AUDIO_DEBUG]:
        cg.add_define("USE_ESPHOME_VOIP_STACK_AUDIO_DEBUG")
    audio_cfg = config[CONF_AUDIO]
    for key, setter in (
        (CONF_TX, var.set_tx_audio_format),
        (CONF_RX, var.set_rx_audio_format),
    ):
        fmt = audio_cfg[key]
        cg.add(
            setter(
                fmt[CONF_SAMPLE_RATE],
                PCM_FORMAT_IDS[fmt[CONF_PCM_FORMAT]],
                fmt[CONF_CHANNELS],
                fmt[CONF_FRAME_MS],
            )
        )
    for key, setter in (
        (CONF_TX_FORMATS, var.add_supported_tx_audio_format),
        (CONF_RX_FORMATS, var.add_supported_rx_audio_format),
    ):
        for fmt in audio_cfg[key]:
            cg.add(
                setter(
                    fmt[CONF_SAMPLE_RATE],
                    PCM_FORMAT_IDS[fmt[CONF_PCM_FORMAT]],
                    fmt[CONF_CHANNELS],
                    fmt[CONF_FRAME_MS],
                )
            )
    cg.add_define("USE_ESPHOME_VOIP_SIP_TRANSPORT")


def _add_transport_settings(var, config):
    # SIP signaling transport: UDP or TCP. Audio remains RTP/UDP.
    if config[CONF_TRANSPORT] == TRANSPORT_UDP:
        cg.add(var.set_protocol(TransportType.UDP))
    else:
        cg.add(var.set_protocol(TransportType.TCP))
    cg.add(var.set_udp_max_payload(config[CONF_UDP_MAX_PAYLOAD]))
    cg.add(var.set_sip_port(config[CONF_SIP_PORT]))
    cg.add(var.set_rtp_port(config[CONF_RTP_PORT]))


def _add_static_contacts(var, config):
    contacts = config.get(CONF_STATIC_CONTACTS, [])
    default_transport = config.get(CONF_TRANSPORT, TRANSPORT_UDP)
    for contact in contacts:
        cg.add(var.add_contact(_static_contact_entry(contact, default_transport)))


async def _add_device_and_audio_settings(var, config):
    # Set device name (used to exclude self from the contacts list)
    from esphome.core import CORE
    cg.add(var.set_device_name(CORE.friendly_name or CORE.name))
    # Stable SIP caller route id (yaml node name slug).
    cg.add(var.set_device_route_id(CORE.name))

    # Ringing timeout (auto-decline if not answered)
    if CONF_RINGING_TIMEOUT in config:
        cg.add(var.set_ringing_timeout(config[CONF_RINGING_TIMEOUT]))

    # Calling timeout: caller in CALLING with no final SIP response.
    # Auto-fires SIP timeout handling when expired.
    if CONF_CALLING_TIMEOUT in config:
        cg.add(var.set_calling_timeout(config[CONF_CALLING_TIMEOUT]))

    # Optional contact pruning configuration. Absent = pruning disabled (the
    # default kept in C++ as prune_threshold_=0).
    if CONF_DELETE_CONTACT_MISSING_FROM in config:
        prune = config[CONF_DELETE_CONTACT_MISSING_FROM]
        cg.add(var.set_prune_threshold(prune[CONF_UPDATES_NUMBER]))


async def _build_voip_automations(var, config):
    # on_ringing automation
    if CONF_ON_RINGING in config:
        await automation.build_automation(
            var.get_ringing_trigger(), [], config[CONF_ON_RINGING]
        )

    # on_in_call automation
    if CONF_ON_IN_CALL in config:
        await automation.build_automation(
            var.get_in_call_trigger(), [], config[CONF_ON_IN_CALL]
        )

    # on_idle automation
    if CONF_ON_IDLE in config:
        await automation.build_automation(
            var.get_idle_trigger(), [], config[CONF_ON_IDLE]
        )

    # FSM triggers.
    if CONF_ON_CALLING in config:
        await automation.build_automation(
            var.get_calling_trigger(), [], config[CONF_ON_CALLING]
        )

    if CONF_ON_DEST_RINGING in config:
        await automation.build_automation(
            var.get_dest_ringing_trigger(), [], config[CONF_ON_DEST_RINGING]
        )

    trigger_args = [
        (cg.std_string, "call_id"),
        (cg.std_string, "caller"),
        (cg.std_string, "callee"),
        (cg.std_string, "uri"),
    ]
    if CONF_ON_INCOMING_CALL in config:
        await automation.build_automation(
            var.get_incoming_call_trigger(), trigger_args, config[CONF_ON_INCOMING_CALL]
        )

    if CONF_ON_OUTGOING_CALL in config:
        await automation.build_automation(
            var.get_outgoing_call_trigger(), trigger_args, config[CONF_ON_OUTGOING_CALL]
        )

    if CONF_ON_BRIDGE_REQUEST in config:
        await automation.build_automation(
            var.get_bridge_request_trigger(), trigger_args, config[CONF_ON_BRIDGE_REQUEST]
        )

    if CONF_ON_HANGUP in config:
        await automation.build_automation(
            var.get_hangup_trigger(), [(cg.std_string, "reason")], config[CONF_ON_HANGUP]
        )

    if CONF_ON_CALL_FAILED in config:
        await automation.build_automation(
            var.get_call_failed_trigger(), [(cg.std_string, "reason")], config[CONF_ON_CALL_FAILED]
        )

    if CONF_ON_DESTINATION_CHANGED in config:
        await automation.build_automation(
            var.get_destination_changed_trigger(), [], config[CONF_ON_DESTINATION_CHANGED]
        )

    if CONF_ON_PHONEBOOK_UPDATE in config:
        await automation.build_automation(
            var.get_phonebook_update_trigger(), [], config[CONF_ON_PHONEBOOK_UPDATE]
        )


async def _new_voip_text_sensor(config, suffix: str, name: str, icon: str):
    sensor_id = cv.declare_id(text_sensor.TextSensor)(f"{config[CONF_ID].id}_{suffix}")
    return await text_sensor.new_text_sensor(
        {
            CONF_ID: sensor_id,
            CONF_NAME: name,
            CONF_ICON: icon,
            CONF_DISABLED_BY_DEFAULT: False,
        }
    )


async def _build_voip_text_sensors(var, config):
    # === Auto-create sensors ===

    # State sensor: always created.
    state_sensor = await _new_voip_text_sensor(
        config, "state", "VoIP State", "mdi:phone-settings"
    )
    cg.add(var.set_state_sensor(state_sensor))

    # Transport sensor: diagnostic, exposes the active transport ("udp" or
    # "tcp") so the HA-side voip_stack integration can route calls from a
    # self-describing `voip_stack:` declaration.
    transport_sensor = await _new_voip_text_sensor(
        config, "transport", "VoIP Transport", "mdi:swap-horizontal"
    )
    cg.add(var.set_transport_sensor(transport_sensor))

    # Endpoint sensor: authoritative HA/ESP routing identity. HA consumes this
    # instead of inferring IP/ports from registry data.
    endpoint_sensor = await _new_voip_text_sensor(
        config, "endpoint", "VoIP Endpoint", "mdi:lan-connect"
    )
    cg.add(var.set_endpoint_sensor(endpoint_sensor))

    # Last terminal reason: required by voip_stack/card mirror mode.
    # ESP-to-ESP direct calls do not pass through HA as a signaling bridge;
    # the source ESP must therefore publish the terminal reason as state so
    # HA can render the real local/remote/decline/timeout outcome.
    last_reason_sensor = await _new_voip_text_sensor(
        config, "last_reason", "VoIP Last Reason", "mdi:phone-alert"
    )
    cg.add(var.set_last_reason_sensor(last_reason_sensor))

    sip_snapshot_sensor = await _new_voip_text_sensor(
        config, "sip_snapshot", "VoIP SIP Snapshot", "mdi:phone-log"
    )
    cg.add(var.set_sip_snapshot_sensor(sip_snapshot_sensor))

    dest_sensor = await _new_voip_text_sensor(
        config, "dest", "Destination", "mdi:phone-forward"
    )
    cg.add(var.set_destination_sensor(dest_sensor))

    caller_sensor = await _new_voip_text_sensor(
        config, "caller", "Caller", "mdi:phone-incoming"
    )
    cg.add(var.set_caller_sensor(caller_sensor))

    contacts_sensor = await _new_voip_text_sensor(
        config, "contacts", "Contacts", "mdi:account-group"
    )
    cg.add(var.set_contacts_sensor(contacts_sensor))

    if CONF_HA_PHONEBOOK_TEXT_SENSOR_ID in config:
        ha_sensor = await cg.get_variable(
            config[CONF_HA_PHONEBOOK_TEXT_SENSOR_ID]
        )
        cg.add(var.set_ha_phonebook_sensor(ha_sensor))


async def _build_voip_auto_entities(var, config):
    # Optional auto-entities: gated to opt-in (auto_entities: true) so yamls
    # that already declare `switch:`/`number: - platform: voip_stack`
    # don't end up with two competing entity registrations. New minimal
    # yamls can flip this on and skip the boilerplate altogether.
    if config[CONF_AUTO_ENTITIES]:
        from esphome.components import switch as switch_module, number as number_module
        from esphome.const import CONF_RESTORE_MODE, CONF_ENTITY_CATEGORY

        aa_id = cv.declare_id(VoipStackAutoAnswerCls)(f"{config[CONF_ID].id}_auto_answer")
        aa = await switch_module.new_switch({
            CONF_ID: aa_id,
            CONF_NAME: "Auto Answer",
            CONF_ICON: "mdi:phone-in-talk",
            CONF_DISABLED_BY_DEFAULT: False,
            CONF_RESTORE_MODE: switch_module.RESTORE_MODES["RESTORE_DEFAULT_ON"],
            CONF_ENTITY_CATEGORY: "config",
        })
        cg.add(aa.set_parent(var))
        cg.add(var.register_auto_answer_switch(aa))

        dnd_id = cv.declare_id(VoipStackDndSwitchCls)(f"{config[CONF_ID].id}_dnd")
        dnd = await switch_module.new_switch({
            CONF_ID: dnd_id,
            CONF_NAME: "Do Not Disturb",
            CONF_ICON: "mdi:minus-circle",
            CONF_DISABLED_BY_DEFAULT: False,
            CONF_RESTORE_MODE: switch_module.RESTORE_MODES["RESTORE_DEFAULT_OFF"],
            CONF_ENTITY_CATEGORY: "config",
        })
        cg.add(dnd.set_parent(var))
        cg.add(var.register_dnd_switch(dnd))

        if CONF_SPEAKER in config:
            vol_id = cv.declare_id(VoipStackVolumeCls)(f"{config[CONF_ID].id}_volume")
            vol = await number_module.new_number(
                {
                    CONF_ID: vol_id,
                    CONF_NAME: "Master Volume",
                    CONF_ICON: "mdi:volume-high",
                    CONF_DISABLED_BY_DEFAULT: False,
                    CONF_MODE: number_module.NUMBER_MODES["SLIDER"],
                },
                min_value=0,
                max_value=100,
                step=5,
            )
            cg.add(vol.set_parent(var))
            cg.add(var.register_volume_number(vol))

        if CONF_MICROPHONE in config or CONF_MICROPHONE_SOURCE in config:
            mg_id = cv.declare_id(VoipStackMicGainCls)(f"{config[CONF_ID].id}_mic_gain")
            mg = await number_module.new_number(
                {
                    CONF_ID: mg_id,
                    CONF_NAME: "Mic Gain",
                    CONF_ICON: "mdi:microphone",
                    CONF_DISABLED_BY_DEFAULT: False,
                    CONF_MODE: number_module.NUMBER_MODES["SLIDER"],
                },
                min_value=-20,
                max_value=20,
                step=1,
            )
            cg.add(mg.set_parent(var))
            cg.add(var.register_mic_gain_number(mg))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    await _add_core_settings(var, config)
    _add_transport_settings(var, config)
    await _add_device_and_audio_settings(var, config)
    _add_static_contacts(var, config)
    await _build_voip_automations(var, config)
    await _build_voip_text_sensors(var, config)
    await _build_voip_auto_entities(var, config)


# === Action registrations ===
# Simple action schema that just references the voip_stack component
PHONE_ACTION_SCHEMA = automation.maybe_simple_id(
    {
        cv.GenerateID(): cv.use_id(VoipStack),
    }
)


async def _new_parented_action(config, action_id, template_arg):
    var = cg.new_Pvariable(action_id, template_arg)
    parent = await cg.get_variable(config[CONF_ID])
    cg.add(var.set_parent(parent))
    return var


def _register_simple_action(name, action_class):
    """Register a parameter-less Parented<VoipStack> action.

    The codegen all the simple actions need is identical:
        var = new_Pvariable; parent = use_id(voip_stack); var.set_parent(parent)
    Bundle it in one helper so adding a new simple action is one line below
    instead of an eight-line decorator + coroutine block.
    """
    @automation.register_action(name, action_class, PHONE_ACTION_SCHEMA, synchronous=True)
    async def _to_code(config, action_id, template_arg, args):
        return await _new_parented_action(config, action_id, template_arg)
    return _to_code


_register_simple_action("voip_stack.next_contact", NextContactAction)
_register_simple_action("voip_stack.prev_contact", PrevContactAction)
_register_simple_action("voip_stack.start", StartAction)
_register_simple_action("voip_stack.stop", StopAction)
_register_simple_action("voip_stack.answer_call", AnswerCallAction)


@automation.register_action(
    "voip_stack.decline_call",
    DeclineCallAction,
    automation.maybe_simple_id(
        {
            cv.GenerateID(): cv.use_id(VoipStack),
            cv.Optional(CONF_REASON): cv.templatable(cv.string),
        }
    ),
    synchronous=True,
)
async def decline_call_action_to_code(config, action_id, template_arg, args):
    var = await _new_parented_action(config, action_id, template_arg)
    if CONF_REASON in config:
        templ = await cg.templatable(config[CONF_REASON], args, cg.std_string)
        cg.add(var.set_reason(templ))
    return var


_register_simple_action("voip_stack.call_toggle", CallToggleAction)
_register_simple_action("voip_stack.publish_entity_states", PublishEntityStatesAction)


# === Parameterized actions ===

CONF_VOLUME = "volume"
CONF_GAIN_DB = "gain_db"
CONF_CONTACTS_CSV = "contacts_csv"
CONF_ROSTER_JSON = "roster_json"


def _register_templated_action(name, action_class, key, validator, cpp_type, setter):
    @automation.register_action(
        name,
        action_class,
        cv.Schema(
            {
                cv.GenerateID(): cv.use_id(VoipStack),
                cv.Required(key): cv.templatable(validator),
            }
        ),
        synchronous=True,
    )
    async def _to_code(config, action_id, template_arg, args):
        var = await _new_parented_action(config, action_id, template_arg)
        templ = await cg.templatable(config[key], args, cpp_type)
        cg.add(setter(var, templ))
        return var
    return _to_code


_register_templated_action(
    "voip_stack.set_volume",
    SetVolumeAction,
    CONF_VOLUME,
    cv.float_range(min=0.0, max=1.0),
    float,
    lambda var, value: var.set_volume(value),
)
_register_templated_action(
    "voip_stack.set_mic_gain_db",
    SetMicGainDbAction,
    CONF_GAIN_DB,
    cv.float_range(min=-20.0, max=20.0),
    float,
    lambda var, value: var.set_gain_db(value),
)
_register_templated_action(
    "voip_stack.set_contacts",
    SetContactsAction,
    CONF_CONTACTS_CSV,
    cv.string,
    cg.std_string,
    lambda var, value: var.set_contacts_csv(value),
)
_register_templated_action(
    "voip_stack.set_roster_json",
    SetContactsAction,
    CONF_ROSTER_JSON,
    cv.string,
    cg.std_string,
    lambda var, value: var.set_contacts_csv(value),
)


_register_templated_action(
    "voip_stack.set_contact",
    SetContactAction,
    CONF_CONTACT,
    cv.string,
    cg.std_string,
    lambda var, value: var.set_contact(value),
)
_register_templated_action(
    "voip_stack.call_contact",
    CallContactAction,
    CONF_CONTACT,
    cv.string,
    cg.std_string,
    lambda var, value: var.set_contact(value),
)


@automation.register_action(
    "voip_stack.set_remote_endpoint",
    SetRemoteEndpointAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(VoipStack),
            cv.Required(CONF_IP): cv.templatable(cv.string),
            cv.Optional(CONF_PORT, default=5060): cv.templatable(cv.port),
            cv.Optional(CONF_RTP_PORT_ACTION, default=0): cv.templatable(cv.int_range(min=0, max=65535)),
        }
    ),
    synchronous=True,
)
async def set_remote_endpoint_action_to_code(config, action_id, template_arg, args):
    var = await _new_parented_action(config, action_id, template_arg)
    templ_ip = await cg.templatable(config[CONF_IP], args, cg.std_string)
    cg.add(var.set_ip(templ_ip))
    templ_port = await cg.templatable(config[CONF_PORT], args, cg.uint16)
    cg.add(var.set_port(templ_port))
    templ_rtp_port = await cg.templatable(config[CONF_RTP_PORT_ACTION], args, cg.uint16)
    cg.add(var.set_rtp_port(templ_rtp_port))
    return var


CONF_CALLER = "caller"


ADD_CONTACT_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(VoipStack),
        cv.Optional(CONF_ENTRY): cv.templatable(cv.string),
        cv.Optional(CONF_NAME): cv.templatable(cv.string),
        cv.Optional(CONF_IP): cv.templatable(cv.string),
        cv.Optional(CONF_PORT, default=5060): cv.templatable(cv.port),
        cv.Optional(CONF_RTP_PORT_ACTION, default=40000): cv.templatable(cv.port),
        cv.Optional(CONF_TRANSPORT): cv.templatable(cv.one_of(TRANSPORT_UDP, TRANSPORT_TCP, lower=True)),
    }
)


def _validate_add_contact_action(config):
    if CONF_ENTRY in config:
        return config
    if CONF_NAME not in config:
        raise cv.Invalid("voip_stack.add_contact requires either entry or name")
    return config


async def _add_contact_action_to_code(config, action_id, template_arg, args):
    var = await _new_parented_action(config, action_id, template_arg)
    if CONF_ENTRY in config:
        templ = await cg.templatable(config[CONF_ENTRY], args, cg.std_string)
        cg.add(var.set_entry(templ))
        return var
    templ_name = await cg.templatable(config[CONF_NAME], args, cg.std_string)
    cg.add(var.set_name(templ_name))
    if CONF_IP in config:
        templ_ip = await cg.templatable(config[CONF_IP], args, cg.std_string)
        cg.add(var.set_ip(templ_ip))
    templ_port = await cg.templatable(config[CONF_PORT], args, cg.uint16)
    cg.add(var.set_port(templ_port))
    templ_rtp_port = await cg.templatable(config[CONF_RTP_PORT_ACTION], args, cg.uint16)
    cg.add(var.set_rtp_port(templ_rtp_port))
    if CONF_TRANSPORT in config:
        templ_transport = await cg.templatable(config[CONF_TRANSPORT], args, cg.std_string)
        cg.add(var.set_sip_transport(templ_transport))
    return var


automation.register_action(
    "voip_stack.add_contact",
    AddContactAction,
    cv.All(ADD_CONTACT_SCHEMA, _validate_add_contact_action),
    synchronous=True,
)(_add_contact_action_to_code)

_register_templated_action(
    "voip_stack.remove_contact",
    RemoveContactAction,
    CONF_ENTRY,
    cv.string,
    cg.std_string,
    lambda var, value: var.set_entry(value),
)
_register_templated_action(
    "voip_stack.set_ha_peer_name",
    SetHaPeerNameAction,
    CONF_NAME,
    cv.string,
    cg.std_string,
    lambda var, value: var.set_name(value),
)


_register_simple_action("voip_stack.flush_contacts", FlushContactsAction)
_register_simple_action("voip_stack.update_contacts", UpdateContactsAction)

# === Condition registrations ===
# Simple condition schema that just references the voip_stack component
PHONE_CONDITION_SCHEMA = automation.maybe_simple_id(
    {
        cv.GenerateID(): cv.use_id(VoipStack),
    }
)


def _register_simple_condition(name, condition_class):
    """Register a parameter-less Parented<VoipStack> condition."""
    @automation.register_condition(name, condition_class, PHONE_CONDITION_SCHEMA)
    async def _to_code(config, condition_id, template_arg, args):
        var = cg.new_Pvariable(condition_id, template_arg)
        parent = await cg.get_variable(config[CONF_ID])
        cg.add(var.set_parent(parent))
        return var
    return _to_code


_register_simple_condition("voip_stack.is_idle", VoipIsIdleCondition)
_register_simple_condition("voip_stack.is_ringing", VoipIsRingingCondition)
_register_simple_condition("voip_stack.is_in_call", VoipIsInCallCondition)
_register_simple_condition("voip_stack.is_calling", VoipIsCallingCondition)
_register_simple_condition("voip_stack.is_incoming", VoipIsIncomingCondition)


CONF_DESTINATION = "destination"

@automation.register_condition(
    "voip_stack.destination_is",
    VoipDestinationIsCondition,
    automation.maybe_simple_id(
        {
            cv.GenerateID(): cv.use_id(VoipStack),
            cv.Required(CONF_DESTINATION): cv.templatable(cv.string),
        }
    ),
)
async def voip_destination_is_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    parent = await cg.get_variable(config[CONF_ID])
    cg.add(var.set_parent(parent))
    templ = await cg.templatable(config[CONF_DESTINATION], args, cg.std_string)
    cg.add(var.set_destination(templ))
    return var


_register_simple_condition("voip_stack.is_ha_destination", VoipIsHaDestinationCondition)
