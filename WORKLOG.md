# Work Log

This file tracks confirmed progress, successful actions, current state, and the next action to take.

Rules:

- Record only completed work as fact.
- Separate confirmed facts from assumptions.
- Leave enough detail to resume work immediately.

---

## 2026-03-08

### Goal

- Build an ESP32-S3 Sense RC car.
- Control it from outside the local network with a phone or PC.
- Keep the first milestone small: prove the remote path works.

### Architecture Decision

Chosen direction:

- `ESP32 -> Oracle VM broker <- browser/client`
- ESP32 will make an outbound connection to the public broker.
- The browser or client will connect to the same public broker.
- `N100` is not required for the initial build.

Rejected as the primary path:

- Nabto Edge: more complexity, paid platform concerns, weak fit for browser-first control.
- Husarnet: support and mobile/browser path concerns for this project.
- N100 as the first broker: possible, but Oracle VM alone is simpler for Phase 1.

### Oracle VM

Confirmed instance:

- Name: `rc-car-broker`
- Public IP: `158.179.163.102`
- OS: `Ubuntu 22.04`
- Shape: `VM.Standard.E2.1.Micro`
- Region: `ap-chuncheon-1`
- User: `ubuntu`

Completed:

- Oracle VM created
- Public IP confirmed
- SSH access confirmed
- Private key permission issue fixed on Windows

SSH command used:

```powershell
ssh -i "C:\Users\AAA\Downloads\ssh-key-2026-03-08.key" ubuntu@158.179.163.102
```

### Repository Bootstrap

Initial state:

- Working directory was effectively empty
- Project started as a greenfield setup

Created structure:

```text
README.md
PHASE1.md
WORKLOG.md
arduino-cli.yaml
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
    secrets.h
```

### Phase 1 Broker

Implemented:

- Node.js WebSocket broker
- Browser test console
- ESP32 firmware skeleton for:
  - Wi-Fi connect
  - outbound WebSocket connect
  - heartbeat
  - reconnect
  - safe-stop
  - `ctrl` receive
  - `status` publish

Local checks completed:

- `npm install`
- `node --check` on broker files
- broker startup log confirmed

Observed broker log:

```text
broker listening on http://0.0.0.0:8080
device ws path: /device
client ws path: /client
```

### Oracle VM Deployment

Completed:

- Uploaded broker and web files to Oracle VM
- Installed Node.js and npm on the VM
- Started the broker on the VM

Deployment paths:

```text
~/rc-car
~/rc-car/broker
```

Confirmed on VM:

- broker process running
- broker listening on `0.0.0.0:8080`
- local VM curl to `http://127.0.0.1:8080/` succeeded

### Network Opening

Initial problem:

- External access to `http://158.179.163.102:8080/` failed

Root causes identified:

- Oracle Security List needed an ingress rule for `TCP 8080`
- The Oracle Ubuntu image had host firewall rules allowing `22` only

Completed fixes:

1. Added Oracle Security List ingress rule:
   - Source: `0.0.0.0/0`
   - Protocol: `TCP`
   - Destination port: `8080`

2. Opened host firewall port `8080` and saved rules:

```bash
sudo iptables -I INPUT 4 -p tcp --dport 8080 -j ACCEPT
sudo netfilter-persistent save
```

### External Access Proof

Confirmed:

- Packet capture showed an external request from the local PC reaching the VM on `8080`
- VM returned `HTTP/1.1 200 OK`
- Browser opened `http://158.179.163.102:8080/`
- The `RC Car Phase 1 Console` page rendered successfully

Meaning:

- The public broker path is real, not theoretical
- The browser-facing control UI path also works externally

### ESP32 Toolchain

Completed:

- Found installed Arduino IDE
- Found bundled `arduino-cli.exe`
- Created project-local Arduino CLI config in `arduino-cli.yaml`
- Installed `esp32:esp32` board core
- Installed required libraries:
  - `WebSockets`
  - `ArduinoJson`
- Detected connected serial device on `COM3`
- Compiled the firmware successfully for `esp32:esp32:XIAO_ESP32S3`

Detected board and port information:

```text
Port: COM3
Detected board family: ESP32 Family Device
Compile target: esp32:esp32:XIAO_ESP32S3
```

Compile result:

```text
Sketch uses 1051672 bytes (31%) of program storage space. Maximum is 3342336 bytes.
Global variables use 46164 bytes (14%) of dynamic memory, leaving 281516 bytes for local variables. Maximum is 327680 bytes.
```

Meaning:

- The Phase 1 firmware builds correctly with the current toolchain
- The ESP32 side is now blocked by credentials and upload/runtime validation, not by missing tools

### ESP32 Firmware Upload

Completed:

- Updated `firmware/phase1_esp32/secrets.h`
  - `WIFI_SSID`: `Public Wifi Free`
  - `WIFI_PASSWORD`: empty string
- Recompiled firmware with the real SSID
- Uploaded firmware successfully to `COM3`

Observed upload facts:

- Target chip identified as `ESP32-S3`
- Embedded PSRAM detected: `8MB`
- Upload completed successfully
- Board reset after flashing

