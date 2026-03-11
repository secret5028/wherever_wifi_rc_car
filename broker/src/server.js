const fs = require("fs");
const http = require("http");
const path = require("path");
const { WebSocketServer } = require("ws");
const config = require("./config");

const clients = new Map();   // clientId -> { ws, clientId, lastSeenAt }
const devices = new Map();   // deviceId -> { ws, deviceId, lastSeenAt, latestFrame, streamClients }

const VIDEO_FRAME_MSG = 1;

function sendJson(ws, payload) {
  if (ws.readyState === ws.OPEN) ws.send(JSON.stringify(payload));
}
function now() { return Date.now(); }
function clamp(v, lo, hi) { return Math.min(Math.max(v, lo), hi); }

function readFile(p) {
  try { return fs.readFileSync(p); } catch { return null; }
}

// ─── MJPEG 스트림 (브라우저 직접 접속용) ───
function getVideoDeviceId(url) {
  const m = new URL(url, "http://x").pathname.match(/^\/video\/([^/]+)$/);
  return m ? decodeURIComponent(m[1]) : null;
}
function writeMjpegFrame(res, buf) {
  res.write(`--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ${buf.length}\r\n\r\n`);
  res.write(buf);
  res.write("\r\n");
}
function attachMjpeg(entry, res) {
  res.writeHead(200, {
    "Content-Type": "multipart/x-mixed-replace; boundary=frame",
    "Cache-Control": "no-cache, no-store, must-revalidate",
    "Access-Control-Allow-Origin": "*",
    "Connection": "close",
    "Pragma": "no-cache"
  });
  if (entry.latestFrame) writeMjpegFrame(res, entry.latestFrame);
  entry.streamClients.add(res);
  const cleanup = () => entry.streamClients.delete(res);
  res.on("close", cleanup);
  res.on("finish", cleanup);
}

// ─── 정적 파일 ───
function serveStatic(req, res) {
  const urlPath = req.url === "/" ? "/index.html" : req.url;
  const safe    = path.normalize(urlPath).replace(/^(\.\.[\\/])+/, "");
  const content = readFile(path.join(config.publicWebDir, safe));
  if (!content) {
    res.writeHead(404, { "content-type": "text/plain; charset=utf-8" });
    return res.end("Not found");
  }
  const ext = path.extname(safe);
  const ct  = ext === ".html" ? "text/html; charset=utf-8"
             : ext === ".js"  ? "application/javascript; charset=utf-8"
             :                  "text/plain; charset=utf-8";
  res.writeHead(200, { "content-type": ct, "Cache-Control": "no-cache, no-store, must-revalidate" });
  res.end(content);
}

// ─── HTTP 서버 ───
const server = http.createServer((req, res) => {
  const deviceId = getVideoDeviceId(req.url);
  if (deviceId) {
    const entry = devices.get(deviceId);
    if (!entry) { res.writeHead(404, {"content-type":"text/plain"}); return res.end("device not found"); }
    return attachMjpeg(entry, res);
  }
  serveStatic(req, res);
});

// ─── WebSocket 서버 (device / client 분리) ───
const deviceWss = new WebSocketServer({ noServer: true });
const clientWss = new WebSocketServer({ noServer: true });

server.on("upgrade", (req, socket, head) => {
  const url = new URL(req.url, "http://x");
  if (url.pathname === config.devicePath) {
    deviceWss.handleUpgrade(req, socket, head, ws => deviceWss.emit("connection", ws, req));
  } else if (url.pathname === config.clientPath) {
    clientWss.handleUpgrade(req, socket, head, ws => clientWss.emit("connection", ws, req));
  } else {
    socket.destroy();
  }
});

// ─── 헬퍼 ───
function broadcastToClients(payload) {
  for (const e of clients.values()) sendJson(e.ws, payload);
}

function notifyClientState() {
  const payload = { type: "client_state", clients: clients.size, ts: now() };
  for (const e of devices.values()) sendJson(e.ws, payload);
}

