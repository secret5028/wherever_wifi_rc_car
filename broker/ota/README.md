Place manual OTA artifacts in this directory.

Expected files:
- `manifest.json`
- `phase1_esp32.bin`

Minimal manifest shape:

```json
{
  "available": true,
  "version": "2026-03-26-1",
  "bin": "/ota/phase1_esp32.bin",
  "notes": "optional release notes"
}
```

The ESP32 downloads `http://<broker-host>:<broker-port>/ota/manifest.json` after receiving an `ota_update` command from the browser UI.
