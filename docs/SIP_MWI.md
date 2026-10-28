# Message Waiting Indication

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
Without an account ID it checks the configured accounts. Advanced lambdas can
read a specific account with `id(phone).get_message_waiting("office")`.

The default is `message_waiting: false`. With that setting, the MWI pool,
protocol code, dispatch and callbacks are excluded from the firmware. Ordinary
HA direct calls and PBX registration keep their existing behavior.

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
