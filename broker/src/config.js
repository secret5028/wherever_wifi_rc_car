const path = require("path");

function intFromEnv(name, fallback) {
  const value = process.env[name];
  if (!value) {
    return fallback;
  }

  const parsed = Number.parseInt(value, 10);
  return Number.isFinite(parsed) ? parsed : fallback;
}

module.exports = {
  host: process.env.HOST || "0.0.0.0",
  port: intFromEnv("PORT", 8080),
  clientPath: process.env.CLIENT_PATH || "/client",
  devicePath: process.env.DEVICE_PATH || "/device",
  publicWebDir: path.join(__dirname, "..", "..", "web"),
  heartbeatMs: intFromEnv("HEARTBEAT_MS", 10000),
  staleClientMs: intFromEnv("STALE_CLIENT_MS", 30000),
  staleDeviceMs: intFromEnv("STALE_DEVICE_MS", 30000)
};
