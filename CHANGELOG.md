# Changelog

## Unreleased

- Add an on-demand runtime diagnostic action that works without verbose audio tracing.
- Exercise failure cleanup and retries with behavioral tests.


## 2026.10.0-dev: native ESP phone integration

This development preview accompanies Intercom 2026.10.0-dev and requires ESPHome 2026.9.0 or newer with the maintained profiles.

- Home Assistant discovery, phonebook delivery and call actions are built into the component. Remove the retired VoIP HA packages and enable `api: custom_services: true` as described in the migration guide.
- Audio accepted only partially by a speaker is retained for the next write.
- P4 JPEG calls make better use of the existing display area.
- P4 clears the video-call display state before deferred cleanup, including when the next destination supports audio only.

PCM and Opus remain separate firmware choices. Full and P4 profiles retain PCM. The component continues to work with native ESPHome microphone/speaker components and with ESP Audio Stack.

Direct calls were retested between Waveshare S3 Audio and Spotpear alongside the updated full-profile packages.

[Migration instructions](https://github.com/n-IA-hane/esphome-intercom/blob/dev/docs/ESP_ENTITY_SURFACE.md) and [complete platform preview](https://github.com/n-IA-hane/esphome-intercom/releases/tag/v2026.10.0-dev).

Thanks to everyone who donated to support the project.

---

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
