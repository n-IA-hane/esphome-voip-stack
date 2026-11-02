# LDAP directory

`ldap_directory` is the standard directory provider for the existing VoIP
phonebook. Static contacts remain available alongside it. Omit the component
for a static or legacy HA roster installation; no directory worker, connection,
stage buffer or provider table is then included.

```yaml
external_components:
  - source: github://n-IA-hane/esphome-voip-stack@main
    components: [voip_stack, ldap_directory, phone_client]

ldap_directory:
  id: directory
  voip_stack_id: phone
  host: ldap.example.test
  port: 636
  tls: true
  username: uid=reader,ou=accounts,dc=example
  password: !secret directory_password
  base_dn: ou=phonebook,dc=example
```

The existing `voip_stack` component must be configured separately. LDAPS checks
the server name and its certificate using the ESP certificate bundle, or the
configured `ca_certificate` PEM. Plain TCP is an explicit configuration choice;
Simple Bind requires a protected network or TLS to keep credentials confidential.
StartTLS, SASL and paged searches are not implemented.

For a shared SIP account, use `account_id: ha` instead of explicit credentials.
The directory reads an owned snapshot from the existing registration owner and
refreshes after its configuration revision changes. Temporary registration loss
keeps the last directory; removing the account clears only this provider's rows.
No credentials are logged or exposed through update/error triggers.

Managed HA mode uses `managed_by_ha: true`, `account_id: ha`, `api_actions: true`,
`api.custom_services: true` and `voip_stack.ha_integration.auto_register: true`.
The real native actions are:

- `configure_directory(server, port, base_dn, bind_dn, account_id, tls, enabled)`
- `refresh_ldap_directory()`

Managed mode starts disabled until HA supplies configuration and replaces the
legacy HA roster source. Identical configuration is idempotent. For a directory
change with identical configuration, call `refresh_ldap_directory`; periodic
refresh defaults to five minutes. Standalone API configuration is optional and
uses `configure_ldap_directory` plus `configure_ldap_credentials` only with
explicit external credentials. YAML automation can call `ldap_directory.refresh`.

A successful search is staged off the main loop and published atomically only
after its terminal LDAP success result. A partial search, malformed response,
wrong message ID, size limit, timeout or authentication failure retains the
previous complete snapshot. An empty successful result clears this provider.
Manual contacts take precedence. Conflicts with another provider or the shared
64-contact capacity reject the whole replacement. Selection follows stable ID
when a contact is renamed.

Standard attributes are `uid` or `entryUUID` for identity, `displayName` or `cn`,
`telephoneNumber`, `labeledURI` and `ou`. A missing identity gets a bounded key
derived from the entry DN. SIP URI authority and transport remain independent of
the display number.

Display kind and number are available even without LDAP or BLF; stable identity
falls back to the existing route. The UI reads the same phonebook and calls the
existing call owner through `get_contact(index)` and `call_contact(stable_id)`.
The old transport-row audio hints are still validated and discarded, never
displayed as a number. Stable IDs, provider and account storage remain gated.
Contact metadata is shared with BLF: managed HA resources
use their declared kind; ordinary external LDAP contacts may opt into automatic
watching as `CONTACT`. Groups, services and automations are excluded by BLF's
existing policy. LDAP itself starts no subscription lifecycle.

Resource limits are explicit:

- One permanent notification-driven worker, one connection and one mutex for
  configuration/results. Descriptor cancellation adds one short descriptor
  lease mutex; only the worker destroys TLS and closes its connection.
- Default worker stack 8192 bytes, priority 2, no affinity. Stack placement is
  internal by default; optional PSRAM stack follows the IDF configuration.
- Default receive buffer 8192 bytes (configurable 1024..65536), up to 64 staged
  contacts. Buffers prefer PSRAM by default with internal fallback.
- The canonical phonebook shares its 64 slots across all sources. With signaling
  PSRAM enabled its records and retained text use that placement and slots are
  reserved at setup. Staged records are moved into the canonical owner.
- Host ABI measurements: ContactEntry 216 bytes with ordinary strings, 264 bytes
  with the signaling wrapper. At 64 rows the stage is 13824 or 16896 host bytes;
  these are not ESP ABI or physical RAM measurements. Warm identical and changed
  nine-row provider commits make zero additional allocations in the host gate.
- Network work stays on the worker. The main loop commits at most 64 rows, then
  publishes existing phonebook callbacks. TLS connection steps have a 25 ms wait
  slice within the overall connection deadline; idle workers do no polling.
  DNS resolution remains the platform resolver's blocking contract.

Host checks cover independent LDAP BER encoding, a real ldaptor server, real
TCP/TLS peers through an OpenSSL implementation of the public ESP-TLS interface,
atomic ownership and failure cleanup under sanitizers. They do not qualify the
Espressif TLS backend, target stack high-water, flash, CPU or physical devices.

Protocol references: [RFC 4511](https://www.rfc-editor.org/rfc/rfc4511),
[RFC 4513](https://www.rfc-editor.org/rfc/rfc4513),
[RFC 2696](https://www.rfc-editor.org/rfc/rfc2696) and
[ESP-TLS](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/api-reference/protocols/esp_tls.html).
