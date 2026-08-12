# Third-Party Notices

Project code in this repository is MIT-licensed unless a file states otherwise.

`esphome/components/ring_buffer/` is an ESPHome-derived local copy used by the
VoIP RTP/audio buffering path. ESPHome's license is included in
`licenses/ESPHOME-LICENSE.txt`.

No third-party source or binary libraries are vendored by `voip_stack`; it
binds to ESPHome microphone, speaker, networking and ESP-IDF socket APIs at
build/runtime.
