# Message Waiting Indication

Available in the **2026.11.1-dev preview**. Choose the source of mailbox
state independently of the audio and display:

| Mailbox owner | Opt-in | Delivery |
| --- | --- | --- |
| Home Assistant VoIP Stack | `ha_integration.message_waiting: true` | Authenticated ESPHome API, with no SIP registration |
| Asterisk, FreeSWITCH or another registrar | `registration.message_waiting: true` | Standard SIP `SUBSCRIBE` and `NOTIFY` |
| Both | Enable both settings | Independent snapshots, with one combined waiting condition |

## Home Assistant mailbox, without REGISTER

Add this to an existing VoIP configuration that already connects to Home
Assistant through the ESPHome API. Configure the ESP's mailbox in Home
Assistant VoIP Stack and use the coordinated development integration.

```yaml
api:
  custom_services: true

voip_stack:
  id: phone
  ha_integration:
    message_waiting: true
  on_message_waiting:
    then:
      - lambda: |-
          ESP_LOGI("phone.mwi", "%s: available=%u waiting=%u new=%u old=%u",
                   account_id.c_str(), available, waiting,
                   static_cast<unsigned>(new_messages),
                   static_cast<unsigned>(old_messages));
```

The account ID is `direct`. Home Assistant owns the archive and pushes its
snapshot on setup, mailbox changes and ESP reconnection. Disabling or removing
the mailbox clears the indication. Unloading the integration or losing its
own authenticated API connection makes that source unavailable. Disconnecting
an unrelated debugger leaves the HA mailbox indication intact.

The native API action is `set_message_waiting`, exposed as
`esphome.<node_name>_set_message_waiting`. Its fields are `available`, `waiting`,
`new_messages`, `old_messages`, `source_client_info` and `source_client_address`.
The HA bridge supplies the identity of its active authenticated API client;
the ESP verifies that identity before accepting the snapshot. Ordinary mailbox
use needs no manual action calls, new task, polling or REGISTER.

## Registered PBX mailbox

MWI reports the mailbox state using SIP `SUBSCRIBE` and `NOTIFY` with the
`message-summary` event package. A successful `REGISTER` makes the phone
reachable; mailbox notification is a separate subscription.

Enable MWI explicitly in the optional registration configuration:

```yaml
voip_stack:
  id: phone
  registration:
    max_accounts: 2
    message_waiting: true
    accounts:
      - account_id: office
        registrar: pbx.local
        username: "100"
        password: !secret pbx_password
  on_message_waiting:
    then:
      - lambda: |-
          ESP_LOGI("phone.mwi", "Account %s: available=%u waiting=%u new=%u old=%u",
                   account_id.c_str(), available, waiting,
                   static_cast<unsigned>(new_messages),
                   static_cast<unsigned>(old_messages));
```

The callback provides `account_id`, `available`, `waiting`, `new_messages`,
`old_messages`, `urgent_new` and `urgent_old`. Use it to update an indicator or
publish ordinary ESPHome entities. `available: false` means that a valid mailbox
state is unavailable, including while waiting for the first notification or
when the PBX does not support the subscription. `available: true` with
`waiting: false` is a valid mailbox-empty indication.

The `voip_stack.has_waiting_messages` condition accepts an optional `account_id`.
Without an account ID it checks every enabled source, including `direct` when
the HA bridge is enabled. Select `account_id: direct` to inspect only the HA
mailbox. Advanced lambdas can read one snapshot with
`id(phone).get_message_waiting("office")` or
`id(phone).get_message_waiting("direct")`.

Keep these meanings distinct when building a display:

| Information | Current ESP interface |
| --- | --- |
| This ESP's call state | Local call triggers, conditions and state entities |
| Messages in the HA-owned mailbox | `on_message_waiting` with `account_id: direct`, the condition and the snapshot |
| Messages in a registered account's mailbox | `on_message_waiting`, `has_waiting_messages` and the account snapshot |
| Busy or ringing state of other extensions through BLF | Remote-extension subscriptions and watchlists are outside the current ESP API |

The local phonebook stores call destinations. MWI reports the
selected HA mailbox or registered account mailbox. It does not subscribe to
every phonebook contact. The
optional Home Assistant backend provides its own BLF and dashboard features.

Both `message_waiting` settings default to false. SIP subscription state and
protocol handling are included only when `registration.message_waiting` is
true. The HA snapshot adapter is included only when
`ha_integration.message_waiting` is true. With both disabled, the shared MWI
triggers and conditions are excluded as well. Audio and registration retain
their existing behavior.

## Protocol and ownership

Each account has its own bounded subscription state. Call-ID, tags, CSeq,
Digest authorization and subscription deadlines are independent of the
registration binding, call dialog and REFER subscription. The existing SIP
worker drives deadlines; MWI creates no task and processes no PCM.

Subscription requests reuse the account's configured signaling destination and
transport. Authenticated SUBSCRIBE uses the shared Digest implementation,
including proxy authentication. The first accepted NOTIFY establishes the
subscription dialog and its Record-Route set, even if it arrives before the
SUBSCRIBE response. A different fork cannot replace the selected dialog.

Notifications must match the outstanding subscription, trusted peer and
transport/flow. Unknown unsolicited `message-summary` notifications receive
481. A notifier may open a new TCP connection to the ESP Contact: valid dialog
tags and the trusted PBX address identify that request, while a connection
assigned to a different account is rejected. Accepted notifications are
acknowledged before the application callback;
duplicates do not publish the same state twice, and older CSeq values do not
replace newer state.

The parser accepts `application/simple-message-summary`, including the boolean
status, optional voice counts and urgent counts. Counts above the RFC maximum
saturate at 4294967295. Malformed notifications leave the preceding state
unchanged. A PBX rejection such as 489 makes MWI unavailable without failing
registration or terminating a call.

Subscriptions refresh before expiry. Timer N bounds the wait for a NOTIFY;
network failures use bounded backoff. A temporary refresh rejection preserves
the existing dialog and state until its known expiry; Retry-After remains in
effect even if that lease expires first. Terminal responses end that dialog.
Unsubscription uses Expires 0 and accepts
the terminal NOTIFY, including retransmission of that notification. Mailbox
playback remains an ordinary call to the PBX's configured mailbox-access
number on the chosen line; MWI only reports message state.

## Verification

The maintained tests exchange SUBSCRIBE, responses and NOTIFY over real local
UDP and TCP sockets using the production engine with ASan/UBSan. They cover
Digest results against an independent hash implementation, early NOTIFY,
peer/tag/flow mismatch, duplicate and old CSeq, refresh and expiry, 423/481/489,
Expires 0, independent accounts and runtime activation of a boot-disabled
account. Compile-off checks verify that MWI emits no code when disabled.

These software checks are separate from a real PBX mailbox and physical ESP
qualification.

Protocol references: [RFC 3842](https://www.rfc-editor.org/rfc/rfc3842.html),
[RFC 6665](https://www.rfc-editor.org/rfc/rfc6665.html).
