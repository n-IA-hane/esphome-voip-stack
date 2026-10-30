# Message Waiting Indication

MWI reports mailbox state using standard SIP `SUBSCRIBE` and `NOTIFY` with
RFC 3842 `message-summary`. It provides the waiting indicator and optional new,
old and urgent counts. Mailbox access uses an ordinary SIP call and the PBX's
voice menu.

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
  registration:
    message_waiting: true
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
should switch to `auto_register: true` plus
`registration.message_waiting: true` and rebuild with the coordinated HA
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
false, including when automatic HA registration is enabled.
Compile-time gates exclude the MWI pool and protocol path when disabled.
[BLF](PHONE_SERVICES.md#blf-subscriptions-and-pickup) uses the same SIP worker to
watch other contacts, with separate dialogs and state.

## Mailbox access with SIP and DTMF

The optional RFC 3842 `Message-Account` URI is exposed in the same mailbox
snapshot. Use the ordinary
call owner to open it, then the PBX's normal DTMF menu:

```yaml
button:
  - platform: template
    name: Open mailbox
    on_press:
      - voip_stack.call_mailbox:
          id: phone
          account_id: office
          target: '*97'  # Optional PBX access code; omit to use Message-Account.
```

Both fields are optional. Without an account, a waiting mailbox takes priority:
the selected account when waiting, otherwise the first waiting account, then
the selected or first available mailbox. An explicit target overrides the
notified URI, supporting a PBX whose mailbox identity URI differs from its
voice-menu address. Only configured, registered account lines are used.

Lambdas can use `can_call_mailbox(account_id, target)` to enable a control and
`call_mailbox(account_id, target)` to place the call. The predicate has no call
side effects. Missing or unsupported addresses return false; the waiting badge
and counts remain usable. The SIP endpoint accepts SIP addresses and configured
dial targets. Mailbox audio follows the ordinary VoIP call path.

## Sending digits during a call

The ESP negotiates RFC 4733 `telephone-event` payloads with the peer. Digits use
the existing RTP worker while microphone and speaker audio continue normally.
Call `can_send_dtmf()` to check negotiated support and queue capacity;
`send_dtmf(char)` returns whether one digit was accepted. The native ESPHome
`send_dtmf` action takes a single-character `digit` string.

```yaml
on_press:
  - voip_stack.send_dtmf:
      id: phone
      digit: '#'
      duration_ms: 160
```

Accepted digits are `0` through `9`, `*`, `#` and `A` through `D`. YAML duration
is 40 through 8000 ms; the negotiated clock must also fit the RFC 4733 16-bit
duration field. Invalid digits, excessive duration, a full eight-command queue
or a call without a negotiated event format return false. The sender keeps
media clocks and SSRC ownership separate when event and audio rates differ.
The component does not log digits or PINs. Optional `on_dtmf` delivers received
digits after duplicate filtering; omit it when inbound digit automation is
unneeded.

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
An initially unavailable mailbox returning 404 is retried with that backoff,
so enabling the mailbox later can restore notifications. A completed positive
REGISTER also requests renewal of the account's existing MWI and BLF dialogs.
In-flight subscription transactions finish first; Retry-After is preserved.
Healthy dialogs are refreshed in place, while a missing dialog such as 481
causes a new initial subscription with a fresh Call-ID and From tag.
Unsubscription uses Expires 0 and accepts
the terminal NOTIFY, including retransmission of that notification. Mailbox
access uses an ordinary call to the PBX's mailbox-access number and voice menu.
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
