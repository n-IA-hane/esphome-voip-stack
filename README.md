# ESPHome VoIP Stack

`voip_stack` is the ESPHome SIP phone component from
[`esphome-intercom`](https://github.com/n-IA-hane/esphome-intercom).

It owns SIP signaling, SDP offer/answer, RTP media, phonebook selection,
call-state automation triggers and call snapshots. It does **not** own echo
cancellation or the physical audio graph. Use it directly with native ESPHome
`microphone`/`speaker` components, or feed it the clean microphone and speaker
surfaces exposed by
[`esphome-audio-stack`](https://github.com/n-IA-hane/esphome-audio-stack).

## What This Repo Contains

| Component | Role |
|---|---|
| `voip_stack` | SIP/SDP/RTP phone endpoint for ESPHome devices. |
| `ring_buffer` | Small helper component used by `voip_stack` for audio/RTP buffering. |

The Home Assistant PBX/router/registrar/trunk integration, Lovelace softphone
card, Assist intents and ready-to-flash product YAMLs stay in
`esphome-intercom`.

## Installation

```yaml
external_components:
  - source: github://n-IA-hane/esphome-voip-stack@main
    components: [voip_stack]
```

`ring_buffer` is included in this repository so `voip_stack` can compile as a
standalone external component.

## Endpoint Capabilities

The advertised endpoint mode is inferred from YAML wiring:

| YAML wiring | Published mode | Behavior |
|---|---|---|
| `microphone` or `microphone_source` + `speaker` | `full_duplex` | Sends mic audio and plays remote audio. |
| `microphone` or `microphone_source` only | `mic_only` | Sends mic audio; incoming audio is ignored. |
| `speaker` only | `speaker_only` | Plays incoming audio; no mic TX task is created. |
| neither mic nor speaker | `control_only` | Signaling, phonebook and call-state only. |

## Minimal Native-Audio Example

```yaml
external_components:
  - source: github://n-IA-hane/esphome-voip-stack@main
    components: [voip_stack]

i2s_audio:
  - id: rx_i2s
    i2s_bclk_pin: GPIO12
    i2s_lrclk_pin: GPIO13
  - id: tx_i2s
    i2s_bclk_pin: GPIO9
    i2s_lrclk_pin: GPIO10

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
  transport: udp
  sip_port: 5060
  rtp_port: 40000
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

## Audio Processing Contract

Software AEC/AFE belongs in the audio owner, not in `voip_stack`.

- For native ESPHome audio or hardware/DSP-processed microphones, bind
  `voip_stack` directly to the microphone and speaker.
- For software AEC, codec routing, TDM references, media player, Voice
  Assistant and Micro Wake Word on one device, use `esp_audio_stack` with
  `esp_aec` or `esp_afe`, then pass its microphone/speaker surfaces to
  `voip_stack`.

Removed legacy options such as `voip_stack.processor_id` and
`voip_stack.aec_reference_delay_ms` intentionally remain unsupported. Configure
processing on `esp_audio_stack` instead.

## Home Assistant Integration

For routing, phonebook sync, softphone card, local SIP accounts and trunks,
install the `voip_stack` Home Assistant integration from
[`esphome-intercom`](https://github.com/n-IA-hane/esphome-intercom).

HACS installs that Home Assistant integration from `custom_components/voip_stack`.
This ESP component repo is separate and is consumed through ESPHome
`external_components`.

## License

MIT. See the upstream source repository for full project notices.

