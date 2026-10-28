# Software JPEG display renderer

`esp_video_renderer` receives complete baseline JPEG images and presents RGB565
surfaces in an LVGL container. It can run independently of VoIP. An optional
adapter connects it to the existing VoIP `video.sink` interface.

The first target is ESP32-S3 with PSRAM. ESP32-P4 can also compile the software
backend, while the existing `p4_video_renderer` remains the hardware-oriented
choice. The decoder is Espressif
[`esp_jpeg` 1.3.2](https://components.espressif.com/components/espressif/esp_jpeg/versions/1.3.2/readme).
Progressive JPEG and H.264 are outside this component's contract.

## Display-only use

Keep the board's normal `display`, `spi`, `psram` and `lvgl` declarations. Add:

```yaml
external_components:
  - source: github://n-IA-hane/esphome-voip-stack@dev
    components: [esp_video_renderer]

esp_video_renderer:
  id: incoming_video
  width: 320                 # Maximum decoded surface width.
  height: 240                # Maximum decoded height.
  framerate: 2               # Limits decode admission, even if a producer is faster.
  max_jpeg_bytes: 131072     # Capacity of each of the two compressed input slots.
  task_core: any            # any, 0 or 1. No forced affinity by default.
  task_priority: 5          # FreeRTOS priority, below the maintained audio tasks.
  task_stack_size: 8192     # Bytes reserved once at boot.
  task_stack_in_psram: true # Keep the worker stack out of internal/DMA memory.
  buffer_in_psram: true     # Placement of JPEG slots, RGB565 surfaces and decoder workspace.

lvgl:
  on_boot:
    - lambda: id(incoming_video).attach_video_container(id(video_surface));
  pages:
    - id: video_page
      widgets:
        - obj:
            id: video_surface
            width: 320
            height: 240
            pad_all: 0
            border_width: 0
```

Start the receiver with `esp_video_renderer.start: incoming_video`, then feed a
complete image from an existing producer:

```cpp
bool accepted = id(incoming_video).submit_jpeg(jpeg_data, jpeg_length);
```

The data is copied before this call returns. The producer may immediately reuse
its own buffer. A false result means the image was not admitted. Stop with
`esp_video_renderer.stop: incoming_video`.

This example does not create a network server or a camera. A source component
owns acquiring and transporting its images. The renderer owns decoding and
presenting them.

## VoIP use

Load both external components, then add an explicit sink adapter:

```yaml
external_components:
  - source: github://n-IA-hane/esphome-voip-stack@dev
    components: [voip_stack, esp_video_renderer]

esp_video_renderer:
  id: incoming_video
  width: 320
  height: 240
  framerate: 2
  voip:
    id: incoming_video_sink
  on_first_frame:
    - lvgl.page.show: video_page
  on_video_ended:
    - lvgl.page.show: home_page

voip_stack:
  video:
    codec: jpeg
    sink: incoming_video_sink
    width: 320
    height: 240
    framerate: 2
```

The existing microphone, speaker, SIP and LVGL configuration is still required.
The VoIP Stack owns negotiation, RTP and session control. The renderer adds no
second transport or call state machine. Add the existing native `camera_id`
option separately if the same S3 board also has a camera.

The Intercom package `packages/voip/s3_video_jpeg.yaml` supplies this connection
using `s3_video`, `s3_video_sink` and the container selected by
`s3_video_container_id` (default `call_video_surface`). The application supplies
the container and its page-navigation policy.

The Waveshare Box S3 has a display but no camera. Its initial receive scenario
uses video sent by the HA browser. Testing simultaneous capture and display on
one S3 requires a board with both peripherals and a compatible pin allocation.

## Buffer ownership and scheduling

The component reserves two JPEG input slots and two RGB565 output surfaces at
boot. With the defaults the image buffers use 569,344 bytes of PSRAM, plus a
3100-byte preallocated decoder workspace (in PSRAM with `buffer_in_psram: true`),
an 8 KiB worker stack in PSRAM and internal task/control structures. The VoIP
transport's JPEG reassembly and LVGL display buffers are additional allocations.

Task and memory options are applied at boot. `task_priority` accepts 1 through
24. `task_stack_size` accepts 4096 through 65536 bytes, in multiples of 16.
`task_core` accepts `any`, `0` or `1`. The default remains unpinned priority 5
with an 8192-byte stack in PSRAM. The task control block stays in internal RAM.

Setting either placement option to `true` requires a declared and functioning
`psram` component. An unavailable requested heap causes explicit setup failure;
the component does not silently substitute internal memory. Internal image
placement requires reducing the configured image/input capacities to a budget
that fits the board.

One worker blocks on notifications when idle. It decodes away from the main loop
at priority 5, below the maintained audio tasks. The mailbox retains only the
latest queued image. An image already being decoded retains its own slot. The
main loop alone calls LVGL, and a surface currently referenced by LVGL stays
immutable until refresh completion permits the next surface swap.

Session generations reject a decode that finishes after stop, suspension or
restart. Stopping preserves allocations for the next call. Shutdown retains
surfaces that a display might still reference until the device resets.

Complete single-scan baseline JPEG headers and the end marker are checked before
the vendor decoder receives input. By default, the S3 uses ROM decoding through
the official component. Source-decoder configurations using its large Huffman lookup
mode reserve a 65,472-byte workspace instead.

Configured widths and heights must be multiples of 8, with at most 640x480
pixels. They size the output surfaces, rather than requiring the sender to use
that exact resolution. The decoder selects native size, half size or quarter
size, choosing the finest result that fits both dimensions. This uses
Espressif's decoder reduction directly, without an additional resize buffer.
For a 320x240 surface, a 640x360 browser image becomes 320x180 and a 1280x720
image also becomes 320x180. Smaller inputs remain at native size.

Native input is limited to four times each configured dimension, 1280 pixels
per axis, and 1,228,800 pixels in total. The compressed image must also fit
`max_jpeg_bytes`. Images beyond these limits are rejected before decoding.
Single-scan baseline validation remains required at every reduction. For odd
dimensions, the decoder rounds each reduced dimension down and drops the final
partial row or column. LVGL receives the resulting width, height and stride.
Images are centered without upscaling or aspect-ratio distortion. Set 224x168
for a 4:3 output that fits a 240x240 display.

The current JPEG call contract negotiates encoding and frame rate, without
transmitting the renderer's width and height as remote camera constraints.
The receiver therefore checks each image and applies the bounded reduction.

`get_stats()` returns admissions, replacements, rejections, successful decodes,
decode errors, LVGL submissions and maximum elapsed decode/frame-age times.
`dump_state()` logs these counters on demand at INFO level, without per-frame
logging. Elapsed decode time includes scheduling and is not exclusive CPU time.
The submission count does not prove that the physical panel displayed every image.

## Wake-word policy and qualification

For FULL Intercom profiles with MWW, include
`packages/voip/video_mww_policy.yaml`. It uses the existing `on_video_start` and
`on_video_end` hooks to suspend recognition without changing the persisted Wake
Word preference. The shared safe-start helper respects video suspension, mute,
OTA and initialization. A Wake Word change during the call remains authoritative.
VoIP-only profiles require neither MWW nor Runtime Controller.

320x240 at 2 fps is the initial qualification target. Raising it to 5 fps requires
measurement with bidirectional audio on the actual board. Host lifecycle tests
and a successful firmware build do not establish physical frame rate, panel
correctness or acoustic continuity.

## Repeatable software pixel check

With Pillow, gcc/g++ and the official unpacked `esp_jpeg` 1.3.2 component:

```bash
python tools/qualify_esp_video_renderer_jpeg.py \
  --component /path/to/espressif__esp_jpeg \
  --output /path/to/capture-directory
```

The check runs the official wrapper and source decoder under ASan/UBSan, checks
RGB565 byte order and color error for 4:4:4, 4:2:2, 4:2:0 and grayscale at native,
half and quarter size, and verifies odd-dimension output strides. It guards the
output bounds and rejects malformed headers and every truncated prefix before
decoding. Add `--input-jpeg /path/to/captured.jpg` to replay an actual image into
a 320x240 surface. The tool produces JPEGs, decoded PNGs and a JSON receipt.
ROM execution, real display presentation and concurrent audio still
require the identified hardware firmware.
