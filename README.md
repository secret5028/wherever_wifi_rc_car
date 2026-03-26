# RC Car Telepresence Prototype

Current architecture:

`browser -> Oracle VM broker -> ESP32-S3 Sense`

The device is intended to behave like this:

- first boot: AP provisioning mode
- AP page stores Wi-Fi + broker settings
- after that: board stays connected to Wi-Fi and broker while power remains on
- browser requests media on demand
- control stays on a separate WebSocket path

## Current State

- broker relay is running on the Oracle VM
- ESP32 keeps an outbound WebSocket connection to the broker
- AP mode serves the local setup/control page on the board
- broker mode exposes a remote browser UI with `START/STOP STREAM`
- broker-mode video and control are currently working again over plain `HTTP/WS`
- audio uplink works through the broker
- video uplink from ESP32 to broker works
- broker-mode video rendering is currently implemented through a hidden MJPEG feed plus visible canvas redraw
- external PTT is not finalized yet because browser microphone permission still requires a proper HTTPS path

## Repository Layout

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
WORKLOG.md
PINMAP.md
```

## Main Runtime Pieces

- `broker/src/server.js`
  - device/client WebSocket relay
  - remote MJPEG relay at `/video/:deviceId`
- `web/index.html`
  - AP mode local UI
  - broker mode remote UI
  - on-demand stream start / stop
  - PTT path
- `firmware/phase1_esp32/phase1_esp32.ino`
  - AP provisioning
  - Wi-Fi / broker reconnect
  - camera/audio capture
  - motor/servo control

## Local Bootstrap

```powershell
.\scripts\bootstrap.ps1
```

Optional broker dependency install:

```powershell
.\scripts\bootstrap.ps1 -InstallBrokerDeps
```

## Manual OTA

Manual OTA now uses the existing broker VM as the firmware host.

- Broker serves OTA artifacts from `broker/ota/`
- ESP32 fetches `http://<broker-host>:<broker-port>/ota/manifest.json`
- Remote browser UI sends an `OTA` command over the broker WebSocket

Expected OTA files:

- `broker/ota/manifest.json`
- `broker/ota/phase1_esp32.bin`

Minimal manifest example:

```json
{
  "available": true,
  "version": "2026-03-26-1",
  "bin": "/ota/phase1_esp32.bin"
}
```

Notes:

- The current firmware builds OTA URLs from the configured broker host/port and uses plain HTTP.
- OTA is disabled while the board is in AP mode.
- During OTA, video/audio streaming is paused and the board restarts after a successful flash.
