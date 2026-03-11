const fs = require("fs");
const http = require("http");
const path = require("path");
const { WebSocketServer } = require("ws");
const config = require("./config");

const clients = new Map();
const devices = new Map();
const VIDEO_FRAME_MESSAGE = 1;

function sendJson(socket, payload) {
  if (socket.readyState === socket.OPEN) {
    socket.send(JSON.stringify(payload));
  }
}

function describeWsError(error) {
  if (!error) {
    return "unknown websocket error";
  }
  return error.code ? `${error.code}: ${error.message}` : (error.message || String(error));
}

function now() {
  return Date.now();
}

function clamp(value, min, max) {
  return Math.min(Math.max(value, min), max);
}

function readStaticFile(filePath) {
  try {
    return fs.readFileSync(filePath);
  } catch {
    return null;
  }
}

function getVideoDeviceId(reqUrl) {
  const parsed = new URL(reqUrl, "http://localhost");
  const match = parsed.pathname.match(/^\/video\/([^/]+)$/);
  return match ? decodeURIComponent(match[1]) : null;
}

function writeMjpegFrame(res, frameBuffer) {
  res.write(`--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ${frameBuffer.length}\r\n\r\n`);
  res.write(frameBuffer);
  res.write("\r\n");
}

function attachVideoClient(deviceEntry, res) {
  if (!deviceEntry.streamClients) {
    deviceEntry.streamClients = new Set();
  }

  res.writeHead(200, {
    "Content-Type": "multipart/x-mixed-replace; boundary=frame",
    "Cache-Control": "no-cache, no-store, must-revalidate",
    "Access-Control-Allow-Origin": "*",
    Connection: "close",
    Pragma: "no-cache"
  });

  if (deviceEntry.latestFrame) {
    writeMjpegFrame(res, deviceEntry.latestFrame);
  }

  deviceEntry.streamClients.add(res);
  reqCleanup(res, () => {
    deviceEntry.streamClients.delete(res);
  });
}

function reqCleanup(res, cleanup) {
  const done = () => cleanup();
  res.on("close", done);
  res.on("finish", done);
}

function broadcastFrame(deviceEntry, frameBuffer) {
  deviceEntry.latestFrame = frameBuffer;
  broadcastVideoFrameToClients(deviceEntry.deviceId, frameBuffer);
  if (!deviceEntry.streamClients) {
    return;
  }

  for (const res of [...deviceEntry.streamClients]) {
    try {
      writeMjpegFrame(res, frameBuffer);
    } catch {
      deviceEntry.streamClients.delete(res);
      try {
        res.end();
      } catch {}
    }
  }
}

function serveStatic(req, res) {
  const urlPath = req.url === "/" ? "/index.html" : req.url;
  const safePath = path.normalize(urlPath).replace(/^(\.\.[/\\])+/, "");
  const filePath = path.join(config.publicWebDir, safePath);
  const content = readStaticFile(filePath);

  if (!content) {
    res.writeHead(404, { "content-type": "text/plain; charset=utf-8" });
    res.end("Not found");
    return;
  }

  const ext = path.extname(filePath);
  const contentType =
    ext === ".html"
      ? "text/html; charset=utf-8"
      : ext === ".js"
        ? "application/javascript; charset=utf-8"
        : "text/plain; charset=utf-8";

  res.writeHead(200, {
    "content-type": contentType,
    "Cache-Control": "no-cache, no-store, must-revalidate",
    Pragma: "no-cache",
    Expires: "0"
  });
  res.end(content);
}

const server = http.createServer((req, res) => {
  const videoDeviceId = getVideoDeviceId(req.url);
  if (videoDeviceId) {
    const deviceEntry = getDevice(videoDeviceId);
    if (!deviceEntry) {
      res.writeHead(404, { "content-type": "text/plain; charset=utf-8" });
      res.end("device not found");
      return;
    }

    attachVideoClient(deviceEntry, res);
    return;
  }

  serveStatic(req, res);
});

const deviceWss = new WebSocketServer({ noServer: true });
const clientWss = new WebSocketServer({ noServer: true });

