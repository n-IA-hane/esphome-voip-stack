# voip_stack

`voip_stack` is the ESP SIP phone component. It owns SIP signaling, SDP
offer/answer, RTP media, phonebook selection and call snapshots. It does not
own echo cancellation.

It can run in two supported shapes:

- **Standalone native ESPHome audio**: bind `voip_stack` directly to standard
  ESPHome `microphone` and/or `speaker` components. This is the right path for
  devices whose hardware already returns processed audio, for example XMOS
  front-ends, DSP codecs, or simple tests with native I2S mic/speaker.
- **Audio stack facade**: bind it to the microphone/speaker exposed by
  `esp_audio_stack`. This is the maintained path when software AEC, AFE, codec
  routing, TDM reference, dual-bus sync, media player, Voice Assistant or Micro
  Wake Word share the same audio backend.

The component negotiates explicit PCM per direction. SIP/SDP selects one
dialog `ptime` shared by both directions, while TX and RX sample rate/PCM
format can still differ. Maintained YAMLs publish their real capabilities; the
internal no-audio placeholder is `16000:s16le:1:16` and is not used to repair
missing endpoint formats. AFE/AEC-backed branches remain
16 kHz/s16/mono because Espressif esp-sr exposes that surface.

## Audio Capabilities

The endpoint capability is inferred from the YAML wiring:

| YAML wiring | Published endpoint mode | Runtime behavior |
|---|---|---|
| `microphone`/`microphone_source` + `speaker` | `full_duplex` | Sends mic audio and plays remote audio. |
| `microphone`/`microphone_source` only | `mic_only` | Sends mic audio; incoming audio is ignored. |
| `speaker` only | `speaker_only` | Plays incoming audio; no mic TX task is created. |

The endpoint sensor publishes this device as a SIP phonebook endpoint. SIP is
implicit. The public concepts are:

- name: display and dial name;
- address: optional host/IP for direct SIP endpoints;
- SIP port and RTP port;
- transport: SIP/TCP or SIP/UDP signaling;
- audio mode: full duplex, microphone only, speaker only or control only;
- advertised TX/RX PCM capabilities.

Home Assistant consumes those fields for routing/card display, format
negotiation and avoiding audio directions that cannot exist. User YAML should
use the structured `add_contact` action or HA-managed roster sync instead of
hand-building serialized endpoint rows.

## Compile-Time Shape

`voip_stack` now defines its own local compile flags from YAML:

- `USE_ESPHOME_VOIP_STACK_MIC` is emitted only when the component has
  `microphone:` or `microphone_source:`.
- `USE_ESPHOME_VOIP_STACK_SPEAKER` is emitted only when the component has `speaker:`.

This keeps the VoIP TX path out of speaker-only builds and keeps the
VoIP RX-to-speaker code out of mic-only builds, even if some other ESPHome
component in the same firmware uses a microphone or speaker.

The only always-present pieces are the finite-state machine, phonebook,
transport listener/client and control signaling.

## Minimal Full-Duplex Example

```yaml
microphone:
  - platform: i2s_audio
    id: native_mic
    adc_type: external
    i2s_audio_id: rx_i2s
    i2s_din_pin: GPIO11
    channel: right
    sample_rate: 48000
    bits_per_sample: 32bit

speaker:
  - platform: i2s_audio
    id: native_speaker
    dac_type: external
    i2s_audio_id: tx_i2s
    i2s_dout_pin: GPIO14
    channel: mono
    sample_rate: 48000
    bits_per_sample: 16bit

voip_stack:
  id: phone
  microphone_source:
    microphone: native_mic
    bits_per_sample: 16
    channels: [0]
  speaker: native_speaker
  audio:
    tx:
      sample_rate: 48000
      pcm_format: s16le
      channels: 1
      frame_ms: 20
    rx:
      sample_rate: 48000
      pcm_format: s16le
      channels: 1
      frame_ms: 20
```

Use `microphone_source:` when the ESPHome microphone is wider than the VoIP
stream and `voip_stack` must select a channel or bit depth. For example, an
I2S microphone can expose 32-bit stereo while SIP/RTP should send 16-bit mono.

