# Phase 1

## Goal

Verify the remote control path:

`phone/PC -> Oracle VM broker -> ESP32`

## Components

- Oracle VM runs the Node.js broker
- ESP32 opens an outbound WebSocket connection to the broker
- Browser test page sends control messages to the broker

## Firmware Scope

- Wi-Fi connect
- Outbound WebSocket connect
- Heartbeat
- Exponential reconnect
- Safe stop on timeout/disconnect
- Receive `ctrl` messages
- Send `status` messages

## Message Shapes

Client to broker:

```json
{ "type": "ctrl", "throttle": 40, "steering": -15 }
```

ESP32 to broker:

```json
{ "type": "status", "rssi": -55, "uptime": 123456, "throttle": 40, "steering": -15 }
```

Heartbeat:

```json
{ "type": "ping" }
{ "type": "pong" }
```

## Oracle VM Run Steps

```bash
sudo apt update
sudo apt install -y nodejs npm
cd ~/rc-car/broker
npm install
node src/server.js
```

Open the browser test page at:

```text
http://YOUR_VM_IP:8080/
```

## Oracle Ingress Rules Needed

- TCP `22` for SSH
- TCP `8080` for Phase 1 broker and test page

Later phases should move the public endpoint to `443` with TLS.

## ESP32 Arduino Libraries

- `WebSockets` by Markus Sattler
- `ArduinoJson`

## ESP32 Setup

1. Copy `firmware/phase1_esp32/secrets.example.h` to `secrets.h`
2. Fill in Wi-Fi credentials and Oracle VM IP
3. Flash the sketch
4. Open serial monitor at `115200`
5. Confirm:

```text
[WIFI] connecting
[WS] connected
```

## Success Criteria

- Browser connects to `/client`
- ESP32 connects to `/device`
- Sending a control message from the browser produces a control log on the ESP32
- Disconnecting the broker or Wi-Fi causes safe stop and reconnect attempts
