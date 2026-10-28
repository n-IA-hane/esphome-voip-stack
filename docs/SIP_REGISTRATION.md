# Optional PBX accounts

The existing direct connection to Home Assistant needs no new configuration.
Add `registration` only when the ESP should also register with a SIP PBX.
Omitting the block excludes the account pool, authentication, DNS callbacks,
registration timers and extra native actions from the firmware.

Registration reuses the existing SIP worker and its `task_stacks_in_psram`
policy. `signaling_buffers_in_psram` also covers retained REGISTER records and
the additional TCP flow buffers. There is no worker per account. The socket
budget reserves one possible TCP connection per compiled account slot, including
accounts provisioned later at runtime.

An **account** is a SIP identity registered with a PBX. A **line** selects the
account, or the existing direct route, for the next outgoing call. Several
accounts can remain registered while one physical call uses the microphone and
speaker. Selecting another line leaves the current call unchanged.

## Configuration

Add the block to the existing board configuration:

```yaml
voip_stack:
  id: phone
  # Keep the board's existing microphone, speaker and audio configuration.
  registration:
    max_accounts: 2
    accounts:
      - account_id: office
        registrar: pbx.example.org
        port: 5060
        domain: example.org
        username: "100"
        password: !secret pbx_password
        transport: udp
```

`domain` defaults to `registrar`. `auth_username` defaults to `username`.
`transport` accepts `udp` or `tcp`; RTP media remains UDP. `expires` defaults to
3600 seconds and accepts 30 through 86400 seconds. The PBX's accepted Contact
expiration determines the actual lifetime and refresh time. Refreshes occur
before expiry with bounded jitter, including during an ongoing call.

An optional `proxy` and `proxy_port` select the signaling next hop separately
from the registrar and SIP domain. This version supports IPv4 addresses and DNS
hostnames with an explicit port and transport. DNS resolution is asynchronous;
renewals reuse the resolved address. Digest supports MD5 and SHA-256, their
session variants and `qop=auth`. When both algorithms are offered, SHA-256 is
preferred. Registrar and proxy challenges retain separate credentials.

Each account advertises its own local Contact token, such as
`sip:esphome-office@device-address:5060`. Its PBX username, address of record and
Digest identity remain the configured values. The Contact token keeps incoming
calls unambiguous even when different accounts use the same PBX username.

The default line remains `direct`. A specific PBX account can be selected in
YAML without changing the phonebook:

```yaml
voip_stack:
  registration:
    default_line: office
    accounts:
      - account_id: office
        registrar: pbx.example.org
        username: "100"
        password: !secret pbx_password
```

For a PBX endpoint that should reject unrelated direct incoming calls, add
`accept_direct_calls: false` inside `registration`. The SIP listener remains
available to receive PBX calls. New calls must match a configured account and
its trusted PBX address or connection. Existing dialog requests still use the
normal Call-ID, tag and transaction checks. With exactly one static account,
this policy also selects that account as the default outgoing line unless
`default_line` is explicitly supplied.

`max_accounts` defaults to two and accepts one through four. The pool is bounded
at compilation, including accounts added later. An empty `accounts` list enables
runtime provisioning within that pool.

Registration and media negotiation are separate. The PBX must support the
configured audio formats, sample rates, directions and packet times. A successful
REGISTER does not demonstrate an audio conversation or configure NAT traversal.

## Actions and state

When the existing native HA adapter is enabled (`api.custom_services: true`),
the firmware exposes these ESPHome actions to Home Assistant:

| Action | Arguments | Purpose |
| --- | --- | --- |
| `set_account` | Account fields and `persist` | Create or replace an account |
| `register_account` | `account_id` | Request registration or recover a failed account |
| `unregister_account` | `account_id` | Remove this account's Contact binding |
| `remove_account` | `account_id`, `persist` | Remove a deregistered account |
| `reset_account` | `account_id` | Restore YAML and clear the saved override |
| `select_line` | `line` | Choose `direct` or an available account for future calls |
| `start_call_on_line` | `dest`, `line` | Place one call on an explicit line |

The existing `start_call_via_ha` remains pinned to the HA route. Its behavior is
independent of `default_line`.

Native `set_account` takes `account_id`, `registrar`, `port`, `domain`,
`username`, `password`, `auth_username`, `proxy`, `proxy_port`, `transport`,
`expires` and `persist`. ESPHome native custom actions expose fixed arguments:
pass an empty string for unused `proxy` or `auth_username`, port 5060 for an
unused `proxy_port`, and `persist: false` for a temporary change. An empty domain
uses the registrar hostname. YAML account actions apply the documented defaults.

Accounts used by a call cannot be replaced, removed or reset. For a registered
account, first request `unregister_account` and wait for the `disabled` event
before changing its credentials or server. The binding and the call connection
have separate owners, so deregistration does not hang up an established call.

Equivalent YAML automations work without the HA adapter:

```yaml
button:
  - platform: template
    name: "Call office"
    on_press:
      - if:
          condition:
            voip_stack.is_registered:
              id: phone
              account_id: office
          then:
            - voip_stack.start_call_on_line:
                id: phone
                target: "101"
                line: office

voip_stack:
  on_registration_state:
    then:
      - lambda: |-
          ESP_LOGI("pbx", "Account %s: %s (SIP %d)",
                   account_id.c_str(), state.c_str(), status_code);
```

`on_registration_state` supplies `account_id`, `state` and `status_code`.
States are `disabled`, `registering`, `registered`, `unregistering`, `retry`
and `failed`. A status of zero identifies a local request rather than a SIP
response. `is_registered` means the accepted binding remains valid; a pending
refresh does not temporarily disable incoming calls.

