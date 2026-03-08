#include <Arduino.h>
#include <ESP_I2S.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <libb64/cencode.h>
#include "esp_camera.h"

#include "secrets.h"

namespace {
WebSocketsClient ws;
WebServer cameraServer(80);
I2SClass microphone;

constexpr unsigned long WIFI_RETRY_MS = 5000;
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
constexpr unsigned long STATUS_INTERVAL_MS = 2000;
constexpr unsigned long PING_INTERVAL_MS = 10000;
constexpr unsigned long COMMAND_TIMEOUT_MS = 1000;
constexpr unsigned long WS_CONNECT_TIMEOUT_MS = 10000;
constexpr unsigned long RESTART_DELAY_MS = 3000;
constexpr unsigned long VIDEO_UPLOAD_INTERVAL_MS = 200;
constexpr unsigned long AUDIO_UPLOAD_INTERVAL_MS = 40;
constexpr uint8_t MAX_PING_FAILS = 3;
constexpr uint32_t AUDIO_CAPTURE_SAMPLE_RATE = 16000;
constexpr uint32_t AUDIO_STREAM_SAMPLE_RATE = 16000;
constexpr size_t AUDIO_CAPTURE_SAMPLES = 640;
constexpr size_t AUDIO_STREAM_SAMPLES = 640;
constexpr size_t AUDIO_CAPTURE_BYTES = AUDIO_CAPTURE_SAMPLES * sizeof(int16_t);
constexpr size_t AUDIO_BASE64_BUFFER_LEN = 857;

unsigned long lastWifiAttemptAt = 0;
unsigned long wifiConnectStartedAt = 0;
unsigned long lastStatusAt = 0;
unsigned long lastPingAt = 0;
unsigned long lastCommandAt = 0;
unsigned long reconnectDelayMs = 1000;
unsigned long nextReconnectAt = 0;
unsigned long wsConnectStartedAt = 0;
unsigned long restartScheduledAt = 0;
unsigned long lastVideoUploadAt = 0;
unsigned long lastAudioUploadAt = 0;

bool wsConnected = false;
bool wifiConnectInFlight = false;
bool wsConnectInFlight = false;
bool wsConfigured = false;
bool cameraReady = false;
bool cameraServerStarted = false;
bool microphoneReady = false;
uint8_t pingFailCount = 0;
int lastThrottle = 0;
int lastSteering = 0;
uint32_t audioSequence = 0;
int32_t audioHighpassState = 0;
int32_t audioHighpassLastInput = 0;

constexpr int CAM_PIN_PWDN = -1;
constexpr int CAM_PIN_RESET = -1;
constexpr int CAM_PIN_XCLK = 10;
constexpr int CAM_PIN_SIOD = 40;
constexpr int CAM_PIN_SIOC = 39;
constexpr int CAM_PIN_D7 = 48;
constexpr int CAM_PIN_D6 = 11;
constexpr int CAM_PIN_D5 = 12;
constexpr int CAM_PIN_D4 = 14;
constexpr int CAM_PIN_D3 = 16;
constexpr int CAM_PIN_D2 = 18;
constexpr int CAM_PIN_D1 = 17;
constexpr int CAM_PIN_D0 = 15;
constexpr int CAM_PIN_VSYNC = 38;
constexpr int CAM_PIN_HREF = 47;
constexpr int CAM_PIN_PCLK = 13;
constexpr int MIC_PIN_CLK = 42;
constexpr int MIC_PIN_DATA = 41;

int16_t audioCaptureBuffer[AUDIO_CAPTURE_SAMPLES] = {};
uint8_t audioMuLawBuffer[AUDIO_STREAM_SAMPLES] = {};
char audioBase64Buffer[AUDIO_BASE64_BUFFER_LEN] = {};
}

void handleRoot();
void handleJpeg();
void handleStream();

void safeStop() {
  lastThrottle = 0;
  lastSteering = 0;
  Serial.println("[SAFE] stop");
}

void publishStatus() {
  if (!wsConnected) {
    return;
  }

  StaticJsonDocument<160> doc;
  doc["type"] = "status";
  doc["rssi"] = WiFi.RSSI();
  doc["uptime"] = millis();
  doc["throttle"] = lastThrottle;
  doc["steering"] = lastSteering;
  doc["cameraReady"] = cameraReady;
  doc["audioReady"] = microphoneReady;
  doc["audioCodec"] = "mulaw";
  doc["audioSampleRate"] = AUDIO_STREAM_SAMPLE_RATE;
  doc["streamPort"] = 80;
  doc["streamPath"] = "/stream";
  doc["localIp"] = WiFi.localIP().toString();

  String payload;
  serializeJson(doc, payload);
  ws.sendTXT(payload);
}

