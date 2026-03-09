#include <Arduino.h>
#include <ESP_I2S.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <libb64/cencode.h>
#include <Preferences.h>
#include "esp_camera.h"

#include "secrets.h"

#ifndef LED_BUILTIN
#define LED_BUILTIN -1
#endif

namespace {
WebSocketsClient ws;
WebServer cameraServer(80);
I2SClass microphone;
Preferences preferences;

constexpr unsigned long WIFI_RETRY_MS = 5000;
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
constexpr unsigned long STATUS_INTERVAL_MS = 2000;
constexpr unsigned long PING_INTERVAL_MS = 10000;
constexpr unsigned long COMMAND_TIMEOUT_MS = 1000;
constexpr unsigned long WS_CONNECT_TIMEOUT_MS = 10000;
constexpr unsigned long RESTART_DELAY_MS = 3000;
constexpr unsigned long AP_AUTO_REBOOT_MS = 2000;
constexpr unsigned long VIDEO_UPLOAD_INTERVAL_MS = 220;
constexpr unsigned long AUDIO_UPLOAD_INTERVAL_MS = 40;
constexpr uint8_t MAX_PING_FAILS = 3;
constexpr uint32_t AUDIO_CAPTURE_SAMPLE_RATE = 16000;
constexpr uint32_t AUDIO_STREAM_SAMPLE_RATE = 16000;
constexpr size_t AUDIO_CAPTURE_SAMPLES = 640;
constexpr size_t AUDIO_STREAM_SAMPLES = 640;
constexpr size_t AUDIO_CAPTURE_BYTES = AUDIO_CAPTURE_SAMPLES * sizeof(int16_t);
constexpr size_t AUDIO_ADPCM_HEADER_BYTES = 4;
constexpr size_t AUDIO_ADPCM_PAYLOAD_BYTES = AUDIO_ADPCM_HEADER_BYTES + ((AUDIO_STREAM_SAMPLES - 1 + 1) / 2);
constexpr size_t AUDIO_BASE64_BUFFER_LEN = 433;

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
bool driveMode = true;
bool ledEnabled = false;
bool talkEnabled = false;
bool apMode = false;
bool serverStarted = false;
bool hasStoredWifi = false;
uint8_t pingFailCount = 0;
uint8_t wifiFailureCount = 0;
int lastThrottle = 0;
int lastSteering = 0;
uint32_t audioSequence = 0;
int32_t audioHighpassState = 0;
int32_t audioHighpassLastInput = 0;
int16_t audioAdpcmPredictor = 0;
int8_t audioAdpcmStepIndex = 0;

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
constexpr int MOTOR_PWM_PIN = 2;
constexpr int SERVO_PWM_PIN = 4;
constexpr int MOTOR_DIR1_PIN = 5;
constexpr int MOTOR_DIR2_PIN = 6;
constexpr int STATUS_LED_PIN = 43;
constexpr int BATTERY_SENSE_PIN = 1;
constexpr uint32_t MOTOR_PWM_FREQ_HZ = 200;
constexpr uint8_t MOTOR_PWM_RES_BITS = 12;
constexpr uint32_t SERVO_PWM_FREQ_HZ = 50;
constexpr uint8_t SERVO_PWM_RES_BITS = 16;
constexpr int STEERING_MIN_US = 1100;
constexpr int STEERING_CENTER_US = 1500;
constexpr int STEERING_MAX_US = 1900;
constexpr char PREF_NAMESPACE[] = "rc-car";
constexpr char PREF_WIFI_SSID[] = "wifi_ssid";
constexpr char PREF_WIFI_PASS[] = "wifi_pass";
constexpr char PREF_BROKER_HOST[] = "broker_host";
constexpr char PREF_BROKER_PORT[] = "broker_port";
constexpr char PREF_DEVICE_ID[] = "device_id";
constexpr char AP_SSID[] = "RC-Car-Setup";
constexpr char AP_PASSWORD[] = "12345678";
constexpr char DEFAULT_DEVICE_ID[] = "rc-car-01";

String activeWifiSsid;
String activeWifiPassword;
String activeBrokerHost;
uint16_t activeBrokerPort = BROKER_PORT;
String activeDeviceId;

int16_t audioCaptureBuffer[AUDIO_CAPTURE_SAMPLES] = {};
uint8_t audioAdpcmBuffer[AUDIO_ADPCM_PAYLOAD_BYTES] = {};
char audioBase64Buffer[AUDIO_BASE64_BUFFER_LEN] = {};

constexpr int8_t IMA_INDEX_TABLE[16] = {
  -1, -1, -1, -1, 2, 4, 6, 8,
  -1, -1, -1, -1, 2, 4, 6, 8
};

constexpr int16_t IMA_STEP_TABLE[89] = {
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
  19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
  50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
  130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
  337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
  876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
  2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
  5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
  15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};
}