function encodeVideoMsg(deviceId, frame) {
  const idBuf = Buffer.from(deviceId, "utf8");
  const out   = Buffer.allocUnsafe(1 + 2 + idBuf.length + frame.length);
  out.writeUInt8(VIDEO_FRAME_MSG, 0);
  out.writeUInt16BE(idBuf.length, 1);
  idBuf.copy(out, 3);
  frame.copy(out, 3 + idBuf.length);
  return out;
}
function sendVideoFrame(ws, deviceId, frame) {
  if (ws.readyState === ws.OPEN)
    ws.send(encodeVideoMsg(deviceId, frame), { binary: true });
}
function broadcastFrame(entry, frame) {
  entry.latestFrame = frame;
  // WS 클라이언트에 바이너리 전송
  for (const e of clients.values()) sendVideoFrame(e.ws, entry.deviceId, frame);
  // MJPEG 스트림 클라이언트에 전송
  for (const res of [...entry.streamClients]) {
    try { writeMjpegFrame(res, frame); }
    catch { entry.streamClients.delete(res); try { res.end(); } catch {} }
  }
}

function getDevice(id)    { return devices.get(id); }
function primaryDevice()  { return devices.values().next().value || null; }
function resolveDevice(m) { return (m.deviceId && getDevice(m.deviceId)) || primaryDevice(); }

function fwdCmd(ws, msg, payload) {
  const d = resolveDevice(msg);
  if (!d) { sendJson(ws, { type: "error", reason: "no_device" }); return; }
  sendJson(d.ws, payload);
  sendJson(ws, { type: "command_ack", commandType: payload.type, deviceId: d.deviceId, ts: now() });
}
function fwdAudio(ws, msg, payload) {
  const d = resolveDevice(msg);
  if (!d) { sendJson(ws, { type: "error", reason: "no_device" }); return; }
  sendJson(d.ws, payload);
}

function pruneStale() {
  const t = now();
  for (const [id, e] of devices.entries()) {
    if (t - e.lastSeenAt > config.staleDeviceMs) {
      e.ws.close(4000, "device timeout");
      devices.delete(id);
      broadcastToClients({ type: "device_offline", deviceId: id });
      console.log(`[device:${id}] stale, removed`);
    }
  }
  for (const [id, e] of clients.entries()) {
    if (t - e.lastSeenAt > config.staleClientMs) {
      e.ws.close(4000, "client timeout");
      clients.delete(id);
      notifyClientState();
      console.log(`[client:${id}] stale, removed`);
    }
  }
}

// ─── Device WebSocket ───
deviceWss.on("connection", (ws, req) => {
  const url      = new URL(req.url, "http://x");
  const deviceId = url.searchParams.get("deviceId") || `esp32-${Math.random().toString(16).slice(2,8)}`;

  // 핵심 버그 수정: 재연결 시 기존 streamClients 유지
  const prev = devices.get(deviceId);
  const entry = {
    ws,
    deviceId,
    lastSeenAt: now(),
    latestFrame: prev ? prev.latestFrame : null,
    streamClients: prev ? prev.streamClients : new Set()  // ← 기존 MJPEG 구독자 보존
  };
  devices.set(deviceId, entry);

  console.log(`[device:${deviceId}] connected (clients preserved: ${entry.streamClients.size})`);

  sendJson(ws, { type: "hello", role: "broker", heartbeatMs: config.heartbeatMs });
  sendJson(ws, { type: "client_state", clients: clients.size, ts: now() });
  broadcastToClients({ type: "device_online", deviceId });

  ws.on("message", (raw, isBinary) => {
    entry.lastSeenAt = now();
    if (isBinary) { broadcastFrame(entry, Buffer.from(raw)); return; }

    let msg;
    try { msg = JSON.parse(raw.toString()); } catch { return; }

    if (msg.type === "ping") {
      sendJson(ws, { type: "pong", ts: now() });
    } else if (msg.type === "status") {
      broadcastToClients({ ...msg, deviceId, remoteVideoUrl: `/video/${encodeURIComponent(deviceId)}` });
    } else if (msg.type === "audio") {
      for (const e of clients.values()) sendJson(e.ws, { ...msg, deviceId });
    } else if (msg.type === "log") {
      broadcastToClients({ type: "device_log", deviceId, message: msg.message || "" });
    }
  });

  ws.on("close", () => {
    // 같은 entry가 아닌 경우(재연결 경쟁) 무시
    if (devices.get(deviceId) !== entry) return;
    // streamClients는 건드리지 않음 (재연결 시 재사용)
    devices.delete(deviceId);
    broadcastToClients({ type: "device_offline", deviceId });
    console.log(`[device:${deviceId}] disconnected`);
  });

  ws.on("error", e => console.warn(`[device:${deviceId}] ws error: ${e.message}`));
});