Meaning:

- The firmware is now on the physical board
- The next validation step is runtime behavior, not build or upload

### Runtime Findings After Upload

Observed issues:

- Open public Wi-Fi failed due to captive portal / timeout behavior
- Switched test network to phone hotspot:
  - SSID: `MyHotspot`
  - password: set
- ESP32 reached the WebSocket stage, but repeated connection attempts caused crashes

Root causes found:

1. Wi-Fi retry bug in firmware
   - `WiFi.begin()` was being called repeatedly while the station was still connecting
   - This produced `sta is connecting, cannot set config`

2. Broker path-matching bug
   - ESP32 connected to `/device?deviceId=rc-car-01`
   - Broker only accepted an exact path match for `/device`
   - Query string caused the upgrade request to be rejected immediately

Fixes applied:

- Firmware:
  - Added guarded Wi-Fi retry logic with timeout
  - Simplified WebSocket recovery logic to avoid repeated in-flight begin calls
- Broker:
  - Changed WebSocket upgrade matching to compare URL pathname instead of full raw URL

Broker redeploy:

- Updated `broker/src/server.js` on the Oracle VM
- Restarted the broker
- Confirmed broker startup log again:

```text
broker listening on http://0.0.0.0:8080
device ws path: /device
client ws path: /client
```

### Camera Phase 1.5

Goal:

- Bring up the on-device camera
- Verify a browser can view live MJPEG from the ESP32

Completed:

- Added camera initialization for XIAO ESP32S3 Sense
- Added local HTTP camera endpoints:
  - `/`
  - `/jpg`
  - `/stream`
- Added camera status fields to broker status messages:
  - `cameraReady`
  - `streamPort`
  - `streamPath`
  - `localIp`

Important finding:

- The XIAO ESP32S3 camera required PSRAM enabled (`PSRAM=opi`) for stable camera bring-up

Problems encountered and fixes:

1. Camera init initially failed with:
   - `0xffffffff`
   - resolved by rebuilding and flashing with `PSRAM=opi`

2. HTTP server startup initially triggered a queue/assert failure
   - camera came up, but starting the HTTP server too early caused instability
   - fixed by deferring camera HTTP server startup until after Wi-Fi was connected

3. Arduino CLI upload artifact handling was inconsistent for the PSRAM-enabled build
   - resolved by using `esptool` directly with the generated:
     - bootloader
     - partitions
     - boot_app0
     - app binary

Confirmed runtime logs:

```text
[BOOT] Phase 1 firmware
[SAFE] stop
[CAM] ready
[WIFI] connecting to MyHotspot
[CAM] http server started on :80
[WS] connecting
[WS] connected
[WS] broker hello
```

Confirmed camera browser access:

- Browser successfully displayed the live stream from:
  - `http://10.190.14.71/stream`

Meaning:

- The ESP32 camera is now working
- The ESP32 can simultaneously:
  - connect to Wi-Fi
  - connect to the Oracle broker
  - serve local MJPEG video

Current limitation:

- Video is only local-network reachable for now
- Remote video through Oracle VM is not implemented yet

### Current Phase 1 Status

Completed:

- Oracle VM created
- SSH access working
- Broker code written
- Broker deployed to Oracle VM
- External web access working
- Browser UI rendered from public IP
- ESP32 toolchain prepared
- ESP32 firmware compiled successfully
- ESP32 firmware uploaded to the board on `COM3`
- ESP32 local MJPEG camera streaming verified in the browser

Not completed yet:

- Verify `ESP32 -> Oracle VM` connection
- Verify `browser -> Oracle VM -> ESP32` control path on hardware
- Expose the camera stream through Oracle VM for remote viewing

### Current Blocker

The next required check is live serial/runtime verification.

The board has been flashed, but connection logs have not yet been captured from the serial console after boot.

Until the runtime log is observed, these two points remain unverified:

- Wi-Fi association to `Public Wifi Free`
- WebSocket connection to the Oracle broker

### Next Actions

1. Add Oracle VM video proxy path
2. Expose the ESP32 MJPEG stream through the public broker/VM
3. Verify remote browser video access
4. Then move to real motor/servo driving or further stream tuning

### Resume Point

If work resumes later, continue from:

- implement remote video proxy via Oracle VM

### Audio Prototype (In Progress)

Goal:

- Add low-bitrate microphone uplink using the XIAO ESP32S3 Sense built-in microphone
- Keep audio separate from video
- Prevent delay from growing over time by bounding the browser playback buffer

Completed locally:

- Confirmed the Arduino ESP32 core exposes `ESP_I2S` with `I2S_MODE_PDM_RX`
- Chosen built-in mic path:
  - PDM clock: `GPIO42`
  - PDM data: `GPIO41`
- Implemented broker forwarding for device `audio` messages
- Implemented browser audio playback path:
  - `AudioContext`
  - mu-law decode
  - bounded scheduling
  - resync/drop behavior when the queued audio gets too far ahead
