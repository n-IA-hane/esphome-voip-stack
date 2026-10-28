# ESP VoIP Stack 2026.11.1-dev

## Use your ESP as a registered PBX phone

Optional SIP REGISTER accounts let the same firmware connect to PBXs such as
Asterisk and FreeSWITCH. Accounts support UDP or TCP, automatic renewal and
Digest authentication with MD5 or SHA-256, including their session variants.

- Keep direct Home Assistant calling and add registered PBX accounts alongside it.
- For a PBX-only endpoint, set `registration.accept_direct_calls: false`.
  The SIP listener stays available for incoming PBX calls.
- Choose an outgoing account with `registration.default_line`, `voip_stack.select_line`, or
  `voip_stack.start_call_on_line`. Existing HA configurations need no line setting.
- Keep several accounts registered while one physical call uses the ESP's audio.
  Changing the selected line applies to future calls.

The account pool defaults to two slots; `max_accounts` accepts one through four.
Omitting `registration` excludes its account, authentication and timer support
from the firmware.

## Configure accounts in YAML or from Home Assistant

Declare accounts in YAML, or provision them through the ESPHome native actions
exposed by the HA adapter. Register, unregister, replace, remove or reset an
account without maintaining a separate firmware for each PBX identity.

Runtime edits are temporary by default. Use `persist: true` when an account
change should survive a restart; reset clears the saved override and restores
the YAML baseline. `on_registration_state` and `voip_stack.is_registered` let
automations react to the actual binding state.

[PBX configuration, actions and persistence guide](https://github.com/n-IA-hane/esphome-voip-stack/blob/dev/docs/SIP_REGISTRATION.md)

## Message waiting from your PBX

Enable `registration.message_waiting: true` to receive standard RFC 3842
`message-summary` notifications for registered accounts.

`on_message_waiting` provides the account, availability, waiting indicator and
voice-message counts, including urgent messages. Use `voip_stack.has_waiting_messages`
in YAML conditions to drive your own LED, badge or display automation.
Mailbox updates have their own subscription lifecycle alongside phone calls.
MWI is optional and excluded from firmware when disabled.

[Message waiting configuration](https://github.com/n-IA-hane/esphome-voip-stack/blob/dev/docs/SIP_MWI.md)

## JPEG video reception for S3 displays

The new `esp_video_renderer` decodes baseline JPEG into an LVGL display surface.
Use its optional adapter with `voip_stack.video.sink`, or feed complete JPEG
images from another producer without loading VoIP.

The renderer defaults to a 320x240 display surface and up to two admitted frames
per second. Bounded native, half-size and quarter-size decoding fits larger
JPEG frames into that surface within the documented input limits. Buffering
keeps the latest queued image, and session changes discard stale decoded frames.
The existing P4 renderer remains the hardware-oriented choice for P4 displays.

Tune the worker and memory placement with `task_core`, `task_priority`,
`task_stack_size`, `task_stack_in_psram` and `buffer_in_psram`. Image dimensions,
`framerate` and `max_jpeg_bytes` provide the corresponding image budget controls.

[Renderer setup and resource options](https://github.com/n-IA-hane/esphome-voip-stack/blob/dev/docs/ESP_VIDEO_RENDERER.md)



## Remote hold and resume

SIP peers can hold and resume an established call using standard media
directions. The call keeps its prepared audio resources while RTP transmission
and reception follow the negotiated direction. Held calls remain available
without triggering the receive-loss timeout; resuming continues the same call.

The existing wake-word listening policy continues during audio hold.

## More reliable SIP negotiation and Home Assistant discovery

Standard bidirectional SIP offers advertise formats the ESP can both send and
receive. Peers can select any advertised codec; endpoints sharing the explicit
directional capability retain independent transmit and receive formats.

Home Assistant endpoint descriptions retain all declared formats in a compact
state. ESPHome validates its 255-character limit before compilation and explains
which label to shorten. Runtime extension edits preserve the previous value
when the endpoint description would exceed that limit.

## Compatibility and migration

PBX accounts, message waiting and the JPEG renderer are opt-in. Existing direct
HA calling keeps its current configuration. Add the desired YAML blocks and
external components, then rebuild and upload the ESP firmware. The SDP, discovery and
remote-hold improvements apply when using the updated firmware.
