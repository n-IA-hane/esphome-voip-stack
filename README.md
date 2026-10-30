# ESPHome VoIP Stack

**ESPHome 2026.10.0 or newer is required.**

Use the official ESPHome audio, mixer, resampler, media player, Voice Assistant,
Micro Wake Word and OTA components. Audio Stack and Runtime Controller are
optional external components for configurations that use them.

*Developed and maintained by n-IA-hane, with community contributions and support. [❤️ Sponsor the project](https://github.com/sponsors/n-IA-hane)*

Stable release: [2026.11.0](https://github.com/n-IA-hane/esphome-voip-stack/releases/tag/v2026.11.0).
See the [changes since 2026.10.1](CHANGELOG.md) and the
[upgrade steps](#upgrading-from-2026101).

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

**Choose direct SIP or a registered phone.** The generic component defaults to direct SIP. Optional [PBX accounts](docs/SIP_REGISTRATION.md) register with a conventional PBX. Maintained Home Assistant device packages now configure a managed SIP account automatically, including shared identity for the optional LDAP directory. One physical conversation owns the audio devices.

## Choose how the ESP connects

One component covers direct SIP and optional registered accounts. The
**2026.11.1-dev preview** adds registered PBX accounts, standard BLF/MWI, LDAP
contacts, SIP mailbox access and S3 JPEG reception. Select `@dev` on the
component source to use these additions. See the [feature guide](docs/PHONE_SERVICES.md)
and [release delta](docs/WHATS_NEW_2026_11_1_DEV.md).

An **account** is an identity registered at a PBX. A **line** chooses the direct
route or one account for a new outgoing call. The PBX's own trunk configuration
stays on the PBX. Each example below extends a working board's microphone,
speaker and audio-format configuration.

| Installation | REGISTER | Outgoing line | Incoming calls |
| --- | --- | --- | --- |
| Generic direct HA / ESP peers | Omitted | `direct` | Direct SIP |
| Maintained HA phone packages | Automatic `ha` account | `ha` | Registered HA plus direct SIP |
| PBX with a static route to the ESP | Omitted | `direct` | Direct SIP from the PBX |
| Registered PBX phone | One account | Account ID | Matching registered PBX |
| HA plus a registered PBX | One account | `direct` or account ID | Both routes |
| Several PBX accounts | Up to four | Explicit account or default | Any matching active account |

Signaling can use UDP or TCP; RTP media uses UDP. Diagram arrows describe SIP
and media between the shown peers. The selected SDP determines the actual media
addresses, independently of registration.

<details>
<summary>Five connection examples, with diagrams and YAML</summary>

### A. Home Assistant and direct ESP calls

```text
┌────────────────────────┐                      ┌────────────────────────────┐
│       ESP phone        │◄──── SIP + RTP ─────►│       HA VoIP Stack        │
│      line: direct      │                      │  HA phone / call routing   │
└────────────────────────┘                      └────────────────────────────┘
```

Keep the current direct SIP setup. The optional HA integration provides the
shared phonebook, browser card and routing. ESPHome's native API exposes its
controls when `api.custom_services: true` is configured. This case needs no
`registration` block or outgoing-line setting.

### B. A PBX reached directly by IP, without REGISTER

```text
┌────────────────────────┐                      ┌────────────────────────────┐
│   ESP at 192.0.2.20    │◄──── SIP + RTP ─────►│     PBX at 192.0.2.10      │
│      line: direct      │                      │      static IP route       │
└────────────────────────┘                      └────────────────────────────┘
```

Configure the PBX with a static SIP route to the ESP's reachable address. On the
ESP, add a direct contact for the PBX destination. Here `100` is the PBX's dialed
extension, and `192.0.2.10` is a documentation address to replace with the PBX's
actual IPv4 address:

```yaml
voip_stack:
  id: phone
  static_contacts:
    - name: "100"
      ip: 192.0.2.10
      port: 5060
      transport: udp

button:
  - platform: template
    name: Call reception
    on_press:
      - voip_stack.call:
          id: phone
          target: "100"
```

The PBX handles routing and its own admission policy. The ESP continues using
its direct line. See the [static Asterisk setup](https://github.com/n-IA-hane/esphome-intercom/blob/main/docs/CLASSIC_PBX.md).

### C. Register at Asterisk, FreeSWITCH or another SIP PBX

```text
┌────────────────────────┐                      ┌────────────────────────────┐
│       ESP phone        │───── REGISTER ──────►│       PBX registrar        │
│    account: office     │◄────── 200 OK ───────│   Asterisk / FreeSWITCH    │
│      line: office      │◄──── SIP + RTP ─────►│       extension 100        │
└────────────────────────┘                      └────────────────────────────┘
```

Add an account when the PBX expects REGISTER and Digest authentication:

```yaml
voip_stack:
  id: phone
  registration:
    max_accounts: 1
    accept_direct_calls: false
    default_line: office
    accounts:
      - account_id: office
        registrar: pbx.example.org
        username: "100"
        password: !secret office_sip_password
        transport: udp
```

`accept_direct_calls: false` limits new incoming calls to matching registered
accounts. The SIP listener stays active so the PBX can ring the ESP. This is
an admission policy, not a switch that shuts down the SIP server socket.
A single configured account becomes the outgoing default automatically in this
mode; `default_line` is written explicitly here to show the selected route.

### D. Keep direct HA calling and add a PBX account

```text
                                              ┌────────────────────────────┐
                                ┌── direct ──►│       Home Assistant       │
                                │             │        direct line         │
┌────────────────────────┐      │             └────────────────────────────┘
│       ESP phone        ├──────┤
│ one live conversation  │      │             ┌────────────────────────────┐
└────────────────────────┘      └── office ──►│   Registered PBX account   │
                                              │        office line         │
                                              └────────────────────────────┘
```

The arrows show outgoing-line choices. Both routes can also receive calls:

```yaml
voip_stack:
  id: phone
  registration:
    accept_direct_calls: true
    default_line: direct
    accounts:
      - account_id: office
        registrar: pbx.example.org
        username: "100"
        password: !secret office_sip_password
```

The existing HA route remains the default. To place a particular call through
the PBX, use `voip_stack.start_call_on_line` with `line: office`. The native HA
`start_call_via_ha` action stays on the direct HA route independently of the
selected default line.

### E. Register several accounts and choose the outgoing line

```text
                                              ┌────────────────────────────┐
                                ┌── direct ──►│    HA / direct SIP peer    │
                                │             │        direct line         │
                                │             └────────────────────────────┘
                                │
┌────────────────────────┐      │             ┌────────────────────────────┐
│       ESP phone        ├──────┼── office ──►│           PBX A            │
│ one live conversation  │      │             │        office line         │
└────────────────────────┘      │             └────────────────────────────┘
                                │
                                │             ┌────────────────────────────┐
                                └─ workshop ─►│           PBX B            │
                                              │       workshop line        │
                                              └────────────────────────────┘
```

The pool defaults to two accounts and supports one through four slots:

```yaml
voip_stack:
  id: phone
  registration:
    max_accounts: 2
    accept_direct_calls: true
    default_line: office
    accounts:
      - account_id: office
        registrar: office-pbx.example.org
        username: "100"
        password: !secret office_sip_password
        transport: udp
      - account_id: workshop
        registrar: workshop-pbx.example.org
        username: "200"
        password: !secret workshop_sip_password
        transport: tcp

button:
  - platform: template
    name: Call through workshop PBX
    on_press:
      - voip_stack.start_call_on_line:
          id: phone
          target: "201"
          line: workshop
```

An explicit call's `line` takes priority for that call. Otherwise ordinary calls
use the selected default, initially `registration.default_line`. The
`voip_stack.select_line` action selects a registered account or `direct` for
future calls. It leaves the current conversation on its original account.
All accounts share the existing SIP worker; one physical conversation owns the
audio devices at a time.

</details>

[Full account fields, actions, persistence and registration state](docs/SIP_REGISTRATION.md)

### A complete phone, with optional directory and voicemail

- **Automatic HA account:** `ha_integration.auto_register: true` lets the
  coordinated integration provision a shared SIP/LDAP identity. The generic
  default remains false; maintained HA packages enable it.
- **MWI:** `registration.message_waiting: true` receives standard mailbox counts
  through SIP, including the managed `ha` account.
- **BLF:** `blf` watches contacts through SIP dialog subscriptions. YAML receives
  idle, ringing, active, held and availability updates, and can perform directed
  pickup through the contact's own account.
- **LDAP:** `ldap_directory` commits a bounded directory into the existing
  phonebook, retaining manual overrides, stable IDs and contact categories.
- **Mailbox access:** `voip_stack.call_mailbox` opens the PBX's voice mailbox
  through the same registered account, using its notified URI or a configured
  access code.

The maintained display packages provide a vertical contact list, waiting badge
and normal call controls. Custom YAML can build its own interface from the same
callbacks and snapshots. [SIP mailbox state and access](docs/SIP_MWI.md) and
[contacts, BLF and SIP voicemail](docs/PHONE_SERVICES.md) describe each layer.

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

**Auto-derivation from the audio graph.** `audio.tx: auto` reads the configured microphone/microphone_source and derives rate, format and channels from what that source actually publishes, with validation errors when the source cannot answer.

**A phonebook that is a dial plan, not a gatekeeper.** Contacts define where outbound calls go. Inbound calls are not rejected just because the caller is unknown; the INVITE carries caller and destination identity, and HA-side resolution handles routed/NAT installs.

**SIP capability discovery.** OPTIONS describes configured media capabilities without opening audio or allocating RTP ports. Offer/answer remains authoritative for each call. Optional entities expose call state, caller, destination, contacts, diagnostics and the last terminal reason.

**Honest failure taxonomy.** The FSM distinguishes `BUSY`, `DECLINED`, `CANCELLED`, `MEDIA_INCOMPATIBLE`, `TRANSPORT_UNREACHABLE` and `AUTH_REQUIRED_UNSUPPORTED`.

**Compile-time pruning.** A speaker-only build contains no VoIP TX path and a mic-only build contains no RX-to-speaker path.

**Optional video.** Native ESP32-S3 cameras transmit JPEG; the separate `esp_video_renderer` receives JPEG on S3 LVGL displays. Camera transmission and display reception are independently selectable. ESP32-P4 adapters provide hardware-oriented video transmission and presentation, with separate JPEG and H.264 profiles. VoIP owns transport, the camera/source owns capture, and the selected renderer owns decoding and presentation. See the [renderer guide](docs/ESP_VIDEO_RENDERER.md) for limits and qualification scope.

**Full ESPHome automation surface.** Triggers, actions and conditions are normal ESPHome automations.

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
| PBX/provider trunk reaching ESP devices | HA can terminate and route the trunk; optional ESP PBX accounts also support direct registration |
| Offline installation with a fixed dial plan | `static_contacts`, no HA required |

## Core concepts

**Endpoint.** One SIP phone with its account identity and actual SDP capabilities. The coordinated HA integration discovers managed phones through their existing API configuration and SIP registration, and queries OPTIONS for capabilities. The legacy `endpoint` text sensor is an explicit compatibility option.

**Capability mode.** Derived from wiring. It gates task creation at runtime and code inclusion at compile time.

**Phonebook.** The outbound dial plan: one ordered contact collection, selectable by stable ID, name or the existing `next_contact`/`prev_contact` actions. Sources include static/manual contacts and optional LDAP directories. Managed LDAP replaces the legacy HA roster source while preserving manual entries.

**Audio format contract.** `audio.tx` describes what the device sends, `audio.rx` what it accepts. The internal no-audio placeholder is never used to repair missing endpoint formats.

**Call FSM.** One explicit state machine drives the lifecycle. Every state is observable through the `state` text sensor, triggers and conditions. Terminal outcomes are published through `last_reason`.

## Installation

```yaml
external_components:
  - source: github://n-IA-hane/esphome-voip-stack@main
    components: [voip_stack]
```

This release requires ESPHome 2026.10.0 or newer.

Requirements: ESP-IDF framework. PSRAM is recommended for full-duplex profiles and required by co-resident AEC/AFE processing. RTP media requires UDP reachability between peers in both signaling modes.

When pairing with the audio stack, pull both:

```yaml
external_components:
  - source: github://n-IA-hane/esphome-audio-stack@main
    components: [esp_audio_stack, esp_aec]
  - source: github://n-IA-hane/esphome-voip-stack@main
    components: [voip_stack]
```

### Upgrading from 2026.10.1

- Update the builder to **ESPHome 2026.10.0 or newer**, then rebuild and upload.
- If custom YAML supplies multiple Opus formats, keep their decoded PCM sample
  rate, sample format and channel count identical within each direction. When
  RX advertises both PCM and Opus, its PCM alternatives must share that same
  geometry. Packet times may differ. PCM-only receive-rate conversion remains
  supported.
- With native HA integration enabled, remove manually declared API actions
  named `start_call_via_ha` or `hangup_call_if_id`; the component provides them.
  Existing `start_call`, answer and hangup action names remain valid.

Microphone-only and speaker-only configurations now advertise their actual SDP
direction. Settings for absent audio hardware remain accepted and are ignored.
The older migration from copied HA packages to native discovery already shipped
in 2026.10.0; it is not another required migration for an up-to-date 2026.10.1 YAML.

### Add a custom ESP phone to Home Assistant

The native adapter exposes call controls and entities. To use the coordinated
managed-account setup, enable automatic registration alongside your board's
existing audio configuration:

```yaml
api:
  custom_services: true

voip_stack:
  ha_integration:
    auto_register: true
```

Keep your microphone and/or speaker configuration. The component works with
native ESPHome audio as well as Audio Stack. The coordinated HA backend provisions
the `ha` SIP identity automatically. Add LDAP and select BLF/MWI using the
[phone-service guide](docs/PHONE_SERVICES.md). The generic component keeps
`auto_register: false`; use `ha_integration: false` to omit its HA adapter.

For an HA action that selects an outgoing trunk for one call, the native API also
provides `start_call_via_ha(dest, call_id)` and `hangup_call_if_id(call_id)`.
The HA integration supplies a fresh SIP Call-ID, binds its selected line to that
call and routes the request through the configured HA peer. Scoped cancellation
cannot hang up a later call with another identity. Ordinary `start_call(dest)`
keeps its existing phonebook/direct-call behavior.

These additional actions are compiled with the HA API adapter. They add no audio
task, audio buffer or per-frame processing. The requested identity is passed
through the existing call setup; there is no stored override for a future call.
Call-IDs supplied through this API must be globally unique and case-sensitive,
with at most 128 ASCII letters, digits, dots, underscores, hyphens and one optional
`@` separator between non-empty parts. Incoming SIP parsing is unchanged.

See the [entity and migration guide](https://github.com/n-IA-hane/esphome-intercom/blob/main/docs/ESP_ENTITY_SURFACE.md)
for the retired packages and entity customization. Old package/action
declarations are rejected rather than silently creating duplicate controls.

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
    tx: { sample_rate: 48000, pcm_format: s16le, channels: 1, frame_ms: 10 }
    rx: { sample_rate: 48000, pcm_format: s16le, channels: 1, frame_ms: 10 }
```

Use `microphone_source:` when the ESPHome microphone is wider than the VoIP
stream and `voip_stack` must select a channel or bit depth. In the example
above the I2S microphone is 32-bit stereo, while the SIP/RTP TX stream is
16-bit mono, so `microphone_source` selects one channel and converts the
sample width before VoIP sends it.

Use `microphone:` when the referenced microphone already exposes the exact PCM
stream you want to advertise in `audio.tx`. This is the normal shape with
`esp_audio_stack`, because its microphone platform already publishes the
post-AEC/AFE clean microphone stream.

### Audio stack facade

This is the maintained path when software AEC/AFE, media player, Voice Assistant, Micro Wake Word or multiple consumers share the backend:

```yaml
external_components:
  - source: github://n-IA-hane/esphome-audio-stack@main
    components: [esp_audio_stack, esp_aec]
  - source: github://n-IA-hane/esphome-voip-stack@main
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
    tx: auto
    rx: auto
```

Audio Stack currently supports one instance per ESP. Its microphone and speaker
bind to it automatically; `esp_audio_stack_id` can be omitted.


During a call, the far end hears near-end speech after echo cancellation. Wake word keeps working locally because the same clean stream feeds it.

For the full Espressif AFE pipeline, import `esp_afe` instead of `esp_aec` and
point `processor_id` at that processor:

```yaml
external_components:
  - source: github://n-IA-hane/esphome-audio-stack@main
    components: [esp_audio_stack, esp_afe]
  - source: github://n-IA-hane/esphome-voip-stack@main
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
| `sample_rate` | 8000, 12000, 16000, 24000, 32000, 44100, 48000 | `auto` |
| `pcm_format` | `s16le`, `s24le`, `s24le_in_s32` (`s32le` is rejected because RTP has no mapping) | `auto` |
| `channels` | 1, 2 | `auto` |
| `frame_ms` | 10, 16, 20, 32 | `auto` |

`tx_formats` / `rx_formats` declare up to seven additional advertised formats
per direction. Extra entries are explicit. TX extras retain the microphone's
PCM sample rate, sample format and channels; supported codec and packet-time
alternatives use that same input geometry. RX extras may describe other supported
speaker-side formats, subject to the mixed PCM/Opus restriction below. Validation
rejects the eighth extra entry.

For standard SIP peers, S16 PCM reception can negotiate a higher sample rate
than microphone transmission, such as RX 48 kHz and TX 16 kHz. A duplex SDP must
retain a shared bidirectional format. The receiver keeps the accepted payload
mappings, and converts alternate negotiated rates to the selected speaker
rate after jitter reordering. Payload numbers outside that negotiation remain
invalid. Opus continues to use its standard 48 kHz RTP clock independently of
the local PCM rates.

With Asterisk, use `allow=slin48:10,slin16:10`, `asymmetric_rtp_codec=yes`,
`direct_media=no` and `use_ptime=yes` on the static ESP endpoint. See the
[classic PBX guide](https://github.com/n-IA-hane/esphome-intercom/blob/main/docs/CLASSIC_PBX.md)
for static routing without REGISTER and the separate codec choices for other
phones.

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

Inbound INVITEs carry caller and destination identity. Direct profiles accept callers independently of the phonebook; optional PBX-only admission can restrict new incoming calls to configured accounts. HA maps its trunk numbers and inbound DTMF routes to local phonebook targets.

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
Typed directory actions `voip_stack.call_contact` and `voip_stack.pickup_contact`
accept `contact_id`; pickup requires the optional BLF configuration.

Audio and diagnostic actions: `voip_stack.set_volume`,
`voip_stack.set_mic_gain_db`, and `voip_stack.publish_entity_states`.

## Entities

`voip_stack:` alone enables the local SIP/RTP engine. HA discovery, card mirror
state, editable groups and diagnostics are exposed by declaring the native
platform entities you need:

| Platform | Type | Content |
|---|---|---|
| `text_sensor` | `endpoint` | Optional legacy discovery record, disabled by default. |
| `text_sensor` | `state` | Current FSM state. |
| `text_sensor` | `caller` | Current/last caller identity. |
| `text_sensor` | `destination` | Currently selected dial-plan target. |
| `text_sensor` | `last_reason` | Last terminal outcome. |
| `text_sensor` | `contacts` | Published local contact list. |
| `text_sensor` | `transport` | Active signaling transport. |
| `text_sensor` | `sip_snapshot` | Diagnostic snapshot of the SIP layer. |
| `text` | `ring_groups` | Editable comma-separated ring group memberships. |
| `text` | `conference_groups` | Editable comma-separated conference group memberships. |
| `switch` | `conference_ring` | Ring this endpoint when another participant starts one of its conference groups. |

The native HA adapter already provides the normal control entities when API is
enabled. Customize them under `voip_stack.ha_integration`. An older HA backend
can explicitly retain its legacy discovery record with
`ha_integration: {endpoint: true}`. For standalone SIP (`ha_integration: false`),
declare individual entities as required:

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
| `audio.tx` / `audio.rx` | `auto` | Per-direction PCM contract. |
| `audio.tx_formats` / `audio.rx_formats` | `[]` | Extra explicit capabilities; TX retains its PCM geometry, RX supports other formats within the mixed-codec constraints. |
| `static_contacts` | `[]` | YAML dial plan. |
| `extension` | `""` | Local SIP extension/user part. |
| `conference_groups` | `""` | Optional HA-managed conference group membership, single name or comma-separated list. |
| `conference_ring` | `false` | Ring this device when another member starts its conference group. |
| `ring_groups` | `""` | Optional HA-managed ring group membership, single name or comma-separated list. |
| `use_ha_as_first_contact` | `false` | Pin the HA peer at the top of the dial plan. |
| `ha_phonebook_text_sensor_id` | none | Bind the HA phonebook sensor. |
| `delete_contact_missing_from` | none | Drop absent roster contact after N updates. |
| `ringing_timeout` / `calling_timeout` | none | Guard timers. |
| `dc_offset_removal` | `false` | Remove DC bias on TX. |
| `audio_debug` | `false` | Verbose audio-path diagnostics. |
| `buffers_in_psram` | `false` | Place VoIP-owned buffers in PSRAM. |
| `signaling_buffers_in_psram` | `false` | Place retained SIP text and directory records/text in PSRAM, independently of PCM staging. |
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
- `signaling_buffers_in_psram` places retained SIP messages, subscription metadata
  and directory records/text in PSRAM. Its 64 contact slots are reserved at startup; the internal
  placement grows on demand within the same 64-contact bound and reuses capacity.
- Keep RTP payloads inside `udp_max_payload`; raising it above the real path MTU trades a compile error for intermittent one-way audio.
- Optional BLF preallocates 16 watchers by default, configurable from 1 to 64,
  with PSRAM placement. LDAP has independent, bounded buffer and worker
  settings, included only when that component is declared.

## Scope and known boundaries

- Optional [SIP registration](docs/SIP_REGISTRATION.md) supports IPv4 UDP/TCP accounts with MD5/SHA-256 Digest. Direct profiles require no registration configuration. Unsupported challenges retain the explicit authentication failure state.
- SIP over UDP/TCP only; no TLS signaling and no SRTP.
- RTP media is UDP in both signaling modes and requires UDP reachability.
- Audio supports PCM and compile-time optional Opus within the supported
  rate/format matrix. Opus is restricted to ESP32-S3 VoIP-only profiles.
- In-dialog hold/resume follows standard media directions while retaining the
  prepared audio path. Unsupported codec or endpoint changes receive `488`,
  preserving the established call.
- The phonebook is independent of incoming admission. Direct profiles accept
  reachable peers; registered-only admission uses the configured account and
  trusted PBX peer. Plaintext SIP/RTP belongs on a trusted LAN/VPN or behind
  firewall/SBC policy.

## Provenance and license

This component is the SIP engine of the [`esphome-intercom`](https://github.com/n-IA-hane/esphome-intercom) platform, where it is exercised together with its Home Assistant integration, Lovelace card, maintained device YAMLs and full-experience packages. Higher layers remain in the intercom repository; this repository tracks the component itself, and `SOURCE.md` records its extraction origin and scope.

The repository also carries a small internal `ring_buffer` component used by
`voip_stack` for CAPS-aware RTP/audio buffering. It is not a user-facing YAML
component.

MIT license.

## Codec profiles and media route

Maintained ESP32-S3 VoIP-only examples prefer Opus with 20 ms packets. Their
commented PCM blocks preserve the alternative configuration. Full profiles
use PCM. ESP32-P4 always uses PCM. Its maintained profiles configure a
16 kHz microphone/TX stream, a 48 kHz speaker and preferred 10 ms packets.
These are profile choices, not fixed rates in the media engine. The 16 kHz
RX alternative remains available through the existing
resampler for direct PCM calls with other ESP devices. Opus is rejected at
configuration time on P4 or with Voice Assistant, micro wake word or Sendspin.
Only PCM and Opus are supported by the ESP codec implementation.

PCM accepts the documented 8/12/16/24/32/44.1/48 kHz rates. The Opus PCM
boundary accepts 8/12/16/24/48 kHz, matching the
[Opus encoder API](https://opus-codec.org/docs/opus_api-1.5/group__opus__encoder.html).
For other hardware rates, configure the existing microphone/speaker resampler
to expose a supported codec rate; `audio.tx` or `audio.rx` set directly to
32 kHz Opus is rejected. Opus RTP always uses `opus/48000/2`, even for mono
16 kHz input, as required by
[RFC 7587](https://www.rfc-editor.org/rfc/rfc7587). This RTP clock does not
imply that the microphone captured fullband audio. Neither codec silently
changes to the other when a peer is incompatible.

A VoIP-only profile may advertise PCM and Opus together as negotiated fallback
formats. Mixed receive formats must expose the same decoded PCM sample rate,
sample format and channel count. Packet duration is selected by SDP from the
configured choices. Each received payload selects its own decoder; a change
between PCM and Opus does not require a new SIP call. Unsupported mixed PCM
geometries fail configuration validation instead of advertising an unusable
fallback. PCM-only profiles retain their supported receive-rate conversion.

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
plus a C++20-capable compiler, `pkg-config`, OpenSSL development headers
(`libssl-dev`) and the native Opus development library (`libopus-dev` on
Debian/Ubuntu). PCM and mixed-codec tests also require the
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
  version. If optional registration is enabled, include registrar, proxy and
  account configuration with credentials removed.
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
