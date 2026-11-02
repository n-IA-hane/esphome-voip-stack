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

This component requires ESPHome **2026.10.0 or newer**. This
configuration fragment assumes that ``clean_mic`` and ``local_speaker`` already
exist. With native Home Assistant discovery enabled, retain
``api: {custom_services: true}`` alongside the device's API configuration.

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
- **registration.keepalive_interval** (*Optional*, duration or seconds): Send
  ordinary SIP OPTIONS on each registered account's existing signaling flow.
  Defaults to ``15s``. Use ``0`` to disable periodic pings. Capability discovery
  after a new binding remains advisory and does not delay calls.
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

Direct SIP works without registration. The optional ``registration`` block adds
PBX accounts and REGISTER Digest authentication on the existing SIP owner.
Provider trunks, central routing and the Lovelace softphone card are also
provided by the Home Assistant integration in ``esphome-intercom``.

The phonebook controls outbound resolution; it is not an inbound caller
allowlist. SIP has no TLS and RTP has no SRTP on ESP, so expose the listeners
only on a trusted LAN/VPN or behind network admission policy. Session-changing
in-dialog INVITE (including hold) is rejected with ``488`` while the original
dialog remains established and can later receive BYE normally.

Native audio capability discovery
---------------------------------

OPTIONS describes configured audio encodings and directions with SDP audio port
zero. It does not acquire a dialog, decoder or audio device. Packet time is
omitted when the capability union has no single supported value. Outside a
dialog, admission follows the endpoint's call readiness; a matching in-dialog
OPTIONS preserves that dialog and returns 200.

After a new account binding, an advisory OPTIONS query can learn explicit
directional-audio support from peer SDP. Ordinary calls work before discovery,
after rejection, and after discovery timeout. Cached support belongs to the
binding and is invalidated on replacement, expiry or flow/mapping changes.
The INVITE answer selects the actual media format.

Periodic OPTIONS uses the registered signaling flow without renewing the
registration or authenticating a probe challenge. Any matching final response
proves reachability; timeout or a changed observed mapping requests a REGISTER
refresh. This mechanism does not negotiate RFC 5626 outbound and does not prove
that an RTP NAT mapping is usable.

The audio RTP port is bound before publishing an original INVITE and is adopted
by final media preparation. Rejection, cancellation and timeout release that
reservation through the existing cleanup barrier. No audio hardware is started
before the negotiated format is known.

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

DTMF telephone events
---------------------

The SIP transport sends negotiated RFC 4733 telephone events on the existing
RTP path. ``can_send_dtmf()`` describes the current call; ``send_dtmf(digit,
 duration_ms)`` independently checks admission and returns whether the bounded
queue accepted the digit. Calls outside IN_CALL, absent negotiation, unsupported
digits, receive-only media, teardown and a full queue reject the request. There
is no SIP INFO or generated in-band audio fallback.

The YAML action is ``voip_stack.send_dtmf`` with ``digit`` and optional
``duration_ms`` (default 160). Digits 0-9, ``*``, ``#`` and A-D are supported
when included in the peer's event mask. Durations must be 40-8000 milliseconds
and fit 65535 ticks at the negotiated event clock. For example, 8000 milliseconds
cannot be represented at 48000 Hz and is rejected rather than truncated.

.. code-block:: yaml

   script:
     - id: send_key
       then:
         - if:
             condition:
               voip_stack.can_send_dtmf: phone
             then:
               - voip_stack.send_dtmf:
                   id: phone
                   digit: '#'
                   duration_ms: 160

Native Home Assistant integration exposes the ordinary ESPHome ``send_dtmf``
action with a single ``digit`` argument. It uses the same owner and admission
checks as the YAML action.

Optional ``on_dtmf`` delivers a ``digit`` string on the ESPHome main loop,
once per received RTP event. Its queue and callback state are compiled only
when the hook is configured. Each loop turn has a bounded delivery budget.
Audio remains on its existing pipeline while events are transmitted. Equal
clock rates share the audio RTP source; a different negotiated clock uses a
separate RTP source on the same socket according to RFC 7160.

Local request and callback generations prevent queued work from a retired call
entering its successor. They do not authenticate network packets: a very late
RTP packet on an identically reused address/port/payload tuple has no SIP
Call-ID with which to prove its original dialog. Firmware and interoperability
qualification must keep that case separate from local queue cancellation.

Message waiting and mailbox calls
--------------------------------

Optional ``registration.message_waiting: true`` enables RFC 3842
``message-summary`` subscriptions for the configured registration pool. Each
account retains its own availability, waiting flag, new/old and urgent counts,
and ``Message-Account`` URI. ``has_waiting_messages(account_id)`` queries one
account; an empty ID queries whether any account has waiting messages.

``on_message_waiting`` runs on the main loop with ``account_id``, ``available``,
``waiting``, ``new_messages``, ``old_messages``, ``urgent_new`` and ``urgent_old``.
Rapid changes are coalesced per account into the latest state. C++ observers
can use ``add_on_message_waiting_callback`` and query that state on the loop.
The feature adds no SIP task or socket. Its bounded pool follows the signaling
memory policy; requested PSRAM allocation never falls back to internal RAM.