void applyControl(int throttle, int steering) {
  lastThrottle = constrain(throttle, -100, 100);
  lastSteering = constrain(steering, -45, 45);
  lastCommandAt = millis();

  Serial.printf("[CTRL] throttle=%d steering=%d\n", lastThrottle, lastSteering);
}

void scheduleReconnect() {
  wsConnected = false;
  wsConnectInFlight = false;
  nextReconnectAt = millis() + reconnectDelayMs;
  reconnectDelayMs = min(reconnectDelayMs * 2UL, 30000UL);
}

void scheduleRestart(const char* reason) {
  if (restartScheduledAt != 0) {
    return;
  }

  safeStop();
  restartScheduledAt = millis() + RESTART_DELAY_MS;
  Serial.printf("[SYS] restart scheduled: %s\n", reason);
}

void configureWebSocket() {
  if (wsConfigured) {
    return;
  }

  ws.onEvent([](WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
      case WStype_CONNECTED:
        wsConnected = true;
        wsConnectInFlight = false;
        pingFailCount = 0;
        reconnectDelayMs = 1000;
        Serial.println("[WS] connected");
        break;
      case WStype_DISCONNECTED:
        Serial.println("[WS] disconnected");
        scheduleRestart("ws disconnected");
        break;
      case WStype_TEXT: {
        StaticJsonDocument<256> doc;
        DeserializationError error = deserializeJson(doc, payload, length);
        if (error) {
          Serial.println("[WS] invalid json");
          return;
        }

        const char* messageType = doc["type"] | "";
        if (strcmp(messageType, "ctrl") == 0) {
          applyControl(doc["throttle"] | 0, doc["steering"] | 0);
        } else if (strcmp(messageType, "pong") == 0) {
          pingFailCount = 0;
        } else if (strcmp(messageType, "hello") == 0) {
          Serial.println("[WS] broker hello");
        }
        break;
      }
      case WStype_ERROR:
        Serial.println("[WS] error");
        break;
      default:
        break;
    }
  });
  ws.setReconnectInterval(0);
  wsConfigured = true;
}

bool initCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = CAM_PIN_D0;
  config.pin_d1 = CAM_PIN_D1;
  config.pin_d2 = CAM_PIN_D2;
  config.pin_d3 = CAM_PIN_D3;
  config.pin_d4 = CAM_PIN_D4;
  config.pin_d5 = CAM_PIN_D5;
  config.pin_d6 = CAM_PIN_D6;
  config.pin_d7 = CAM_PIN_D7;
  config.pin_xclk = CAM_PIN_XCLK;
  config.pin_pclk = CAM_PIN_PCLK;
  config.pin_vsync = CAM_PIN_VSYNC;
  config.pin_href = CAM_PIN_HREF;
  config.pin_sccb_sda = CAM_PIN_SIOD;
  config.pin_sccb_scl = CAM_PIN_SIOC;
  config.pin_pwdn = CAM_PIN_PWDN;
  config.pin_reset = CAM_PIN_RESET;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 20;
  config.fb_count = 2;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = CAMERA_FB_IN_PSRAM;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[CAM] init failed: 0x%x\n", err);
    return false;
  }

  sensor_t* sensor = esp_camera_sensor_get();
  if (sensor != nullptr) {
    sensor->set_framesize(sensor, FRAMESIZE_QVGA);
    sensor->set_quality(sensor, 20);
    sensor->set_brightness(sensor, 0);
    sensor->set_saturation(sensor, 0);
    sensor->set_hmirror(sensor, 0);
  }

  Serial.println("[CAM] ready");
  return true;
}

