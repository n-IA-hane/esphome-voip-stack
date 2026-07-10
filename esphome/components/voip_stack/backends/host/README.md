# Host Backend

Design placeholder only. No Linux host backend or virtual-device binary ships
from this directory. The original proposal considered:

- WAV/PCM microphone input;
- WAV/PCM speaker output;
- virtual codec state;
- framebuffer/screenshot backend;
- virtual input controls;
- deterministic clock;
- JSON-RPC control socket;
- fault injection.

Tests in this repository must not be described as a hardware-equivalent host
backend unless an implementation and its validation artifacts are added.
