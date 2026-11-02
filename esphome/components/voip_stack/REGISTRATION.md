# Optional SIP registration

This recovery candidate adds manually configured PBX accounts to the stable
2026.11 VoIP component. ESP-IDF is required. Omitting `registration` emits no
registration define, account pool, persistence state, DNS contexts or extra TCP
slot. Direct Home Assistant media and video keep the stable SDP implementation.

```yaml
voip_stack:
  id: phone
  registration:
    max_accounts: 2
    accept_direct_calls: true
    default_line: direct
    accounts:
      - account_id: office
        registrar: pbx.example.org
        username: '100'
        password: !secret office_sip_password
        transport: udp
```

`max_accounts` is the compiled capacity, from 1 to 4, default 2. An empty account
list permits runtime provisioning and restores previously persisted overrides.
`accept_direct_calls` defaults to true. `default_line` defaults to `direct`;
when direct calls are explicitly disabled and exactly one account is configured,
that account becomes the default. A line selection made at runtime is volatile.

Each account has these fields:

| Field | Default or limit |
| --- | --- |
| `account_id` | Required, 1-32 letters, digits, dots, underscores or hyphens; `direct` is reserved |
| `registrar` | Required IPv4 address or DNS hostname, with port configured separately |
| `port` | 5060 |
| `domain` | Registrar hostname |
| `username` | Required SIP username, at most 64 UTF-8 bytes, without URI delimiters |
| `password` | Required, at most 128 UTF-8 bytes |
| `auth_username` | SIP username |
| `proxy`, `proxy_port` | No explicit outbound proxy, 5060 |
| `transport` | `udp`, or `tcp` |
| `expires` | 3600 seconds, accepted range 30-86400 |
| `enabled` | true |

An enabled manual account starts REGISTER through the existing SIP worker.
There is no managed Home Assistant account provisioning. The engine handles
renewal, UDP retransmission, response deadlines, bounded retry, 401/407 Digest
and 423 minimum expiry. Supported Digest algorithms are MD5 and SHA-256,
including their session variants, with `qop=auth` or the legacy no-qop form.
Successful bindings must match the account Contact, including a verified
Via-derived mapped address when the registrar rewrites that Contact.

YAML actions and conditions:

| Name | Arguments |
| --- | --- |
| `voip_stack.set_account` | `id`, `account` using the schema above, optional templatable `persist` (false) |
| `voip_stack.remove_account` | `id`, templatable `account_id`, optional templatable `persist` (false) |
| `voip_stack.reset_account` | `id`, templatable `account_id` |
| `voip_stack.register_account` | `id`, templatable `account_id` |
| `voip_stack.unregister_account` | `id`, templatable `account_id` |
| `voip_stack.select_line` | `id`, templatable `line` |
| `voip_stack.start_call_on_line` | `id`, templatable `target` and `line` |
| `voip_stack.is_registered` | Condition with `id` and templatable `account_id` |

`on_registration_state` supplies `account_id`, `state` and integer `status_code`.
States are `disabled`, `registering`, `registered`, `unregistering`, `retry` and
`failed`. Delivery is deferred to the component main context.

When the existing native HA integration is enabled, it also exposes
`set_account`, `remove_account`, `reset_account`, `register_account`,
`unregister_account`, `select_line` and `start_call_on_line`. Native
`start_call_on_line` takes `dest` and `line`. Native `set_account` takes
`account_id`, `registrar`, `port`, `domain`, `username`, `password`,
`auth_username`, `proxy`, `proxy_port`, `transport`, `expires`, `persist`.
Empty `domain` uses `registrar`; empty `auth_username` uses `username`.
Native account provisioning enables the account. `unregister_account` changes
its current registration intent; this intent and runtime line selection are
not written to flash. Existing `start_call_via_ha(dest, call_id)` retains the
explicit HA path and the supplied case-sensitive Call-ID.