bool initMicrophone() {
  microphone.setPinsPdmRx(MIC_PIN_CLK, MIC_PIN_DATA);
  bool ok = microphone.begin(I2S_MODE_PDM_RX, AUDIO_CAPTURE_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
  if (!ok) {
    Serial.printf("[MIC] init failed: %d\n", microphone.lastError());
    return false;
  }

  Serial.println("[MIC] ready");
  return true;
}

void startCameraServer() {
  if (cameraServerStarted) {
    return;
  }
  cameraServer.on("/", HTTP_GET, handleRoot);
  cameraServer.on("/jpg", HTTP_GET, handleJpeg);
  cameraServer.on("/stream", HTTP_GET, handleStream);
  cameraServer.begin();
  cameraServerStarted = true;
  Serial.println("[CAM] http server started on :80");
}

void handleRoot() {
  String html;
  html += "<!doctype html><html><head><meta charset='utf-8'><title>RC Car Camera</title>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'></head><body>";
  html += "<h1>RC Car Camera</h1><img src='/stream' style='width:100%;max-width:960px;height:auto;' />";
  html += "</body></html>";
  cameraServer.send(200, "text/html", html);
}

void handleJpeg() {
  camera_fb_t* fb = esp_camera_fb_get();
  if (fb == nullptr) {
    cameraServer.send(503, "text/plain", "camera frame unavailable");
    return;
  }

  cameraServer.sendHeader("Content-Type", "image/jpeg");
  cameraServer.sendHeader("Content-Length", String(fb->len));
  cameraServer.send(200);
  WiFiClient client = cameraServer.client();
  client.write(fb->buf, fb->len);
  esp_camera_fb_return(fb);
}

void handleStream() {
  WiFiClient client = cameraServer.client();
  client.setTimeout(5);

  cameraServer.sendContent("HTTP/1.1 200 OK\r\n");
  cameraServer.sendContent("Content-Type: multipart/x-mixed-replace; boundary=frame\r\n");
  cameraServer.sendContent("Cache-Control: no-cache\r\n");
  cameraServer.sendContent("Connection: close\r\n\r\n");

  while (client.connected()) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb == nullptr) {
      Serial.println("[CAM] frame failed");
      break;
    }

    client.printf("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", fb->len);
    client.write(fb->buf, fb->len);
    client.print("\r\n");
    esp_camera_fb_return(fb);

    if (!client.connected()) {
      break;
    }

    delay(30);
  }
}

void uploadVideoFrameIfNeeded() {
  if (!cameraReady || !wsConnected) {
    return;
  }

  unsigned long current = millis();
  if (current - lastVideoUploadAt < VIDEO_UPLOAD_INTERVAL_MS) {
    return;
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (fb == nullptr) {
    Serial.println("[CAM] upload frame unavailable");
    return;
  }

  bool sent = ws.sendBIN(fb->buf, fb->len);
  esp_camera_fb_return(fb);

  if (sent) {
    lastVideoUploadAt = current;
  } else {
    Serial.println("[CAM] upload frame failed");
  }
}

uint8_t encodeMuLawSample(int16_t sample) {
  constexpr int16_t MULAW_MAX = 0x1FFF;
  constexpr int16_t MULAW_BIAS = 33;
  uint8_t sign = 0;

  sample >>= 2;
  if (sample < 0) {
    sample = -sample;
    sign = 0x80;
  }

  if (sample > MULAW_MAX) {
    sample = MULAW_MAX;
  }
  sample += MULAW_BIAS;

  uint8_t exponent = 7;
  for (int16_t mask = 0x400; exponent > 0 && (sample & mask) == 0; mask >>= 1) {
    exponent--;
  }

  uint8_t mantissa = (sample >> (exponent + 1)) & 0x0F;
  return static_cast<uint8_t>(~(sign | (exponent << 4) | mantissa));
}

int16_t filterAudioSample(int16_t sample) {
  int32_t input = sample;
  audioHighpassState = (995 * (audioHighpassState + input - audioHighpassLastInput)) / 1000;
  audioHighpassLastInput = input;

  int32_t filtered = audioHighpassState;
  if (abs(filtered) < 220) {
    filtered = 0;
  }

  filtered /= 2;
  filtered = constrain(filtered, -32768, 32767);
  return static_cast<int16_t>(filtered);
}

void uploadAudioChunkIfNeeded() {
  if (!microphoneReady || !wsConnected) {
    return;
  }

  unsigned long current = millis();
  if (current - lastAudioUploadAt < AUDIO_UPLOAD_INTERVAL_MS) {
    return;
  }

  if (microphone.available() < static_cast<int>(AUDIO_CAPTURE_BYTES)) {
    return;
  }

  size_t bytesRead = microphone.readBytes(reinterpret_cast<char*>(audioCaptureBuffer), AUDIO_CAPTURE_BYTES);
  if (bytesRead != AUDIO_CAPTURE_BYTES) {
    Serial.printf("[MIC] short read: %u\n", static_cast<unsigned>(bytesRead));
    return;
  }

  for (size_t i = 0; i < AUDIO_STREAM_SAMPLES; i++) {
    int16_t filtered = filterAudioSample(audioCaptureBuffer[i]);
    audioMuLawBuffer[i] = encodeMuLawSample(filtered);
  }

  int encodedLen = base64_encode_chars(reinterpret_cast<const char*>(audioMuLawBuffer), AUDIO_STREAM_SAMPLES, audioBase64Buffer);
  if (encodedLen <= 0 || encodedLen >= static_cast<int>(sizeof(audioBase64Buffer))) {
    Serial.println("[MIC] base64 encode failed");
    return;
  }
  audioBase64Buffer[encodedLen] = '\0';

  StaticJsonDocument<512> doc;
  doc["type"] = "audio";
  doc["codec"] = "mulaw";
  doc["sampleRate"] = AUDIO_STREAM_SAMPLE_RATE;
  doc["samples"] = AUDIO_STREAM_SAMPLES;
  doc["seq"] = audioSequence++;
  doc["payload"] = audioBase64Buffer;

  String payload;
  serializeJson(doc, payload);
  if (!ws.sendTXT(payload)) {
    Serial.println("[MIC] upload failed");
    return;
  }

  lastAudioUploadAt = current;
}

void connectWebSocket() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  configureWebSocket();
  ws.begin(BROKER_HOST, BROKER_PORT, String("/device?deviceId=") + DEVICE_ID);
  wsConnectStartedAt = millis();
  wsConnectInFlight = true;
}