Use `microphone:` when the referenced microphone already exposes the exact PCM
stream you want to advertise in `audio.tx`. This is the normal shape with
`esp_audio_stack`, because its microphone platform already publishes the
post-AEC/AFE clean microphone stream. Maintained AEC/AFE profiles publish
`16000:s16le:1` toward VoIP.

## Software AEC / AFE

`voip_stack` does not own echo cancellation. Import `esp_audio_stack` plus the
processor you want, attach that processor to `esp_audio_stack`, then give
`voip_stack` the resulting clean microphone and speaker surfaces:

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
  # pins / codec / reference topology go here

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

For the full Espressif AFE pipeline, import `esp_afe` instead of `esp_aec`:

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

## Mic-Only And Speaker-Only

Mic-only:

```yaml
voip_stack:
  id: phone
  microphone: processed_mic
```

Speaker-only:

```yaml
voip_stack:
  id: phone
  speaker: local_speaker
```

These are first-class modes, not degraded modes. They are useful for split
installations, paging speakers, monitor/listen devices, and hardware that
already exposes only one audio direction.

## Removed Legacy AEC / AFE Options

Standalone `voip_stack` AEC has been removed.

Removed options:

| Removed | Replacement |
|---|---|
| `voip_stack.processor_id` | Configure `processor_id` on `esp_audio_stack`. |
| `voip_stack.aec_reference_delay_ms` | Configure AEC/AFE reference buffering on `esp_audio_stack`. |
| `switch: platform: voip_stack, aec:` | Use `esp_audio_stack`, `esp_aec` or `esp_afe` controls. |

Reason: `voip_stack` is not the right owner for I2S timing, speaker reference
capture or AFE cadence. When software processing is required, `esp_audio_stack`
owns the audio graph and exposes normal ESPHome microphone/speaker interfaces
back to `voip_stack`.

## Transport

SIP is the only call-control protocol. `transport` selects only the SIP signaling
transport exposed by this phone. RTP audio remains UDP in both modes.

SIP/UDP signaling:

```yaml
voip_stack:
  id: phone
  transport: udp
  sip_port: 5060
  rtp_port: 40000
```

SIP/TCP signaling:

```yaml
voip_stack:
  id: phone
  transport: tcp
  sip_port: 5060
  rtp_port: 40000
```

RTP media remains UDP for both signaling transports. Setting `transport: tcp`
does not tunnel audio over TCP; it makes SIP INVITE/ACK/BYE use TCP while SDP
still negotiates RTP/UDP media.

## Phonebook And HA Routing

Contacts can be declared directly in `voip_stack` for installs that do not
want HA to be the only phonebook source. HA-managed sync through
`sensor.voip_phonebook` is still the recommended path for normal installs;
static contacts are for offline installs, diagnostics, direct SIP peers that
must exist before HA connects, or tiny fixed systems:

```yaml
voip_stack:
  id: phone
  transport: udp  # SIP signaling transport only; audio is always RTP/UDP.
  static_contacts:
    - name: Workshop
      ip: 192.168.1.44
      port: 5060
      rtp_port: 40000
    - name: Front Gate
```

`name` is required. `ip`, `port`, `rtp_port`, and `transport` are optional.
When `transport` is omitted, the contact uses the component SIP signaling
transport (`transport: udp` or `transport: tcp` on `voip_stack`). A name-only
contact is a logical target that can later be upgraded by the HA roster or
routed through HA.

Runtime YAML automations can mutate the local dial plan with
`voip_stack.add_contact`, `voip_stack.remove_contact`,
`voip_stack.set_contacts` and `voip_stack.flush_contacts`. The standard
packages expose those through ESPHome native API actions named
`esphome.<slug>_add_contact`, `esphome.<slug>_remove_contact`,
`esphome.<slug>_set_contacts`, `esphome.<slug>_flush_contacts` and
`esphome.<slug>_update_contacts`. Use them for custom local behavior only; HA
central contacts should be managed with HA `voip_stack.add_contact`,
`voip_stack.remove_contact`, `voip_stack.set_contacts`,
`voip_stack.clear_contacts` and `voip_stack.push_phonebook` services.

