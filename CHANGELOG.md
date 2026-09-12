# Changelog

## ESPHome VoIP Stack 2026.9.2

This release fixes microphone-only calls and silent playback after a speaker has become idle.

- Microphone-only calls no longer end because no incoming audio is expected.
- Incoming audio can restart an idle native ESPHome speaker, including when speech begins after a delay.
- SIP receive memory is shared within the signaling task to avoid stack overflow in minimal profiles.
- SIP task control data stays in internal RAM.
- Ordinary two-way SDP answers select a format supported in both directions. Explicit directional negotiation retains separate transmit and receive rates.

PCM and Opus remain separate firmware choices. Existing Full and P4 profiles retain PCM. Rebuild and upload firmware to receive these fixes.

Thanks to @TheEris and @Dovapi for the detailed reports and real-device confirmation.

---

## ESPHome VoIP Stack 2026.9.1

This stable release accompanies ESPHome Intercom 2026.9.1, adding Opus support for supported VoIP-only profiles and improving audio playback and call controls.

### Improvements

- Supported VoIP-only profiles can use Opus. Maintained Full and P4 profiles continue to use PCM.
- Compatible phones can call directly. When audio formats are incompatible, a configured Home Assistant peer can provide transcoding.
- The media-route text sensor can report `direct` or `ha_transcoding` for an established call.
- The RTP jitter buffer preserves its established playback state between packet arrivals.
- Camera-send preferences are preserved before and between calls.
- H.264 source and renderer components share one dependency adapter.

Each firmware uses its configured audio format; it does not silently switch between PCM and Opus. Use the maintained profile for your device to keep audio and video settings consistent.

[Platform release notes and update instructions](https://github.com/n-IA-hane/esphome-intercom/releases/tag/v2026.9.1)


## 2026.9.0, 2026-08-29

Audio-only YAML from the 2026.8.0 profiles remains compatible. Video and the
new memory-placement controls are opt-in and compile-time gated.

### Added

- Bidirectional RTP/JPEG and H.264 SIP video for ESP32-P4, with independent
  encoded video sources and one public renderer.
- Initial video offers and audio-first video added or removed through standard
  in-dialog SIP negotiation.
- Configurable audio task stack sizes, optional PSRAM placement for audio task
  stacks and signaling buffers, and bounded video RTP payload sizing.
- Video lifecycle triggers and compile-time diagnostics for encoded, received,
  presented and discarded frames.

### Changed

- SIP parsing, SDP negotiation and message rendering now have focused owners
  instead of sharing one signaling implementation.
- Call termination uses one event-driven cleanup policy. Public idle is
  published only after signaling, RTP, media tasks and buffers are reusable.
- Video cadence is paced once by the RTP media clock and carried through
  capture, transport, decode and presentation without competing timers.
- P4 camera, H.264 and display work use bounded queues, persistent workers and
  reusable buffers.

### Fixed

- Preserve asymmetric audio and video contracts, connected SIP identities and
  standard display names across call updates.
- Preserve established audio while video is added, removed or rejected.
- Release large SIP payloads and media resources after each call without
  making immediate redial race the previous cleanup.
- Recover from unanswered calls, transient UDP pressure and remote video
  removal without leaving the endpoint busy.

### Removed

- The unused private `ring_buffer` fork. ESPHome's supported buffer primitives
  remain the only ring buffer implementation consumed by this component.
