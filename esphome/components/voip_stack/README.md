# voip_stack

A native SIP endpoint for ESPHome **2026.10.0 or newer**. The component owns SIP
signaling over UDP/TCP, SDP negotiation, RTP/UDP media, the call state machine
and one bounded phonebook. Microphones, speakers, echo cancellation and display
rendering retain their own component owners.

Use the [main guide](../../../README.md) for installation, entities, actions and
automations. Development additions are summarized in the
[2026.11.1-dev release delta](../../../docs/WHATS_NEW_2026_11_1_DEV.md).

## Audio wiring

Bind a standard ESPHome microphone and/or speaker. Native I2S, external DSPs and
Audio Stack facades use the same interface. With Audio Stack, pass its clean
post-AEC/AFE microphone to VoIP; Audio Stack owns codec gains, reference timing,
TDM slots and DSP cadence.

| Configured hardware | Role |
| --- | --- |
| Microphone and speaker | Full duplex |
| Microphone only | Send-only monitor |
| Speaker only | Receive-only paging speaker |

The call advertises its actual directions and formats through SIP SDP. Ordinary
bidirectional peers receive shared wire formats; explicitly coordinated peers
can negotiate directional PCM capabilities. Read-only OPTIONS reports configured
capabilities without opening audio or reserving media ports.

This fragment assumes the two clock buses exist. Adapt the pins to your board.
The complete [native example](../../../examples/native-full-duplex.yaml) includes
the board, network and buses. A 48 kHz mono S16 frame of 10 ms fits the default
1200-byte RTP payload budget.

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
      frame_ms: 10
    rx:
      sample_rate: 48000
      pcm_format: s16le
      channels: 1
      frame_ms: 10
```

## Direct calls and PBX accounts

The generic component defaults to direct SIP. Optional
[REGISTER accounts](../../../docs/SIP_REGISTRATION.md) add conventional PBX
registration, Digest authentication, outgoing line selection and persistent
account overrides. One physical call owns the audio path at a time.

Maintained HA phone packages enable `ha_integration.auto_register`; custom
firmware opts in explicitly. HA provisions the managed `ha` account through the
existing native API. Friendly labels remain separate from technical credentials.
For standalone SIP, use `ha_integration: false` and configure manual accounts or
static destinations as needed.

## Optional phone services

- [MWI](../../../docs/SIP_MWI.md) receives standard SIP mailbox counts.
- [BLF](../../../docs/PHONE_SERVICES.md#blf-subscriptions-and-pickup) subscribes to
  contacts' dialog state and supports PBX-directed pickup. Its preallocated pool
  defaults to 16 watches and accepts 1 through 64, with optional PSRAM placement.
- [LDAP](../../../docs/PHONE_SERVICES.md#ldap-contact-ownership-and-mapping)
  imports stable contact identities into the existing 64-contact phonebook.
- [IMAP](../../../docs/PHONE_SERVICES.md#imap-visual-voicemail) supplies a
  paginated message catalog and native MediaSource playback, with UID-scoped
  read/delete operations.

SIP subscriptions share the existing SIP worker. LDAP and IMAP are independent,
optional clients with bounded buffers and configurable worker affinity and
memory placement. The shared account view is read-only; each session copies its
credentials and rejects stale-generation results.

The legacy endpoint text sensor is explicit opt-in with
`ha_integration.endpoint: true`. Coordinated HA discovers managed phones through
registration and queries SIP capabilities instead of relying on a capability
blob in entity state.

## Video and integration boundaries

[esp_video_renderer](../../../docs/ESP_VIDEO_RENDERER.md) adds optional JPEG
reception on S3 LVGL displays; camera transmission remains independently
selectable. P4 has its own hardware-oriented renderer and separate JPEG/H.264
profiles. A renderer owns scaling, buffers and presentation, while VoIP owns
encoded-frame transport.

SIP uses UDP/TCP and media uses RTP/UDP. Deploy plaintext SIP/RTP on a trusted
LAN/VPN or behind an appropriate firewall/SBC. Hold/resume follows negotiated
media direction; unsupported session changes preserve the established call.
Audio-only builds omit video dependencies, and absent audio directions omit
the corresponding media task. Optional registration, MWI, BLF and directory
features are selected at compile time.