``voip_stack.call_mailbox`` accepts optional ``account_id`` and ``target``.
Without a target it uses that account's current ``Message-Account``. An empty
account ID prefers the selected waiting account, then another waiting account,
then the selected available account. The native API action is
``call_mailbox(account_id, target)``. Both use the ordinary account call path,
including its audio direction and negotiated DTMF. No IMAP client or separate
mailbox player runs on the ESP.

Mailbox targets are validated SIP destinations. For example, a PBX can publish
``sip:*97@pbx.example``; a logical mailbox user containing a colon must encode it
as ``%3A``. Unsupported URI schemes reject the call without changing counts.
Mailbox state and its service URI remain separate from the telephone's identity.

Unregistering retires the event subscription with ``Expires: 0``. The account's
credentials and signaling flow remain owned until the final NOTIFY or bounded
timeout. Removing or replacing that account may return false during retirement;
retry after it completes. Omitting the MWI option compiles out its pool, state,
callbacks and mailbox actions.

Busy lamp field and presence
---------------------------

Optional ``voip_stack.blf`` uses the existing SIP worker and registration
accounts for RFC 4235 ``dialog`` subscriptions. ``max_subscriptions`` bounds
the watched resources from 1 to 64 (default 16). Optional ``presence: true``
adds one independent RFC 3863 subscription for each resource. No additional
task, socket or media decoder is created. ``buffers_in_psram`` defaults to
whether PSRAM is configured; allocation uses the selected heap without fallback.
Retained SIP strings follow the existing signaling memory setting separately.

``get_contact_blf(stable_id)`` returns a small status value without copying
owned identity strings. ``available`` means that the dialog table is coherent;
``state`` reports unknown, idle, calling, ringing, active or held. The separate
``presence`` field reports unknown, open or closed. An idle dialog table does
not establish presence, and closed presence does not clear an active call.

The parser accepts bounded full/partial documents with at most four dialogs,
128-byte dialog IDs and a 4096-byte body. A missing partial version makes call
state unavailable and requests one full refresh. Older documents cannot restore
stale state. A new subscription dialog resets the version fence. Rejected
NOTIFY records and failed response sends cannot commit document state.

``set_blf_watches(vector<BlfWatchConfig>)`` replaces the desired watch list.
Each row contains ``contact_id``, ``account_id`` and a validated SIP resource
``target``. Invalid batches, duplicate IDs and capacity overflow are rejected
before changing the current list. Removed or replaced rows first retire their
old subscriptions. Their credentials and flows remain owned through the final
NOTIFY, Timer N or loss of that flow.

With ``watch_contacts: true`` (default), explicit Phonebook metadata selects
the first eligible resources up to the configured bound. HA roster v2 preserves
its existing keys: ``contacts[].id`` is the dial target,
``metadata.endpoint_id`` supplies the stable identity, and
``metadata.account_id`` selects the SIP account. Explicit kinds esphome,
sip_account and browser enable automatic watches unless disabled. A generic
contact requires an explicit ``auto_blf: true`` provider policy. Unknown,
group, service and automation entries remain excluded. Static contacts can
supply ``contact_id``, ``kind``, ``account_id`` and ``auto_blf`` without LDAP.
An explicit static ``route`` also requires its SIP endpoint ``ip``.

``on_blf_state`` runs on the main loop with ``contact_id``, ``account_id``,
``state`` and ``available``. C++ ``add_on_blf_state_callback`` receives the full
status including independent presence. Notifications are optional, coalesced
per slot into the latest state, and limited to eight slots per loop turn.
A state getter is suitable for periodic UI queries without registering callbacks.

Directed pickup requires an explicitly configured ``pickup_prefix`` understood
by the PBX. ``pickup_contact(stable_id)`` and the corresponding YAML/API action
only originate a normal call when a coherent dialog explicitly identifies an
incoming ringing recipient and the selected account is registered. Outgoing
initiator dialogs, missing direction and stale state do not allow pickup.
The prefix is applied to the contact's validated dial target on that account.
There is no universal pickup code and no implicit Replaces implementation.
Omitting BLF compiles out its subscription rows, cache, callbacks and actions.

Connected peer and presentation
-------------------------------

``get_active_peer_name()`` returns the remote party of the owned call identity,
including calling and ringing, independently of the selected Phonebook contact.
It returns an empty string after that identity is cleared. An accepted connected
identity update changes this value without starting a new call transition.

Optional ``on_peer_changed`` receives ``peer`` on the main loop after the owned
dialog's connected identity changes. Use it to refresh labels. ``on_in_call``
remains the connected transition and can start the call duration timer once;
``on_idle`` remains the lifecycle reset. Changing a contact selection does not
change the current peer. Omitting the automation removes its trigger storage.

With the switch component enabled, ``get_conference_ring_switch() const``
returns the optional registered conference setting as a borrowed pointer. Check
for ``nullptr`` when no switch entity was configured; the getter creates none.
