# RC Car Next Session Prompt

## Current Architecture

- board: `Seeed Studio XIAO ESP32S3 Sense`
- public broker: Oracle VM at `158.179.163.102:8080`
- device path: `/device`
- client path: `/client`
- control: WebSocket relay
- media uplink: ESP32 -> broker
- remote video display: broker MJPEG feed rendered into browser canvas

## Current Intended Runtime

1. first boot enters AP mode
2. AP page stores Wi-Fi + broker settings
3. after reboot the board reconnects to Wi-Fi and broker automatically
4. board remains connected to broker while powered
5. browser `START` requests remote stream
6. `TALK` pauses video and resumes it on release, but public browser use still needs HTTPS

## What Is Confirmed

- Oracle VM broker is reachable from the public internet
- broker process is running on the VM
- ESP32 control path works through the broker
- ESP32 audio uplink works through the broker
- ESP32 video frames are reaching the broker
- broker-mode page, video, and control are currently working again on `http://158.179.163.102:8080`
- pin map for servo / motor / amp is fixed in `PINMAP.md`

## Current Suspect Area

The main remaining unfinished area is public PTT:

- browser microphone permission is blocked on plain `HTTP`
- temporary self-signed HTTPS attempt was rolled back
- next PTT work should use a cleaner HTTPS path instead of patching around it

Video and control should not be redesigned first; they are in a usable state again.

## Important Local Facts

- Oracle SSH key exists on this PC in `C:\Users\song-bj\Downloads\ssh-key-2026-03-08.key`
- latest broker and web files have already been redeployed to the Oracle VM
- latest firmware has already been reflashed to `COM10`

## Next Tasks

1. Keep AP provisioning policy clear
   - AP for first setup
   - broker connection persistent afterward
   - media on demand only

2. Keep broker mode stable on plain `HTTP/WS`
   - do not re-enable TLS on the broker until a cleaner deployment path is chosen
   - do not change ESP32 back to `beginSSL()` yet

3. Revisit PTT only after transport is settled
   - likely solution is reverse proxy or proper HTTPS endpoint
   - browser microphone policy is the blocker, not basic relay logic

4. Do not redesign networking yet
   - keep broker relay model
   - keep control/media separation
