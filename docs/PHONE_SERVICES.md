# Contacts, BLF and SIP voicemail

The development components add optional standard phone services to the existing
SIP endpoint. Each service has one responsibility:

| Service | What it provides | Owner |
| --- | --- | --- |
| SIP REGISTER | A reachable account and Digest credentials | `voip_stack.registration` |
| SIP MWI | Mailbox waiting state and counts | `registration.message_waiting` |
| SIP BLF | Other contacts' idle, ringing, active or held state | `voip_stack.blf` |
| LDAPv3 | Names, stable contact IDs and dialable destinations | `ldap_directory` |
| SIP mailbox access | Opens the configured voice mailbox | `voip_stack.call_mailbox` |
| Display | Layout, colors, touch actions and page selection | Your YAML or maintained device package |

Call signaling, MWI, BLF and mailbox calls share the existing SIP worker.
The optional LDAP component has bounded network buffers and its own worker settings.
Audio Stack and Runtime Controller remain independent choices. Use the
coordinated `@dev` sources for these additions.

Runnable board examples: [managed HA phone](../examples/managed-ha-phone.yaml)
and [independent server configuration](../examples/external-phone-services.yaml).
Both reuse the native I2S example and the ordinary VoIP speaker path.

## Automatic Home Assistant setup

Maintained packages expose directory, BLF and MWI as independent phone-service
choices. A custom firmware selects the services it needs:

```yaml
api:
  custom_services: true

external_components:
  - source: github://n-IA-hane/esphome-voip-stack@dev
    components: [voip_stack, ldap_directory, phone_client]

voip_stack:
  id: phone
  # Keep your microphone, speaker and audio formats.
  ha_integration:
    auto_register: true
  registration:
    message_waiting: true
  blf:
    max_subscriptions: 16
    presence: true
    buffers_in_psram: true

ldap_directory:
  id: directory
  voip_stack_id: phone
  account_id: ha
  managed_by_ha: true
```

Home Assistant generates the account password and configures the existing ESPHome
native account action. The SIP account and optional LDAP client use that shared
identity. Directory settings arrive through its configuration action; phone data
travels through standard SIP and LDAP.

The managed account ID is `ha`. Its technical username identifies the device;
the Home Assistant device name supplies the SIP caller display name and contact
label. Renaming that label preserves the account. Repeating the same account
configuration preserves its binding and an active call. A genuine account
replacement waits for the current call/REFER cleanup, retires the old binding,
and commits persistence from the application context before the existing SIP
worker resumes registration.

`auto_register` defaults to **false** in the generic component. Enabling it adds
registration with `ha` as the default line, unless that line is explicitly
overridden. MWI, BLF and LDAP each have their own configuration; automatic
registration enables none of them implicitly. Reserve one account slot for `ha` when
adding manual PBX accounts. Direct incoming SIP remains available by default.

### Legacy HA rosters

The legacy adapter accepts `set_roster_json` when
`ha_integration.legacy_roster: true`. Legacy HA adapters enable this compatibility
surface by default. Managed automatic registration uses `legacy_roster: false`.
A firmware without the HA adapter also excludes this JSON parser. An explicit true setting retains the
legacy path when required.

With the option disabled, the JSON RPC, parser and its cJSON dependency are
excluded at compile time. Static contacts and CSV operations keep using the
same phonebook. Managed LDAP owns its directory rows and needs no JSON push.
Enable `registration.message_waiting: true` explicitly when adding SIP MWI to
an existing automatically registered configuration.

Managed LDAP replaces the legacy HA roster source, preserving manual contacts.
It starts only after HA configures the service. Removing or changing an account
invalidates dependent snapshots before the clients reconnect.

## A conventional PBX and directory

Use the same components with Asterisk, FreeSWITCH or another compatible PBX.
Configure each server's actual protocol services and credentials. A SIP account
alone does not imply that the PBX exposes an LDAP directory.