void writeMotorOutput(int throttle);
void writeSteeringOutput(int steering);
void setLedState(bool enabled);
void applyCameraQuality(const char* quality);
void initActuators();
bool loadWifiCredentials();
void saveConfig(const String& ssid, const String& password, const String& brokerHost, uint16_t brokerPort, const String& deviceId);
void startProvisioningAp();
void startHttpServer();
uint32_t readBatteryMilliVolts();
uint8_t estimateBatteryPercent(uint32_t batteryMv);

void handleRoot();
void handleStatusJson();
void handleControlJson();
void handleConfigSave();
void handleJpeg();
void handleStream();

void safeStop() {
  lastThrottle = 0;
  lastSteering = 0;
  writeMotorOutput(0);
  writeSteeringOutput(0);
  Serial.println("[SAFE] stop");
}

bool loadWifiCredentials() {
  preferences.begin(PREF_NAMESPACE, true);
  activeWifiSsid = preferences.getString(PREF_WIFI_SSID, "");
  activeWifiPassword = preferences.getString(PREF_WIFI_PASS, "");
  activeBrokerHost = preferences.getString(PREF_BROKER_HOST, "");
  activeBrokerPort = preferences.getUShort(PREF_BROKER_PORT, 0);
  activeDeviceId = preferences.getString(PREF_DEVICE_ID, "");
  preferences.end();

  if (activeWifiSsid.length() == 0 && strlen(WIFI_SSID) > 0) {
    activeWifiSsid = WIFI_SSID;
    activeWifiPassword = WIFI_PASSWORD;
  }
  if (activeBrokerHost.length() == 0 && strlen(BROKER_HOST) > 0) {
    activeBrokerHost = BROKER_HOST;
  }
  if (activeBrokerPort == 0) {
    activeBrokerPort = static_cast<uint16_t>(BROKER_PORT);
  }
  if (activeDeviceId.length() == 0 && strlen(DEVICE_ID) > 0) {
    activeDeviceId = DEVICE_ID;
  }
  if (activeDeviceId.length() == 0) {
    activeDeviceId = DEFAULT_DEVICE_ID;
  }

  hasStoredWifi = activeWifiSsid.length() > 0;
  return hasStoredWifi;
}

void saveConfig(const String& ssid, const String& password, const String& brokerHost, uint16_t brokerPort, const String& deviceId) {
  preferences.begin(PREF_NAMESPACE, false);
  preferences.putString(PREF_WIFI_SSID, ssid);
  preferences.putString(PREF_WIFI_PASS, password);
  preferences.putString(PREF_BROKER_HOST, brokerHost);
  preferences.putUShort(PREF_BROKER_PORT, brokerPort);
  preferences.putString(PREF_DEVICE_ID, deviceId);
  preferences.end();
  activeWifiSsid = ssid;
  activeWifiPassword = password;
  activeBrokerHost = brokerHost;
  activeBrokerPort = brokerPort;
  activeDeviceId = deviceId;
  hasStoredWifi = true;
}