void ensureWifiConnected() {
  wl_status_t wifiStatus = WiFi.status();
  if (wifiStatus == WL_CONNECTED) {
    wifiConnectInFlight = false;
    return;
  }

  unsigned long current = millis();

  if (wifiConnectInFlight) {
    if (current - wifiConnectStartedAt >= WIFI_CONNECT_TIMEOUT_MS) {
      Serial.println("[WIFI] connect timeout, retrying");
      WiFi.disconnect(true, true);
      wifiConnectInFlight = false;
      lastWifiAttemptAt = current;
    }
    return;
  }

  if (current - lastWifiAttemptAt < WIFI_RETRY_MS) {
    return;
  }

  lastWifiAttemptAt = current;
  wifiConnectStartedAt = current;
  wifiConnectInFlight = true;
  Serial.printf("[WIFI] connecting to %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void ensureWebSocketConnected() {
  if (!WiFi.isConnected()) {
    wsConnected = false;
    wsConnectInFlight = false;
    return;
  }

  if (wsConnected) {
    return;
  }

  if (wsConnectInFlight) {
    if (millis() - wsConnectStartedAt >= WS_CONNECT_TIMEOUT_MS) {
      Serial.println("[WS] connect timeout");
      scheduleRestart("ws timeout");
    }
    return;
  }

  if (millis() < nextReconnectAt) {
    return;
  }

  Serial.println("[WS] connecting");
  connectWebSocket();
}

void sendPingIfNeeded() {
  if (!wsConnected) {
    return;
  }

  unsigned long current = millis();
  if (current - lastPingAt < PING_INTERVAL_MS) {
    return;
  }

  StaticJsonDocument<64> doc;
  doc["type"] = "ping";

  String payload;
  serializeJson(doc, payload);
  bool ok = ws.sendTXT(payload);
  lastPingAt = current;

  if (!ok) {
    pingFailCount++;
    Serial.printf("[WS] ping send failed (%u)\n", pingFailCount);
    if (pingFailCount >= MAX_PING_FAILS) {
      safeStop();
      ws.disconnect();
      scheduleReconnect();
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[BOOT] Phase 1 firmware");
  safeStop();
  cameraReady = initCamera();
  microphoneReady = initMicrophone();
  configureWebSocket();
  ensureWifiConnected();
}

void loop() {
  if (restartScheduledAt != 0 && millis() >= restartScheduledAt) {
    Serial.println("[SYS] restarting now");
    delay(100);
    ESP.restart();
  }

  ensureWifiConnected();
  if (cameraReady && WiFi.isConnected() && !cameraServerStarted) {
    startCameraServer();
  }
  ensureWebSocketConnected();
  ws.loop();
  if (cameraReady && cameraServerStarted) {
    cameraServer.handleClient();
  }
  sendPingIfNeeded();
  uploadVideoFrameIfNeeded();
  uploadAudioChunkIfNeeded();

  unsigned long current = millis();
  if (wsConnected && current - lastStatusAt >= STATUS_INTERVAL_MS) {
    lastStatusAt = current;
    publishStatus();
  }

  if (lastCommandAt > 0 && current - lastCommandAt > COMMAND_TIMEOUT_MS) {
    safeStop();
    lastCommandAt = 0;
  }
}