```yaml
voip_stack:
  id: phone
  ha_integration: false
  registration:
    max_accounts: 1
    default_line: office
    message_waiting: true
    accounts:
      - account_id: office
        registrar: pbx.example.org
        username: "100"
        password: !secret office_sip_password
  blf:
    account_id: office
    max_subscriptions: 16
    # Use the directed-pickup prefix configured by your PBX administrator.
    pickup_prefix: "**"

ldap_directory:
  id: directory
  voip_stack_id: phone
  account_id: office
  host: directory.example.org
  base_dn: ou=people,dc=example,dc=org
  bind_dn: uid=100,ou=people,dc=example,dc=org
```

`account_id` copies the account credentials when configuring a network session.
An explicit `host` overrides the account's registrar host. For servers using a
separate identity, replace `account_id` with `username` and `password` in that
client. LDAP also needs its `base_dn` and, where required, `bind_dn`.
LDAP defaults to port 389. `tls: true` selects implicit TLS; set the server's
TLS port explicitly (commonly 636). Certificate verification uses
`ca_certificate` or the ESP-IDF trust bundle. `tls_support` allows a managed
client to select TLS later. Plaintext services suit a trusted LAN; choose the
server policy appropriate to your installation.

## LDAP contact ownership and mapping

A directory update replaces its own contacts atomically in the existing
64-contact phonebook. Manual destinations take priority. Conflicting automatic
providers reject the replacement, retaining the preceding valid snapshot.
Four directory-provider bindings are available; each carries the account used
for calls and subscriptions rather than duplicating credentials in every row.

Directory results stream into one bounded staging table and move into the
canonical phonebook after complete validation. Updates reuse its storage,
preserving manual entries and selection without a second merged roster.
`voip_stack.signaling_buffers_in_psram` controls the canonical table separately
from PCM buffers. With that flag, the table and its text payloads use PSRAM;
64 records are reserved at startup, occupying 10,240 bytes on the measured
ESP32-S3 ABI before dynamic text. Internal placement grows on demand
within the same contact bound and retains capacity for later updates. LDAP's `buffer_in_psram`
controls its receive buffer and staging table. Text payloads are additional to
those table budgets. The new typed directory API exposes borrowed
`std::string_view` values, valid until the next phonebook mutation. Queries and
display-row rendering read them directly. Public string-valued phone controls
retain their existing owned return values; profiles without a directory retain
the legacy string fields and reference getters.

| LDAP attribute | ESP contact field |
| --- | --- |
| `uid`, then `entryUUID` or a bounded key derived from the DN | Stable ID |
| `displayName`, then `cn` | Display name |
| `labeledURI` containing a SIP URI | Preferred call route, including host, port and transport |
| `telephoneNumber` | Number fallback when a SIP URI is absent |
| `ou` | Contact category when it matches a supported kind |

Supported kinds include ESP, SIP account, browser, contact, automation and group.
A directory's category conventions determine the available icon detail. Common
LDAP fields remain usable with a conventional directory.

`max_entries` defaults to 64, `receive_buffer_size` to 8192 bytes and
`refresh_interval` to 5 minutes. `ldap_directory.refresh` requests an update.
`on_update` receives the accepted contact count; `on_error` receives the LDAP
result code. Unsupported filters and oversized values are rejected explicitly.
Display names fit 64 UTF-8 bytes, with a safe character boundary when shortened.
Routes exceeding 96 bytes reject the snapshot.

Lambdas can read `get_contact_count()`, `get_contact(index)` and
`find_contact(stable_id)`. Views belong to the main loop and remain valid until
the next phonebook mutation. Use `voip_stack.call_contact` with `contact_id` to
call that stable identity through its provider's account.

## BLF subscriptions and pickup

```yaml
voip_stack:
  blf:
    account_id: office
    max_subscriptions: 16
    watch_contacts: true
    presence: true
    buffers_in_psram: true
    pickup_prefix: "**"
  on_blf_state:
    then:
      - lambda: |-
          ESP_LOGI("phone.blf", "%s: %s available=%u",
                   contact_id.c_str(), state.c_str(), available);
```

