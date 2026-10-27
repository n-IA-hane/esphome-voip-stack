VoIP Stack
==========

.. seo::
    :description: Instructions for the ESPHome VoIP Stack SIP/SDP/RTP endpoint component.

The ``voip_stack`` component turns an ESP32 ESPHome device into a SIP endpoint.
It handles SIP signaling over UDP or TCP, SDP offer/answer, RTP media over UDP,
phonebook selection, call-state automation triggers, endpoint identity
publishing and diagnostic snapshots.

The component binds to standard ESPHome ``microphone`` and ``speaker``
components. It does not own audio hardware or echo cancellation. For codec
ownership, TDM layouts or software AEC/AFE, use the ``esp_audio_stack``
component and pass its clean microphone and speaker surfaces to ``voip_stack``.

The development component requires ESPHome **2026.10.0 or newer**. This
configuration fragment assumes that ``clean_mic`` and ``local_speaker`` already
exist. With native Home Assistant discovery enabled, retain
``api: {custom_services: true}`` alongside the device's API configuration.

.. code-block:: yaml

    external_components:
      - source: github://n-IA-hane/esphome-voip-stack@dev
        components: [voip_stack]

    voip_stack:
      id: phone
      transport: udp
      sip_port: 5060
      rtp_port: 40000
      microphone: clean_mic
      speaker: local_speaker
      audio:
        tx: auto
        rx:
          sample_rate: 16000
          pcm_format: s16le
          channels: 1
          frame_ms: 20

Endpoint capabilities
---------------------

The published endpoint mode is inferred from YAML wiring:

- microphone plus speaker: ``full_duplex``;
- microphone only: ``mic_only``;
- speaker only: ``speaker_only``.

The mode is published in the endpoint text sensor so peers and Home Assistant do
not attempt an audio direction that the device cannot provide. At least one
audio direction is required; a signaling-only endpoint is not supported.
Microphone-only offers advertise ``sendonly`` and speaker-only offers
``recvonly``. Settings for absent hardware do not add a fictitious direction.

Configuration variables
-----------------------

- **id** (*Optional*, :ref:`config-id`): Manually specify the ID.
- **transport** (*Optional*, string): SIP signaling transport, ``udp`` or
  ``tcp``. RTP media is always UDP. Defaults to ``udp``.
- **sip_port** (*Optional*, int): Local SIP port. Defaults to ``5060``.
- **rtp_port** (*Optional*, int): Local RTP port. Defaults to ``40000``.
- **udp_max_payload** (*Optional*, int): RTP payload budget in the accepted
  range ``576..1488``. Defaults to ``1200``.
- **microphone** (*Optional*, :ref:`config-id`): ESPHome microphone used for TX.
- **microphone_source** (*Optional*): Microphone source block with microphone
  ID, bit-depth and channel selection.
- **speaker** (*Optional*, :ref:`config-id`): ESPHome speaker used for RX.
- **audio.tx** / **audio.rx** (*Optional*): Per-direction PCM format contract.
  Each direction supports ``auto`` or explicit ``sample_rate``, ``pcm_format``,
  ``channels`` and ``frame_ms``.
- **audio.tx_formats** / **audio.rx_formats** (*Optional*, list): Up to seven
  additional explicit capabilities advertised in SDP. TX entries retain its
  decoded PCM geometry and can select supported codec/packet-time alternatives.
  RX entries may describe other supported speaker-side formats. Opus entries
  within one direction share decoded PCM rate, sample format and channels;
  mixed PCM/Opus RX entries share that same geometry. PCM-only receive-rate
  conversion remains supported.
- **static_contacts** (*Optional*, list): Local outbound dial plan entries.
  Each structured contact requires ``name`` and may include ``ip``, ``port``,
  ``rtp_port`` and ``transport``. A pre-serialized ``entry`` is also accepted.
- **extension** (*Optional*, string): Local SIP extension/user part.
- **conference_groups** / **ring_groups** (*Optional*, string): One HA-managed
  group name or a comma-separated membership list.
- **conference_ring** (*Optional*, boolean): Ring when another participant
  starts one of this endpoint's conference groups. Requires
  ``conference_groups``. Defaults to ``false``.
- **use_ha_as_first_contact** (*Optional*, boolean): Put the Home Assistant peer
  first in the dial plan. Defaults to ``false``.
- **ha_phonebook_text_sensor_id** (*Optional*, :ref:`config-id`): Text sensor
  containing the HA-managed roster JSON.
- **delete_contact_missing_from** (*Optional*): Roster cleanup policy.
- **ringing_timeout** / **calling_timeout** (*Optional*, time): Guard timers.
- **dc_offset_removal** (*Optional*, boolean): Remove DC bias on TX.
- **audio_debug** (*Optional*, boolean): Verbose audio diagnostics.
- **buffers_in_psram** (*Optional*, boolean): Store VoIP-owned staging buffers in
  PSRAM. Defaults to ``false``.
