# Contacts, BLF and visual voicemail

The development components add optional standard phone services to the existing
SIP endpoint. Each service has one responsibility:

| Service | What it provides | Owner |
| --- | --- | --- |
| SIP REGISTER | A reachable account and Digest credentials | `voip_stack.registration` |
| SIP MWI | Mailbox waiting state and counts | `registration.message_waiting` |
| SIP BLF | Other contacts' idle, ringing, active or held state | `voip_stack.blf` |
| LDAPv3 | Names, stable contact IDs and dialable destinations | `ldap_directory` |
| IMAP | A voicemail catalog, audio attachments and message flags | `imap_voicemail` |
| Display | Layout, colors, touch actions and page selection | Your YAML or maintained device package |

Call signaling, MWI and BLF share the existing SIP worker. LDAP and IMAP are
optional components with bounded network buffers and their own worker settings.
Audio Stack and Runtime Controller remain independent choices. Use the
coordinated `@dev` sources for these additions.

Runnable board examples: [managed HA phone](../examples/managed-ha-phone.yaml)
and [independent server configuration](../examples/external-phone-services.yaml).
Both reuse the native I2S example and a shared native mixer/player package.

## Automatic Home Assistant setup

Maintained packages enable automatic registration and LDAP. Display packages
also include BLF and visual voicemail; headless packages leave those additions
to an explicit user configuration. A custom firmware selects the services it needs:

```yaml
api:
  custom_services: true

external_components:
  - source: github://n-IA-hane/esphome-voip-stack@dev
    components: [voip_stack, ldap_directory, imap_voicemail, phone_client]

voip_stack:
  id: phone
  # Keep your microphone, speaker and audio formats.
  ha_integration:
    auto_register: true
  blf:
    max_subscriptions: 16
    buffers_in_psram: true

ldap_directory:
  id: directory
  voip_stack_id: phone
  account_id: ha
  managed_by_ha: true

imap_voicemail:
  id: voicemail
  voip_stack_id: phone
  account_id: ha
  managed_by_ha: true
```

Home Assistant generates the account password and configures the existing ESPHome
native account action. The SIP account, LDAP and IMAP clients use that shared
identity. Server addresses and directory/mailbox settings arrive through their
configuration actions; phone data travels through SIP, LDAP and IMAP.

The managed account ID is `ha`. Its technical username identifies the device;
the Home Assistant device name supplies the SIP caller display name and contact
label. Renaming that label preserves the account. Repeating the same account
configuration preserves its binding and an active call. A genuine account
replacement waits for the current call/REFER cleanup, retires the old binding,
and applies the latest requested configuration on the existing SIP worker.

`auto_register` defaults to **false** in the generic component. Enabling it adds
registration with `ha` as the default line and SIP MWI enabled, unless those
settings are explicitly overridden. Reserve one account slot for `ha` when
adding manual PBX accounts. Direct incoming SIP remains available by default.

Managed LDAP replaces the legacy HA roster source, preserving manual contacts.
It starts only after HA configures the service. Removing or changing an account
invalidates dependent snapshots before the clients reconnect.

## A conventional PBX and directory

Use the same components with Asterisk, FreeSWITCH or another compatible PBX.
Configure each server's actual protocol services and credentials. A SIP account
alone does not imply that the PBX exposes an LDAP server or an IMAP mailbox.

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

imap_voicemail:
  id: voicemail
  voip_stack_id: phone
  account_id: office
  host: mail.example.org
  folder: INBOX
