# Source

Extracted from `n-IA-hane/esphome-intercom`.

This repository contains the standalone ESPHome `voip_stack` component and its
optional ESP32-P4 encoded-video adapters. `esp_jpeg_video_source` and
`esp_h264_video_source` bridge an external ESPHome camera to the common video
contract, while `p4_video_renderer` owns receive decode and presentation.

Camera sensor and V4L2 lifecycle support remain owned by
[`Psix-anp/esphome-esp-video-camera`](https://github.com/Psix-anp/esphome-esp-video-camera).

The Home Assistant integration, Lovelace card, maintained product YAMLs, voice
assistant packages and full device examples remain in
[`esphome-intercom`](https://github.com/n-IA-hane/esphome-intercom).
