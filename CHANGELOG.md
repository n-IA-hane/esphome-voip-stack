# Changelog

## 2026.11.0-dev

Changes since stable **2026.10.1**.

### New call and video capabilities

- Receive blind in-dialog audio REFER over UDP or TCP, including mixed-transport
  handoffs to an IPv4 SIP destination. Reuse the existing SIP task and media
  owner, report the real result through NOTIFY, and retain the original caller
  on failure when that caller is still connected. Hostname targets, video
  transfers, attended-transfer URI headers and Digest-authenticated retry are
  outside this receiver's supported scope.
- Negotiate different PCM transmit and receive rates with ordinary SIP peers.
  Keep accepted receive payload mappings through jitter reordering and convert
  supported alternate S16 rates to the selected speaker format.
- Accept negotiated PCM/Opus fallback within the supported shared decoded PCM
  geometry. Each received payload selects its negotiated decoder. PCM-only
  receive-rate conversion remains available.
- Finalize delayed-offer media only after the matching ACK supplies its answer,
  including packet time, converter configuration and microphone ownership.
- Add native HA API actions `start_call_via_ha(dest, call_id)` and
  `hangup_call_if_id(call_id)` for one-call trunk selection and scoped
  cancellation. Existing call-control actions retain their names.
- Transmit JPEG from native ESPHome cameras on ESP32-S3, including headless
  devices. Remote-video rendering on S3 is future work. ESP32-P4 retains camera
  transmission and remote-video decode/display; JPEG and H.264 are separate
  compile-time profiles. Devices with both audio endpoints retain two-way audio.
  Based on [#6](https://github.com/n-IA-hane/esphome-voip-stack/pull/6).

### Reliability and resource use

- Ignore late local termination for a retired dialog, and reject foreign dialog
  tags before they can replace the active call identity. Crossed hangups no
  longer recreate an already-retired conversation.
- Close failed incoming media/ringing setup with the owned transaction's final
  response and release resources for the next call. Close confirmed outgoing
  dialogs with BYE when local audio preparation fails.
- Keep an accepted REFER attempt alive after the original caller hangs up.
  Suspend the RTP inactivity watchdog only while the replacement is still
  ringing, retaining SIP deadlines and restoring the watchdog after connection.
- Preserve each dialog's TCP flow and transport across handoff, EOF and cleanup.
  Replay retired UDP transactions over their original transport.
- Advertise only the audio directions physically configured. Microphone-only
  builds exclude speaker reset helpers; speaker-only builds omit TX processing.
- Keep independent DC-filter state for each native stereo microphone channel.
- Size persistent JPEG receive buffers for their codec and actual direction.
  Retain experimental H.264 resources when a timed-out worker still owns them.
- Read lightweight call-health counters without the diagnostic dialog lock,
  distinguish discard events from discarded frames, and suppress optional
  Wi-Fi roaming scans while the native media guard owns them.
- Follow the released ESPHome ring-buffer interfaces and qualify POSIX socket
  waits for the updated namespace layout. Local hangup logs identify their
  calling location for diagnosis.

### Breaking changes and custom YAML migration

- **ESPHome 2026.10.0 or newer is required.** Update the builder before compiling.
- Within each direction, Opus entries must share decoded PCM sample rate, sample
  format and channel count. When RX mixes PCM and Opus, its PCM alternatives
  must share that geometry too. Incompatible lists now fail validation. Packet
  times may differ; PCM-only multi-rate reception remains supported.
- With native HA integration enabled, remove custom API declarations named
  `start_call_via_ha` and `hangup_call_if_id`, now supplied by the component.

Existing microphone/speaker bindings and ordinary call-control names remain
valid. One-way SDP follows the actual hardware; settings for absent directions
remain accepted and are ignored. The migration from copied HA packages already
shipped in 2026.10.0 and is not a new requirement for an updated 2026.10.1 YAML.

Use the matching maintained Intercom device YAML when adopting its packages,
then rebuild and upload. Component test results and individual device checks
retain their documented scope; they do not qualify every PBX or long-uptime
workload. See the [upgrade steps](README.md#upgrading-from-2026101).

## 2026.10.1

Changes since stable **2026.10.0**.

### ☎️ Check whether a call is active directly in YAML

The new `voip_stack.is_active` condition lets automations check the component's
existing active-call state without repeating it in a C++ lambda.

It includes setup and ringing as well as an established call. Termination and
terminal states are excluded, so it is more precise than negating `is_idle`.
Use `voip_stack.is_in_call` when an action requires an answered call specifically.

```yaml
- if:
    condition:
      voip_stack.is_active:
    then:
      - voip_stack.stop:
```

### 🧩 Less instance wiring in examples

The updated entity examples and documentation omit redundant parent IDs where
the component already supports automatic binding. Custom configurations can keep
explicit IDs when needed.

### 🧪 Validation

Behavioral tests exercise the new condition against the production call-state
predicate, including setup, ringing, established and terminal states. The updated
component was also used in the Spotpear, WS3 Audio and Waveshare BOX V2 call tests.

### 📦 Updating

Use `main` or tag `v2026.10.1` for the external component, then rebuild and upload.
Requires ESPHome **2026.9.0 or newer**.

[Documentation](https://github.com/n-IA-hane/esphome-voip-stack/blob/main/README.md)

## 2026.10.0: built-in HA phone integration, playback fixes and diagnostics

Changes since stable **2026.9.2**.

### Home Assistant integration built into the component

The maintained entity examples also omit redundant `voip_stack_id` references.
Only one stack instance is supported per ESP, and entity platforms already
resolve it automatically.

Home Assistant discovery metadata, phonebook reception and call-control API
services are now provided by `voip_stack` itself. Custom firmware no longer
needs the separate VoIP HA packages to connect these functions.

**Upgrading your firmware:** start from one of our updated
[maintained YAML profiles](https://github.com/n-IA-hane/esphome-intercom/tree/main/yamls)
and reapply your board settings and customizations. These profiles already
include the migration changes.

**Alternatively, migrate your existing YAML as follows.** Comment out or remove
the entries in `packages:` that include any of these retired files:

- `voip/ha_phone.yaml`
- `voip/ha_integration.yaml`
- `voip/ha_actions.yaml`
- `voip/ha_api.yaml`
- `voip/phonebook_subscribe.yaml`

Leaving these includes enabled causes configuration validation to fail. Also
remove any copied definitions of the call-control actions or managed entities
now supplied by the component.

For full profiles, replace `voip/ha_api_runtime.yaml` with
`runtime/ha_connectivity.yaml`. Keep the `voip_stack:` component configuration
and the hardware, audio, display and ringtone packages.

Then add `custom_services: true` to your existing `api:` block:

```yaml
api:
  custom_services: true
```

Use **ESPHome 2026.9.0 or newer** with the maintained profiles. The native
microphone/speaker example now declares this minimum version and enables the
required API services.

[Migration instructions](https://github.com/n-IA-hane/esphome-intercom/blob/main/docs/ESP_ENTITY_SURFACE.md)

### Audio playback fixes

When a speaker accepts only part of a received audio frame, the remaining samples
are retained and retried instead of being discarded after a few unsuccessful
writes. Pending writes stop when the call ends or the receive stream is reset,
so old audio cannot continue into the replacement stream.

### P4 video display fixes

- Received JPEG video can occupy more of the P4 screen, scaled proportionally to
  the available display area and allocated surface capacity.
- Ending a video call clears its display state immediately, before deferred
  cleanup. A following audio-only call therefore opens the audio interface
  instead of inheriting the previous video screen.

### On-demand diagnostics

The new `voip_stack.dump_diagnostics` action captures call state, SIP status,
negotiated audio formats, RTP counters, queue drops and the last termination
reason. Video builds also report video activity and frame counters.

```yaml
- voip_stack.dump_diagnostics:
    id: voip
```

Use your component's configured ID in place of `voip`. The action uses existing
runtime state and counters; it does not enable verbose audio or packet tracing.
The README now explains which configuration details and diagnostic logs to
include in an issue.

### Build and regression coverage

- Declare the ESP-IDF JSON dependency explicitly.
- Add behavioral tests for partial setup cleanup, retries after allocation and
  socket failures, SIP send failures, TCP recovery, Opus failures, RTP fault
  sequences and diagnostic snapshots.

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
