# RC Car Phase 1

Phase 1 verifies the remote control path:

`browser/client -> Oracle VM broker -> ESP32`

This repository currently contains:

- `broker/`: Node.js WebSocket broker for the public VM
- `web/`: simple browser test client
- `firmware/phase1_esp32/`: Arduino-based ESP32 firmware for Wi-Fi + broker connection
- `WORKLOG.md`: cumulative execution log and confirmed results

Current prototype features:

- remote control through the Oracle VM broker
- local and remote MJPEG video
- browser-side audio playback path for low-bitrate mu-law chunks
- ESP32 firmware scaffolding for built-in microphone uplink

## Phase 1 Success Criteria

- ESP32 connects to Wi-Fi
- ESP32 opens an outbound WebSocket connection to the broker
- Broker keeps the ESP32 session alive
- A browser client connects to the broker and sends control messages
- ESP32 receives control messages and reports status back

## Layout

```text
broker/
  package.json
  src/
    config.js
    server.js
web/
  index.html
firmware/
  phase1_esp32/
    phase1_esp32.ino
    secrets.example.h
```

## Notes

- The broker is designed to run on the Oracle VM.
- The firmware is intentionally limited to Phase 1 scope: networking, heartbeat, reconnect, command reception, and safe-stop behavior.
- Motor and servo drive hooks are already present, but hardware tuning belongs to the next step after the end-to-end control path is verified.
- Confirmed progress and environment decisions are tracked in `WORKLOG.md`.
