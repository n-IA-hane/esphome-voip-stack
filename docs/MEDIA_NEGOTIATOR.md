# Firmware media contract owner

`media_negotiator.h/.cpp` prepares a media value from the exact local formats,
the sent offer, retained media and the remote description. SIP dialogs still
own commit and rollback; RTP tasks still own packet transport. The optional
trace callbacks observe decisions without allocating or owning another call.

Firmware formats are fixed sets. A feasible receiver packet-time request wins
(T1); otherwise the existing YAML preference wins among formats actually
declared and inside `maxptime` and the datagram budget. The dynamic HA default
policy in CC-11 does not enlarge an ESP fixed set. The compiled production
fixture preserves 16 kHz/20 ms transmit and 48 kHz/10 ms receive as caller and
callee with the independent directional profile.

The static audio registry recognizes G.722/9 and L16 at 44.1 kHz as stereo/10
and mono/11 (CC-12). An explicit conflicting mapping removes the L16 payload
without suppressing another usable mapping. Recognition grants no encoder or
duration: undeclared 44.1 kHz remains unavailable, and stereo/10 at 10 ms
exceeds the 1200-byte budget and is refused. A declared mono/11 at 10 ms uses
882 bytes and keeps the configured duration.

Opus receiver parameters belong to the selected transmitter. The bounded
`opus_tx` value follows `SdpAudioMedia` through proposals, commit, rollback and
REFER exchange. Only the selected remote payload's `maxaveragebitrate`,
`maxplaybackrate` and `stereo` are read; missing parameters restore defaults.
Invalid bitrate values outside 6000..510000 are ignored per RFC 7587. Playback
rate controls the encoder bandwidth, never the 48 kHz RTP clock or local PCM
shape. Stereo is a receiver preference and does not grant a local stereo input.
Our answer keeps our own receive parameters instead of echoing the peer's.

The existing encoder owner applies libopus controls before encoding a stable
committed frame. It retains codec allocations and decoder state. The local
24000 bit/s target and VBR remain the defaults without a remote bitrate ceiling.
An explicit ceiling selects CBR at the smaller of 24000 and that ceiling:
libopus speech VBR can exceed its nominal average target and ignores the VBR
constraint control. This is an encoder policy to satisfy an average bound,
not an interpretation of `maxaveragebitrate` as a per-packet limit. Complete
production signaling and codec units link to libopus in the host witness;
actual RTP packets verify clock, payload, bandwidth and rollback behavior.

Telephone events negotiate payload, events and RTP clock separately per
direction (P5/CC-4). G.722 uses its 8 kHz RTP clock even though decoded PCM is
16 kHz. An offered 8 kHz event mapping remains the fallback. A running digit
keeps its format, clock, SSRC and timestamp through a re-offer and its three
terminal repeats. The next digit uses the newly committed contract. The RTP
worker fixture checks this against independent UDP packets.

For H.264, a renumbered answer selects its payload for transmit and preserves
the offer's payload for receive (P3/CC-9). No receive alias is created. JPEG
keeps static payload 26. Video meanings survive optional removal and later
re-offers; changing a used payload to another codec or clock rejects the
proposal before commit (P4). Two-bit meaning tables use 67 bytes per stored
video contract, compile-gated with video. There is no allocation in the
history lookup and no extra media task or lifecycle.

Direct unit fixtures link complete production C++ files. Existing signaling
fixtures retain their platform adapters and compile the same owner. Some
video-only fixtures previously depended on header instantiation omitting
unused audio code; their linker now discards unused function sections, matching
the native production test link. This resolved missing unused audio parser
symbols without changing media expectations. The previous single-PT source
assertion was updated only after directional SDP and independent RTP witnesses
passed.

These host fixtures are software and packet evidence. They do not qualify
acoustic performance, hardware panels or a deployed device.
