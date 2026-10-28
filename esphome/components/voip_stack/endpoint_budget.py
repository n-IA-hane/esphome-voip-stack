"""Validate the existing compact HA endpoint description before firmware build."""

import esphome.config_validation as cv

HA_STATE_CHARACTERS = 255
BUILDER_CAPACITY_BYTES = 640
MAX_IPV4_TEXT = "255.255.255.255"


def _formats(audio, direction):
    primary = audio[direction]
    if primary is None:
        return []
    result, seen = [], set()
    for item in (primary, *audio.get(direction + "_formats", [])):
        key = tuple(
            item[field]
            for field in ("sample_rate", "pcm_format", "channels", "frame_ms")
        ) + (item.get("codec", "pcm"),)
        if key not in seen:
            result.append(item)
            seen.add(key)
    return result


def _pcm(item):
    return f"{item['sample_rate']}:{item['pcm_format']}:{item['channels']}:{item['frame_ms']}"


def _rtp(item):
    if item.get("codec", "pcm") == "opus":
        return f"o/48/2/{item['frame_ms']}"
    wire_bytes = 2 if item["pcm_format"] == "s16le" else 3
    if (
        item["sample_rate"] * item["frame_ms"] // 1000 * item["channels"] * wire_bytes
        > 1200
    ):
        return ""
    encoding = {"s16le": "l", "s24le": "h", "s24le_in_s32": "h"}.get(item["pcm_format"])
    return (
        ""
        if encoding is None
        else f"{encoding}/{item['sample_rate'] // 1000}/{item['channels']}/{item['frame_ms']}"
    )


def endpoint_text(config, name, ipv4=MAX_IPV4_TEXT):
    """Match build_endpoint_string_ after audio auto-resolution and de-duplication."""
    tx, rx = (_formats(config["audio"], direction) for direction in ("tx", "rx"))
    capability = "full_duplex" if tx and rx else "mic_only" if tx else "speaker_only"
    codec_aware = any(item.get("codec", "pcm") != "pcm" for item in (*tx, *rx))
    tx_pcm = "" if codec_aware else ";".join(_pcm(item) for item in tx)
    rx_pcm = "" if codec_aware else ";".join(_pcm(item) for item in rx)
    fields = [
        name,
        ipv4,
        str(config["sip_port"]),
        str(config["rtp_port"]),
        capability,
        tx_pcm,
        rx_pcm,
        "sip_" + config["transport"],
        config["extension"].strip(" \t\r\n"),
    ]
    if codec_aware:
        for key, formats in (("at", tx), ("ar", rx)):
            tokens = ",".join(token for item in formats if (token := _rtp(item)))
            if tokens:
                fields.append(key + "=" + tokens)
    fields.append("sf=d1")
    if "video" in config:
        fields.append("video=" + config["video"]["codec"])
    return "|".join(fields)


def validate(config, name):
    if config.get("ha_integration") is False:
        return config
    text = endpoint_text(config, name)
    # Home Assistant counts Unicode characters; the C++ snprintf buffer counts
    # UTF-8 bytes. Both limits matter, and neither changes the declared formats.
    if len(text) > HA_STATE_CHARACTERS:
        raise cv.Invalid(
            f"VoIP HA endpoint announcement needs {len(text)} characters, above Home Assistant's 255-character limit "
            "(allowing a 15-character IPv4 address). Shorten esphome.friendly_name or voip_stack.extension, "
            "or use ha_integration: false for standalone SIP."
        )
    if len(text.encode("utf-8")) >= BUILDER_CAPACITY_BYTES:
        raise cv.Invalid(
            "VoIP HA endpoint announcement exceeds the firmware builder's 639-byte UTF-8 capacity"
        )
    return config


def extension_limits(config, name):
    """Return the extension's remaining character/byte budget without allocations at runtime."""
    if config.get("ha_integration") is False:
        return 32, 32
    base = dict(config, extension="")
    text = endpoint_text(base, name)
    return min(32, max(0, HA_STATE_CHARACTERS - len(text))), min(32, max(0, BUILDER_CAPACITY_BYTES - 1 - len(text.encode("utf-8"))))
