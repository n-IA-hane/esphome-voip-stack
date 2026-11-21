# Subscription state and recovery

The `SipMwi` subscription owner is shared by mailbox, dialog and presence
packages. BLF is a view of accepted dialog documents, not an inference from
how recently a phone sent a packet.

The owner renews a lease at 80 percent of its lifetime. Each SUBSCRIBE starts
Timer N (64 times T1, currently 32 seconds). An unchanged resource can remain
quiet until renewal. A successful response without its required NOTIFY does
not manufacture a snapshot or extend the last confirmed lease. The earlier
of Timer N and lease expiry invalidates the public state to `unknown` and
starts recovery. Only an accepted complete empty dialog document means idle.

References: [RFC 6665 sections 4.1.2.2 and 4.1.2.4](https://www.rfc-editor.org/rfc/rfc6665.html#section-4.1.2.2).

The consolidation comparison found that these lease rules already belonged
to the production owner. The falsifiable hypothesis was that a quiet active
dialog would survive until renewal, a missing refresh NOTIFY would expose
`unknown` at Timer N, and a new full snapshot would recover it. The direct
production fixture `tests/fixtures/blf/adversarial.cpp` covers both successful
renewal and missing refresh notification. No quiet-period watchdog was added.

The independent admission witness also showed that one unresolved Account SIP
previously rejected the complete contact watch list. Known accounts are now
applied, unresolved rows are omitted, and the caller receives a partial result
and logs it. Account changes trigger another contact synchronization.

UDP ingress now uses `recvmsg` and rejects `MSG_TRUNC` before SIP dispatch.
This matters because lwIP can report the original datagram length after
copying only a prefix. Both that length and the truncation flag are checked.
The implementation was compared with the ESP-IDF 5.5.6 lwIP adapter and the
[lwIP 2.2.1 implementation](https://github.com/lwip-tcpip/lwip/blob/STABLE-2_2_1_RELEASE/src/api/sockets.c).
The socket fixture checks fitting, exact-limit and oversized datagrams.

The inbound TCP spare still closes only after all framed records and unread
socket data are drained. Its existing bounded burst fixture covers auxiliary
traffic during a registered call. Registration flows and dialog identity
remain owned separately; a drained spare does not replace either owner.