BLF uses SIP `SUBSCRIBE`/`NOTIFY`, the `dialog` event package and
`application/dialog-info+xml`. It tracks the configured contacts, skipping
local automations. A directory contact uses its directory account; other
contacts use the selected default or `blf.account_id` for a direct default.
The PBX controls permission to watch a resource.

`max_subscriptions` defaults to **16** and accepts **1 through 64**. Watchers are
preallocated at startup. With signaling PSRAM enabled, the measured ESP32-S3
watcher occupies 1392 bytes. The pool uses 22,272 bytes for 16 or 89,088 bytes
for 64, before variable SIP strings and allocator overhead.
`buffers_in_psram` defaults to true when PSRAM is present.
Choose an explicit bound suitable for your contact list and memory budget.

`presence: true` additionally subscribes to the standard `presence` event with
`application/pidf+xml`. It distinguishes availability from call activity:
`PresenceState::OPEN`, `CLOSED` or `UNKNOWN` is available through
`get_contact_blf(id).presence`. An idle dialog snapshot alone cannot establish
whether the phone is online. A PBX that does not support presence leaves this
field unknown while dialog BLF continues to work.

Presence defaults to false in a standalone configuration. Enabling it
preallocates one additional subscription transaction per watched contact,
using the same SIP worker, transport, account and buffer placement as BLF.
On that ABI, this adds 684 bytes per contact: the combined dialog and presence
pools occupy 33,216 bytes for 16 contacts or 132,864 bytes for 64.
Both subscriptions count toward the PBX's subscription limits.

`blf.buffers_in_psram` places the pools. The existing
`voip_stack.signaling_buffers_in_psram` option also places retained subscription
messages, dialog identifiers and route metadata in PSRAM. Digest state, shared
account credentials and temporary wire-message builders retain their existing
allocation policy.

Getter-only watches keep their shared state current without posting unused
application callbacks. Adding a C++ observer or a YAML automation enables
delivery, including when the automation is currently inactive.

The public states are `idle`, `ringing`, `active`, `held` and `unknown`, with a
separate `available` flag. Missing permission, a lost subscription or a partial
update gap produces unavailable data. It does not prove the phone is offline.
This flag describes the dialog cache; the `presence` field describes the
resource's reported availability.
Versioned partial notifications are merged only with a valid full snapshot.

The callback supplies `contact_id`, `account_id`, `state` and `available`.
`id(phone).get_contact_blf(id)` returns a `BlfStatus` snapshot containing the
primitive activity, presence, version and dialog counts. Reading it allocates
no identity strings. C++ observers receive `BlfStateInfo` with owned account and
contact IDs, copied only when event delivery needs them.
Use `voip_stack.pickup_contact` with `contact_id` while that resource is
ringing. It applies the configured prefix to the SIP URI's user, preserving the
server address and transport. The managed HA setup defaults to `pickup.`;
external PBXs use their own configured feature code. Pickup availability depends
on the PBX's dial plan and authorization.

## SIP mailbox access

MWI provides the waiting indicator and new/old message counts. Mailbox access
uses an ordinary SIP call through the same registered account, with the PBX's
voice menu and keypad controls. `voip_stack.call_mailbox` uses the notified
`Message-Account` URI or an explicit PBX access target. The call retains the
normal audio, hold and hangup behavior.

The display package exposes the waiting badge and in-call controls. Custom
YAML can use `can_call_mailbox(account_id, target)` before enabling a button.
See [mailbox configuration and access](SIP_MWI.md#mailbox-access-with-sip-and-dtmf)
for the account selection policy and an example access-code override.

LDAP exposes `task_core`, `task_priority`, `task_stack_size`,
`task_stack_in_psram` and `buffer_in_psram`. Network/task storage is allocated
at startup and reused across directory refreshes.

Protocol references: [SIP message waiting, RFC 3842](https://www.rfc-editor.org/rfc/rfc3842.html),
[SIP dialog state, RFC 4235](https://www.rfc-editor.org/rfc/rfc4235.html),
[SIP presence, RFC 3856](https://www.rfc-editor.org/rfc/rfc3856.html),
[LDAPv3, RFC 4511](https://www.rfc-editor.org/rfc/rfc4511.html).
