# VoIP Stack Backends

These directories are design placeholders retained from the Phase 00V host
simulation proposal. No selectable backend abstraction or host simulator ships
in this standalone component today; production code uses ESP-IDF directly.

Rules:

- domain logic must not include platform-specific code;
- ESP-IDF and host implementations live in sibling backend folders;
- `#ifdef USE_HOST` is allowed in backend/factory files only;
- tests must use snapshots exposed through the simulator control socket.

Interfaces considered by that proposal were:

- `AudioBackend`
- `CodecBackend`
- `DisplayBackend`
- `InputBackend`
- `RuntimeClock`
- `TaskExecutor`
- `NetworkBackend`
- `FaultInjector`

Do not configure or depend on these names as public APIs. The directories may
be removed or populated only as part of a separately tested implementation.