Each ESP publishes an `voip_endpoint` text sensor. Home Assistant builds the
central `sensor.voip_phonebook` from those endpoints and adds itself as the
HA peer.

Inbound ESP calls are not rejected just because the caller is missing from the
callee phonebook. The phonebook is the outbound SIP dial plan; inbound INVITE
carries caller and destination identity. When HA is in the path, it resolves
inbound SIP callers by:

1. socket source IP when it matches the endpoint host;
2. SIP From URI user/name;
3. caller friendly name from SIP headers.

That keeps routed subnet/NAT/VPN installs working when HA sees a socket source
address that differs from the ESP endpoint IP, while preserving the endpoint IP
as the address other peers should dial.

`advertise_host` in the HA integration is an advanced override for HA
multihomed/LXC/NAT installs where Home Assistant would otherwise publish an
address ESP devices cannot reach. It is not required just because ESPs and HA
are on different routed subnets.

ESP devices never register to the optional HA/provider trunk. If HA has a trunk,
HA maps external numbers or inbound DTMF route digits to local phonebook
targets and then calls ESP devices as normal SIP phones.

## SIP Automation Hooks

SIP-aware hooks expose the call identity directly:

```yaml
voip_stack:
  id: phone
  on_incoming_call:
    then:
      - logger.log:
          format: "incoming call_id=%s caller=%s callee=%s uri=%s"
          args: [call_id.c_str(), caller.c_str(), callee.c_str(), uri.c_str()]
  on_outgoing_call:
    then:
      - logger.log:
          format: "outgoing call_id=%s caller=%s callee=%s uri=%s"
          args: [call_id.c_str(), caller.c_str(), callee.c_str(), uri.c_str()]
  on_bridge_request:
    then:
      - logger.log:
          format: "bridge request call_id=%s caller=%s callee=%s uri=%s"
          args: [call_id.c_str(), caller.c_str(), callee.c_str(), uri.c_str()]
```

`on_bridge_request` fires for ESP-originated routes that explicitly target the
HA peer. HA-side dial-plan decisions are exposed as Home Assistant bus events
and services by `voip_stack`.

## Optional Entity Platforms

Declare only the native ESPHome platforms an endpoint needs:

- `auto_answer`
- `dnd`
- `master_volume`, only when a speaker is configured
- `mic_gain`, only when a mic is configured

The core component does not auto-load primary entity platforms. The maintained
YAML packages provide explicit `switch:`, `number:`, `button:`, `text:` and
`text_sensor:` entries with stable names and UI layout.

## Resource Notes

- Mic ring buffer and TX chunk are allocated only when a mic path is configured.
- No speaker ring, speaker task or AEC reference buffer exists in
  `voip_stack`.
- Incoming audio is written directly to the configured ESPHome speaker.
- `buffers_in_psram: true` affects only VoIP-owned staging buffers.
- `task_stacks_in_psram: true` applies to the VoIP TX task and transport
  tasks where supported by the transport.

## Experimental Native Dual-Bus Test

See:

```text
yamls/voip-only/dual-bus/generic-s3-voip.yaml
```

That profile intentionally avoids `esp_audio_stack` and binds `voip_stack` to
native ESPHome `i2s_audio` microphone/speaker components. It is for regression
testing standalone full-duplex, mic-only and speaker-only modes.

## Protocol And Security Boundaries

- ESP endpoints do not register and do not implement SIP Digest challenges.
- The phonebook is an outbound dial plan, not an inbound caller allowlist. Any
  network-reachable peer may send an INVITE; normal busy/DND and SDP validation
  still apply.
- SIP is UDP/TCP without TLS and RTP is plaintext UDP without SRTP. Use a
  trusted LAN/VPN or enforce admission at a firewall/SBC boundary.
- In-dialog re-INVITE (including hold or codec renegotiation) receives `488`
  without replacing the established dialog. Existing media and a later BYE
  continue on the original session.
