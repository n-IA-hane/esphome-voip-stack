# ESPHome VoIP Stack

A native SIP phone component for ESPHome. `voip_stack` turns an ESP32 device into a real SIP endpoint: SIP signaling over UDP or TCP, SDP offer/answer, RTP media, a local phonebook and dial plan, explicit per-direction PCM negotiation, and a full set of ESPHome entities, triggers, actions and conditions on top.

It binds to standard ESPHome `microphone` and `speaker` components, so the audio source can be anything from a bare I2S MEMS microphone to the echo-cancelled output of [`esphome-audio-stack`](https://github.com/n-IA-hane/esphome-audio-stack). The component is a complete SIP phone on its own; the optional Home Assistant integration from [`esphome-intercom`](https://github.com/n-IA-hane/esphome-intercom) adds central phonebook management, call routing, a softphone/B2BUA and a Lovelace card on top of the same devices.

## 1. What This Is

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

**Devices are peers, not trunk subscribers.** ESP devices call each other and Home Assistant directly by SIP; they never register to a PBX or provider trunk. If an installation has a trunk, Home Assistant terminates it and routes calls to ESP devices as normal SIP phones.

## 2. What It Gives You

**A real SIP endpoint.** INVITE/ACK/BYE over UDP or TCP signaling, SDP offer/answer, RTP media that stays UDP in both signaling modes, and a jitter buffer on the receive path.

**Four first-class capability modes, inferred from YAML.**

| YAML wiring | Published mode | Runtime behavior |
|---|---|---|
| microphone + speaker | `full_duplex` | Sends mic audio, plays remote audio. |
| microphone only | `mic_only` | Sends mic audio; incoming audio is ignored. |
| speaker only | `speaker_only` | Plays incoming audio; no TX task is created. |
| neither | `control_only` | Signaling and phonebook only. |

Mic-only and speaker-only are not degraded modes. They exist for paging speakers, monitor devices, split installations, and hardware that physically has one direction.

**Explicit per-direction PCM negotiation.** TX and RX formats are declared independently: sample rate from 8000 to 48000 Hz, PCM format `s16le`/`s24le`/`s24le_in_s32`/`s32le`, 1 or 2 channels, packet time 10/16/20/32 ms. Devices advertise their real capabilities instead of pretending everything is 8 kHz telephony audio.

**Auto-derivation from the audio graph.** `audio.tx: auto` reads the configured microphone/microphone_source and derives rate, format and channels from what that source actually publishes, with validation errors when the source cannot answer.

**A phonebook that is a dial plan, not a gatekeeper.** Contacts define where outbound calls go. Inbound calls are not rejected just because the caller is unknown; the INVITE carries caller and destination identity, and HA-side resolution handles routed/NAT installs.

**Self-describing devices.** The component publishes text sensors for state, transport, endpoint identity, destination, caller, contacts, SIP snapshot and last terminal reason.

**Honest failure taxonomy.** The FSM distinguishes `BUSY`, `DECLINED`, `CANCELLED`, `MEDIA_INCOMPATIBLE`, `TRANSPORT_UNREACHABLE` and `AUTH_REQUIRED_UNSUPPORTED`.

**Compile-time pruning.** A speaker-only build contains no VoIP TX path and a mic-only build contains no RX-to-speaker path.

**Full ESPHome automation surface.** Triggers, actions and conditions are normal ESPHome automations.

## 3. Scenarios It Covers

| Goal | Shape |
|---|---|
| Two ESP intercoms calling each other on a LAN, no server | `voip_stack` + static contacts, direct SIP/UDP |
| Doorbell that rings phones and HA dashboards | `voip_stack` + the HA integration from `esphome-intercom` |
| Voice assistant device that is also an intercom | `esp_audio_stack` + `voip_stack` |
| Paging speaker in a workshop | speaker-only `voip_stack` |
| Baby monitor / listen-in device | mic-only `voip_stack` |
| Wall panel that only dials and shows state | `control_only` + buttons and text sensors |
| PBX/provider trunk reaching ESP devices | HA terminates the trunk and routes; ESPs stay plain SIP peers |
| Offline installation with a fixed dial plan | `static_contacts`, no HA required |

# The Manual

## 4. Core Concepts

**Endpoint.** The published identity of a device: name, optional host/IP, SIP port, RTP port, signaling transport, capability mode, and advertised TX/RX PCM capabilities. The `endpoint` text sensor is the authoritative record Home Assistant consumes.

**Capability mode.** Derived from wiring. It gates task creation at runtime and code inclusion at compile time.

**Phonebook.** The outbound dial plan: an ordered list of contacts the device can call, navigated by `next_contact`/`prev_contact` or addressed by name. Sources are static YAML contacts, runtime actions, and the HA-managed roster.

**Audio format contract.** `audio.tx` describes what the device sends, `audio.rx` what it accepts. The internal no-audio placeholder is never used to repair missing endpoint formats.

**Call FSM.** One explicit state machine drives the lifecycle. Every state is observable through the `state` text sensor, triggers and conditions. Terminal outcomes are published through `last_reason`.

## 5. Installation

```yaml
external_components:
  - source: github://n-IA-hane/esphome-voip-stack@main
    components: [voip_stack]
```

Requirements: ESP-IDF framework. PSRAM is recommended for full-duplex profiles and required by co-resident AEC/AFE processing. RTP media requires UDP reachability between peers in both signaling modes.

When pairing with the audio stack, pull both:

```yaml
external_components:
  - source: github://n-IA-hane/esphome-audio-stack@main
    components: [esp_audio_stack, esp_aec]
  - source: github://n-IA-hane/esphome-voip-stack@main
    components: [voip_stack]
```

## 6. Audio Wiring

### 6.1 Standalone Native ESPHome Audio

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
    tx: { sample_rate: 48000, pcm_format: s16le, channels: 1, frame_ms: 20 }
    rx: { sample_rate: 48000, pcm_format: s16le, channels: 1, frame_ms: 20 }
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

### 6.2 Audio Stack Facade

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
    esp_audio_stack_id: audio_stack

speaker:
  - platform: esp_audio_stack
    id: stack_speaker
    esp_audio_stack_id: audio_stack

voip_stack:
  id: phone
  microphone: clean_mic
  speaker: stack_speaker
  audio:
    tx: auto
    rx: auto
```

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

### 6.3 Mic-Only and Speaker-Only

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

## 7. Audio Format Negotiation

Per-direction format blocks accept `auto` per field or as the whole block:

| Field | Values | Default |
|---|---|---|
| `sample_rate` | 8000, 12000, 16000, 24000, 32000, 44100, 48000 | `auto` |
| `pcm_format` | `s16le`, `s24le`, `s24le_in_s32`, `s32le` | `auto` |
| `channels` | 1, 2 | `auto` |
| `frame_ms` | 10, 16, 20, 32 | `auto` |

`tx_formats` / `rx_formats` declare up to seven additional advertised formats per direction. They must be fully explicit and may only vary `frame_ms` relative to the base format.

RTP packet sizes are guarded by `udp_max_payload` (default 1200 bytes), enforced at validation.

## 8. Transport

SIP is the only call-control protocol. `transport` selects the SIP signaling transport; RTP media remains UDP in both modes:

```yaml
voip_stack:
  id: phone
  transport: tcp        # or udp
  sip_port: 5060
  rtp_port: 40000
```

TCP signaling is useful where UDP SIP is filtered or unreliable; per-contact `sip_transport` allows mixing.

## 9. Phonebook and Routing

```yaml
voip_stack:
  id: phone
  static_contacts:
    - name: Workshop
      ip: 192.168.1.44
      port: 5060
      rtp_port: 40000
      sip_transport: udp
    - name: Front Gate
```

`name` is required; everything else is optional. Static contacts serve offline installs, diagnostics and direct SIP peers. Normal installs use the HA-managed roster through `ha_phonebook_text_sensor_id`.

Inbound INVITEs carry caller and destination identity and are not rejected for being unknown. ESP devices never register to an HA or provider trunk; trunk numbers and inbound DTMF routes are mapped by HA to local phonebook targets.

## 10. Call Lifecycle, Triggers and Conditions

FSM states: `IDLE`, `CALLING`, `REMOTE_RINGING`, `RINGING`, `CONNECTING`, `IN_CALL`, `TERMINATING`, and terminal outcomes `BUSY`, `DECLINED`, `CANCELLED`, `MEDIA_INCOMPATIBLE`, `TRANSPORT_UNREACHABLE`, `AUTH_REQUIRED_UNSUPPORTED`.

Triggers:

| Trigger | Fires |
|---|---|
| `on_ringing` | Inbound call starts ringing locally. |
| `on_calling` | Outbound call enters `CALLING`. |
| `on_dest_ringing` | Remote side reports ringing. |
| `on_in_call` | Media is established. |
| `on_hangup` | Call ends normally. |
| `on_call_failed` | Call ends in a failure terminal state. |
| `on_idle` | FSM returns to idle. |
| `on_incoming_call` | Inbound INVITE parsed; provides `call_id`, `caller`, `callee`, `uri`. |
| `on_outgoing_call` | Outbound call placed. |
| `on_bridge_request` | An ESP-originated route explicitly targets the HA peer. |
| `on_destination_changed` | Selected destination changed. |
| `on_phonebook_update` | Contact list changed. |

Conditions: `voip_stack.is_idle`, `voip_stack.is_ringing`, `voip_stack.is_in_call`, `voip_stack.is_calling`, `voip_stack.is_incoming`, `voip_stack.destination_is`.

## 11. Actions

Call control:

| Action | Meaning |
|---|---|
| `voip_stack.start` | Call the selected destination. |
| `voip_stack.stop` | Hang up / cancel. |
| `voip_stack.call_toggle` | Start when idle, stop otherwise. |
| `voip_stack.answer_call` | Answer ringing inbound call. |
| `voip_stack.decline_call` | Decline with optional reason. |
| `voip_stack.call_contact` | Dial a phonebook entry by name. |
| `voip_stack.set_remote_endpoint` | Point the next call at an explicit address. |

Phonebook actions: `next_contact`, `prev_contact`, `add_contact`, `remove_contact`, `set_contact`, `set_contacts`, `flush_contacts`, `update_contacts`, `set_roster_json`, `set_ha_peer_name`.

Audio and misc actions: `set_volume`, `set_mic_gain_db`, `publish_entity_states`.

## 12. Entities

Auto-created text sensors:

| Sensor | Content |
|---|---|
| `VoIP State` | Current FSM state. |
| `VoIP Transport` | Active signaling transport. |
| `VoIP Endpoint` | Authoritative endpoint identity record. |
| `VoIP Last Reason` | Last terminal outcome. |
| `VoIP SIP Snapshot` | Diagnostic snapshot of the SIP layer. |
| `Destination` | Currently selected dial-plan target. |
| `Caller` | Current/last caller identity. |
| `Contacts` | Published contact list. |

Declared entities:

```yaml
switch:
  - platform: voip_stack
    voip_stack_id: phone
    auto_answer: { name: Auto Answer }
    dnd: { name: Do Not Disturb }

number:
  - platform: voip_stack
    voip_stack_id: phone
    master_volume: { name: Call Volume }
    mic_gain: { name: Call Mic Gain }

button:
  - platform: voip_stack
    voip_stack_id: phone
    call: { name: Call }
    decline: { name: Decline }
```

## 13. Configuration Reference

| Option | Default | Meaning |
|---|---|---|
| `transport` | `udp` | SIP signaling transport, `udp` or `tcp`. RTP is always UDP. |
| `sip_port` | `5060` | Local SIP port. |
| `rtp_port` | `40000` | Local RTP port. |
| `udp_max_payload` | `1200` | RTP payload budget. |
| `microphone` / `microphone_source` | none | TX audio source. |
| `speaker` | none | RX audio sink. |
| `audio.tx` / `audio.rx` | `auto` | Per-direction PCM contract. |
| `audio.tx_formats` / `audio.rx_formats` | `[]` | Extra packet-time reframes. |
| `static_contacts` | `[]` | YAML dial plan. |
| `extension` | `""` | Local SIP extension/user part. |
| `use_ha_as_first_contact` | `false` | Pin the HA peer at the top of the dial plan. |
| `ha_phonebook_text_sensor_id` | none | Bind the HA phonebook sensor. |
| `delete_contact_missing_from` | none | Drop absent roster contact after N updates. |
| `ringing_timeout` / `calling_timeout` | none | Guard timers. |
| `auto_entities` | `false` | Create common entity set automatically. |
| `dc_offset_removal` | `false` | Remove DC bias on TX. |
| `audio_debug` | `false` | Verbose audio-path diagnostics. |
| `buffers_in_psram` | `false` | Place VoIP-owned buffers in PSRAM. |
| `task_stacks_in_psram` | `false` | Place TX/transport task stacks in PSRAM where supported. |
| `network_socket_headroom` | `0` | Extra lwIP sockets reserved at validation. |

Removed options rejected with migration guidance: `processor_id`, `aec_reference_delay_ms`, and the old `aec:` switch platform. Software audio processing belongs to `esp_audio_stack`.

## 14. Resource Notes

- The mic ring buffer and TX chunk exist only when a mic path is configured.
- There is no speaker ring, speaker task or AEC reference buffer inside `voip_stack`; incoming audio is written directly to the configured ESPHome speaker.
- `buffers_in_psram` affects only VoIP-owned staging buffers.
- Keep RTP payloads inside `udp_max_payload`; raising it above the real path MTU trades a compile error for intermittent one-way audio.

## 15. Scope and Known Boundaries

- No SIP registration or digest authentication. Calls that hit an auth challenge terminate in `AUTH_REQUIRED_UNSUPPORTED`.
- SIP over UDP/TCP only; no TLS signaling and no SRTP.
- RTP media is UDP in both signaling modes and requires UDP reachability.
- Audio is uncompressed PCM within the supported rate/format matrix; there are no compressed codecs.

## 16. Provenance and License

This component is the SIP engine of the [`esphome-intercom`](https://github.com/n-IA-hane/esphome-intercom) platform, where it is exercised daily together with its Home Assistant integration, Lovelace card, maintained device YAMLs and full-experience packages. Higher layers remain in the intercom repository; this repository tracks the component itself, and `SOURCE.md` records the exact source commit of each snapshot.

MIT license.
