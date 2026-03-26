const fs = require("fs");
const http = require("http");
const https = require("https");
const path = require("path");
const { WebSocketServer } = require("ws");
const config = require("./config");

const clients = new Map();
const devices = new Map();

function sendJson(socket, payload) {
  if (socket.readyState === socket.OPEN) {
    socket.send(JSON.stringify(payload));
  }
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

function getContentType(filePath) {
  const ext = path.extname(filePath).toLowerCase();
  if (ext === ".html") return "text/html; charset=utf-8";
  if (ext === ".js") return "application/javascript; charset=utf-8";
  if (ext === ".json") return "application/json; charset=utf-8";
  if (ext === ".bin") return "application/octet-stream";
  if (ext === ".txt") return "text/plain; charset=utf-8";
  return "application/octet-stream";
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

function serveFromDir(req, res, rootDir, requestPath, defaultFile = null) {
  const normalizedPath = requestPath === "/" && defaultFile ? `/${defaultFile}` : requestPath;
  const safePath = path.normalize(normalizedPath).replace(/^(\.\.[/\\])+/, "");
  const filePath = path.join(rootDir, safePath);
  const content = readStaticFile(filePath);

  if (!content) {
    res.writeHead(404, { "content-type": "text/plain; charset=utf-8" });
    res.end("Not found");
    return;
  }

  res.writeHead(200, {
    "content-type": getContentType(filePath),
    "Cache-Control": "no-cache, no-store, must-revalidate",
    Pragma: "no-cache",
    Expires: "0"
  });
  res.end(content);
}

function serveStatic(req, res) {
  const requestUrl = new URL(req.url, "http://localhost");
  serveFromDir(req, res, config.publicWebDir, requestUrl.pathname, "index.html");
}

function serveOta(req, res) {
  const requestUrl = new URL(req.url, "http://localhost");
  serveFromDir(req, res, config.otaDir, requestUrl.pathname.replace(/^\/ota/, "") || "/", "manifest.json");
}

function createHttpHandler(req, res) {
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

  if (req.url.startsWith("/ota")) {
    serveOta(req, res);
    return;
  }

  serveStatic(req, res);
}

function createBrokerServer() {
  if (!config.tlsEnabled) {
    return {
      server: http.createServer(createHttpHandler),
      protocol: "http"
    };
  }

  const tlsOptions = {
    key: fs.readFileSync(config.tlsKeyPath),
    cert: fs.readFileSync(config.tlsCertPath)
  };

  return {
    server: https.createServer(tlsOptions, createHttpHandler),
    protocol: "https"
  };
}

const { server, protocol } = createBrokerServer();

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

function broadcastAudioToClients(payload) {
  for (const client of clients.values()) {
    if (client.ws.readyState === client.ws.OPEN) {
      client.ws.send(JSON.stringify(payload));
    }
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
  sendJson(ws, {
    type: "command_ack",
    commandType: payload.type,
    deviceId: deviceEntry.deviceId,
    sentAt: now()
  });
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
  const existing = devices.get(deviceId);
  const preservedStreamClients = existing && existing.streamClients ? existing.streamClients : new Set();
  const preservedLatestFrame = existing && existing.latestFrame ? existing.latestFrame : null;
  if (existing && existing.ws !== ws) {
    try {
      existing.ws.close(4001, "replaced");
    } catch {}
  }
  const entry = {
    ws,
    deviceId,
    lastSeenAt: now(),
    latestFrame: preservedLatestFrame,
    streamClients: preservedStreamClients
  };

  devices.set(deviceId, entry);
  sendJson(ws, { type: "hello", role: "broker", heartbeatMs: config.heartbeatMs });
  broadcastToClients({ type: "device_online", deviceId });

  ws.on("message", (raw, isBinary) => {
    markAlive(entry);

    if (isBinary) {
      entry.videoFrameCount = (entry.videoFrameCount || 0) + 1;
      if ((entry.videoFrameCount % 20) === 0) {
        console.log(`[video:${deviceId}] frames=${entry.videoFrameCount} bytes=${raw.length}`);
      }
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

    if (message.type === "ota_status") {
      broadcastToClients({
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
    if (devices.get(deviceId) === entry) {
      for (const res of entry.streamClients) {
        try {
          res.end();
        } catch {}
      }
      devices.delete(deviceId);
      broadcastToClients({ type: "device_offline", deviceId });
    }
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
      const clientSentAt = Number(message.sentAt) || now();
      if (now() - clientSentAt > config.ctrlMaxAgeMs) {
        sendJson(ws, { type: "command_drop", commandType: "ctrl", reason: "stale_ctrl", ageMs: now() - clientSentAt });
        return;
      }
      const payload = {
        type: "ctrl",
        throttle: clamp(Number(message.throttle) || 0, -100, 100),
        steering: clamp(Number(message.steering) || 0, -100, 100),
        sentAt: clientSentAt,
        relayedAt: now()
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

    if (message.type === "sfx") {
      forwardClientCommand(ws, message, {
        type: "sfx",
        sentAt: now()
      });
      return;
    }

    if (message.type === "stream") {
      forwardClientCommand(ws, message, {
        type: "stream",
        enabled: Boolean(message.enabled),
        sentAt: now()
      });
      return;
    }

    if (message.type === "steering_trim") {
      forwardClientCommand(ws, message, {
        type: "steering_trim",
        point: String(message.point || ""),
        delta: clamp(Number(message.delta) || 0, -20, 20),
        reset: Boolean(message.reset),
        sentAt: now()
      });
      return;
    }

    if (message.type === "ota_update") {
      forwardClientCommand(ws, message, {
        type: "ota_update",
        sentAt: now()
      });
      return;
    }

    if (message.type === "talk_audio") {
      const clientSentAt = Number(message.sentAt) || now();
      if (now() - clientSentAt > config.talkAudioMaxAgeMs) {
        sendJson(ws, { type: "command_drop", commandType: "talk_audio", reason: "stale_talk_audio", ageMs: now() - clientSentAt });
        return;
      }
      forwardClientAudio(ws, message, {
        type: "talk_audio",
        codec: String(message.codec || "adpcm_ima"),
        sampleRate: clamp(Number(message.sampleRate) || 16000, 8000, 24000),
        samples: clamp(Number(message.samples) || 0, 1, 1024),
        seq: Number(message.seq) || 0,
        payload: String(message.payload || ""),
        sentAt: clientSentAt,
        relayedAt: now()
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
  console.log(`broker listening on ${protocol}://${config.host}:${config.port}`);
  console.log(`device ws path: ${config.devicePath}`);
  console.log(`client ws path: ${config.clientPath}`);
});
