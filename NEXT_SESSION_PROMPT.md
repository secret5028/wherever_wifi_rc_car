# RC Car Next Session Prompt

## Current Goal Context

This project is an ESP32-S3 Sense based remote RC car system.

The current architecture is:

- `ESP32-S3 Sense -> outbound WebSocket -> Oracle VM broker`
- `browser client -> Oracle VM broker -> ESP32`
- `ESP32 camera -> Oracle VM relay -> browser`
- `ESP32 built-in microphone -> Oracle VM relay -> browser`

The Oracle VM is the public rendezvous/broker point. The ESP32 never requires inbound port forwarding.

## What Is Already Working

### Control Path

- Remote control path works end to end:
  - `browser -> Oracle VM -> ESP32`
- ESP32 connects to Wi-Fi and keeps an outbound broker connection alive.
- Browser can send throttle/steering control messages.
- ESP32 receives control packets and reports status back.

### Video Path

- Local camera stream works on the ESP32.
- Remote video relay through the Oracle VM works in the browser.
- Current remote video settings are tuned for usable latency/stability:
  - `FRAMESIZE_QVGA`
  - `jpeg_quality = 14`
  - `VIDEO_UPLOAD_INTERVAL_MS = 220`

### Audio Path

- Built-in XIAO ESP32S3 Sense microphone is used.
- Remote audio uplink works:
  - ESP32 encodes
  - browser decodes
  - broker relays
- Audio currently uses `16kHz IMA ADPCM`.
- Latest tuning that the user considered acceptable:
  - reduced video burden
  - audio gate relaxed
  - audio gain increased

### Orientation

- Sensor-side mirror control was inconsistent on this board/camera combo.
- Final practical fix:
  - user-facing local page mirrors in browser
  - remote console mirrors in browser
- Raw `/stream` remains an unmodified low-level MJPEG endpoint.

## Important Current Notes

- `arduino-cli.compile.yaml` is a local-only file and should not be committed.
- Oracle VM broker is already running and the repo has been pushed to GitHub.
- Confirmed progress is logged in `WORKLOG.md`.
- Current main branch already contains the latest accepted state.
- The current PC does not have the working Oracle VM SSH private key, so broker/web redeploy to the VM was not completed from this machine.

## March 11 Tasks

The next session should execute these in order:

1. Deploy the latest `main` branch to the Oracle VM broker host
   - use the company PC or Oracle console recovery path if needed
   - `cd ~/rc-car && git pull origin main`
   - restart the Node broker process and confirm startup logs

2. Verify the mobile browser UI on a real phone
   - portrait layout should fill the screen with no top/bottom empty margins
   - confirm the controller page reflects the latest `web/index.html`

3. Verify snapshot UX
   - while streaming in `QVGA`, pressing `SNAP` should temporarily switch to `UXGA`
   - snapshot should be captured after the temporary quality bump
   - stream should return to the previous quality after the snapshot

4. Verify signal display
   - HUD label should be `SIG`
   - signal should display as percent instead of raw RSSI dBm

5. Review and confirm the hardware pin map document
   - use `PINMAP.md` as the current reference
   - confirm the recommended TB6612FNG, servo, LED, and MAX98357A assignments
   - keep the motor plan aligned to the 3-pin drive layout: `1 PWM + 2 direction pins`
   - treat `GPIO9` as optional spare / standby only if needed

6. If remote web still shows the old layout, treat that as a deployment issue first
   - do not debug frontend behavior before confirming the VM is serving the new files

## Next Session Main Tasks

The next conversation should focus on:

1. Decide the user control UI
   - phone-first or desktop-first
   - joystick layout
   - throttle/steering UX
   - audio/video placement
   - status indicators

2. Finalize motor/servo pin mapping and hardware control
   - TB6612FNG motor driver pins
   - PWM channel assignment
   - steering servo pin
   - standby pin
   - failsafe stop behavior

3. Implement real motor output
   - connect throttle to DC motor PWM/direction
   - connect steering to servo
   - preserve broker control path already proven in Phase 1

4. Keep the current networking/video/audio architecture unless there is a strong reason to change it

## Current Working Assumptions

- Board: `Seeed Studio XIAO ESP32S3 Sense`
- Public broker: Oracle VM
- User client: browser
- Remote transport:
  - control via WebSocket messages
  - video via remote MJPEG relay
  - audio via browser-decoded ADPCM stream

## Recommended Starting Point For The Next Conversation

Start from:

"Use the current working Oracle VM + browser + ESP32 architecture.
Do not redesign networking.
Next, define the user control UI and then wire real motor/servo control with final pin assignments."
