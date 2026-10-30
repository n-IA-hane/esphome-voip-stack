# Message Waiting Indication

MWI reports mailbox state using standard SIP `SUBSCRIBE` and `NOTIFY` with
RFC 3842 `message-summary`. It provides the waiting indicator and optional new,
old and urgent counts. The separate [IMAP client](PHONE_SERVICES.md#imap-visual-voicemail)
provides a message catalog and audio playback.

## Home Assistant mailbox

The coordinated HA phone packages provision the managed SIP account `ha` and
enable MWI. A custom board can opt in using its existing microphone and speaker:

```yaml
api:
  custom_services: true

voip_stack:
  id: phone
  ha_integration:
    auto_register: true
  on_message_waiting:
    then:
      - lambda: |-
          ESP_LOGI("phone.mwi", "%s: available=%u waiting=%u new=%u old=%u",
                   account_id.c_str(), available, waiting,
                   static_cast<unsigned>(new_messages),
                   static_cast<unsigned>(old_messages));
```

HA generates the account credential and provisions the existing native account
action. The phone then uses standard REGISTER and MWI subscription traffic. The
native API carries configuration; SIP carries mailbox notifications. The
account ID for this mailbox is `ha`.

Earlier development configurations using `ha_integration.message_waiting`
should switch to `auto_register: true` and rebuild with the coordinated HA
integration. The old native `set_message_waiting` channel has been retired.
Generic direct SIP remains available with automatic registration disabled.

## External PBX mailbox

```yaml
voip_stack:
  id: phone
  ha_integration: false
  registration:
    max_accounts: 1
    message_waiting: true
    default_line: office
    accounts:
      - account_id: office
        registrar: pbx.example.org
        username: "100"
        password: !secret pbx_password
```

A successful REGISTER makes the phone reachable. The independent MWI
subscription asks the PBX for mailbox notifications; the PBX must support and
authorize that event package.

The callback provides `account_id`, `available`, `waiting`, `new_messages`,
`old_messages`, `urgent_new` and `urgent_old`. An unavailable subscription and a
valid empty mailbox are distinct: inspect `available` before interpreting
`waiting` or the counts.

`voip_stack.has_waiting_messages` accepts an optional `account_id`. Omit it to
combine all enabled account mailboxes, or select `ha`/`office` explicitly.
Advanced lambdas can read `id(phone).get_message_waiting("office")`.
`add_on_message_waiting_callback` delivers aggregate changes on the main loop,
so a custom display can refresh only when mailbox state changes.

Enable `registration.message_waiting: true` for manual accounts. It defaults to
false; automatic HA registration supplies true unless explicitly overridden.
Compile-time gates exclude the MWI pool and protocol path when disabled.
[BLF](PHONE_SERVICES.md#blf-subscriptions-and-pickup) uses the same SIP worker to
watch other contacts, with separate dialogs and state.

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
playback can use an ordinary call to the PBX's mailbox-access number and DTMF
menu, or the optional IMAP MediaSource when that server exposes audio messages.
MWI itself reports mailbox state.

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
