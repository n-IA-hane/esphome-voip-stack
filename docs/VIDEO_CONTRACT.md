# Optional video contracts

Video remains compile-gated. An encoded source and sink use the existing SIP
dialog, RTP session and media lifecycle. Audio-only profiles add no video tasks,
frame storage or codec dependency. JPEG and H.264 profiles select one codec.

## Direction and cadence

Local transmission and reception retain their own capability and frame budget.
The peer's SDP `a=framerate` recommends our encoding rate, as specified by
[RFC 8866 section 6.13](https://www.rfc-editor.org/rfc/rfc8866#section-6.13).
It does not lower our decoder's local admission budget. Our answer advertises
our receive budget when receiving. For example, a 15 FPS camera, 2 FPS sink and
peer recommendation of 10 yield TX 10 FPS, RX 2 FPS and answer `framerate:2`.
The inverse 2/15 configuration retains TX 2 FPS and RX 15 FPS.

JPEG supports bounded [RFC 6236](https://www.rfc-editor.org/rfc/rfc6236)
`imageattr` for static payload 26 and wildcard selectors. Peer `send` matches
the local receiver; peer `recv` matches the local camera's actual dimensions.
Payload-specific preferences override wildcard preferences separately for
each declared direction. Unknown parameters are ignored. Omission preserves
legacy negotiation. An incompatible answer requests at most one fallback
offer without image attributes through the existing re-INVITE transaction.
H.264 retains its existing RFC 6184 profile and level-asymmetry negotiation.

The stable P4 envelopes remain JPEG 800x800 at 15 FPS, and H.264 TX 400x400 /
RX 352x288 at 10 FPS. These are configured ceilings, not new hardware throughput
measurements. Camera software admission retains fractional phase, bounds
recovery credit and runs before PPA/JPEG work. Driver-reported frame intervals
are checked rather than assuming a successful ioctl applied the requested rate.

## Frame ownership and bounds

`EncodedVideoSink::max_encoded_access_unit_bytes()` defaults to zero, preserving
the codec's existing envelope. A bounded sink may lower the receive pool size.
Resizing is refused while workers or outstanding receiver leases own storage.
The optional software JPEG renderer defaults to 131072 bytes per encoded frame,
320x240 RGB565 and 2 FPS admission, independently of camera TX capabilities.

`EncodedVideoSource::uses_access_unit_leases()` defaults to false. The existing
P4 H.264 source returns true because each output carries its actual release
callback. RTP queues that same access unit and releases it on completion, drop
or teardown. Its redundant transport TX copy buffers are omitted. JPEG camera
callbacks remain borrowed; their bounded TX copies remain necessary.

Completed RTP/JPEG frames carry validated dimensions and sampling metadata to
the P4 decoder. Metadata belongs to the exact access unit and generation;
actual decoded output is checked before publication. Delayed frames cannot
reopen stopped video or appear in a later audio-only call.

An MIPI provider that advertises and implements owned RGB565 transformation
allows P4 to retain one decode surface and transform into the display's existing
framebuffer. PPA and panel completion own the source until return. Providers
without that build-time capability retain the two-surface path. A Python marker
alone does not establish the runtime contract.

## Resource accounting and evidence limits

Transport pool requests, excluding codec/source/display-owned storage:

| Configuration | Bytes |
| --- | ---: |
| JPEG full receive envelope, one RX slot plus quantization cache | 540544 |
| JPEG software sink at 131072, one RX slot plus cache | 147328 |
| JPEG borrowed TX copy, per slot | 524288 |
| H.264 three RX slots | 393216 |
| H.264 leased TX, additional transport copy storage | 0 |

The existing paced H.264 transport omits two 131072-byte TX buffers; the unpaced
transport omits one. IDF task control blocks already owned by the managed task
helper are no longer duplicated in video RTP and the H.264 source. Actual target
TCB size and final firmware flash/internal/DMA usage require the combined build.

One owned JPEG surface at the 800x800 decode bound saves 1280000 bytes versus
two surfaces. Older adapters keep both surfaces. This does not reduce the
display framebuffer or decoder/PPA allocation budgets. Optional facilities
introduce no additional per-call task or allocator owner.

Host gates execute actual production methods, allocation requests and lease
callbacks under sanitizers, plus real loopback UDP/RFC 2435 and the official
portable `esp_jpeg` 1.3.2 decoder. Independent libjpeg pixels verify restored
bitstreams, RGB565 byte order and native/half/quarter output. Platform adapters
cover RTOS, PPA, hardware codecs and displays. They do not qualify peripheral
DMA/cache behavior, ROM execution, physical panel output, acoustic continuity,
exclusive CPU load or sustained hardware FPS. The combined firmware and exact
physical call routes remain separate qualification gates.
