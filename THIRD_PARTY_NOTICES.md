# Third-party notices

Project code in this repository is MIT-licensed unless a file states otherwise.

`voip_stack` binds to ESPHome microphone, speaker, networking and ESP-IDF
socket APIs. Codec libraries are resolved at build time by ESP-IDF Component
Manager according to the configured audio codecs. Their upstream licenses
continue to apply to the resulting firmware.

## Opus codec dependency

When Opus is configured for transmission or reception, `voip_stack` selects
[`esphome-libs/micro-opus`](https://github.com/esphome-libs/micro-opus/tree/93bf9c10196e368405ee1501bfd72f0be2518741)
at commit `93bf9c10196e368405ee1501bfd72f0be2518741`. This dependency is
compiled only when the Opus codec is enabled by this component or another
firmware component. It provides the Opus codec and includes the following
upstream components:

| Component | License | Preserved license and notices |
|---|---|---|
| microOpus wrapper and ESP32 adaptations, copyright 2025-2026 Kevin Ahrendt | Apache-2.0 | [LICENSE](licenses/MICRO-OPUS-LICENSE.txt), [NOTICE](licenses/MICRO-OPUS-NOTICE.txt) |
| [Opus audio codec](https://github.com/xiph/opus/tree/22244de5a79bd1d6d623c32e72bf1954b56235be), commit `22244de5a79bd1d6d623c32e72bf1954b56235be` | BSD-3-Clause | [COPYING](licenses/OPUS-COPYING.txt), including upstream copyright, conditions, disclaimer and links to the royalty-free patent-license grants |
| microOggDemuxer bundled with microOpus, copyright 2025 Kevin Ahrendt | Apache-2.0 | [LICENSE](licenses/MICRO-OGG-DEMUXER-LICENSE.txt), attribution in the [microOpus NOTICE](licenses/MICRO-OPUS-NOTICE.txt) |

The four legal files above are unmodified copies from the pinned microOpus
dependency. Paths such as `lib/opus/COPYING` inside its NOTICE refer to the
upstream dependency layout; the table maps them to the copies kept here.

When redistributing firmware containing this dependency, include this notice
and all four legal files above with the firmware or its accompanying
documentation. A dependency downloaded into a local build directory does not
by itself provide those materials to firmware recipients. Preserve upstream
copyright, license, patent and attribution notices when redistributing the
dependency's source, and identify any changes to upstream files as required
by their licenses. Do not use upstream contributor names to endorse derived
products without permission.

## ESPHome and other firmware dependencies

The ESPHome license is preserved in
[`licenses/ESPHOME-LICENSE.txt`](licenses/ESPHOME-LICENSE.txt). ESPHome and
other components selected by the complete firmware configuration retain
their own license and source-distribution requirements. The Opus materials
above cover this codec dependency, not every component of a complete firmware.
