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
account; its credential also serves the optional LDAP directory. The
friendly device name becomes caller display and contact label. Custom firmware
chooses this behavior explicitly.

**SIP BLF and MWI.** Watch other contacts' idle, calling, ringing, active and held states,
perform directed pickup through the correct account, and receive mailbox waiting
counts. Both subscriptions share the existing SIP worker and standard Digest
handling. BLF preallocates 16 watchers by default, accepts a configurable limit
from 1 through 64, and supports PSRAM placement.
Optional standard SIP presence adds online, offline and unknown availability,
independently of call activity. An idle dialog no longer needs to be interpreted
as proof that a phone is reachable.

**LDAP directories.** The optional `ldap_directory` component imports stable
contact IDs, names, SIP destinations and categories into the existing bounded
phonebook. Updates preserve manual overrides and replace only their own provider.
Call and pickup actions can address a contact by stable ID.

**Connected identity events.** Optional `on_peer_changed` automations receive the
connected-party name after a SIP identity update. `get_active_peer_name()` gives
custom screens the current call's remote name independently of phonebook
selection. Connection setup remains in `on_in_call`.

**SIP mailbox access.** `voip_stack.call_mailbox` opens the notified mailbox URI
or a configured PBX access code through the ordinary registered call path.
Aggregate waiting badges select an account with waiting messages. Audio, keypad,
hold and hangup use the normal phone controls. Negotiated RFC 4733 DTMF sends
keypad digits while audio continues in both directions, using the existing RTP
worker and a bounded command queue.

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

- Contact-based pickup uses the target's friendly name during setup and applies
  the PBX's connected identity after answer. Maintained screens refresh the call
  label and keypad when that identity arrives later in the same call.
- Idle keypads clear their editable number buffer after a call. Delete keeps
  an empty composer empty, ready for a new number.
- LDAP streams into a bounded staging table and commits directly into the
  existing phonebook. Reused storage removes transient merged-roster copies;
  signaling placement stays independent of realtime PCM staging.
- Retained directory text and subscription metadata follow the signaling PSRAM
  option. Typed contact queries use borrowed views; BLF and presence transactions
  carry only their own protocol state. Watches without application listeners
  keep their SIP state current without scheduling unused UI notifications.
- The new typed BLF getter returns a compact status snapshot for rendering.
  Callbacks retain owned identity metadata; repeated row queries avoid those
  string copies.
- Mailbox subscriptions retry temporarily missing resources with bounded
  backoff. Accepted REGISTER transactions renew MWI and BLF through the same
  subscription owner, retaining healthy dialogs and rebuilding missing ones.
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
LDAP, BLF, MWI and the JPEG renderer are optional additions.
Automatic HA registration also leaves MWI disabled until
`registration.message_waiting: true` is selected. Managed phones exclude the
legacy JSON roster parser by default; `ha_integration.legacy_roster: true`
keeps that API available for an existing configuration.

The legacy endpoint capability sensor is now opt-in with
`ha_integration.endpoint: true`. Current HA discovers managed accounts and queries
SIP capabilities; older firmware discovery remains supported by the backend.
The sensor's 255-character validation applies only when that legacy surface is enabled.

For earlier preview configurations, replace
`ha_integration.message_waiting: true` with `ha_integration.auto_register: true`
and `registration.message_waiting: true`, or use an explicitly configured SIP
account with message waiting enabled.
Mailbox notifications now use standard SIP for both HA and conventional PBXs.
