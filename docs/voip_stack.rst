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

.. code-block:: yaml

    external_components:
      - source: github://n-IA-hane/esphome-voip-stack@main
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
- speaker only: ``speaker_only``;
- neither microphone nor speaker: ``control_only``.

The mode is published in the endpoint text sensor so peers and Home Assistant do
not attempt an audio direction that the device cannot provide.

Configuration variables
-----------------------

- **id** (*Optional*, :ref:`config-id`): Manually specify the ID.
- **transport** (*Optional*, string): SIP signaling transport, ``udp`` or
  ``tcp``. RTP media is always UDP. Defaults to ``udp``.
- **sip_port** (*Optional*, int): Local SIP port. Defaults to ``5060``.
- **rtp_port** (*Optional*, int): Local RTP port. Defaults to ``40000``.
- **udp_max_payload** (*Optional*, int): RTP payload budget. Defaults to
  ``1200``.
- **microphone** (*Optional*, :ref:`config-id`): ESPHome microphone used for TX.
- **microphone_source** (*Optional*): Microphone source block with microphone
  ID, bit-depth and channel selection.
- **speaker** (*Optional*, :ref:`config-id`): ESPHome speaker used for RX.
- **audio.tx** / **audio.rx** (*Optional*): Per-direction PCM format contract.
  Each direction supports ``auto`` or explicit ``sample_rate``, ``pcm_format``,
  ``channels`` and ``frame_ms``.
- **audio.tx_formats** / **audio.rx_formats** (*Optional*, list): Additional
  packet-time reframes advertised in SDP.
- **static_contacts** (*Optional*, list): Local outbound dial plan entries.
  Each contact requires ``name`` and may include ``ip``, ``port``, ``rtp_port``
  and ``sip_transport``.
- **extension** (*Optional*, string): Local SIP extension/user part.
- **use_ha_as_first_contact** (*Optional*, boolean): Put the Home Assistant peer
  first in the dial plan. Defaults to ``false``.
- **ha_phonebook_text_sensor_id** (*Optional*, :ref:`config-id`): Text sensor
  containing the HA-managed roster JSON.
- **delete_contact_missing_from** (*Optional*): Roster cleanup policy.
- **ringing_timeout** / **calling_timeout** (*Optional*, time): Guard timers.
- **auto_entities** (*Optional*, boolean): Create common switches, buttons and
  numbers automatically. Defaults to ``false``.
- **dc_offset_removal** (*Optional*, boolean): Remove DC bias on TX.
- **audio_debug** (*Optional*, boolean): Verbose audio diagnostics.
- **buffers_in_psram** (*Optional*, boolean): Store VoIP-owned staging buffers in
  PSRAM. Defaults to ``false``.
- **task_stacks_in_psram** (*Optional*, boolean): Store VoIP task stacks in
  PSRAM where supported. Defaults to ``false``.
- **network_socket_headroom** (*Optional*, int): Extra lwIP sockets reserved at
  validation time. Defaults to ``0``.

Triggers
--------

- **on_ringing**: Inbound call starts ringing.
- **on_calling**: Outbound call starts.
- **on_dest_ringing**: Remote side reports ringing.
- **on_in_call**: Media is established.
- **on_hangup**: Call ends normally.
- **on_call_failed**: Call ends in a failure terminal state.
- **on_idle**: FSM returns to idle.
- **on_incoming_call**: Inbound INVITE parsed. Provides ``call_id``, ``caller``,
  ``callee`` and ``uri``.
- **on_outgoing_call**: Outbound call placed.
- **on_bridge_request**: ESP-originated route explicitly targets HA.
- **on_destination_changed**: Selected contact changed.
- **on_phonebook_update**: Contact list changed.

Actions
-------

Call control: ``voip_stack.start``, ``voip_stack.stop``,
``voip_stack.call_toggle``, ``voip_stack.answer_call``,
``voip_stack.decline_call``, ``voip_stack.call_contact`` and
``voip_stack.set_remote_endpoint``.

Phonebook: ``voip_stack.next_contact``, ``voip_stack.prev_contact``,
``voip_stack.add_contact``, ``voip_stack.remove_contact``,
``voip_stack.set_contact``, ``voip_stack.set_contacts``,
``voip_stack.flush_contacts``, ``voip_stack.update_contacts``,
``voip_stack.set_roster_json`` and ``voip_stack.set_ha_peer_name``.

Audio and diagnostics: ``voip_stack.set_volume``,
``voip_stack.set_mic_gain_db`` and ``voip_stack.publish_entity_states``.

Conditions
----------

``voip_stack.is_idle``, ``voip_stack.is_ringing``,
``voip_stack.is_in_call``, ``voip_stack.is_calling``,
``voip_stack.is_incoming`` and ``voip_stack.destination_is``.

Boundaries
----------

ESP devices are SIP peers, not registrar clients. There is no SIP registration
or digest authentication support in the ESP component. Provider trunks,
registrar accounts, central routing and the Lovelace softphone card are provided
by the Home Assistant integration in ``esphome-intercom``.

See Also
--------

- :doc:`/components/microphone/index`
- :doc:`/components/speaker/index`
- :doc:`/components/voice_assistant`
- :doc:`/components/micro_wake_word`
- :apiref:`voip_stack/voip_stack.h`
- :ghedit:`Edit`