server.on("upgrade", (req, socket, head) => {
  const requestUrl = new URL(req.url, "http://localhost");

  if (requestUrl.pathname === config.devicePath) {
    deviceWss.handleUpgrade(req, socket, head, (ws) => {
      deviceWss.emit("connection", ws, req);
    });
    return;
  }

  if (requestUrl.pathname === config.clientPath) {
    clientWss.handleUpgrade(req, socket, head, (ws) => {
      clientWss.emit("connection", ws, req);
    });
    return;
  }

  socket.destroy();
});

function broadcastToClients(payload) {
  for (const client of clients.values()) {
    sendJson(client.ws, payload);
  }
}

function encodeVideoFrameMessage(deviceId, frameBuffer) {
  const deviceIdBuffer = Buffer.from(deviceId, "utf8");
  const output = Buffer.allocUnsafe(1 + 2 + deviceIdBuffer.length + frameBuffer.length);
  output.writeUInt8(VIDEO_FRAME_MESSAGE, 0);
  output.writeUInt16BE(deviceIdBuffer.length, 1);
  deviceIdBuffer.copy(output, 3);
  frameBuffer.copy(output, 3 + deviceIdBuffer.length);
  return output;
}

function sendVideoFrame(socket, deviceId, frameBuffer) {
  if (socket.readyState !== socket.OPEN) {
    return;
  }
  socket.send(encodeVideoFrameMessage(deviceId, frameBuffer), { binary: true });
}

function broadcastAudioToClients(payload) {
  for (const client of clients.values()) {
    if (client.ws.readyState === client.ws.OPEN) {
      client.ws.send(JSON.stringify(payload));
    }
  }
}

function broadcastVideoFrameToClients(deviceId, frameBuffer) {
  for (const client of clients.values()) {
    sendVideoFrame(client.ws, deviceId, frameBuffer);
  }
}

function getDevice(deviceId) {
  return devices.get(deviceId);
}

function getPrimaryDevice() {
  return devices.values().next().value || null;
}

function resolveTargetDevice(message) {
  return (message.deviceId && getDevice(message.deviceId)) || getPrimaryDevice();
}

function forwardClientCommand(ws, message, payload) {
  const deviceEntry = resolveTargetDevice(message);
  if (!deviceEntry) {
    sendJson(ws, { type: "error", reason: "no_device_connected" });
    return;
  }

  sendJson(deviceEntry.ws, payload);
  sendJson(ws, {
    type: "command_ack",
    commandType: payload.type,
    deviceId: deviceEntry.deviceId,
    sentAt: now()
  });
}

function forwardClientAudio(ws, message, payload) {
  const deviceEntry = resolveTargetDevice(message);
  if (!deviceEntry) {
    sendJson(ws, { type: "error", reason: "no_device_connected" });
    return false;
  }

  sendJson(deviceEntry.ws, payload);
  return true;
}

function markAlive(entry) {
  entry.lastSeenAt = now();
}

function closeStaleConnections() {
  const current = now();

  for (const [id, entry] of devices.entries()) {
    if (current - entry.lastSeenAt > config.staleDeviceMs) {
      entry.ws.close(4000, "device timeout");
      devices.delete(id);
      broadcastToClients({ type: "device_offline", deviceId: id });
    }
  }

  for (const [id, entry] of clients.entries()) {
    if (current - entry.lastSeenAt > config.staleClientMs) {
      entry.ws.close(4000, "client timeout");
      clients.delete(id);
    }
  }
}

