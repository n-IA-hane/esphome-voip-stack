# ESP VoIP Stack 2026.11.1-dev

Changes since stable **2026.11.0**.

## New phone services

**Registered PBX accounts.** Connect an ESP to Asterisk, FreeSWITCH or another
compatible SIP registrar, alongside its direct route. Configure up to four
accounts in YAML or through native ESPHome actions. Accounts support UDP/TCP,
Digest authentication, automatic renewal and optional persistent overrides.
The default generic configuration remains direct SIP.

**Automatic Home Assistant identity.** Maintained HA phone packages opt in to
`ha_integration.auto_register`. The integration creates and provisions one `ha`
account; its credential also serves the optional LDAP and IMAP clients. The
friendly device name becomes caller display and contact label. Custom firmware
chooses this behavior explicitly.

**SIP BLF and MWI.** Watch other contacts' idle, ringing, active and held states,
perform directed pickup through the correct account, and receive mailbox waiting
counts. Both subscriptions share the existing SIP worker and standard Digest
handling. BLF preallocates 16 watchers by default, accepts a configurable limit
from 1 through 64, and supports PSRAM placement.

**LDAP directories.** The optional `ldap_directory` component imports stable
contact IDs, names, SIP destinations and categories into the existing bounded
phonebook. Updates preserve manual overrides and replace only their own provider.
Call and pickup actions can address a contact by stable ID.

**IMAP visual voicemail.** The optional `imap_voicemail` component provides a
paginated catalog, message metadata and native MediaSource playback. Read/delete
operations use UID and UIDVALIDITY; UIDPLUS deletion targets the selected
message. Use independent server credentials or share a configured SIP account.
The existing SIP/DTMF voicemail menu remains another access method.

[Phone services and examples](PHONE_SERVICES.md) ·
[PBX accounts](SIP_REGISTRATION.md) · [MWI](SIP_MWI.md)

## JPEG reception on ESP32-S3

`esp_video_renderer` decodes complete baseline JPEG frames into an LVGL surface.
It can receive through `voip_stack.video.sink` or work with a separate JPEG
producer. Bounded scaling fits larger images into the configured display;
standard SDP `imageattr` lets compatible senders resize before compression.
The renderer keeps the newest queued frame and releases display surfaces only
through its presentation handoff.

The coordinated 240x240 receive profile has a 10 FPS budget. Camera transmission
and display reception have been exercised separately on S3; simultaneous camera,
rendering and the complete FULL workload still need their own qualification.
The P4 hardware renderer remains a separate implementation. Tune image limits,
worker affinity and stack/buffer placement using the [renderer settings](ESP_VIDEO_RENDERER.md).

## Reliability and interoperability

- Managed account updates are idempotent. Real replacements wait for active call
  and REFER cleanup, retire the previous binding, and apply the latest request.
- OPTIONS reports configured audio/video capabilities without opening audio or
  reserving media ports. Each call's SDP exchange remains authoritative.
- Ordinary bidirectional SIP offers describe shared send/receive formats.
  Explicitly coordinated peers can negotiate directional PCM capabilities.
- Standard hold/resume updates media direction while retaining prepared audio.
- Subscription refresh, transport loss, partial BLF resynchronization and account
  replacement keep state scoped to their actual account and dialog generation.

## Migration

Update the coordinated HA integration before adopting managed phone services,
then rebuild with the current device package. The generic component keeps its
existing direct configuration; manual external PBX accounts remain selectable.
LDAP, IMAP, BLF and the JPEG renderer are optional additions.

The legacy endpoint capability sensor is now opt-in with
`ha_integration.endpoint: true`. Current HA discovers managed accounts and queries
SIP capabilities; older firmware discovery remains supported by the backend.
The sensor's 255-character validation applies only when that legacy surface is enabled.

For earlier preview configurations, replace
`ha_integration.message_waiting: true` with `ha_integration.auto_register: true`
(or an explicitly configured SIP account with `registration.message_waiting`).
Mailbox notifications now use standard SIP for both HA and conventional PBXs.