- **task_stacks_in_psram** (*Optional*, boolean): Store VoIP task stacks in
  PSRAM where supported. Defaults to ``false``.
- **audio_task_stacks_in_psram** (*Optional*, boolean): Independently store
  realtime TX/RX task stacks in PSRAM. Defaults to ``false``.
- **network_socket_headroom** (*Optional*, int): Extra lwIP sockets reserved at
  validation time. Defaults to ``0``.

Triggers
--------

- **on_ringing**: Inbound call starts ringing. Provides ``peer``.
- **on_calling**: Outbound call starts. Provides ``peer``.
- **on_dest_ringing**: Remote side reports ringing. Provides ``peer``.
- **on_in_call**: Media is established. Provides ``peer``.
- **on_hangup**: Call ends normally. Provides ``peer`` and ``reason``.
- **on_call_failed**: Call ends in a failure terminal state. Provides ``peer`` and ``reason``.
- **on_idle**: FSM returns to idle.
- **on_incoming_call**: Inbound INVITE parsed. Provides ``call_id``, ``caller``,
  ``callee`` and ``uri``.
- **on_outgoing_call**: Outbound call placed.
- **on_bridge_request**: ESP-originated route explicitly targets HA.
- **on_destination_changed**: Selected contact changed. Provides ``destination``.
- **on_phonebook_update**: Contact list changed. Provides ``destination``.

Actions
-------

Call control: ``voip_stack.start``, ``voip_stack.stop``,
``voip_stack.call_toggle``, ``voip_stack.answer_call``,
``voip_stack.decline_call``, ``voip_stack.call`` and
``voip_stack.set_remote_endpoint``.

Phonebook: ``voip_stack.next_contact``, ``voip_stack.prev_contact``,
``voip_stack.add_contact``, ``voip_stack.remove_contact``,
``voip_stack.set_contact``, ``voip_stack.set_contacts``,
``voip_stack.flush_contacts``, ``voip_stack.update_contacts``,
``voip_stack.set_roster_json`` and ``voip_stack.set_ha_peer_name``.

Audio and diagnostics: ``voip_stack.set_volume``,
``voip_stack.set_mic_gain_db``, ``voip_stack.publish_entity_states`` and
``voip_stack.dump_diagnostics``.

Conditions
----------

``voip_stack.is_idle``, ``voip_stack.is_active``, ``voip_stack.is_ringing``,
``voip_stack.is_in_call``, ``voip_stack.is_calling``,
``voip_stack.is_incoming``, ``voip_stack.destination_is`` and
``voip_stack.is_ha_destination``.

Boundaries
----------

ESP devices are SIP peers, not registrar clients. There is no SIP registration
or digest authentication support in the ESP component. Provider trunks,
registrar accounts, central routing and the Lovelace softphone card are provided
by the Home Assistant integration in ``esphome-intercom``.

The phonebook controls outbound resolution; it is not an inbound caller
allowlist. SIP has no TLS and RTP has no SRTP on ESP, so expose the listeners
only on a trusted LAN/VPN or behind network admission policy. Session-changing
in-dialog INVITE (including hold) is rejected with ``488`` while the original
dialog remains established and can later receive BYE normally.

Correlated Home Assistant calls
-------------------------------

With the native HA API adapter enabled, ``start_call_via_ha`` accepts ``dest``
and ``call_id``. It routes that destination through the configured HA peer and
uses the supplied globally unique SIP Call-ID. Home Assistant uses the identity
to bind an explicit trunk choice to one actual call. ``hangup_call_if_id`` ends
only a call whose current identity matches exactly. Empty, stale and differently
cased identifiers cannot hang up another call.

The existing ``start_call`` action remains available for ordinary phonebook
calls. Neither action changes the audio pipeline. The correlated identity is
passed as a call-setup argument, without a persistent next-call override.
With native HA integration enabled, remove custom API actions that duplicate
these two component-provided names when upgrading from 2026.10.1.

Video scope
-----------

An existing native ESPHome ``esp32_camera`` can transmit JPEG on ESP32-S3.
Displaying the remote video on an S3 screen remains future work. ESP32-P4
adapters support both camera transmission and remote-video decode/display.
JPEG and H.264 are separate compile-time profiles. Two-way audio is retained
on devices with both microphone and speaker.


Call-transfer scope
-------------------

Established IPv4 audio calls can accept blind in-dialog REFER over UDP or TCP,
including mixed-transport handoffs. The current caller remains available while
the destination rings, unless that caller hangs up. Local hangup cancels the
attempt; the real result is reported with NOTIFY. Video transfers, hostname
targets, attended-transfer URI headers and Digest-authenticated retry are
outside this receiver's supported scope.

See Also
--------

- :doc:`/components/microphone/index`
- :doc:`/components/speaker/index`
- :doc:`/components/voice_assistant`
- :doc:`/components/micro_wake_word`
- :apiref:`voip_stack/voip_stack.h`
- :ghedit:`Edit`
