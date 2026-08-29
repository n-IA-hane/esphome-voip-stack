# Changelog

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
