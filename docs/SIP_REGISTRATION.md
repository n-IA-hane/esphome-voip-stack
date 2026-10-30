# Optional PBX accounts

Available in the **2026.11.1-dev preview**. Select the `dev` component source
when using these account options. [Illustrated connection examples](../README.md#choose-how-the-esp-connects)
cover direct HA calls, a static PBX route, registered PBX calls and mixed lines.

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

Direct SIP also works with a PBX configured to route to the ESP's IP address.
That installation uses the direct line and a static PBX route; it needs no
REGISTER account. The ESP remains an endpoint in each model. Its SIP listener
receives INVITEs, while the PBX owns its extension dial plan and trunks.

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

Incoming admission and outgoing selection are independent:

| Setting | Effect |
| --- | --- |
| Omit `registration` | Keep direct SIP; exclude account-management code |
| `accept_direct_calls: true` | Allow direct incoming calls alongside matching PBX calls |
| `accept_direct_calls: false` | Admit new incoming calls through matching registered accounts; keep the listener running |
| Omit `default_line` with one static account and direct admission disabled | Use that account for outgoing calls |
| Omit `default_line` in other configurations | Start on `direct` |
| Supply `default_line` | Use the declared direct route or static account as the initial outgoing choice |

For a PBX-only installation with several accounts, set `default_line` to the
account that should originate ordinary calls. Incoming policy leaves explicit
outgoing-line choices available. Incoming calls on any admitted account use
that account's identity regardless of the outgoing default.

Registration and media negotiation are separate. The PBX must support the
configured audio formats, sample rates, directions and packet times. A successful
REGISTER does not demonstrate an audio conversation or configure NAT traversal.

## Automatic HA account

The generic component leaves automatic registration disabled. Maintained HA
phone packages set `ha_integration.auto_register: true`; a custom board can opt
in to the same setting. HA generates and provisions the `ha` account through
the existing ESPHome native API. The actual device name is the technical
username; HA's friendly name becomes the SIP caller display name.

Keep one free `max_accounts` slot for `ha`. It may coexist with manual PBX
accounts and direct incoming calls. An identical configuration is an idempotent
operation. Replacing a managed account waits until the current call, REFER and
terminal transaction have finished; the existing SIP worker unregisters the old
binding before applying the latest requested account. A failed retirement is
reported while the old remote lease is left to expire normally.

The managed native `set_account` action accepts `transport: auto` for account
`ha`, using the ESP's configured UDP/TCP transport. Manual accounts retain the
explicit `udp` or `tcp` choices. `set_ha_display_name` updates caller display
without changing the authentication identity.

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

YAML `voip_stack.start_call_on_line` uses `target` and `line`; the native HA
action uses `dest` and `line`. A line supplied for one call takes priority over
the selected default. `select_line` changes the runtime default for subsequent
calls; declare `registration.default_line` in YAML for the initial choice after
restart. An unavailable chosen account does not silently switch to another
PBX or to the direct route.

Native `set_account` takes `account_id`, `registrar`, `port`, `domain`,
`username`, `password`, `auth_username`, `proxy`, `proxy_port`, `transport`,
`expires` and `persist`. ESPHome native custom actions expose fixed arguments:
pass an empty string for unused `proxy` or `auth_username`, port 5060 for an
unused `proxy_port`, and `persist: false` for a temporary change. An empty domain
uses the registrar hostname. YAML account actions apply the documented defaults.

Manual accounts used by a call retain their existing replacement/removal guards.
For a registered manual account, request `unregister_account` and wait for the
`disabled` event before changing its credentials or server. Managed `ha`
replacements use the deferred transaction described above. Binding and call
connections have separate owners, so deregistration preserves an established call.

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

The coordinated HA backend resolves managed phones through their SIP binding
and queries media capabilities with OPTIONS. Each call negotiates its final
formats through SDP. The optional legacy endpoint sensor retains its
255-character validation when explicitly enabled with
`ha_integration.endpoint: true`.


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
RFC 3842 `message-summary` events. The default is false; the SIP subscription
pool and protocol handling are excluded from registration-only firmware.

The managed Home Assistant account uses the same SIP MWI path under account ID
`ha`. `ha_integration.auto_register: true` configures that account automatically;
enable MWI separately with `registration.message_waiting: true`.
See [mailbox examples](SIP_MWI.md)
and [LDAP, BLF and mailbox access](PHONE_SERVICES.md).

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
checks every enabled mailbox source. The C++ getter
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