Registered targets accept a label/extension or a validated `sip:user@host:port`
URI, up to 96 UTF-8 bytes after normalization, preserving its authority and URI
parameters. `sips:`, control characters,
URI headers and unsupported transports are rejected. Selecting `direct` keeps
the existing phonebook/HA routing behavior. A new account call requires a current
binding. Line selection affects subsequent calls, without retargeting an active
dialog. REGISTER bindings and call dialogs have separate owners; accepted TCP
storage cannot claim a dialog before request validation and account policy.

Runtime `set_account` and `remove_account` are volatile unless `persist: true`
is explicitly supplied. Flash overrides take precedence over YAML rows after
restart. Persisted removal of a YAML account stores a tombstone. `reset_account`
erases that account's override/tombstone and restores its YAML row, or removes
an account that has no YAML row. Persistence failure leaves the current runtime
configuration unchanged. Unregister successfully before replacing credentials
or removing a bound account; active call/REFER/transaction owners also prevent
replacement. The C++ methods return success/failure; native API actions do not
provide a result response.

YAML account rows and strings remain an immutable Flash baseline. An incremental
C++ builder allocates owned baseline strings only if explicitly used. There is
one compiled runtime pool, no added task, and `max_accounts + 1` additional TCP
socket reservations, including the bounded accepted spare and the existing
transient accept/reject reserve. DNS uses bounded asynchronous IPv4 contexts and
publishes wake requests through the main owner, without BSD socket calls from
the lwIP callback.

Persistence reserves one `SavedRegistration` pool whenever registration is
compiled, including an empty YAML list, so saved runtime accounts can be restored.
`signaling_buffers_in_psram: true` selects strict external allocation; the default
selects strict internal allocation. Allocation failure preserves volatile YAML
registration and direct SIP; operations requiring the missing persistence pool
fail. Loading a prior four-slot NVS blob can allocate up to 4415 additional bytes
temporarily, even with a smaller compiled capacity.

The actual fixed-layout declarations measured on the host have these sizes:

| Compiled accounts | Saved pool / temporary heap stage | Pool plus stage during a commit |
| --- | --- | --- |
| 1 | 1112 bytes | 2224 bytes |
| 2 | 2212 bytes | 4424 bytes |
| 3 | 3312 bytes | 6624 bytes |
| 4 | 4412 bytes | 8824 bytes |

Persistent set/remove and reset allocate one temporary stage through the same
strict policy as the saved pool. RAII releases it on success and every failure.
Staging OOM returns false before NVS, overlay or runtime account mutation;
volatile provisioning still works without a saved pool. There is no additional
permanent staging pool and no capacity-sized local stack copy.

Ordinary native API execution owns persistence on ESPHome `loopTask`, not the
SIP worker. Before this storage correction, the 4412-byte four-account copy alone
occupied 53.86% of the default 8192-byte loop stack. GCC 16.2.1 host measurements
with `-O2 -fstack-usage`, without ASAN, reduced the save frame from 2272-5600
bytes across capacities 1-4 to 1200-1232 bytes. Reset commit frames changed from
2240-5552 to 1168-1184 bytes, with a 592-byte outer reset frame after correction.
The helper frame is 48 bytes. These are individual host function frames,
excluding nested NVS/API calls; they are not ESP ABI or stack high-water proof.

Before promotion, verify the exact target stack configuration and measure its
high-water, internal/DMA largest block, PSRAM and API/main-loop latency under
the same normal audio/video workload, including persistence operations and
failed commits at every supported compiled capacity. No stack-size increase or
hardware qualification is implied by the host tests.

The candidate contains no subscription, mailbox, directory, OPTIONS discovery,
managed account or renderer feature. Registered enhanced asymmetric 48/16 kHz
negotiation is deferred to the next media stage. Ordinary registered calls must
use real shared supported formats. Stage-2 P4 test profiles need an explicit
supported 16 kHz / 20 ms format alongside preferred 10 ms formats where the PBX
requires it; production SDP and audio capabilities are unchanged here.