`voip_stack.set_account` uses an `account` mapping with the same fields as the
static list, plus an optional `persist` flag. The other YAML actions use the
same argument names as their native counterparts, except
`voip_stack.start_call_on_line` uses `target` instead of the native `dest`.

## Explicit persistence

YAML is the boot baseline. Runtime edits are temporary unless `persist: true`
is explicitly requested. Saved changes form a bounded, versioned preference
overlay. A saved removal suppresses the corresponding YAML account. Resetting
that account removes the overlay and restores YAML; resetting a runtime-only
account removes it.

The stored overlay carries explicit record counts, so increasing
`max_accounts` preserves entries saved by a smaller build. A smaller build
loads entries up to its compiled capacity; the on-flash overlay remains intact
until an explicit persistent edit writes the smaller pool. Removed YAML account
IDs are stored separately from account credentials, so removals do not consume
the slots needed by new runtime-only accounts.


## Standard audio negotiation

For ordinary sendrecv SIP sessions, the advertised codecs are the intersection
of the configured transmit and receive wire formats. The PBX can choose any
advertised format without selecting a receive-only capability for both
directions. Single-direction endpoints advertise all formats for their active
direction. Peers with the bilateral directional audio capability can still use
independent transmit and receive rates.

This selection leaves the configured microphone, speaker and resampler
capabilities intact. For example, a 16 kHz transmitter and a 16/48 kHz receiver
advertise 16 kHz to an ordinary bidirectional SIP peer, while the explicitly
directional path can negotiate 16 kHz transmit and 48 kHz receive.

When the HA adapter is enabled, schema validation reserves enough space for a
full IPv4 address in the endpoint state and enforces HA's 255-character limit.
An error identifies configurations whose names or extensions need shortening.
Runtime extension edits use the remaining character and UTF-8 byte budget;
rejected edits preserve the current extension and its saved value.


## Remote hold and resume

An established SIP call accepts direction-only re-INVITEs for sendonly,
recvonly, inactive and sendrecv. The same call and prepared audio resources
remain active, with RTP gated by the negotiated direction. The receive-loss
watchdog allows a held call to remain silent and resumes its normal observation
after media is restored.

Codec, payload type, packet-time and RTP endpoint changes use a new call.
Wake-word and microphone ownership remain controlled by the existing firmware
policy; remote hold adds no worker or buffer pool.

## Optional message waiting

Set `registration.message_waiting: true` to subscribe to the registered PBXs'
RFC 3842 `message-summary` events. The default is false; its pool and callbacks
are excluded from registration-only firmware.

The subscription shares the existing SIP worker and account connections. A
notification changes mailbox data and does not change the call FSM. The
component exposes data to automations; YAML owns any LED, badge or screen effect.

```yaml
voip_stack:
  registration:
    message_waiting: true
    # Keep the accounts configured above.
  on_message_waiting:
    then:
      - lambda: |-
          ESP_LOGI("mailbox", "%s: available=%s waiting=%s new=%lu old=%lu",
                   account_id.c_str(), YESNO(available), YESNO(waiting),
                   static_cast<unsigned long>(new_messages),
                   static_cast<unsigned long>(old_messages));
```

The callback also supplies `urgent_new` and `urgent_old`. Condition
`voip_stack.has_waiting_messages` accepts an optional `account_id`; omitting it
checks all configured accounts. The C++ getter
`id(phone).get_message_waiting("office")` returns the same snapshot, including
`available` and `waiting`. Unavailable subscriptions report unavailable data
rather than a claim that the mailbox is empty.

Changing an account or selecting a line affects future work. Neither operation
retargets an existing dialog, and a second unrelated call receives Busy while
the physical endpoint is occupied.

## Reproducible qualification

The host suite compiles the production registration engine with ASan/UBSan and
exchanges SIP records over real localhost UDP and TCP sockets. Digest results
are checked with an independent Python hashlib oracle. The configuration suite
checks direct-only exclusion, PBX-only defaults and action generation.

The maintained native PBX runner also checks actual Asterisk bindings:

```sh
python3 tools/qualify_sip_registration.py \
  --output /path/to/private-test-artifacts \
  --refresh
```

It requires native Asterisk, its XML documentation, a C++17 compiler and OpenSSL
development files. It generates temporary test credentials, binds the PBX to
loopback, verifies bindings and unregister, and stops its own PBX process.
The `--documentation` option can select a readable directory containing the
installed Asterisk XML files.

This runner qualifies registration against the PBX. It does not replace real
ESP call, audio, network-recovery or long-duration testing.

An equivalent runner uses a native FreeSWITCH installation and the same
production REGISTER engine:

```sh
python3 tools/qualify_sip_registration_freeswitch.py \
  --output /path/to/private-test-artifacts \
  --refresh
```

It checks UDP/TCP, MD5/SHA-256, the PBX's actual binding and removal, and an
optional timed refresh. It has the same registration-only qualification scope.

The maintained ESP runner uses the existing native actions and observes actual
device call states. With a private firmware YAML and a matching device MAC,
`tools/qualify_sip_registration_esp.py` can provision an account, place a call,
remove the account or run an isolated native Asterisk call matrix. The matrix
checks incoming and outgoing calls on UDP and TCP accounts, including Digest
challenges and both hangup directions. It keeps Master Volume at zero and
records firmware identity, state transitions and existing diagnostics.

Use `--help` for its arguments. Credentials stay in the private YAML or account
JSON, and are excluded from result files. Call-state and RTP evidence remain
distinct from acoustic qualification.