```

`account_id` copies the account credentials when configuring a network session.
An explicit `host` overrides the account's registrar host. For servers using a
separate identity, replace `account_id` with `username` and `password` in that
client. LDAP also needs its `base_dn` and, where required, `bind_dn`.
Standalone IMAP can omit `voip_stack_id` entirely when using explicit credentials.
Keeping `voip_stack_id` with separate credentials also binds playback to that
phone's call policy: requests are admitted while idle, including a second check
when a deferred URI reaches the player. An independent IMAP source without that
binding retains its normal media-player behavior.

LDAP defaults to port 389 and IMAP to port 143. `tls: true` selects implicit TLS;
set the corresponding server port explicitly (commonly 636 or 993). Certificate
verification uses `ca_certificate` or the ESP-IDF trust bundle. `tls_support`
allows a managed client to select TLS later. Plaintext services suit a trusted
LAN; choose the server policy appropriate to your installation.

## LDAP contact ownership and mapping

A directory update replaces its own contacts atomically in the existing
64-contact phonebook. Manual destinations take priority. Conflicting automatic
providers reject the replacement, retaining the preceding valid snapshot.
Four directory-provider bindings are available; each carries the account used
for calls and subscriptions rather than duplicating credentials in every row.

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
preallocated at startup. On the measured ESP32-S3 ABI, each watcher occupies
1376 bytes, so the pool uses 22,016 bytes for 16 or 88,064 bytes for 64, before
variable SIP strings. `buffers_in_psram` defaults to true when PSRAM is present.
Choose an explicit bound suitable for your contact list and memory budget.

The public states are `idle`, `ringing`, `active`, `held` and `unknown`, with a
separate `available` flag. Missing permission, a lost subscription or a partial
update gap produces unavailable data. It does not prove the phone is offline.
Versioned partial notifications are merged only with a valid full snapshot.

The callback supplies `contact_id`, `account_id`, `state` and `available`.
`id(phone).get_contact_blf(id)` also exposes ringing, active and held dialog
counts. Use `voip_stack.pickup_contact` with `contact_id` while that resource is
ringing. It applies the configured prefix to the SIP URI's user, preserving the
server address and transport. The managed HA setup defaults to `pickup.`;
external PBXs use their own configured feature code. Pickup availability depends
on the PBX's dial plan and authorization.

## IMAP visual voicemail

SIP MWI supplies a lamp and counts. IMAP supplies the message list, caller/date
metadata, optional duration and audio attachment. The client uses stable
`UID` plus `UIDVALIDITY` identities and a bounded page of messages.

Attach the component as a source to an existing native player:

```yaml
media_player:
  - id: !extend speaker_media_player
    announcement_pipeline:
      sources: [voicemail]
```

The maintained display packages provide the shared list and player wiring.
Custom YAML owns its playback controls and the policy for an incoming call.
Use the native mixer when voicemail and VoIP share a hardware speaker.

| Action | Required fields |
| --- | --- |
| `imap_voicemail.refresh` | `id` |
| `imap_voicemail.play` | `id`, `uid`; optionally `uid_validity` |
| `imap_voicemail.mark_read` | `id`, `uid`, `uid_validity`; `read` defaults to true |
| `imap_voicemail.delete` | `id`, `uid`, `uid_validity` |
| `imap_voicemail.page` | `id`, `page`; optionally `uid_validity` |

After a catalog update, read `message_count()` and `get_message(index)` for the
current page. A message includes its UID, UIDVALIDITY, caller, date, seen/deleted
flags and supported audio-part metadata. `total_count()`, `has_older()`,
`has_newer()`, `older()` and `newer()` support pagination. Select the UID and
UIDVALIDITY from that snapshot when playing or deleting a message.

Deletion requires UIDPLUS and uses `UID EXPUNGE` for the selected message.
Success is published after the server's tagged acknowledgement. A changed
UIDVALIDITY invalidates stale selections. Read flags use standard IMAP flags;
`mark_read_on_play` defaults to true. The account's normal SIP voicemail-access
number and DTMF menu remain available independently.

Defaults: 16 messages per page, 16 KiB metadata storage, 8 KiB fetch chunks,
16 KiB audio ring and 8 KiB decoder output. `max_audio_bytes` caps an attachment
at 16 MiB while playback streams through bounded buffers. WAV is enabled by
default; `codecs` can add `mp3`, `opus` or `flac`. The server must provide a
supported MIME audio attachment; this is standard IMAP voicemail rather than a
claim of every mobile-carrier visual-voicemail profile.

Both clients expose `task_core`, `task_priority`, `task_stack_size`,
`task_stack_in_psram` and `buffer_in_psram`. IMAP additionally exposes the
`fetch_task_*` settings. Network/task storage is allocated at startup; the
existing public ESPHome audio decoder handles each selected recording.

Protocol references: [SIP dialog state, RFC 4235](https://www.rfc-editor.org/rfc/rfc4235.html),
[LDAPv3, RFC 4511](https://www.rfc-editor.org/rfc/rfc4511.html),
[IMAP4rev1, RFC 3501](https://www.rfc-editor.org/rfc/rfc3501.html),
[UIDPLUS, RFC 4315](https://www.rfc-editor.org/rfc/rfc4315.html).
