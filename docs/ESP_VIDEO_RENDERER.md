# ESP Video Renderer

`esp_video_renderer` is maintained in the standalone
[ESP Video Renderer repository](https://github.com/n-IA-hane/esphome-video-renderer/tree/dev).
The [canonical guide](https://github.com/n-IA-hane/esphome-video-renderer/blob/dev/README.md)
covers standalone JPEG rendering, settings, examples, qualification and the
optional `voip_adapter.h` adapter.

Load `esp_video_renderer` and `voip_stack` from their separate external
component sources, then enable `esp_video_renderer.voip` and select its sink
in `voip_stack.video`. The transport and renderer contracts are unchanged.