deviceWss.on("connection", (ws, req) => {
  const url = new URL(req.url, "http://localhost");
  const deviceId = url.searchParams.get("deviceId") || `esp32-${Math.random().toString(16).slice(2, 8)}`;
  const entry = { ws, deviceId, lastSeenAt: now(), latestFrame: null, streamClients: new Set() };

  devices.set(deviceId, entry);
  sendJson(ws, { type: "hello", role: "broker", heartbeatMs: config.heartbeatMs });
  broadcastToClients({ type: "device_online", deviceId });

  ws.on("message", (raw, isBinary) => {
    markAlive(entry);

    if (isBinary) {
      broadcastFrame(entry, Buffer.from(raw));
      return;
    }

    let message;
    try {
      message = JSON.parse(raw.toString());
    } catch {
      sendJson(ws, { type: "error", reason: "invalid_json" });
      return;
    }

    if (message.type === "ping") {
      sendJson(ws, { type: "pong", ts: now() });
      return;
    }

    if (message.type === "status") {
      broadcastToClients({
        ...message,
        deviceId,
        remoteVideoUrl: `/video/${encodeURIComponent(deviceId)}`
      });
      return;
    }

    if (message.type === "audio") {
      broadcastAudioToClients({
        ...message,
        deviceId
      });
      return;
    }

    if (message.type === "log") {
      broadcastToClients({ type: "device_log", deviceId, message: message.message || "" });
    }
  });

  ws.on("close", () => {
    for (const res of entry.streamClients) {
      try {
        res.end();
      } catch {}
    }
    devices.delete(deviceId);
    broadcastToClients({ type: "device_offline", deviceId });
  });

  ws.on("error", (error) => {
    console.warn(`[device:${deviceId}] websocket error ${describeWsError(error)}`);
  });
});

clientWss.on("connection", (ws) => {
  const clientId = `client-${Math.random().toString(16).slice(2, 8)}`;
  const entry = { ws, clientId, lastSeenAt: now() };
  clients.set(clientId, entry);

  sendJson(ws, {
    type: "welcome",
    clientId,
    devices: [...devices.keys()]
  });

  for (const deviceEntry of devices.values()) {
    if (deviceEntry.latestFrame) {
      sendVideoFrame(ws, deviceEntry.deviceId, deviceEntry.latestFrame);
    }
  }

  ws.on("message", (raw) => {
    markAlive(entry);

    let message;
    try {
      message = JSON.parse(raw.toString());
    } catch {
      sendJson(ws, { type: "error", reason: "invalid_json" });
      return;
    }

    if (message.type === "ping") {
      sendJson(ws, { type: "pong", ts: now() });
      return;
    }

    if (message.type === "ctrl") {
      const payload = {
        type: "ctrl",
        throttle: clamp(Number(message.throttle) || 0, -100, 100),
        steering: clamp(Number(message.steering) || 0, -100, 100),
        sentAt: now()
      };
      forwardClientCommand(ws, message, payload);
      return;
    }

    if (message.type === "camera_quality") {
      forwardClientCommand(ws, message, {
        type: "camera_quality",
        quality: String(message.quality || "QVGA").toUpperCase(),
        sentAt: now()
      });
      return;
    }

    if (message.type === "led") {
      forwardClientCommand(ws, message, {
        type: "led",
        enabled: Boolean(message.enabled),
        sentAt: now()
      });
      return;
    }

    if (message.type === "mode") {
      forwardClientCommand(ws, message, {
        type: "mode",
        mode: message.mode === "monitor" ? "monitor" : "drive",
        sentAt: now()
      });
      return;
    }

    if (message.type === "talk") {
      forwardClientCommand(ws, message, {
        type: "talk",
        enabled: Boolean(message.enabled),
        sentAt: now()
      });
      return;
    }

    if (message.type === "talk_audio") {
      forwardClientAudio(ws, message, {
        type: "talk_audio",
        codec: String(message.codec || "adpcm_ima"),
        sampleRate: clamp(Number(message.sampleRate) || 16000, 8000, 24000),
        samples: clamp(Number(message.samples) || 0, 1, 1024),
        seq: Number(message.seq) || 0,
        payload: String(message.payload || ""),
        sentAt: now()
      });
      return;
    }

    if (message.type === "snapshot") {
      forwardClientCommand(ws, message, {
        type: "snapshot",
        sentAt: now()
      });
    }
  });

  ws.on("close", () => {
    clients.delete(clientId);
  });

  ws.on("error", (error) => {
    console.warn(`[client:${clientId}] websocket error ${describeWsError(error)}`);
  });
});

setInterval(() => {
  closeStaleConnections();
  broadcastToClients({
    type: "broker_status",
    ts: now(),
    devices: [...devices.keys()],
    clients: clients.size
  });
}, config.heartbeatMs);

server.listen(config.port, config.host, () => {
  console.log(`broker listening on http://${config.host}:${config.port}`);
  console.log(`device ws path: ${config.devicePath}`);
  console.log(`client ws path: ${config.clientPath}`);
});
