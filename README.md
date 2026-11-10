# ESPHome VoIP Stack

**ESPHome 2026.10.0 or newer is required.**

Use the official ESPHome audio, mixer, resampler, media player, Voice Assistant,
Micro Wake Word and OTA components. VoIP Stack does not require replacement
providers for those components. Audio Stack and Runtime Controller remain
optional external components for configurations that use them.

*Developed and maintained by n-IA-hane, with community contributions and support. [❤️ Sponsor the project](https://github.com/sponsors/n-IA-hane)*

Stable release: [2026.11.0](https://github.com/n-IA-hane/esphome-voip-stack/releases/tag/v2026.11.0).
See the [changes since 2026.10.1](CHANGELOG.md) and the
[upgrade steps](#codec-schema-migration).

A native SIP phone component for ESPHome. `voip_stack` turns an ESP32 device into a real SIP endpoint: SIP signaling over UDP or TCP, SDP offer/answer, RTP media, a local phonebook and dial plan, explicit per-direction PCM negotiation, and a full set of ESPHome entities, triggers, actions and conditions on top.

It binds to standard ESPHome `microphone` and `speaker` components, so the audio source can be anything from a bare I2S MEMS microphone to the echo-cancelled output of [`esphome-audio-stack`](https://github.com/n-IA-hane/esphome-audio-stack). The component is a complete SIP phone on its own; the optional Home Assistant integration from [`esphome-intercom`](https://github.com/n-IA-hane/esphome-intercom) adds central phonebook management, call routing, a softphone/B2BUA and a Lovelace card on top of the same devices.

## One VoIP stack per ESP

The component currently supports one `voip_stack` instance per ESP firmware.
Its switches, numbers, buttons, text entities and text sensors resolve that
instance automatically, so examples omit the optional `voip_stack_id` field.
Keep the stack's explicit `id`, such as `phone`, when actions or lambdas refer
to it. Microphone and speaker IDs still select the actual audio endpoints and
must be kept where needed.

## What this is

ESPHome has excellent building blocks for voice devices, but no native way to make two devices call each other or carry a two-way conversation with a phone, PBX extension, Home Assistant dashboard, or another ESP. `voip_stack` fills that layer with the protocol the rest of the telephony world already speaks: SIP for call control, SDP for capability negotiation, RTP for media.

```text
   ESPHome microphone / microphone_source        ESPHome speaker
   (native i2s_audio, esp_audio_stack,           (native i2s_audio,
    any processed source)                         esp_audio_stack, mixer...)
                 \                                   /
                  \                                 /
                   +--------- voip_stack ----------+
                   |  SIP signaling (UDP or TCP)   |
                   |  SDP offer/answer             |
                   |  RTP media (always UDP)       |
                   |  jitter buffer                |
                   |  call finite-state machine    |
                   |  phonebook / dial plan        |
                   |  endpoint identity publishing |
                   +-------------------------------+
                     |            |            |
                 triggers      actions      entities
```

Two scope decisions shape the component:

**`voip_stack` does not own audio hardware or echo cancellation.** It consumes microphone and speaker interfaces. When software AEC/AFE is needed, [`esp_audio_stack`](https://github.com/n-IA-hane/esphome-audio-stack) owns the audio graph and hands `voip_stack` a clean post-processor microphone. Legacy processing options on `voip_stack` are intentionally gone.

**Direct SIP and optional PBX accounts.** ESP devices can call each other and Home Assistant directly. An optional `registration` block adds bounded PBX accounts using the same SIP and media owners. Provider trunks and central routing remain available through Home Assistant.

## What it gives you

**A real SIP endpoint.** INVITE/ACK/BYE over UDP or TCP signaling, SDP offer/answer, RTP media that stays UDP in both signaling modes, and a jitter buffer on the receive path.

**Three supported media roles, inferred from YAML.**

| YAML wiring | Published mode | Runtime behavior |
|---|---|---|
| microphone + speaker | `full_duplex` | Sends mic audio, plays remote audio. |
| microphone only | `mic_only` | Sends mic audio; incoming audio is ignored. |
| speaker only | `speaker_only` | Plays incoming audio; no TX task is created. |

Mic-only and speaker-only are not degraded modes. They exist for paging speakers, monitor devices, split installations, and hardware that physically has one direction.

**Explicit per-direction PCM negotiation.** TX and RX formats are declared independently: sample rate from 8000 to 48000 Hz, RTP-mappable PCM format `s16le`/`s24le`/`s24le_in_s32`, 1 or 2 channels, packet time 10/16/20/32 ms. Devices advertise their real capabilities instead of pretending everything is 8 kHz telephony audio.

**Explicit codec declarations.** `audio.codecs` declares each supported codec and its ordered TX/RX formats. Validation checks those PCM boundaries against the microphone and speaker. Maintained packages supply the declarations, so their defaults need no user edits.

**A phonebook that is a dial plan, not a gatekeeper.** Contacts define where outbound calls go. Inbound calls are not rejected just because the caller is unknown; the INVITE carries caller and destination identity, and HA-side resolution handles routed/NAT installs.

**Self-describing devices.** The component publishes text sensors for state, transport, endpoint identity, destination, caller, contacts, SIP snapshot and last terminal reason.

**Honest failure taxonomy.** The FSM distinguishes `BUSY`, `DECLINED`, `CANCELLED`, `MEDIA_INCOMPATIBLE`, `TRANSPORT_UNREACHABLE` and `AUTH_REQUIRED_UNSUPPORTED`.

**Compile-time pruning.** A speaker-only build contains no VoIP TX path and a mic-only build contains no RX-to-speaker path.

**Optional video.** Native ESP32-S3 cameras can transmit JPEG during calls. Supported S3 LVGL displays can receive JPEG through the separate `esp_video_renderer` sink adapter. Camera and receive-display roles are independent; S3 does not decode H.264. ESP32-P4 adapters support camera transmission and remote-video decode/display. JPEG and H.264 remain separate compile-time profiles. The camera component owns capture and borrowed frames, the codec-specific source owns encoding or compressed-frame forwarding, and `p4_video_renderer` owns receive decode and panel presentation. Audio-only builds do not include these components or their codec dependencies.

**Full ESPHome automation surface.** Triggers, actions and conditions are normal ESPHome automations.

## Native media discovery and keepalive

An OPTIONS request can read the configured audio capabilities without opening a
call or acquiring audio hardware. The SDP uses audio port zero and advertises
the configured codecs and directions. It includes a packet-time hint only when
all described directions support that value. A busy endpoint replies 486 to an
out-of-dialog request; a matching in-dialog request replies 200 and preserves
the call.

Registered accounts query peer capabilities after a new binding. Discovery is
advisory: calls can start immediately with ordinary SDP, including when OPTIONS
is rejected or unanswered. Directional-audio support comes from the peer's SDP,
and expires with the binding. It is never inferred from an account or device
name. The INVITE answer remains authoritative for rates, payloads and framing.

`registration.keepalive_interval` defaults to `15s`; `0` disables periodic
OPTIONS. Each ping reuses the registered UDP socket or TCP flow and does not
renew the binding or use Digest credentials. A matching final reply, including
401 or 405, proves the signaling flow answered. A timeout or changed observed
mapping starts a REGISTER refresh. This is ordinary SIP OPTIONS reachability,
without RFC 5626 outbound negotiation or an RTP NAT keepalive guarantee.

Before sending an original INVITE, the endpoint binds its advertised audio RTP
port. Final media preparation adopts that socket, retaining packets received
before the answer. Cancellation, rejection and timeout release it through the
existing media cleanup barrier. Decoders and audio devices still start only
after negotiation. Packet times and alternatives come from the declared codec
lists. Maintained FULL packages prefer 16 kHz/20 ms TX and 48 kHz/10 ms RX.

## Blind audio call transfer

An established audio call over UDP or TCP can receive a blind SIP REFER from a PBX
or another SIP phone. The ESP calls the supplied IPv4 SIP destination and
reports the outcome with NOTIFY (`message/sipfrag`). The existing conversation
continues while the destination rings. A rejection or failed attempt leaves
that conversation available; a successful answer switches to the new peer.

The transfer reuses the SIP task, SDP negotiation and audio path. During a TCP
handoff, each dialog owns its own bounded signalling flow; no extra SIP task
or media engine is started. Completed UDP transactions retain bounded replay
history; completed reliable transactions do not occupy those replay slots.
Receive-rate conversion uses the existing PCM converter, prepared before the
handoff. The established transmit format and speaker format remain unchanged.

This receiver supports **in-dialog blind transfers of UDP or TCP audio calls
to IPv4 SIP addresses**, including a change of signalling transport. Video
transfers, hostname targets and attended-transfer URI headers remain unsupported.
Unsupported requests are rejected without replacing the current conversation.

The replacement offer retains the active transmit codec and packet time. On
Spotpear with a 16 kHz microphone, the tested replacement dialog uses L16/16000
in both directions. Ordinary calls can still negotiate 48 kHz receive and
16 kHz transmit; that asymmetric combination is not currently offered by the
REFER replacement path.

A remote BYE ends the original conversation without cancelling an accepted
transfer. The ESP continues calling the destination and reports the result.
Hanging up on the ESP itself cancels the pending transfer. Authentication challenges (401/407) fail the
transfer explicitly: this receiver does not retry with SIP Digest credentials. A destination failure
preserves the original conversation only if its caller has not already hung up.

Rebuild the ESP with the 2026.11.0 component to use this capability.
Updating the Home Assistant integration alone does not update ESP firmware.
The Spotpear Ball V2 FULL profile has been checked with Waveshare S3 Audio,
including an occupied destination, immediate retry and hangup from both ends.
The TCP-capable candidate was also tested on Spotpear Ball V2 FULL with real HA
browser phones: all four UDP/TCP combinations, busy and connection-refused
responses, timeout, caller BYE during transfer, local cancellation, immediate
redial and parking/retrieval. SIP captures and decoded RTP were checked.
Other hardware and PBX combinations still need their own runtime tests.

## Scenarios

| Goal | Shape |
|---|---|
| Two ESP intercoms calling each other on a LAN, no server | `voip_stack` + static contacts, direct SIP/UDP |
| Doorbell that rings phones and HA dashboards | `voip_stack` + the HA integration from `esphome-intercom` |
| Voice assistant device that is also an intercom | `esp_audio_stack` + `voip_stack` |
| Paging speaker in a workshop | speaker-only `voip_stack` |
| Baby monitor / listen-in device | mic-only `voip_stack` |
| PBX/provider trunk reaching ESP devices | HA terminates the trunk and routes; ESPs stay plain SIP peers |
| Offline installation with a fixed dial plan | `static_contacts`, no HA required |

## Core concepts

**Endpoint.** The published identity of a device: name, optional host/IP, SIP port, RTP port, signaling transport, capability mode, and advertised TX/RX PCM capabilities. The `endpoint` text sensor is the authoritative record Home Assistant consumes.

**Capability mode.** Derived from wiring. It gates task creation at runtime and code inclusion at compile time.

**Phonebook.** The outbound dial plan: an ordered list of contacts the device can call, navigated by `next_contact`/`prev_contact` or addressed by name. Sources are static YAML contacts, runtime actions, and the HA-managed roster.

**Audio format contract.** Each codec's `tx` list describes what the device sends, and its `rx` list what it accepts. Packet times are independent. Missing codec declarations are configuration errors.

**Call FSM.** One explicit state machine drives the lifecycle. Every state is observable through the `state` text sensor, triggers and conditions. Terminal outcomes are published through `last_reason`.

## Installation

```yaml
external_components:
  - source: github://n-IA-hane/esphome-voip-stack@dev
    components: [voip_stack]
```

This release requires ESPHome 2026.10.0 or newer.

Requirements: ESP-IDF framework. PSRAM is recommended for full-duplex profiles and required by co-resident AEC/AFE processing. RTP media requires UDP reachability between peers in both signaling modes.

When pairing with the audio stack, pull both:

```yaml
external_components:
  - source: github://n-IA-hane/esphome-audio-stack@dev
    components: [esp_audio_stack, esp_aec]
  - source: github://n-IA-hane/esphome-voip-stack@dev
    components: [voip_stack]
```

### Codec schema migration

Custom configurations now declare `audio.codecs` explicitly. Move their primary
formats and alternatives into each codec's ordered `tx` and `rx` lists. The old
`audio.tx/rx` and `tx_formats/rx_formats` syntax is rejected. Maintained packages
already supply the declarations. Follow the
[migration guide](https://github.com/n-IA-hane/esphome-intercom/blob/dev/docs/ESP_AUDIO_CODECS.md#migrate-an-existing-custom-yaml).

### Earlier upgrades from 2026.10.1

- Update the builder to **ESPHome 2026.10.0 or newer**, then rebuild and upload.
- If custom S3 VoIP-only YAML supplies multiple Opus formats, keep their decoded PCM sample
  rate, sample format and channel count identical within each direction. When
  Opus is declared for RX, all receive formats must share its decoded PCM
  geometry. Packet times may differ. This Opus restriction does not apply to
  mixed PCM/G.722 receive-rate conversion without Opus.
- With native HA integration enabled, remove manually declared API actions
  named `start_call_via_ha` or `hangup_call_if_id`; the component provides them.
  Existing `start_call`, answer and hangup action names remain valid.

Microphone-only and speaker-only configurations now advertise their actual SDP
direction. Settings for absent audio hardware remain accepted and are ignored.
The older migration from copied HA packages to native discovery already shipped
in 2026.10.0; it is not another required migration for an up-to-date 2026.10.1 YAML.

### Add a custom ESP phone to Home Assistant

With ESPHome 2026.10.0 or newer, `voip_stack` provides discovery entities,
call-control actions and HA phonebook reception directly. No VoIP HA package
is needed. Enable native service registration alongside any API encryption:

```yaml
api:
  custom_services: true
```

Keep your microphone and/or speaker configuration. The component works with
native ESPHome audio as well as Audio Stack. HA integration is enabled when
API is configured; use `voip_stack.ha_integration: false` for standalone SIP.
Routing through HA remains controlled separately by `use_ha_as_first_contact`.

Automatic discovery exposes the short **VoIP State** entity. SIP Contact and SDP
provide routing and media capabilities; **VoIP Endpoint** is not generated
automatically. Older consumers can explicitly retain it alongside native HA:

```yaml
text_sensor:
  - platform: voip_stack
    type: endpoint
    name: Legacy VoIP Endpoint
```

Update the HA integration to support state-based discovery before removing an
existing legacy endpoint entity.

Managed SIP registration with Home Assistant is optional:

```yaml
voip_stack:
  ha_integration:
    auto_register: true
```

The existing ESPHome API connection exposes real `set_ha_display_name` and
`set_account` actions for HA provisioning. HA supplies account `ha`, its
credentials and registrar; `transport: auto` follows the configured SIP UDP/TCP
transport. This enables registration with default line `ha` and reserves one
account slot. Manual PBX accounts can coexist within `registration.max_accounts`
(default 2, maximum 4); they use another account ID and explicit UDP/TCP.
An explicit `registration.default_line` still selects the default outgoing line.
`start_call_on_line` can select a manual account for an individual call.

Identical provisioning keeps the current binding and call. Changes wait for the
current call and the old registration transaction before applying the latest
configuration through the existing registration owner. The HA display name
changes the outgoing caller name while preserving the device's routing identity.
Disconnecting the native API does not remove the SIP account.

Omitting `auto_register`, or setting it to `false`, preserves direct/phonebook
behavior and optional manual PBX registration. Without a `registration` section,
the registration engine and its saved-account pool are compiled out. Managed
registration adds no audio worker, media buffer or dependency on LDAP, BLF or an
ESP mailbox client.

### Per-call paging answer mode

Paging uses the standard SIP `Answer-Mode` header from
[RFC 5373](https://www.rfc-editor.org/rfc/rfc5373.html). An authorized
`Answer-Mode: Auto` request can answer that call without changing the persistent
**Auto Answer** switch. A `Manual` request, including `Manual;require`, waits for
local acceptance even when that switch is on. Unknown mode tokens and extension
parameters are ignored. An unauthorized `Auto;require` request is rejected with
403; ordinary `Auto` falls back to the existing local answer policy. Busy and
Do Not Disturb checks still apply.

The managed HA account established by `ha_integration.auto_register: true`
allows paging from its matched registrar peer. Manual PBX accounts must opt in:

```yaml
voip_stack:
  registration:
    accounts:
      - account_id: office
        registrar: pbx.example.org
        username: "100"
        password: !secret pbx_password
        allow_paging_auto_answer: true
```

`registration.accounts[].allow_paging_auto_answer` defaults to `false`. The same
account field is supported by the YAML `voip_stack.set_account` action. Account
matching includes the registrar's bound signaling peer; a caller-supplied name
alone does not grant paging access.

For direct SIP without a matched account, opt in specific source IPv4 addresses:

```yaml
voip_stack:
  paging_auto_answer_peers: [192.0.2.20]
```

This list is empty by default and retains at most 16 distinct addresses. It does
not override a matched PBX account whose paging permission is disabled.
`Priv-Answer-Mode` and vendor-specific auto-answer headers do not grant paging
permission. Answer mode controls acceptance only; the negotiated SDP direction
still determines whether the microphone transmits.

For an HA action that selects an outgoing trunk for one call, the native API also
provides `start_call_via_ha(dest, call_id)` and `hangup_call_if_id(call_id)`.
The HA integration supplies a fresh SIP Call-ID, binds its selected line to that
call and routes the request through the configured HA peer. Scoped cancellation
cannot hang up a later call with another identity. Ordinary `start_call(dest)`
keeps its existing phonebook/direct-call behavior when managed registration is
disabled. With `auto_register: true`, HA-correlated calls use the managed `ha`
account while retaining the supplied Call-ID.

These additional actions are compiled with the HA API adapter. They add no audio
task, audio buffer or per-frame processing. The requested identity is passed
through the existing call setup; there is no stored override for a future call.
Call-IDs supplied through this API must be globally unique and case-sensitive,
with at most 128 ASCII letters, digits, dots, underscores, hyphens and one optional
`@` separator between non-empty parts. Incoming SIP parsing is unchanged.

See the [entity and migration guide](https://github.com/n-IA-hane/esphome-intercom/blob/main/docs/ESP_ENTITY_SURFACE.md)
for the retired packages and entity customization. Old package/action
declarations are rejected rather than silently creating duplicate controls.

The standard [LDAP directory](docs/LDAP_DIRECTORY.md) refreshes the existing
phonebook atomically, preserves manual contacts and shares contact metadata with
BLF. It is optional for static or legacy roster installations.

## Audio wiring

### Standalone native ESPHome audio

Use native ESPHome `microphone` and `speaker` components when the hardware already returns processed audio, or for simple full-duplex tests:

```yaml
voip_stack:
  id: phone
  microphone_source:
    microphone: native_mic
    bits_per_sample: 16
    channels: [0]
  speaker: native_speaker
  audio:
    codecs:
      - codec: pcm
        tx:
          - { sample_rate: 48000, pcm_format: s16le, channels: 1, frame_ms: 10 }
        rx:
          - { sample_rate: 48000, pcm_format: s16le, channels: 1, frame_ms: 10 }
```

Use `microphone_source:` when the ESPHome microphone is wider than the VoIP
stream and `voip_stack` must select a channel or bit depth. In the example
above the I2S microphone is 32-bit stereo, while the SIP/RTP TX stream is
16-bit mono, so `microphone_source` selects one channel and converts the
sample width before VoIP sends it.

Use `microphone:` when the referenced microphone already exposes the exact PCM
stream you declare in the applicable `audio.codecs[].tx` list. This is the normal shape with
`esp_audio_stack`, because its microphone platform already publishes the
post-AEC/AFE clean microphone stream.

### Audio stack facade

This is the maintained path when software AEC/AFE, media player, Voice Assistant, Micro Wake Word or multiple consumers share the backend:

```yaml
external_components:
  - source: github://n-IA-hane/esphome-audio-stack@dev
    components: [esp_audio_stack, esp_aec]
  - source: github://n-IA-hane/esphome-voip-stack@dev
    components: [voip_stack]

esp_aec:
  id: aec_processor
  mode: sr_low_cost

esp_audio_stack:
  id: audio_stack
  processor_id: aec_processor
  sample_rate: 48000
  output_sample_rate: 16000

microphone:
  - platform: esp_audio_stack
    id: clean_mic

speaker:
  - platform: esp_audio_stack
    id: stack_speaker

voip_stack:
  id: phone
  microphone: clean_mic
  speaker: stack_speaker
  audio:
    codecs:
      - codec: pcm
        tx:
          - { sample_rate: 16000, frame_ms: 20 }
          - { sample_rate: 16000, frame_ms: 10 }
        rx:
          - { sample_rate: 48000, frame_ms: 10 }
          - { sample_rate: 16000, frame_ms: 20 }
          - { sample_rate: 16000, frame_ms: 10 }
```

Audio Stack currently supports one instance per ESP. Its microphone and speaker
bind to it automatically; `esp_audio_stack_id` can be omitted.


During a call, the far end hears near-end speech after echo cancellation. Wake word keeps working locally because the same clean stream feeds it.

For the full Espressif AFE pipeline, import `esp_afe` instead of `esp_aec` and
point `processor_id` at that processor:

```yaml
external_components:
  - source: github://n-IA-hane/esphome-audio-stack@dev
    components: [esp_audio_stack, esp_afe]
  - source: github://n-IA-hane/esphome-voip-stack@dev
    components: [voip_stack]

esp_afe:
  id: afe_processor
  type: fd
  mode: high_perf

esp_audio_stack:
  id: audio_stack
  processor_id: afe_processor
  sample_rate: 48000
  output_sample_rate: 16000
```

### Microphone-only and speaker-only

```yaml
# Paging speaker
voip_stack:
  id: pager
  speaker: local_speaker

# Listen-in device
voip_stack:
  id: monitor
  microphone: processed_mic
```

## Audio format negotiation

Per-direction format blocks accept `auto` per field or as the whole block:

| Field | Values | Default |
|---|---|---|
| `sample_rate` | Codec-supported local PCM rate | required |
| `pcm_format` | `s16le`, `s24le`, `s24le_in_s32` for applicable PCM profiles | `s16le` |
| `channels` | Codec-supported channel count | `1` |
| `frame_ms` | PCM 10/16/20/32; G.722 and Opus 10/20 | required |

`audio.codecs` is required and contains one object per declared codec. Each
object has ordered `tx` and `rx` format lists. `sample_rate` and `frame_ms` are
explicit; `pcm_format` defaults to `s16le`, and `channels` to 1. Up to eight
formats are allowed per direction across all codecs. Only declared codecs and formats are enabled.
The old `audio.tx/rx` and `tx_formats/rx_formats` syntax is rejected with
migration instructions.

Transmit and receive packetization are negotiated independently. Local SDP
`ptime` describes our receive preference; the remote value guides transmission.
The remote `maxptime` bounds transmission to declared formats within that limit.
The maintained FULL packages prefer PCM TX at 16 kHz/20 ms and RX at 48 kHz/10 ms,
with explicit compatible alternatives. RX at 16 kHz must not displace the
preferred 48 kHz HA-to-ESP stream when it is supported.

See the [codec and migration guide](https://github.com/n-IA-hane/esphome-intercom/blob/dev/docs/ESP_AUDIO_CODECS.md)
for complete PCM, G.722 and Opus examples and the distinction between local
PCM rate and RTP clock rate.

For a conventional PBX account, use optional SIP REGISTER. PCM remains the
default; declare G.722 alongside it when the PBX permits that codec. See the
[registration guide](https://github.com/n-IA-hane/esphome-intercom/blob/dev/docs/ESP_REGISTRATION.md)
for accounts and the
[classic PBX guide](https://github.com/n-IA-hane/esphome-intercom/blob/dev/docs/CLASSIC_PBX.md)
for the separate legacy static L16 examples.

RTP packet sizes are guarded by `udp_max_payload` (default 1200 bytes), enforced at validation.

## Transport

SIP is the only call-control protocol. `transport` selects the SIP signaling transport; RTP media remains UDP in both modes:

```yaml
voip_stack:
  id: phone
  transport: tcp        # or udp
  sip_port: 5060
  rtp_port: 40000
```

TCP signaling is useful where UDP SIP is filtered or unreliable; per-contact
`transport` allows mixing.

SIP/UDP also requires IPv4 reassembly when a peer's request exceeds the link
MTU. The component enables `CONFIG_LWIP_IP4_REASSEMBLY` in ESP-IDF builds for
both direct and registered calls. Without it, a fragmented INVITE can be
discarded before the SIP parser even though REGISTER succeeds. Rebuild the
firmware to apply this SDK configuration.

Reassembly retains ESP-IDF's bounded default pool, with 10 pending pbufs in
[ESP-IDF 5.5.6](https://github.com/espressif/esp-idf/blob/v5.5.6/components/lwip/Kconfig).
This does not increase the 2047-byte SIP/UDP receive limit or enable IPv6
reassembly. Larger messages still require an appropriate transport and message
size; see [RFC 3261 section 18.1.1](https://www.rfc-editor.org/rfc/rfc3261.html#section-18.1.1).

## Phonebook and routing

```yaml
voip_stack:
  id: phone
  static_contacts:
    - name: Workshop
      ip: 192.168.1.44
      port: 5060
      rtp_port: 40000
      transport: udp
    - name: Front Gate
```

`name` is required; everything else is optional. Static contacts serve offline installs, diagnostics and direct SIP peers. Normal installs receive the HA-managed roster through the native `set_roster_json` action.

Inbound INVITEs carry caller and destination identity. Direct SIP remains enabled
by default. Optional [registration](esphome/components/voip_stack/REGISTRATION.md)
adds independent PBX accounts and a selected outgoing line; it does not replace
Home Assistant's trunk settings. In calls routed through HA, trunk numbers and
inbound DTMF routes are mapped by HA to local phonebook targets.

ESP devices can also publish optional HA-managed group membership:

```yaml
voip_stack:
  id: phone
  conference_groups: CG Home, CG Workshop
  conference_ring: true
  ring_groups: RG Home

text:
  - platform: voip_stack
    type: ring_groups
    name: VoIP Ring Groups
  - platform: voip_stack
    type: conference_groups
    name: VoIP Conference Groups

switch:
  - platform: voip_stack
    conference_ring:
      name: VoIP Ring On Conference
```

`conference_groups` and `ring_groups` accept one name or a comma-separated list. `conference_groups` means calling the group joins an HA-mixed conference immediately. `conference_ring` makes this device ring when another member starts one of its conference groups; with it disabled, the device can still join by calling the conference contact manually. `ring_groups` means calling the group makes all members ring and the first answer wins. All group routing is handled by the Home Assistant `voip_stack` integration; from the ESP point of view the group is just another phonebook contact. They require the HA integration release that documents group routing support.

The ESP component does not implement a PBX. It only publishes local membership
metadata through dedicated HA entities and calls the resulting phonebook
contact by name. Home Assistant materializes the group entries, performs SIP
forking for ring groups, mixes conference rooms, and pushes the updated roster
back to the ESP devices.

Queue declarations use the same per-phone ownership as ring groups:

```yaml
voip_stack:
  queues: "Support, 700"
```

With native HA integration enabled, **VoIP Queues** is an editable configuration
text. `ha_integration: { queues: false }` omits that entity. With
`ha_integration: false`, an optional manual text uses `platform: voip_stack`,
`type: queues` and a name, just like the group texts above. Queue names are
limited to 32 UTF-8 bytes each, with a 240-byte comma-separated list. Their
persisted value uses a separate preference key, preserving existing routing
identity and group preferences.

HA owns queue devices, routing and each agent's persistent available, paused or
logged-out state. Membership updates do not re-register the SIP account. The HA
editor can queue changes while an ESP is offline; a stale reconnect report does
not overwrite pending HA changes. See the
[HA group and queue guide](https://github.com/n-IA-hane/esphome-intercom/blob/dev/docs/GROUPS_AND_QUEUES.md)
for central member editing, manual and automatic group lifetime, services and
queue login/pause BLF keys. This requires the matching HA integration version.

### Home Assistant inbound routing

Provider/PBX trunks and automation routing belong to the optional Home
Assistant integration, never to ESP firmware. That integration keeps the same
shared phonebook as its canonical dial plan and offers two explicit trunk
inbound modes:

- **Direct** immediately follows the configured default phonebook target.
- **DTMF** answers the trunk leg and lets explicit digits select a phonebook
  extension; no digits use the configured default target.

An unknown explicit extension fails instead of falling through to another
destination. A separate experimental switch may expose a short Home Assistant
automation decision before Direct routing or the no-digits DTMF fallback.
Explicit DTMF digits always keep priority, and disabling that switch restores
the ordinary phonebook route.

After HA starts ringing, an ordinary HA state trigger with `for:` can redirect
the same unanswered call to another phonebook destination such as an ESP,
registered phone or Assist. The ESP remains a normal SIP peer and contains no
parallel automation dial plan.

See the main project's
[Automation Dial Plan](https://github.com/n-IA-hane/esphome-intercom/blob/main/docs/AUTOMATION_DIALPLAN.md)
guide for the exact config-flow semantics and native HA automation examples.

## Call lifecycle, triggers and conditions

FSM states: `IDLE`, `CALLING`, `REMOTE_RINGING`, `RINGING`, `CONNECTING`, `IN_CALL`, `TERMINATING`, and terminal outcomes `BUSY`, `DECLINED`, `CANCELLED`, `MEDIA_INCOMPATIBLE`, `TRANSPORT_UNREACHABLE`, `AUTH_REQUIRED_UNSUPPORTED`.

Triggers:

| Trigger | Fires |
|---|---|
| `on_ringing` | Inbound call starts ringing locally; provides `peer`. |
| `on_calling` | Outbound call enters `CALLING`; provides `peer`. |
| `on_dest_ringing` | Remote side reports ringing; provides `peer`. |
| `on_in_call` | Media is established; provides `peer`. |
| `on_video_start` | Negotiated video media becomes active. Audio-only calls do not fire it. |
| `on_video_end` | Active video media ends or is removed from an established call. |
| `on_hangup` | Call ends normally; provides `peer` and `reason`. |
| `on_call_failed` | Call ends in a failure terminal state; provides `peer` and `reason`. |
| `on_idle` | FSM returns to idle. |
| `on_incoming_call` | Inbound INVITE parsed; provides `call_id`, `caller`, `callee`, `uri`. |
| `on_outgoing_call` | Outbound call placed. |
| `on_bridge_request` | An ESP-originated route explicitly targets the HA peer. |
| `on_destination_changed` | Selected destination changed; provides `destination`. |
| `on_phonebook_update` | Contact list changed; provides `destination`. |

Version 2026.10.1 adds `voip_stack.is_active`, which is true during call setup,
ringing and an established call. It is false during termination and terminal
states. Use `voip_stack.is_idle` when you specifically need the idle state.
Both conditions resolve the single stack automatically when no ID is supplied.

Conditions: `voip_stack.is_active`, `voip_stack.is_idle`, `voip_stack.is_ringing`,
`voip_stack.is_in_call`, `voip_stack.is_calling`, `voip_stack.is_incoming`,
`voip_stack.destination_is`, and `voip_stack.is_ha_destination`.

## Actions

Call control:

| Action | Meaning |
|---|---|
| `voip_stack.start` | Call the selected destination. |
| `voip_stack.stop` | Hang up / cancel. |
| `voip_stack.call_toggle` | Start when idle, stop otherwise. |
| `voip_stack.answer_call` | Answer ringing inbound call. |
| `voip_stack.decline_call` | Decline with optional reason. |
| `voip_stack.call` | Dial a destination string. Local phonebook matches go direct; unresolved targets route through HA. |
| `voip_stack.set_remote_endpoint` | Point the next call at an explicit address. |

Phonebook actions: `voip_stack.next_contact`, `voip_stack.prev_contact`,
`voip_stack.add_contact`, `voip_stack.remove_contact`,
`voip_stack.set_contact`, `voip_stack.set_contacts`,
`voip_stack.flush_contacts`, `voip_stack.update_contacts`,
`voip_stack.set_roster_json`, and `voip_stack.set_ha_peer_name`.

Audio and diagnostic actions: `voip_stack.set_volume`,
`voip_stack.set_mic_gain_db`, and `voip_stack.publish_entity_states`.

## Entities

`voip_stack:` alone enables the local SIP/RTP engine. HA discovery, card mirror
state, editable groups and diagnostics are exposed by declaring the native
platform entities you need:

| Platform | Type | Content |
|---|---|---|
| `text_sensor` | `endpoint` | Short endpoint identity record used by HA phonebook discovery. |
| `text_sensor` | `state` | Current FSM state. |
| `text_sensor` | `caller` | Current/last caller identity. |
| `text_sensor` | `destination` | Currently selected dial-plan target. |
| `text_sensor` | `last_reason` | Last terminal outcome. |
| `text_sensor` | `contacts` | Published local contact list. |
| `text_sensor` | `transport` | Active signaling transport. |
| `text_sensor` | `sip_snapshot` | Diagnostic snapshot of the SIP layer. |
| `text` | `ring_groups` | Editable comma-separated ring group memberships. |
| `text` | `queues` | Editable comma-separated queue declarations; HA owns agent login and pause. |
| `text` | `conference_groups` | Editable comma-separated conference group memberships. |
| `switch` | `conference_ring` | Ring this endpoint when another participant starts one of its conference groups. |

The following manual surface is for configurations with automatic HA integration
disabled (`ha_integration: false`). With native HA enabled, its state and routing
entities already exist; only optional legacy or diagnostic sensors need an
explicit declaration:

```yaml
text_sensor:
  - platform: voip_stack
    type: endpoint
    name: VoIP Endpoint
  - platform: voip_stack
    type: state
    name: VoIP State
  - platform: voip_stack
    type: caller
    name: VoIP Caller
  - platform: voip_stack
    type: destination
    name: VoIP Destination
  - platform: voip_stack
    type: last_reason
    name: VoIP Last Reason
  - platform: voip_stack
    type: contacts
    name: VoIP Contacts

text:
  - platform: voip_stack
    type: ring_groups
    name: VoIP Ring Groups
  - platform: voip_stack
    type: conference_groups
    name: VoIP Conference Groups

switch:
  - platform: voip_stack
    conference_ring:
      name: VoIP Ring On Conference
```

Declared entities:

```yaml
switch:
  - platform: voip_stack
    active: { name: Call Active }
    auto_answer: { name: Auto Answer }
    dnd: { name: Do Not Disturb }

number:
  - platform: voip_stack
    master_volume: { name: Call Volume }
    mic_gain: { name: Call Mic Gain }

button:
  - platform: voip_stack
    call: { name: Call }
    next_contact: { name: Next Contact }
    previous_contact: { name: Previous Contact }
    decline: { name: Decline }
```

## Configuration reference

| Option | Default | Meaning |
|---|---|---|
| `transport` | `udp` | SIP signaling transport, `udp` or `tcp`. RTP is always UDP. |
| `sip_port` | `5060` | Local SIP port. |
| `rtp_port` | `40000` | Local RTP port. |
| `udp_max_payload` | `1200` | RTP payload budget. |
| `microphone` / `microphone_source` | none | TX audio source. |
| `speaker` | none | RX audio sink. |
| `audio.codecs` | required | Ordered declared codecs with separate ordered TX/RX format lists. |
| `static_contacts` | `[]` | YAML dial plan. |
| `extension` | `""` | Local SIP extension/user part. |
| `conference_groups` | `""` | Optional HA-managed conference group membership, single name or comma-separated list. |
| `conference_ring` | `false` | Ring this device when another member starts its conference group. |
| `ring_groups` | `""` | Optional HA-managed ring group membership, single name or comma-separated list. |
| `queues` | empty | Optional HA queue declarations, single name or comma-separated list. |
| `ha_integration.queues` | enabled with HA | Queue configuration text; `false` omits it. |
| `paging_auto_answer_peers` | `[]` | Trusted source IPv4 addresses for per-call direct paging; at most 16 retained. |
| `registration.accounts[].allow_paging_auto_answer` | `false` | Allow per-call paging from that matched manual PBX account. |
| `use_ha_as_first_contact` | `false` | Pin the HA peer at the top of the dial plan. |
| `ha_phonebook_text_sensor_id` | none | Bind the HA phonebook sensor. |
| `delete_contact_missing_from` | none | Drop absent roster contact after N updates. |
| `ringing_timeout` / `calling_timeout` | none | Guard timers. |
| `dc_offset_removal` | `false` | Remove DC bias on TX. |
| `audio_debug` | `false` | Verbose audio-path diagnostics. |
| `buffers_in_psram` | `false` | Place VoIP-owned buffers in PSRAM. |
| `task_stacks_in_psram` | `false` | Place signaling and media-transport task stacks in PSRAM where supported. |
| `audio_task_stacks_in_psram` | `false` | Independently place the realtime `voip_tx`/`voip_rx` task stacks in PSRAM. |
| `tx_task_stack_size` | `12288` | Realtime transmit task stack in bytes, minimum 4096. Change only from measured high-water marks. |
| `rx_task_stack_size` | `12288` | Realtime receive task stack in bytes, minimum 4096. Change only from measured high-water marks. |
| `video.codec` | required with `video:` | Compile exactly one video backend: `jpeg` or `h264`. |
| `video.camera_id` | none | Standard ESPHome camera JPEG source; valid only with `codec: jpeg`. |
| `video.source` / `video.sink` | none | Encoded source and/or sink matching the selected codec. |
| `video.offer_payload_type` | `26` / `103` | Static PT 26 for JPEG; dynamic PT 103 by default for H.264. |
| `network_socket_headroom` | `0` | Extra lwIP sockets reserved at validation. |

Removed options rejected with migration guidance: `processor_id`, `aec_reference_delay_ms`, and the old `aec:` switch platform. Software audio processing belongs to `esp_audio_stack`.

## Resource notes

- The mic ring buffer and TX chunk exist only when a mic path is configured.
- There is no speaker ring, speaker task or AEC reference buffer inside `voip_stack`; incoming audio is written directly to the configured ESPHome speaker.
- `buffers_in_psram` affects only VoIP-owned staging buffers.
- Keep RTP payloads inside `udp_max_payload`; raising it above the real path MTU trades a compile error for intermittent one-way audio.

## Scope and known boundaries

- Optional SIP registration supports Digest authentication for managed HA and manual PBX accounts; direct calling remains available without registration.
- SIP over UDP/TCP only; no TLS signaling and no SRTP.
- RTP media is UDP in both signaling modes and requires UDP reachability.
- Audio supports explicitly declared PCM and optional G.722/Opus within the
  supported rate/format matrix. Opus is currently admitted only on supported S3
  VoIP-only profiles; the validator rejects FULL/P4 while qualification is pending.
- Established calls accept audio hold/resume through re-INVITE with `sendonly`,
  `recvonly`, `inactive` or legacy `c=IN IP4 0.0.0.0`, retaining the existing call and
  media owners and the negotiated codec/RTP mappings. Ordinary inactive streams
  retain RTCP; streams with a zero connection address send neither RTP nor RTCP.
  After reception resumes, a valid source on the negotiated IP may restart with
  both a new RTP source port and a new SSRC. Further accepted reoffers preserve
  the pending receive reset until the first valid audio packet is admitted.
- Initial audio offers with `inactive` or `c=IN IP4 0.0.0.0` still receive
  `488 Not Acceptable Here`. In-dialog codec migration is unsupported. Rejected
  proposals preserve the established dialog and media configuration for a later
  resume or BYE.
- The phonebook is not an inbound allowlist. Any peer with network reachability
  may send an INVITE, so plaintext SIP/RTP belongs on a trusted LAN/VPN or
  behind firewall/SBC policy.

## Provenance and license

This component is the SIP engine of the [`esphome-intercom`](https://github.com/n-IA-hane/esphome-intercom) platform, where it is exercised together with its Home Assistant integration, Lovelace card, maintained device YAMLs and full-experience packages. Higher layers remain in the intercom repository; this repository tracks the component itself, and `SOURCE.md` records its extraction origin and scope.

The repository also carries a small internal `ring_buffer` component used by
`voip_stack` for CAPS-aware RTP/audio buffering. It is not a user-facing YAML
component.

Project-authored code is MIT-licensed. Optional codec dependencies retain their
own licenses. See [Third-party notices](THIRD_PARTY_NOTICES.md) for the Opus
copyrights, license texts and materials to include when distributing firmware.

## Codec profiles and media route

Maintained FULL packages explicitly declare PCM: 16 kHz TX with 20 ms preferred
and 10 ms available, 48 kHz RX with 10 ms preferred and 16 kHz receive alternatives.
Opus remains the maintained S3 VoIP-only choice. G.722 is an optional 64 kbit/s
wideband codec using 16 kHz PCM and an 8 kHz RTP timestamp clock. Its direct
Espressif backend is pinned to `esp_audio_codec` 2.5.0 for early P4 compatibility.

Optional codec modules are compile-gated. Adding G.722 or Opus alongside PCM
retains both only when both are declared. No codec is inserted as a fallback.
The existing microphone and speaker adapters keep PCM boundaries separate
from network codecs. Resource preparation must succeed before starting media.

When PCM is the first configured RX format, compatible PCM and decoded G.722
use that PCM rate for local speaker playback. Conversion reuses the receive
worker and its prepared buffers; the negotiated network rate and packet time
stay unchanged. G.722-only configurations keep their native 16 kHz playback.

Opus PCM accepts 8/12/16/24/48 kHz and 10/20 ms frames. RTP always advertises
`opus/48000/2`, including mono streams, as required by
[RFC 7587](https://www.rfc-editor.org/rfc/rfc7587.html#section-7).
The current validator rejects FULL/P4 Opus; qualification is pending for those profiles.
See the [codec guide](https://github.com/n-IA-hane/esphome-intercom/blob/dev/docs/ESP_AUDIO_CODECS.md)
for declaration, package defaults, resource implications and migration.

An incoming INVITE without SDP is answered with an offer in the 200 response.
Capture and playout remain inactive until the matching ACK supplies a compatible
answer. The final answer determines packet duration, receive conversion and
microphone ownership. A missing or invalid answer ends the confirmed dialog
through its normal BYE transaction.

SIP registration is not required for direct calls. A compatible SIP peer can
call or be called without Home Assistant. HA conversion is a fallback when
the two legs cannot negotiate compatible media; an attempted fallback alone
does not prove that conversion is active.

The optional text sensor projects the existing call lifecycle:

```yaml
text_sensor:
  - platform: voip_stack
    type: media_route
    name: VoIP Media Route
    on_value:
      then:
        - lambda: |-
            const char *label = x == "ha_transcoding" ? "HA transcoding" :
                                x == "direct" ? "Direct" : "";
            ESP_LOGI("call", "Media route: %s", label);
```

Values are `direct`, `ha_transcoding`, or empty outside a confirmed call or
while waiting for HA's media owner. `direct` means compatible media without
HA conversion; an HA packet relay can preserve media unchanged. The sensor
is a presentation hook, not a second call state machine. For a display, use
`x` in its `on_value` action. The shared HA package already creates
`voip_media_route`, so extend that id instead of adding a second sensor.

The companion integration reports actual committed audio/video conversion
through the existing authenticated ESPHome API action `set_media_route`.
It supplies the SIP Call-ID of that physical leg. Firmware rejects unknown
values, updates outside a call and updates for an earlier Call-ID, then clears
the route at termination. This notification never negotiates codecs or starts
a separate media path.

## Run the native regressions

The native suite compiles production C++ with host adapters and exercises SIP
socket paths, transaction cleanup and PCM conversion. It does not run an ESP
scheduler or prove physical audio quality. Use Python with ESPHome and pytest,
plus a C++20-capable compiler, `pkg-config` and the native Opus development library
(`libopus-dev` on Debian/Ubuntu). PCM and mixed-codec tests also require the
real audio library:

```sh
git clone --branch v4.0.1 --depth 1 https://github.com/esphome-libs/esp-audio-libs.git .test-deps/esp-audio-libs
git -C .test-deps/esp-audio-libs rev-parse HEAD
ESP_AUDIO_LIBS_SOURCE="$PWD/.test-deps/esp-audio-libs" python -m pytest -q
```

The expected 4.0.1 revision is
`891fdfc531ad38d9e90b5e9088be60a66c82ed4c`, matching the official ESPHome
2026.10.0 firmware dependency. CI also tests 4.1.0 at
`e06373fa41529064cb852e2312a32a8148ddaaac` for compatibility, preserving the
actual SHA beside each JUnit artifact. Missing DSP sources fail the PCM test
instead of silently skipping it. Firmware dependency selection still belongs
to its ESPHome/IDF configuration; these host-test pins do not change a device.

## Before opening an issue

Reproduce the problem with the current maintained release/profile. Open the
ESPHome device logs and, on firmware built with ESP VoIP Stack 2026.10.0 or newer,
press **VoIP Diagnostics** during the fault and again after hangup. For a
custom configuration, use:

```yaml
button:
  - platform: template
    name: VoIP Diagnostics
    entity_category: diagnostic
    on_press:
      - voip_stack.dump_diagnostics:
          id: voip
```

Replace `voip` with your configured ID (`phone` in maintained Intercom profiles).
Keep the logger at INFO or above and attach each complete `BEGIN v=1` / `END v=1`
block. This action works with `audio_debug: false`; it does not enable SIP/RTP
packet tracing. Overlapping requests and requests within one second are ignored. Output is
split across normal ESPHome loop turns. Call state, media configuration and
counters are collected when requested, retained in one bounded temporary
snapshot, and released when printing finishes. Untracked metrics are unavailable, not zero. The short call hash is
for correlating a dump without printing the complete Call-ID.

In `rx_jitter`, `drops` counts discard/window-realignment events. One event can
remove several queued frames. From 2026.11.0, `discarded_frames` counts the buffered frames
actually removed; explicit session resets are excluded. These local playout
counters are distinct from network packet loss, which requires checking RTP
sequence numbers. The `rx_drops` queue summary retains its event-count meaning.

While media is active, VoIP Stack holds a native ESPHome Wi-Fi roaming
suppression request. Periodic access-point scans resume after the call once
all requesting components have released their requests. This prevents new
post-connect scans; it does not cancel a scan already in progress or replace
normal Wi-Fi disconnection recovery.

This action is available from 2026.10.0; older firmware must be rebuilt and
uploaded before it can expose it. Include the exact version you tested.

For device-wide memory information, include the native ESPHome `debug` sensor
readings when available. The component dump reports its own buffers and state;
system heap and PSRAM measurements remain with ESPHome/ESP-IDF diagnostics.

Include:

- Exact board/model and ESP32 variant/revision, if known.
- ESPHome version and ESP VoIP Stack tag or commit.
- Complete relevant VoIP YAML and the microphone/speaker configuration.
- Calling direction, SIP transport (`udp` or `tcp`), and peer/PBX software and
  version. Identify the route exercised: `direct`, managed `ha`, or a manual PBX
  account; include its `account_id` and registrar when registration is enabled.
- Negotiated codec and packet time (`ptime`), if known.
- Category: signaling, RTP, audio, DTMF, re-INVITE, video, teardown or other.
- Reproduction steps, expected behavior, actual behavior, boot log, diagnostic
  blocks and relevant SIP/RTP logs. Include both sides where available.

Never publish SIP passwords, authentication headers, API keys, tokens or other
credentials. Review YAML, logs and packet captures before uploading them.
A dump describes state and counters; packet captures may still be needed to
investigate protocol timing. Issues without enough information to reproduce
or classify the problem may be closed as incomplete.

If you ignore these instructions and open a useless issue anyway, I'll get pissed off like there's no tomorrow.

## Support the project

If this project is useful to you, [consider sponsoring its development](https://github.com/sponsors/n-IA-hane). Contributions help fund development tools, services and test hardware.
