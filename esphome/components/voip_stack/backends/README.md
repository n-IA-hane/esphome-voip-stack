# VoIP Stack Backends

Phase 00V introduces backend boundaries for host simulation.

Rules:

- domain logic must not include platform-specific code;
- ESP-IDF and host implementations live in sibling backend folders;
- `#ifdef USE_HOST` is allowed in backend/factory files only;
- tests must use snapshots exposed through the simulator control socket.

Initial backend interfaces to extract:

- `AudioBackend`
- `CodecBackend`
- `DisplayBackend`
- `InputBackend`
- `RuntimeClock`
- `TaskExecutor`
- `NetworkBackend`
- `FaultInjector`
