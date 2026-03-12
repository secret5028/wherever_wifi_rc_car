# Phase 1

Phase 1 is no longer just "basic control path verified". The current target behavior is:

- AP mode for initial provisioning
- stored Wi-Fi + broker config
- persistent broker connection while power stays on
- browser-triggered media start
- control path independent from media path

## Current Message Families

Client to broker:

```json
{ "type": "ctrl", "throttle": 40, "steering": -15 }
{ "type": "stream", "enabled": true }
{ "type": "talk", "enabled": true }
```

ESP32 to broker:

```json
{ "type": "status", "rssi": -55, "uptime": 123456, "streamEnabled": true }
```

Heartbeat:

```json
{ "type": "ping" }
{ "type": "pong" }
```

## Oracle VM Runtime

```bash
cd ~/rc-car/broker
npm install
node src/server.js
```

Public page:

```text
http://YOUR_VM_IP:8080/
```

Required ingress:

- TCP `22`
- TCP `8080`

## Device Expectations

Expected serial sequence in broker mode:

```text
[WIFI] connecting to ...
[WS] connecting
[WS] connected
[WS] broker hello
```

Expected behavior:

- control works without media start
- audio/video are broker-relayed
- browser `START` triggers remote stream
- browser-triggered stream and broker-mode control are currently stable on plain `HTTP/WS`
- PTT logic exists in code, but public browser use still needs HTTPS before it is treated as complete