- Implemented firmware-side audio scaffolding:
  - built-in mic initialization through `ESP_I2S`
  - 16kHz capture
  - downsample to 8kHz
  - mu-law encode
  - base64 wrap into JSON `audio` messages

Intended audio stream characteristics:

- codec: `mu-law`
- effective payload: `160 bytes / 20ms`
- approximate raw stream rate: `8KB/s`
- browser drops or resyncs late audio rather than letting delay accumulate

Current blocker:

- Rebuilding the updated firmware from the coding environment is currently blocked by Arduino CLI access/permission issues around the installed core path
- The source changes are present in the repository, but the audio firmware has not yet been re-flashed and verified on hardware from this session

Update:

- Worked around Arduino CLI issues by manually:
  - recompiling the sketch object with the cached Xtensa toolchain
  - relinking the ELF with the existing build artifacts
  - regenerating the firmware image through `esptool`
  - flashing the updated firmware to `COM3`

Flash result:

- bootloader / partitions / app all written successfully
- board reset completed after flashing

Next actions:

1. Deploy updated broker and web files to the Oracle VM
2. Rebuild/flash the updated firmware through a working Arduino compile path
3. Verify:
   - built-in microphone startup
   - browser audio playback
   - no runaway audio delay

### Audio Noise Mitigation

Observed issue:

- Voice was audible, but periodic noise kept riding on top of the signal.

Mitigation applied:

- Stopped dumping every `audio` packet to the browser log in `web/index.html`
- Added a simple first-order high-pass filter in firmware to reduce DC/low-frequency rumble
- Added a small noise gate so very low-level background hiss is zeroed before mu-law encoding
- Reduced filtered sample gain before mu-law encode to avoid harsh clipping artifacts

Deployment:

- Recompiled the sketch object against the cached ESP32 toolchain
- Relinked the ELF manually
- Regenerated the BIN with elevated `esptool`
- Reflashed the board on `COM3`

Expectation after this update:

- less periodic hum/click noise
- cleaner idle background
- lower chance of browser-side crackle from log flooding

### Audio Stability Follow-up

- Added browser-side client heartbeat so the broker no longer closes idle client WebSockets during audio monitoring.
- Added no-cache headers for static files so the console refreshes to the latest JavaScript instead of serving a stale cached page.
- Increased audio chunk size from `20ms` to `40ms`:
  - capture: `640 @ 16kHz`
  - stream: `320 @ 8kHz`
- Goal: reduce WebSocket/message overhead and browser scheduling jitter that can show up as periodic crackle.
- Rebuilt the sketch object, relinked the ELF, regenerated the BIN, and reflashed `COM3`.
- Updated `broker/src/server.js` and `web/index.html` were also redeployed to the Oracle VM.

### ADPCM And VGA Experiment

- Switched audio from `16kHz mu-law` to `16kHz IMA ADPCM`.
- Kept codec work on the edges:
  - ESP32 encodes
  - browser decodes
  - broker remains a relay
- Increased remote camera settings to:
  - `VGA`
  - `jpeg_quality = 12`
- Rebuilt the firmware, regenerated the image, and reflashed the board on `COM3`.

Observed issue:

- Severe crackle remained after the first ADPCM pass.

Root cause and fix:

- The browser ADPCM decoder was treating the chunk header predictor as a real output sample.
- This introduced a discontinuity at every audio chunk boundary.
- Fixed `web/index.html` so ADPCM decoding starts writing output samples only from the packed nibbles, while still using the header predictor and step index purely as decoder state.
- Redeployed the fixed `web/index.html` to the Oracle VM.

### ADPCM Block Format Correction

- Voice was still largely unintelligible even after the first ADPCM rollout.
- The next issue was the encoder/decoder pair not using the same block convention:
  - encoder header carried the previous predictor state
  - decoder behavior was then changed away from standard block playback
- Corrected both sides to a consistent IMA ADPCM block:
  - chunk header predictor = first PCM sample of the block
  - header step index = carried encoder state
  - encoder compresses the remaining samples
  - browser outputs the header predictor as the first decoded sample and expands the remaining nibbles after it
- Rebuilt the sketch object, relinked the ELF, regenerated the BIN, and reflashed the board on `COM3`.
- Re-uploaded the updated `web/index.html` to the Oracle VM.

### Final Camera Orientation Fix

- Sensor-side horizontal mirror control on the XIAO ESP32S3 Sense remained inconsistent.
- Finalized the practical fix by mirroring both user-facing camera views in the browser layer instead:
  - remote console page on the Oracle VM
  - local ESP32 root page served by `handleRoot()`
- Left the raw `/stream` MJPEG endpoint unmodified and treated it as a low-level feed.
- Reflashed the board on `COM3` and confirmed the user-facing views now show the expected left/right orientation.

### Session Handoff Prompt

- Added `NEXT_SESSION_PROMPT.md` to summarize the current proven architecture and the next intended work.
- The next planned step is not networking redesign.
- The next planned step is:
  - decide the user control UI
  - finalize motor/servo pin assignments
  - connect real motor output to the already working remote control path