// ─── Client WebSocket ───
clientWss.on("connection", ws => {
  const clientId = `client-${Math.random().toString(16).slice(2,8)}`;
  const entry    = { ws, clientId, lastSeenAt: now() };
  clients.set(clientId, entry);
  notifyClientState();

  sendJson(ws, { type: "welcome", clientId, devices: [...devices.keys()] });

  // 신규 접속 즉시 최신 프레임 전송
  for (const d of devices.values()) {
    if (d.latestFrame) sendVideoFrame(ws, d.deviceId, d.latestFrame);
  }

  ws.on("message", raw => {
    entry.lastSeenAt = now();
    let msg;
    try { msg = JSON.parse(raw.toString()); } catch { return; }

    if (msg.type === "ping") {
      sendJson(ws, { type: "pong", ts: now() });

    } else if (msg.type === "ctrl") {
      fwdCmd(ws, msg, {
        type: "ctrl",
        throttle: clamp(Number(msg.throttle)||0, -100, 100),
        steering: clamp(Number(msg.steering)||0, -100, 100),
        ts: now()
      });

    } else if (msg.type === "camera_quality") {
      fwdCmd(ws, msg, { type: "camera_quality", quality: String(msg.quality||"QVGA").toUpperCase(), ts: now() });

    } else if (msg.type === "led") {
      fwdCmd(ws, msg, { type: "led", enabled: Boolean(msg.enabled), ts: now() });

    } else if (msg.type === "mode") {
      fwdCmd(ws, msg, { type: "mode", mode: msg.mode==="monitor"?"monitor":"drive", ts: now() });

    } else if (msg.type === "talk") {
      fwdCmd(ws, msg, { type: "talk", enabled: Boolean(msg.enabled), ts: now() });

    } else if (msg.type === "talk_audio") {
      fwdAudio(ws, msg, {
        type: "talk_audio",
        codec:      String(msg.codec||"adpcm_ima"),
        sampleRate: clamp(Number(msg.sampleRate)||16000, 8000, 24000),
        samples:    clamp(Number(msg.samples)||0, 1, 1024),
        seq:        Number(msg.seq)||0,
        payload:    String(msg.payload||""),
        ts: now()
      });

    } else if (msg.type === "snapshot") {
      fwdCmd(ws, msg, { type: "snapshot", ts: now() });
    }
  });

  ws.on("close", () => {
    clients.delete(clientId);
    notifyClientState();
    console.log(`[client:${clientId}] disconnected`);
  });

  ws.on("error", e => console.warn(`[client:${clientId}] ws error: ${e.message}`));
});

// ─── 주기 작업: stale 정리 + 브로커 상태 브로드캐스트 ───
setInterval(() => {
  pruneStale();
  broadcastToClients({ type: "broker_status", ts: now(), devices: [...devices.keys()], clients: clients.size });
}, config.heartbeatMs);

// ─── 서버 시작 ───
server.listen(config.port, config.host, () => {
  console.log(`broker listening on http://${config.host}:${config.port}`);
  console.log(`device  ws: ${config.devicePath}`);
  console.log(`client  ws: ${config.clientPath}`);
});