void startProvisioningAp() {
  if (apMode) {
    return;
  }

  safeStop();
  ws.disconnect();
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  apMode = true;
  wifiConnectInFlight = false;
  wsConnectInFlight = false;
  wsConnected = false;
  Serial.printf("[AP] started ssid=%s ip=%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}

uint32_t readBatteryMilliVolts() {
  return analogReadMilliVolts(BATTERY_SENSE_PIN) * 2U;
}

uint8_t estimateBatteryPercent(uint32_t batteryMv) {
  // Simple 2S Li-ion estimate for the dashboard: 6.4V empty, 8.4V full.
  long percent = map(static_cast<long>(constrain(batteryMv, 6400UL, 8400UL)), 6400L, 8400L, 0L, 100L);
  return static_cast<uint8_t>(constrain(percent, 0L, 100L));
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
  doc["audioCodec"] = "adpcm_ima";
  doc["audioSampleRate"] = AUDIO_STREAM_SAMPLE_RATE;
  doc["mode"] = driveMode ? "drive" : "monitor";
  doc["ledEnabled"] = ledEnabled;
  doc["talkEnabled"] = talkEnabled;
  doc["batteryMv"] = readBatteryMilliVolts();
  doc["batteryPct"] = estimateBatteryPercent(doc["batteryMv"]);
  doc["apMode"] = apMode;
  doc["streamPort"] = 80;
  doc["streamPath"] = "/stream";
  doc["localIp"] = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();

  String payload;
  serializeJson(doc, payload);
  ws.sendTXT(payload);
}

void applyControl(int throttle, int steering) {
  if (!driveMode) {
    safeStop();
    Serial.println("[CTRL] ignored in monitor mode");
    return;
  }

  lastThrottle = constrain(throttle, -100, 100);
  lastSteering = constrain(steering, -100, 100);
  lastCommandAt = millis();
  writeMotorOutput(lastThrottle);
  writeSteeringOutput(lastSteering);

  Serial.printf("[CTRL] throttle=%d steering=%d\n", lastThrottle, lastSteering);
}

void initActuators() {
  pinMode(MOTOR_DIR1_PIN, OUTPUT);
  pinMode(MOTOR_DIR2_PIN, OUTPUT);
  digitalWrite(MOTOR_DIR1_PIN, LOW);
  digitalWrite(MOTOR_DIR2_PIN, LOW);

  ledcAttach(MOTOR_PWM_PIN, MOTOR_PWM_FREQ_HZ, MOTOR_PWM_RES_BITS);
  ledcWrite(MOTOR_PWM_PIN, 0);

  ledcAttach(SERVO_PWM_PIN, SERVO_PWM_FREQ_HZ, SERVO_PWM_RES_BITS);
  writeSteeringOutput(0);

  if (STATUS_LED_PIN >= 0) {
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, LOW);
  }

  analogReadResolution(12);
}

void writeMotorOutput(int throttle) {
  // Preserve the older car tuning: UI sends -100..100, legacy drive code used -90..90.
  int legacyThrottle = map(throttle, -100, 100, -90, 90);
  int pwmValue = map(legacyThrottle, -90, 90, -2000, 2000);
  uint32_t maxDuty = (1U << MOTOR_PWM_RES_BITS) - 1U;
  uint32_t duty = 0;

  if (pwmValue > 600) {
    digitalWrite(MOTOR_DIR1_PIN, HIGH);
    digitalWrite(MOTOR_DIR2_PIN, LOW);
    duty = map(pwmValue, 0, 2000, 0, static_cast<int>(maxDuty));
  } else if (pwmValue < -600) {
    digitalWrite(MOTOR_DIR1_PIN, LOW);
    digitalWrite(MOTOR_DIR2_PIN, HIGH);
    duty = map(abs(pwmValue), 0, 2000, 0, static_cast<int>(maxDuty));
  } else {
    digitalWrite(MOTOR_DIR1_PIN, LOW);
    digitalWrite(MOTOR_DIR2_PIN, LOW);
  }

  ledcWrite(MOTOR_PWM_PIN, duty);
}

void writeSteeringOutput(int steering) {
  // Match the older steering calibration rather than a generic centered servo map.
  int legacySteering = map(steering, -100, 100, -90, 90);
  int servoValue = map(legacySteering, -90, 90, 128, 55);
  uint32_t duty = (8191U * static_cast<uint32_t>(servoValue)) / 180U;
  ledcWrite(SERVO_PWM_PIN, duty);
}

void setLedState(bool enabled) {
  ledEnabled = enabled;
  if (STATUS_LED_PIN >= 0) {
    digitalWrite(STATUS_LED_PIN, enabled ? HIGH : LOW);
  }
  Serial.printf("[LED] %s\n", enabled ? "on" : "off");
}

void applyCameraQuality(const char* quality) {
  sensor_t* sensor = esp_camera_sensor_get();
  if (sensor == nullptr) {
    return;
  }

  framesize_t frameSize = FRAMESIZE_QVGA;
  int jpegQuality = 14;

  if (strcmp(quality, "VGA") == 0) {
    frameSize = FRAMESIZE_VGA;
    jpegQuality = 12;
  } else if (strcmp(quality, "SVGA") == 0) {
    frameSize = FRAMESIZE_SVGA;
    jpegQuality = 12;
  } else if (strcmp(quality, "UXGA") == 0) {
    frameSize = FRAMESIZE_UXGA;
    jpegQuality = 10;
  }

  sensor->set_framesize(sensor, frameSize);
  sensor->set_quality(sensor, jpegQuality);
  Serial.printf("[CAM] quality=%s\n", quality);
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
        } else if (strcmp(messageType, "led") == 0) {
          setLedState(doc["enabled"] | false);
        } else if (strcmp(messageType, "mode") == 0) {
          driveMode = strcmp(doc["mode"] | "drive", "monitor") != 0;
          if (!driveMode) {
            safeStop();
          }
          Serial.printf("[MODE] %s\n", driveMode ? "drive" : "monitor");
        } else if (strcmp(messageType, "talk") == 0) {
          talkEnabled = doc["enabled"] | false;
          Serial.printf("[TALK] %s\n", talkEnabled ? "on" : "off");
        } else if (strcmp(messageType, "camera_quality") == 0) {
          applyCameraQuality(doc["quality"] | "QVGA");
        } else if (strcmp(messageType, "snapshot") == 0) {
          Serial.println("[SNAP] requested");
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
  config.jpeg_quality = 14;
  config.fb_count = 2;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;

  Serial.printf("[CAM] psram=%s\n", psramFound() ? "yes" : "no");

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    // Fallback path for board/profile mismatches: force low-memory camera mode.
    config.frame_size = FRAMESIZE_QQVGA;
    config.jpeg_quality = 20;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
    err = esp_camera_init(&config);
  }
  if (err != ESP_OK) {
    Serial.printf("[CAM] init failed: 0x%x\n", err);
    return false;
  }

  sensor_t* sensor = esp_camera_sensor_get();
  if (sensor != nullptr) {
    sensor->set_framesize(sensor, FRAMESIZE_QVGA);
    sensor->set_quality(sensor, 14);
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

void startHttpServer() {
  if (serverStarted) {
    return;
  }
  cameraServer.on("/", HTTP_GET, handleRoot);
  cameraServer.on("/api/status", HTTP_GET, handleStatusJson);
  cameraServer.on("/api/control", HTTP_POST, handleControlJson);
  cameraServer.on("/api/config", HTTP_POST, handleConfigSave);
  cameraServer.on("/jpg", HTTP_GET, handleJpeg);
  cameraServer.on("/stream", HTTP_GET, handleStream);
  cameraServer.begin();
  serverStarted = true;
  cameraServerStarted = true;
  Serial.println("[HTTP] server started on :80");
}

void handleRoot() {
  String html;
  html += "<!doctype html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1,viewport-fit=cover'>";
  html += "<title>RC Car</title>";
  html += "<style>body{font-family:sans-serif;margin:0;background:#111;color:#f4f4f4}main{max-width:720px;margin:0 auto;padding:16px}form,input,button{font:inherit}button{padding:12px 16px;border:0;border-radius:10px}input{padding:12px;border-radius:10px;border:1px solid #444;background:#1d1d1d;color:#fff;width:100%;box-sizing:border-box} .stack{display:grid;gap:12px} .panel{background:#1b1b1b;padding:16px;border-radius:16px} .row{display:grid;grid-template-columns:1fr 1fr;gap:12px} .stream{width:100%;border-radius:16px;transform:scaleX(-1);background:#000} .controls{display:grid;grid-template-columns:1fr 1fr 1fr;gap:12px;margin-top:12px}.wide{grid-column:1/-1}.pill{font-size:14px;color:#9ad}</style></head><body><main>";

  if (apMode) {
    html += "<div class='panel stack'><h1>Wi-Fi Setup</h1>";
    html += "<div class='pill'>AP SSID: ";
    html += AP_SSID;
    html += " / password: ";
    html += AP_PASSWORD;
    html += "</div>";
    html += "<img class='stream' src='/stream'>";
    html += "<div class='pill'>Camera preview: /stream</div>";
    html += "<form class='stack' method='post' action='/api/config'>";
    html += "<input name='ssid' placeholder='Wi-Fi SSID' required value='";
    html += activeWifiSsid;
    html += "'>";
    html += "<input name='password' placeholder='Wi-Fi Password' type='password'>";
    html += "<input name='brokerHost' placeholder='Broker Host/IP' value='";
    html += activeBrokerHost;
    html += "'>";
    html += "<input name='brokerPort' placeholder='Broker Port' type='number' min='1' max='65535' value='";
    html += String(activeBrokerPort);
    html += "'>";
    html += "<input name='deviceId' placeholder='Device ID' value='";
    html += activeDeviceId;
    html += "'>";
    html += "<button type='submit'>Save And Reboot</button>";
    html += "</form></div>";
  } else {
    html += "<div class='panel stack'><h1>RC Car Control</h1>";
    html += "<div id='status' class='pill'>connecting...</div>";
    html += "<img class='stream' src='/stream'>";
    html += "<div class='controls'>";
    html += "<button onclick='send(100,0)' class='wide'>Forward</button>";
    html += "<button onclick='send(0,-35)'>Left</button>";
    html += "<button onclick='send(0,0)'>Stop</button>";
    html += "<button onclick='send(0,35)'>Right</button>";
    html += "<button onclick='send(-70,0)' class='wide'>Reverse</button>";
    html += "</div>";
    html += "<div class='stack'><label>Throttle <input id='throttle' type='range' min='-100' max='100' value='0'></label>";
    html += "<label>Steering <input id='steering' type='range' min='-45' max='45' value='0'></label>";
    html += "<div class='row'><button onclick='applySliders()'>Apply</button><button onclick='toggleLed()'>LED</button></div></div>";
    html += "</div><script>";
    html += "async function send(t,s){await fetch('/api/control',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({throttle:t,steering:s})});}";
    html += "function applySliders(){send(+throttle.value,+steering.value)}";
    html += "let led=false; async function toggleLed(){led=!led; await fetch('/api/control',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ledEnabled:led})});}";
    html += "setInterval(async()=>{const r=await fetch('/api/status'); const j=await r.json(); status.textContent='IP '+j.ip+' | RSSI '+j.rssi+' dBm | BAT '+j.batteryMv+' mV | mode '+j.mode;},1500);";
    html += "</script>";
  }

  html += "</main></body></html>";
  cameraServer.send(200, "text/html", html);
}

void handleStatusJson() {
  StaticJsonDocument<192> doc;
  doc["apMode"] = apMode;
  doc["ip"] = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  doc["rssi"] = WiFi.isConnected() ? WiFi.RSSI() : 0;
  doc["batteryMv"] = readBatteryMilliVolts();
  doc["batteryPct"] = estimateBatteryPercent(doc["batteryMv"]);
  doc["mode"] = driveMode ? "drive" : "monitor";
  doc["ledEnabled"] = ledEnabled;
  doc["throttle"] = lastThrottle;
  doc["steering"] = lastSteering;

  String payload;
  serializeJson(doc, payload);
  cameraServer.send(200, "application/json", payload);
}

void handleControlJson() {
  StaticJsonDocument<192> doc;
  DeserializationError error = deserializeJson(doc, cameraServer.arg("plain"));
  if (error) {
    cameraServer.send(400, "application/json", "{\"ok\":false}");
    return;
  }

  if (doc.containsKey("ledEnabled")) {
    setLedState(doc["ledEnabled"] | false);
  }
  if (doc.containsKey("throttle") || doc.containsKey("steering")) {
    applyControl(doc["throttle"] | lastThrottle, doc["steering"] | lastSteering);
  }
  cameraServer.send(200, "application/json", "{\"ok\":true}");
}

void handleConfigSave() {
  String ssid = cameraServer.arg("ssid");
  String password = cameraServer.arg("password");
  String brokerHost = cameraServer.arg("brokerHost");
  String brokerPortText = cameraServer.arg("brokerPort");
  String deviceId = cameraServer.arg("deviceId");
  ssid.trim();
  password.trim();
  brokerHost.trim();
  brokerPortText.trim();
  deviceId.trim();

  if (ssid.length() == 0) {
    ssid = activeWifiSsid;
  }
  if (brokerHost.length() == 0) {
    brokerHost = activeBrokerHost;
  }
  if (deviceId.length() == 0) {
    deviceId = activeDeviceId;
  }

  uint16_t brokerPort = activeBrokerPort;
  if (brokerPortText.length() > 0) {
    long parsedPort = brokerPortText.toInt();
    if (parsedPort >= 1 && parsedPort <= 65535) {
      brokerPort = static_cast<uint16_t>(parsedPort);
    }
  }

  if (ssid.length() == 0 || brokerHost.length() == 0 || deviceId.length() == 0 || brokerPort == 0) {
    cameraServer.send(400, "text/plain", "invalid config");
    return;
  }

  saveConfig(ssid, password, brokerHost, brokerPort, deviceId);
  cameraServer.send(200, "text/html", "<!doctype html><html><body><h1>Saved</h1><p>Rebooting...</p></body></html>");
  restartScheduledAt = millis() + AP_AUTO_REBOOT_MS;
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

int16_t filterAudioSample(int16_t sample) {
  int32_t input = sample;
  audioHighpassState = (995 * (audioHighpassState + input - audioHighpassLastInput)) / 1000;
  audioHighpassLastInput = input;

  int32_t filtered = audioHighpassState;
  if (abs(filtered) < 32) {
    filtered = 0;
  }

  filtered *= 8;
  filtered = constrain(filtered, -32768, 32767);
  return static_cast<int16_t>(filtered);
}

uint8_t encodeAdpcmNibble(int16_t sample, int16_t& predictor, int8_t& stepIndex) {
  int step = IMA_STEP_TABLE[stepIndex];
  int diff = sample - predictor;
  uint8_t nibble = 0;

  if (diff < 0) {
    nibble = 8;
    diff = -diff;
  }

  int delta = step >> 3;
  if (diff >= step) {
    nibble |= 4;
    diff -= step;
    delta += step;
  }
  if (diff >= (step >> 1)) {
    nibble |= 2;
    diff -= step >> 1;
    delta += step >> 1;
  }
  if (diff >= (step >> 2)) {
    nibble |= 1;
    delta += step >> 2;
  }

  predictor += (nibble & 8) ? -delta : delta;
  predictor = constrain(predictor, -32768, 32767);

  stepIndex += IMA_INDEX_TABLE[nibble & 0x0F];
  stepIndex = constrain(stepIndex, 0, 88);
  return nibble & 0x0F;
}

size_t encodeAdpcmBlock(const int16_t* input, size_t sampleCount, uint8_t* output) {
  if (sampleCount == 0) {
    return 0;
  }

  int16_t predictor = input[0];
  int8_t stepIndex = audioAdpcmStepIndex;
  output[0] = static_cast<uint8_t>(predictor & 0xFF);
  output[1] = static_cast<uint8_t>((predictor >> 8) & 0xFF);
  output[2] = static_cast<uint8_t>(stepIndex);
  output[3] = 0;

  size_t outIndex = AUDIO_ADPCM_HEADER_BYTES;
  for (size_t i = 1; i < sampleCount; i += 2) {
    uint8_t low = encodeAdpcmNibble(input[i], predictor, stepIndex);
    uint8_t high = 0;
    if (i + 1 < sampleCount) {
      high = encodeAdpcmNibble(input[i + 1], predictor, stepIndex);
    }
    output[outIndex++] = static_cast<uint8_t>(low | (high << 4));
  }

  audioAdpcmStepIndex = stepIndex;
  return outIndex;
}

void uploadAudioChunkIfNeeded() {
  if (!microphoneReady || !wsConnected || talkEnabled) {
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
    audioCaptureBuffer[i] = filterAudioSample(audioCaptureBuffer[i]);
  }

  size_t adpcmLen = encodeAdpcmBlock(audioCaptureBuffer, AUDIO_STREAM_SAMPLES, audioAdpcmBuffer);
  int encodedLen = base64_encode_chars(reinterpret_cast<const char*>(audioAdpcmBuffer), adpcmLen, audioBase64Buffer);
  if (encodedLen <= 0 || encodedLen >= static_cast<int>(sizeof(audioBase64Buffer))) {
    Serial.println("[MIC] base64 encode failed");
    return;
  }
  audioBase64Buffer[encodedLen] = '\0';

  StaticJsonDocument<512> doc;
  doc["type"] = "audio";
  doc["codec"] = "adpcm_ima";
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
  if (apMode || WiFi.status() != WL_CONNECTED) {
    return;
  }

  configureWebSocket();
  if (activeDeviceId.length() == 0) {
    activeDeviceId = DEFAULT_DEVICE_ID;
  }
  Serial.printf("[WS] target=%s:%u deviceId=%s\n", activeBrokerHost.c_str(), activeBrokerPort, activeDeviceId.c_str());
  ws.begin(activeBrokerHost.c_str(), activeBrokerPort, String("/device?deviceId=") + activeDeviceId);
  wsConnectStartedAt = millis();
  wsConnectInFlight = true;
}

void ensureWifiConnected() {
  if (apMode) {
    return;
  }

  if (!hasStoredWifi) {
    startProvisioningAp();
    return;
  }

  wl_status_t wifiStatus = WiFi.status();
  if (wifiStatus == WL_CONNECTED) {
    wifiConnectInFlight = false;
    wifiFailureCount = 0;
    return;
  }

  unsigned long current = millis();

  if (wifiConnectInFlight) {
    if (current - wifiConnectStartedAt >= WIFI_CONNECT_TIMEOUT_MS) {
      Serial.println("[WIFI] connect timeout, retrying");
      WiFi.disconnect(true, true);
      wifiConnectInFlight = false;
      lastWifiAttemptAt = current;
      wifiFailureCount++;
      if (wifiFailureCount >= 3) {
        Serial.println("[WIFI] falling back to AP mode");
        startProvisioningAp();
      }
    }
    return;
  }

  if (current - lastWifiAttemptAt < WIFI_RETRY_MS) {
    return;
  }

  lastWifiAttemptAt = current;
  wifiConnectStartedAt = current;
  wifiConnectInFlight = true;
  Serial.printf("[WIFI] connecting to %s\n", activeWifiSsid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(activeWifiSsid.c_str(), activeWifiPassword.c_str());
}

void ensureWebSocketConnected() {
  if (apMode || !WiFi.isConnected()) {
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
  initActuators();
  safeStop();
  cameraReady = initCamera();
  microphoneReady = initMicrophone();
  configureWebSocket();
  loadWifiCredentials();
  if (!hasStoredWifi) {
    startProvisioningAp();
    startHttpServer();
  } else {
    ensureWifiConnected();
  }
}

void loop() {
  if (restartScheduledAt != 0 && millis() >= restartScheduledAt) {
    Serial.println("[SYS] restarting now");
    delay(100);
    ESP.restart();
  }

  ensureWifiConnected();
  if ((apMode || WiFi.isConnected()) && !serverStarted) {
    startHttpServer();
  }
  ensureWebSocketConnected();
  ws.loop();
  if (serverStarted) {
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
