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
  tlsEnabled: process.env.TLS_ENABLED === "1",
  tlsKeyPath: process.env.TLS_KEY_PATH || path.join(__dirname, "..", "key.pem"),
  tlsCertPath: process.env.TLS_CERT_PATH || path.join(__dirname, "..", "cert.pem"),
  clientPath: process.env.CLIENT_PATH || "/client",
  devicePath: process.env.DEVICE_PATH || "/device",
  publicWebDir: path.join(__dirname, "..", "..", "web"),
  otaDir: path.join(__dirname, "..", "ota"),
  heartbeatMs: intFromEnv("HEARTBEAT_MS", 10000),
  ctrlMaxAgeMs: intFromEnv("CTRL_MAX_AGE_MS", 5000),
  talkAudioMaxAgeMs: intFromEnv("TALK_AUDIO_MAX_AGE_MS", 800),
  staleClientMs: intFromEnv("STALE_CLIENT_MS", 30000),
  staleDeviceMs: intFromEnv("STALE_DEVICE_MS", 30000)
};
