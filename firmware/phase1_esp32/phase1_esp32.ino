#include <Arduino.h>
#include <ESP_I2S.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <libb64/cencode.h>
#include <memory>
#include <Preferences.h>
#include <ESP32Servo.h>
#include <mbedtls/base64.h>
#include "esp_camera.h"

#include "secrets.h"
#include "web_index.h"

#ifndef LED_BUILTIN
#define LED_BUILTIN -1
#endif

namespace {
WebSocketsClient ws;
WebServer cameraServer(80);
WebSocketsServer localAudioWs(81);
I2SClass microphone;
I2SClass speaker;
Preferences preferences;
Servo steeringServo;
SemaphoreHandle_t latestFrameMutex = nullptr;
TaskHandle_t cameraCaptureTaskHandle = nullptr;

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
constexpr unsigned long CAMERA_CAPTURE_INTERVAL_MS = 80;
constexpr uint8_t MAX_PING_FAILS = 3;
constexpr uint8_t WIFI_FAILURES_BEFORE_AP = 3;
constexpr uint8_t BROKER_RECOVERY_PING_FAILS = 2;
constexpr uint8_t BROKER_RECOVERY_DISCONNECTS = 2;
constexpr uint8_t BROKER_RECOVERY_WS_ERRORS = 2;
constexpr uint8_t BROKER_RECOVERY_VIDEO_FAILS = 4;
constexpr uint8_t BROKER_RECOVERY_AUDIO_FAILS = 6;
constexpr uint32_t AUDIO_CAPTURE_SAMPLE_RATE = 16000;
constexpr uint32_t AUDIO_STREAM_SAMPLE_RATE = 16000;
constexpr uint32_t AUDIO_PLAYBACK_SAMPLE_RATE = 16000;
constexpr size_t AUDIO_CAPTURE_SAMPLES = 640;
constexpr size_t AUDIO_STREAM_SAMPLES = 640;
constexpr size_t AUDIO_CAPTURE_BYTES = AUDIO_CAPTURE_SAMPLES * sizeof(int16_t);
constexpr size_t AUDIO_ADPCM_HEADER_BYTES = 4;
constexpr size_t AUDIO_ADPCM_PAYLOAD_BYTES = AUDIO_ADPCM_HEADER_BYTES + ((AUDIO_STREAM_SAMPLES - 1 + 1) / 2);
constexpr size_t AUDIO_BASE64_BUFFER_LEN = 433;
constexpr int32_t AUDIO_HPF_ALPHA_NUM = 995;
constexpr int32_t AUDIO_HPF_ALPHA_DEN = 1000;
constexpr int32_t AUDIO_NOISE_GATE = 32;
constexpr int32_t AUDIO_SOFTWARE_GAIN = 10;

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
bool speakerReady = false;
bool driveMode = true;
bool ledEnabled = false;
bool talkEnabled = false;
bool apMode = false;
bool serverStarted = false;
bool hasStoredWifi = false;
bool localAudioWsStarted = false;
bool cameraCaptureTaskStarted = false;
bool brokerDisconnectLatched = false;
uint8_t pingFailCount = 0;
uint8_t wifiFailureCount = 0;
uint8_t brokerDisconnectCount = 0;
uint8_t brokerWsErrorCount = 0;
uint8_t brokerVideoFailCount = 0;
uint8_t brokerAudioFailCount = 0;
int lastThrottle = 0;
int lastSteering = 0;
uint32_t audioSequence = 0;
uint32_t talkAudioSequence = 0;
uint32_t latestFrameSequence = 0;
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
constexpr int SERVO_PWM_PIN = 3;
constexpr int MOTOR_DIR1_PIN = 5;
constexpr int MOTOR_DIR2_PIN = 6;
constexpr int STATUS_LED_PIN = 43;
constexpr int BATTERY_SENSE_PIN = 1;
constexpr int AMP_BCLK_PIN = 7;
constexpr int AMP_WS_PIN = 8;
constexpr int AMP_DIN_PIN = 4;
constexpr uint8_t MOTOR_PWM_CHANNEL = 2;
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
constexpr char PREF_WIFI_MEMORY[] = "wifi_memory";
constexpr char PREF_CONNECT_ONCE[] = "connect_once";
constexpr char AP_SSID[] = "RC-Car-Setup";
constexpr char AP_PASSWORD[] = "12345678";
constexpr char DEFAULT_DEVICE_ID[] = "rc-car-01";
constexpr uint8_t MAX_REMEMBERED_WIFI = 5;

String activeWifiSsid;
String activeWifiPassword;
String activeBrokerHost;
uint16_t activeBrokerPort = BROKER_PORT;
String activeDeviceId;
String rememberedSsids[MAX_REMEMBERED_WIFI];
String rememberedPasswords[MAX_REMEMBERED_WIFI];
uint8_t rememberedWifiCount = 0;
bool connectOnBoot = false;

int16_t audioCaptureBuffer[AUDIO_CAPTURE_SAMPLES] = {};
uint8_t audioAdpcmBuffer[AUDIO_ADPCM_PAYLOAD_BYTES] = {};
char audioBase64Buffer[AUDIO_BASE64_BUFFER_LEN] = {};
uint8_t audioDecodeBuffer[AUDIO_ADPCM_PAYLOAD_BYTES] = {};
int16_t audioPlaybackBuffer[AUDIO_STREAM_SAMPLES] = {};
uint8_t speakerFrameBuffer[AUDIO_STREAM_SAMPLES * 4] = {};
uint8_t* latestJpegFrame = nullptr;
size_t latestJpegFrameLen = 0;

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
void loadRememberedWifi();
void saveRememberedWifi();
void rememberWifiCredentials(const String& ssid, const String& password);
String getRememberedPassword(const String& ssid);
void setConnectOnBoot(bool enabled);
void startProvisioningAp();
void startHttpServer();
uint32_t readBatteryMilliVolts();
uint8_t estimateBatteryPercent(uint32_t batteryMv);
void resetBrokerFailureCounters();
void scheduleBrokerRecovery(const char* reason);

void handleRoot();
void handleStatusJson();
void handleControlJson();
void handleConfigSave();
void handleJpeg();
void handleStream();
void handleWifiScan();
void handleWifiConnect();
void handleTalkAudio();
void playSpeakerBootTone();
void startCameraCaptureTask();
void cameraCaptureTask(void* arg);
bool copyLatestJpegFrame(std::unique_ptr<uint8_t[]>& frameCopy, size_t& frameLen);
size_t decodeBase64Payload(const char* encoded, uint8_t* output, size_t outputSize);
size_t decodeAdpcmBlock(const uint8_t* input, size_t inputLen, int16_t* output, size_t maxSamples);
void playSpeakerSamples(const int16_t* samples, size_t sampleCount);

void safeStop() {
  lastThrottle = 0;
  lastSteering = 0;
  writeMotorOutput(0);
  writeSteeringOutput(0);
  Serial.println("[SAFE] stop");
}

void playSpeakerBootTone() {
  if (!speakerReady) {
    return;
  }

  constexpr int16_t amplitude = 2800;
  constexpr uint32_t toneHz = 880;
  constexpr size_t sampleCount = AUDIO_PLAYBACK_SAMPLE_RATE / 12;
  int16_t sample = amplitude;
  uint32_t halfWaveSamples = AUDIO_PLAYBACK_SAMPLE_RATE / (toneHz * 2);
  if (halfWaveSamples == 0) {
    halfWaveSamples = 1;
  }

  for (size_t i = 0; i < sampleCount; ++i) {
    if ((i % halfWaveSamples) == 0) {
      sample = -sample;
    }
    uint8_t frame[4] = {
      static_cast<uint8_t>(sample & 0xff),
      static_cast<uint8_t>((sample >> 8) & 0xff),
      static_cast<uint8_t>(sample & 0xff),
      static_cast<uint8_t>((sample >> 8) & 0xff)
    };
    speaker.write(frame, sizeof(frame));
  }
}

bool loadWifiCredentials() {
  preferences.begin(PREF_NAMESPACE, true);
  activeWifiSsid = preferences.getString(PREF_WIFI_SSID, "");
  activeWifiPassword = preferences.getString(PREF_WIFI_PASS, "");
  activeBrokerHost = preferences.getString(PREF_BROKER_HOST, "");
  activeBrokerPort = preferences.getUShort(PREF_BROKER_PORT, 0);
  activeDeviceId = preferences.getString(PREF_DEVICE_ID, "");
  connectOnBoot = preferences.getBool(PREF_CONNECT_ONCE, false);
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

  Serial.printf("[CFG] wifiSsidLen=%u wifiPassLen=%u brokerHost=%s brokerPort=%u deviceId=%s\n",
    static_cast<unsigned>(activeWifiSsid.length()),
    static_cast<unsigned>(activeWifiPassword.length()),
    activeBrokerHost.c_str(),
    activeBrokerPort,
    activeDeviceId.c_str());

  hasStoredWifi = activeWifiSsid.length() > 0;
  return hasStoredWifi;
}

void loadRememberedWifi() {
  rememberedWifiCount = 0;

  preferences.begin(PREF_NAMESPACE, true);
  String memoryJson = preferences.getString(PREF_WIFI_MEMORY, "");
  preferences.end();

  if (memoryJson.length() == 0) {
    if (activeWifiSsid.length() > 0) {
      rememberWifiCredentials(activeWifiSsid, activeWifiPassword);
    }
    return;
  }

  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, memoryJson)) {
    return;
  }

  JsonArray items = doc["items"].as<JsonArray>();
  for (JsonObject item : items) {
    if (rememberedWifiCount >= MAX_REMEMBERED_WIFI) {
      break;
    }
    String ssid = String(item["ssid"] | "");
    if (ssid.length() == 0) {
      continue;
    }
    rememberedSsids[rememberedWifiCount] = ssid;
    rememberedPasswords[rememberedWifiCount] = String(item["password"] | "");
    rememberedWifiCount++;
  }

  if (rememberedWifiCount == 0 && activeWifiSsid.length() > 0) {
    rememberWifiCredentials(activeWifiSsid, activeWifiPassword);
  }
}

void saveRememberedWifi() {
  StaticJsonDocument<512> doc;
  JsonArray items = doc.createNestedArray("items");
  for (uint8_t i = 0; i < rememberedWifiCount; ++i) {
    JsonObject item = items.createNestedObject();
    item["ssid"] = rememberedSsids[i];
    item["password"] = rememberedPasswords[i];
  }

  String payload;
  serializeJson(doc, payload);

  preferences.begin(PREF_NAMESPACE, false);
  preferences.putString(PREF_WIFI_MEMORY, payload);
  preferences.end();
}

void rememberWifiCredentials(const String& ssid, const String& password) {
  if (ssid.length() == 0) {
    return;
  }

  int existingIndex = -1;
  for (uint8_t i = 0; i < rememberedWifiCount; ++i) {
    if (rememberedSsids[i] == ssid) {
      existingIndex = i;
      break;
    }
  }

  String nextPassword = password;

  if (existingIndex > 0) {
    for (int i = existingIndex; i > 0; --i) {
      rememberedSsids[i] = rememberedSsids[i - 1];
      rememberedPasswords[i] = rememberedPasswords[i - 1];
    }
    rememberedSsids[0] = ssid;
    rememberedPasswords[0] = nextPassword;
  } else if (existingIndex == 0) {
    rememberedPasswords[0] = nextPassword;
  } else {
    if (rememberedWifiCount < MAX_REMEMBERED_WIFI) {
      for (int i = rememberedWifiCount; i > 0; --i) {
        rememberedSsids[i] = rememberedSsids[i - 1];
        rememberedPasswords[i] = rememberedPasswords[i - 1];
      }
      rememberedWifiCount++;
    } else {
      for (int i = MAX_REMEMBERED_WIFI - 1; i > 0; --i) {
        rememberedSsids[i] = rememberedSsids[i - 1];
        rememberedPasswords[i] = rememberedPasswords[i - 1];
      }
    }
    rememberedSsids[0] = ssid;
    rememberedPasswords[0] = nextPassword;
  }

  saveRememberedWifi();
}

String getRememberedPassword(const String& ssid) {
  for (uint8_t i = 0; i < rememberedWifiCount; ++i) {
    if (rememberedSsids[i] == ssid) {
      return rememberedPasswords[i];
    }
  }
  return "";
}

void setConnectOnBoot(bool enabled) {
  connectOnBoot = enabled;
  preferences.begin(PREF_NAMESPACE, false);
  preferences.putBool(PREF_CONNECT_ONCE, enabled);
  preferences.end();
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
  Serial.printf("[CFG] saved wifiSsidLen=%u wifiPassLen=%u brokerHost=%s brokerPort=%u deviceId=%s\n",
    static_cast<unsigned>(ssid.length()),
    static_cast<unsigned>(password.length()),
    brokerHost.c_str(),
    brokerPort,
    deviceId.c_str());
  rememberWifiCredentials(ssid, password);
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

  ledcAttachChannel(MOTOR_PWM_PIN, MOTOR_PWM_FREQ_HZ, MOTOR_PWM_RES_BITS, MOTOR_PWM_CHANNEL);
  ledcWriteChannel(MOTOR_PWM_CHANNEL, 0);

  steeringServo.setPeriodHertz(SERVO_PWM_FREQ_HZ);
  steeringServo.attach(SERVO_PWM_PIN, 500, 2500);

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

  ledcWriteChannel(MOTOR_PWM_CHANNEL, duty);
}

void writeSteeringOutput(int steering) {
  int servoAngle = map(steering, -100, 100, 180, 35);
  servoAngle = constrain(servoAngle, 0, 180);
  steeringServo.write(servoAngle);
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

void resetBrokerFailureCounters() {
  pingFailCount = 0;
  brokerDisconnectCount = 0;
  brokerWsErrorCount = 0;
  brokerVideoFailCount = 0;
  brokerAudioFailCount = 0;
  brokerDisconnectLatched = false;
}

void scheduleBrokerRecovery(const char* reason) {
  if (restartScheduledAt != 0) {
    return;
  }

  Serial.printf("[SYS] broker recovery: %s\n", reason);
  safeStop();
  resetBrokerFailureCounters();
  setConnectOnBoot(true);
  wsConnected = false;
  wsConnectInFlight = false;
  wifiConnectInFlight = false;
  nextReconnectAt = 0;
  ws.disconnect();
  WiFi.disconnect(true, true);
  scheduleRestart(reason);
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
        resetBrokerFailureCounters();
        reconnectDelayMs = 1000;
        Serial.println("[WS] connected");
        break;
      case WStype_DISCONNECTED:
        Serial.println("[WS] disconnected");
        safeStop();
        if (!brokerDisconnectLatched) {
          brokerDisconnectLatched = true;
          brokerDisconnectCount++;
          if (brokerDisconnectCount >= BROKER_RECOVERY_DISCONNECTS) {
            scheduleBrokerRecovery("ws_disconnected_repeated");
            break;
          }
        }
        scheduleReconnect();
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
        } else if (strcmp(messageType, "talk_audio") == 0) {
          if (!speakerReady) {
            return;
          }
          const char* payloadBase64 = doc["payload"] | "";
          Serial.printf("[SPK] talk_audio seq=%lu samples=%u b64=%u\n",
            static_cast<unsigned long>(doc["seq"] | 0),
            static_cast<unsigned>(doc["samples"] | 0),
            static_cast<unsigned>(strlen(payloadBase64)));
          size_t decodedLen = decodeBase64Payload(payloadBase64, audioDecodeBuffer, sizeof(audioDecodeBuffer));
          if (decodedLen == 0) {
            Serial.println("[SPK] base64 decode failed");
            return;
          }
          size_t sampleCount = static_cast<size_t>(doc["samples"] | static_cast<int>(AUDIO_STREAM_SAMPLES));
          sampleCount = min(sampleCount, static_cast<size_t>(AUDIO_STREAM_SAMPLES));
          size_t pcmSamples = decodeAdpcmBlock(audioDecodeBuffer, decodedLen, audioPlaybackBuffer, sampleCount);
          if (pcmSamples == 0) {
            Serial.println("[SPK] adpcm decode failed");
            return;
          }
          playSpeakerSamples(audioPlaybackBuffer, pcmSamples);
          talkAudioSequence++;
        } else if (strcmp(messageType, "camera_quality") == 0) {
          applyCameraQuality(doc["quality"] | "QVGA");
        } else if (strcmp(messageType, "snapshot") == 0) {
          Serial.println("[SNAP] requested");
        } else if (strcmp(messageType, "pong") == 0) {
          pingFailCount = 0;
          brokerWsErrorCount = 0;
        } else if (strcmp(messageType, "hello") == 0) {
          Serial.println("[WS] broker hello");
        }
        break;
      }
      case WStype_ERROR:
        Serial.println("[WS] error");
        brokerWsErrorCount++;
        if (brokerWsErrorCount >= BROKER_RECOVERY_WS_ERRORS) {
          scheduleBrokerRecovery("ws_error_repeated");
        }
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
    sensor->set_vflip(sensor, 1);
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

bool initSpeaker() {
  speaker.setPins(AMP_BCLK_PIN, AMP_WS_PIN, AMP_DIN_PIN);
  bool ok = speaker.begin(I2S_MODE_STD, AUDIO_PLAYBACK_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  if (!ok) {
    Serial.printf("[SPK] init failed: %d\n", speaker.lastError());
    return false;
  }

  Serial.printf("[SPK] ready bclk=%d ws=%d din=%d\n", AMP_BCLK_PIN, AMP_WS_PIN, AMP_DIN_PIN);
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
  cameraServer.on("/api/wifi-scan", HTTP_GET, handleWifiScan);
  cameraServer.on("/api/wifi-connect", HTTP_POST, handleWifiConnect);
  cameraServer.on("/api/talk-audio", HTTP_POST, handleTalkAudio);
  cameraServer.on("/jpg", HTTP_GET, handleJpeg);
  cameraServer.on("/stream", HTTP_GET, handleStream);
  cameraServer.onNotFound([]() {
    Serial.printf("[HTTP] 404 %s\n", cameraServer.uri().c_str());
    cameraServer.send(404, "text/plain", "not found");
  });
  cameraServer.begin();
  serverStarted = true;
  cameraServerStarted = true;
  if (!localAudioWsStarted) {
    localAudioWs.begin();
    localAudioWs.onEvent([](uint8_t clientNum, WStype_t type, uint8_t* payload, size_t length) {
      switch (type) {
        case WStype_CONNECTED: {
          IPAddress ip = localAudioWs.remoteIP(clientNum);
          Serial.printf("[WS-LOCAL] client %u connected from %u.%u.%u.%u\n", clientNum, ip[0], ip[1], ip[2], ip[3]);
          break;
        }
        case WStype_DISCONNECTED:
          Serial.printf("[WS-LOCAL] client %u disconnected\n", clientNum);
          break;
        case WStype_TEXT:
          if (length == 4 && memcmp(payload, "ping", 4) == 0) {
            localAudioWs.sendTXT(clientNum, "pong");
          }
          break;
        default:
          break;
      }
    });
    localAudioWsStarted = true;
    Serial.println("[WS-LOCAL] listening on :81");
  }
  Serial.println("[HTTP] server started on :80");
}

void handleRoot() {
  // web/index.html 내용을 직접 전송 (PROGMEM 대신 LittleFS 미사용 환경 대응)
  // AP/STA 모드 공통 UI: web/index.html의 isApOrLocal 분기로 자동 처리됨
  Serial.printf("[HTTP] GET / from %s\n", cameraServer.client().remoteIP().toString().c_str());
  cameraServer.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  cameraServer.send_P(200, "text/html; charset=utf-8", WEB_INDEX_HTML);
}


void handleStatusJson() {
  cameraServer.sendHeader("Access-Control-Allow-Origin", "*");
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
  doc["wsConnected"] = wsConnected;
  String payload;
  serializeJson(doc, payload);
  cameraServer.send(200, "application/json", payload);
}

void handleControlJson() {
  cameraServer.sendHeader("Access-Control-Allow-Origin", "*");
  StaticJsonDocument<512> doc;
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
  if (doc.containsKey("talk")) {
    talkEnabled = doc["talk"] | false;
    Serial.printf("[TALK] %s (AP mode)\n", talkEnabled ? "on" : "off");
  }
  if (doc.containsKey("quality")) {
    applyCameraQuality(doc["quality"] | "QVGA");
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
  const bool ssidChanged = ssid != activeWifiSsid;
  if (password.length() == 0) {
    if (ssidChanged) {
      String rememberedPassword = getRememberedPassword(ssid);
      if (rememberedPassword.length() > 0) {
        password = rememberedPassword;
      }
    }
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
  setConnectOnBoot(true);
  cameraServer.send(200, "text/html", "<!doctype html><html><body><h1>Saved</h1><p>Connecting after reboot...</p></body></html>");
  Serial.println("[CFG] connect + reboot scheduled");
  restartScheduledAt = millis() + AP_AUTO_REBOOT_MS;
}

void startCameraCaptureTask() {
  if (!cameraReady || cameraCaptureTaskStarted) {
    return;
  }
  if (latestFrameMutex == nullptr) {
    latestFrameMutex = xSemaphoreCreateMutex();
    if (latestFrameMutex == nullptr) {
      Serial.println("[CAM] frame mutex init failed");
      return;
    }
  }
  BaseType_t rc = xTaskCreatePinnedToCore(
    cameraCaptureTask, "camera_capture", 6144, nullptr, 1, &cameraCaptureTaskHandle, 0
  );
  if (rc != pdPASS) {
    Serial.println("[CAM] capture task start failed");
    return;
  }
  cameraCaptureTaskStarted = true;
  Serial.println("[CAM] capture task started");
}

void cameraCaptureTask(void* arg) {
  (void)arg;
  for (;;) {
    if (!cameraReady) {
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }

    camera_fb_t* fb = esp_camera_fb_get();
    if (fb == nullptr) {
      Serial.println("[CAM] capture task frame unavailable");
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    auto nextFrame = std::make_unique<uint8_t[]>(fb->len);
    if (nextFrame) {
      memcpy(nextFrame.get(), fb->buf, fb->len);
      if (latestFrameMutex != nullptr && xSemaphoreTake(latestFrameMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        delete[] latestJpegFrame;
        latestJpegFrame = nextFrame.release();
        latestJpegFrameLen = fb->len;
        latestFrameSequence++;
        xSemaphoreGive(latestFrameMutex);
      }
    }
    esp_camera_fb_return(fb);
    vTaskDelay(pdMS_TO_TICKS(CAMERA_CAPTURE_INTERVAL_MS));
  }
}

bool copyLatestJpegFrame(std::unique_ptr<uint8_t[]>& frameCopy, size_t& frameLen) {
  frameLen = 0;
  if (latestFrameMutex == nullptr) {
    return false;
  }
  if (xSemaphoreTake(latestFrameMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
    return false;
  }
  if (latestJpegFrame == nullptr || latestJpegFrameLen == 0) {
    xSemaphoreGive(latestFrameMutex);
    return false;
  }

  frameLen = latestJpegFrameLen;
  frameCopy = std::make_unique<uint8_t[]>(frameLen);
  if (frameCopy) {
    memcpy(frameCopy.get(), latestJpegFrame, frameLen);
  } else {
    frameLen = 0;
  }
  xSemaphoreGive(latestFrameMutex);
  return frameLen > 0;
}

void handleJpeg() {
  std::unique_ptr<uint8_t[]> frameCopy;
  size_t frameLen = 0;
  if (!copyLatestJpegFrame(frameCopy, frameLen)) {
    Serial.println("[CAM] /jpg cached frame unavailable");
    cameraServer.send(503, "text/plain", "camera frame unavailable");
    return;
  }

  Serial.printf("[CAM] /jpg %u bytes\n", static_cast<unsigned>(frameLen));
  WiFiClient client = cameraServer.client();
  client.print("HTTP/1.1 200 OK\r\n");
  client.print("Content-Type: image/jpeg\r\n");
  client.print("Cache-Control: no-cache, no-store, must-revalidate\r\n");
  client.printf("Content-Length: %u\r\n", static_cast<unsigned>(frameLen));
  client.print("Connection: close\r\n\r\n");
  client.write(frameCopy.get(), frameLen);
}

void handleStream() {
  WiFiClient client = cameraServer.client();
  client.setTimeout(5);

  cameraServer.sendContent("HTTP/1.1 200 OK\r\n");
  cameraServer.sendContent("Content-Type: multipart/x-mixed-replace; boundary=frame\r\n");
  cameraServer.sendContent("Cache-Control: no-cache\r\n");
  cameraServer.sendContent("Connection: close\r\n\r\n");

  while (client.connected()) {
    std::unique_ptr<uint8_t[]> frameCopy;
    size_t frameLen = 0;
    if (!copyLatestJpegFrame(frameCopy, frameLen)) {
      delay(40);
      continue;
    }

    client.printf("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", static_cast<unsigned>(frameLen));
    client.write(frameCopy.get(), frameLen);
    client.print("\r\n");

    if (!client.connected()) {
      break;
    }

    delay(30);
  }
}
void handleTalkAudio() {
  cameraServer.sendHeader("Access-Control-Allow-Origin", "*");
  if (!speakerReady) {
    cameraServer.send(503, "application/json", "{\"ok\":false,\"reason\":\"speaker_not_ready\"}");
    return;
  }

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, cameraServer.arg("plain"));
  if (err) {
    cameraServer.send(400, "application/json", "{\"ok\":false}");
    return;
  }

  const char* payloadBase64 = doc["payload"] | "";
  size_t decodedLen = decodeBase64Payload(payloadBase64, audioDecodeBuffer, sizeof(audioDecodeBuffer));
  if (decodedLen == 0) {
    cameraServer.send(400, "application/json", "{\"ok\":false,\"reason\":\"decode_failed\"}");
    return;
  }
  size_t sampleCount = static_cast<size_t>(doc["samples"] | static_cast<int>(AUDIO_STREAM_SAMPLES));
  sampleCount = min(sampleCount, static_cast<size_t>(AUDIO_STREAM_SAMPLES));
  size_t pcmSamples = decodeAdpcmBlock(audioDecodeBuffer, decodedLen, audioPlaybackBuffer, sampleCount);
  if (pcmSamples > 0) {
    playSpeakerSamples(audioPlaybackBuffer, pcmSamples);
  }
  cameraServer.send(200, "application/json", "{\"ok\":true}");
}

void handleWifiScan() {
  cameraServer.sendHeader("Access-Control-Allow-Origin", "*");
  int scanResult = WiFi.scanComplete();

  if (scanResult == WIFI_SCAN_FAILED) {
    WiFi.scanNetworks(true, false, false, 300);
    cameraServer.send(202, "application/json", "{\"scanning\":true}");
    Serial.println("[SCAN] started async scan");
    return;
  }

  if (scanResult == WIFI_SCAN_RUNNING) {
    cameraServer.send(202, "application/json", "{\"scanning\":true}");
    return;
  }

  DynamicJsonDocument doc(2048);
  doc["scanning"] = false;
  JsonArray networks = doc.createNestedArray("networks");

  if (scanResult > 0) {
    for (int i = 0; i < scanResult && i < 20; i++) {
      JsonObject net = networks.createNestedObject();
      net["ssid"]   = WiFi.SSID(i);
      net["rssi"]   = WiFi.RSSI(i);
      net["secure"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }
  }
  WiFi.scanDelete();

  JsonObject saved = doc.createNestedObject("saved");
  for (uint8_t i = 0; i < rememberedWifiCount; i++) {
    saved[rememberedSsids[i]] = rememberedPasswords[i];
  }

  String payload;
  serializeJson(doc, payload);
  cameraServer.send(200, "application/json", payload);
  Serial.printf("[SCAN] returned %d networks\n", scanResult);
}

void handleWifiConnect() {
  cameraServer.sendHeader("Access-Control-Allow-Origin", "*");

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, cameraServer.arg("plain"));
  if (err) {
    cameraServer.send(400, "application/json", "{\"ok\":false,\"reason\":\"invalid_json\"}");
    return;
  }

  String ssid     = String(doc["ssid"] | "");
  String password = String(doc["password"] | "");
  bool secure = doc["secure"].isNull() ? true : static_cast<bool>(doc["secure"]);
  ssid.trim();
  password.trim();

  if (ssid.length() == 0) {
    cameraServer.send(400, "application/json", "{\"ok\":false,\"reason\":\"ssid_required\"}");
    return;
  }

  // Open network should preserve an explicitly empty password.
  if (secure && password.length() == 0) {
    String saved = getRememberedPassword(ssid);
    if (saved.length() > 0) {
      password = saved;
    }
  }

  // broker 정보는 기존 것 유지
  saveConfig(ssid, password, activeBrokerHost, activeBrokerPort, activeDeviceId);
  setConnectOnBoot(true);

  cameraServer.send(200, "application/json", "{\"ok\":true}");
  Serial.printf("[CFG] wifi-connect ssid=%s -> reboot\n", ssid.c_str());
  restartScheduledAt = millis() + AP_AUTO_REBOOT_MS;
}


void uploadVideoFrameIfNeeded() {
  if (!cameraReady || !wsConnected) {
    return;
  }

  unsigned long current = millis();
  if (current - lastVideoUploadAt < VIDEO_UPLOAD_INTERVAL_MS) {
    return;
  }

  std::unique_ptr<uint8_t[]> frameCopy;
  size_t frameLen = 0;
  if (!copyLatestJpegFrame(frameCopy, frameLen)) {
    Serial.println("[CAM] upload cached frame unavailable");
    return;
  }

  bool sent = ws.sendBIN(frameCopy.get(), frameLen);

  if (sent) {
    brokerVideoFailCount = 0;
    lastVideoUploadAt = current;
  } else {
    Serial.println("[CAM] upload frame failed");
    brokerVideoFailCount++;
    if (brokerVideoFailCount >= BROKER_RECOVERY_VIDEO_FAILS) {
      scheduleBrokerRecovery("video_upload_failed");
    }
  }
}

int16_t filterAudioSample(int16_t sample) {
  int32_t input = sample;
  audioHighpassState =
    (AUDIO_HPF_ALPHA_NUM * (audioHighpassState + input - audioHighpassLastInput)) / AUDIO_HPF_ALPHA_DEN;
  audioHighpassLastInput = input;

  int32_t filtered = audioHighpassState;
  if (abs(filtered) < AUDIO_NOISE_GATE) {
    filtered = 0;
  }

  filtered *= AUDIO_SOFTWARE_GAIN;
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

size_t decodeBase64Payload(const char* encoded, uint8_t* output, size_t outputSize) {
  if (encoded == nullptr || output == nullptr || outputSize == 0) {
    return 0;
  }

  size_t decodedLen = 0;
  int rc = mbedtls_base64_decode(output, outputSize, &decodedLen,
    reinterpret_cast<const unsigned char*>(encoded), strlen(encoded));
  if (rc != 0) {
    return 0;
  }
  return decodedLen;
}

size_t decodeAdpcmBlock(const uint8_t* input, size_t inputLen, int16_t* output, size_t maxSamples) {
  if (input == nullptr || output == nullptr || inputLen < AUDIO_ADPCM_HEADER_BYTES || maxSamples == 0) {
    return 0;
  }

  int16_t predictor = static_cast<int16_t>(input[0] | (static_cast<uint16_t>(input[1]) << 8));
  int8_t stepIndex = static_cast<int8_t>(constrain(static_cast<int>(input[2]), 0, 88));
  size_t outIndex = 0;
  output[outIndex++] = predictor;

  for (size_t i = AUDIO_ADPCM_HEADER_BYTES; i < inputLen && outIndex < maxSamples; ++i) {
    uint8_t packed = input[i];
    for (uint8_t nibbleIndex = 0; nibbleIndex < 2 && outIndex < maxSamples; ++nibbleIndex) {
      uint8_t nibble = (nibbleIndex == 0) ? (packed & 0x0F) : ((packed >> 4) & 0x0F);
      int step = IMA_STEP_TABLE[stepIndex];
      int diff = step >> 3;
      if (nibble & 1) diff += step >> 2;
      if (nibble & 2) diff += step >> 1;
      if (nibble & 4) diff += step;

      predictor += (nibble & 8) ? -diff : diff;
      predictor = constrain(predictor, static_cast<int16_t>(-32768), static_cast<int16_t>(32767));
      stepIndex = static_cast<int8_t>(constrain(stepIndex + IMA_INDEX_TABLE[nibble], 0, 88));
      output[outIndex++] = predictor;
    }
  }

  return outIndex;
}

void playSpeakerSamples(const int16_t* samples, size_t sampleCount) {
  if (!speakerReady || samples == nullptr || sampleCount == 0) {
    return;
  }

  for (size_t i = 0; i < sampleCount; ++i) {
    int16_t sample = samples[i];
    size_t offset = i * 4;
    speakerFrameBuffer[offset + 0] = static_cast<uint8_t>(sample & 0xff);
    speakerFrameBuffer[offset + 1] = static_cast<uint8_t>((sample >> 8) & 0xff);
    speakerFrameBuffer[offset + 2] = static_cast<uint8_t>(sample & 0xff);
    speakerFrameBuffer[offset + 3] = static_cast<uint8_t>((sample >> 8) & 0xff);
  }

  speaker.write(speakerFrameBuffer, sampleCount * 4);
}

void uploadAudioChunkIfNeeded() {
  const bool hasLocalAudioClients = localAudioWsStarted && localAudioWs.connectedClients() > 0;
  const bool shouldSendBrokerAudio = wsConnected;
  if (!microphoneReady || talkEnabled || (!shouldSendBrokerAudio && !hasLocalAudioClients)) {
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
  bool sent = false;
  if (shouldSendBrokerAudio) {
    sent = ws.sendTXT(payload);
    if (!sent) {
      Serial.println("[MIC] broker upload failed");
      brokerAudioFailCount++;
      if (brokerAudioFailCount >= BROKER_RECOVERY_AUDIO_FAILS) {
        scheduleBrokerRecovery("audio_upload_failed");
      }
    } else {
      brokerAudioFailCount = 0;
    }
  }
  if (hasLocalAudioClients) {
    localAudioWs.broadcastTXT(payload);
    sent = true;
  }
  if (!sent) {
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
      if (wifiFailureCount >= WIFI_FAILURES_BEFORE_AP) {
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
  if (activeWifiPassword.length() == 0) {
    WiFi.begin(activeWifiSsid.c_str());
  } else {
    WiFi.begin(activeWifiSsid.c_str(), activeWifiPassword.c_str());
  }
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
      safeStop();
      ws.disconnect();
      scheduleReconnect();
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
    if (pingFailCount >= BROKER_RECOVERY_PING_FAILS) {
      scheduleBrokerRecovery("ping_send_failed");
    } else if (pingFailCount >= MAX_PING_FAILS) {
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
  startCameraCaptureTask();
  initActuators();
  microphoneReady = initMicrophone();
  speakerReady = initSpeaker();
  playSpeakerBootTone();
  configureWebSocket();
  loadWifiCredentials();
  loadRememberedWifi();
  if (connectOnBoot && hasStoredWifi) {
    setConnectOnBoot(false);
    ensureWifiConnected();
  } else {
    startProvisioningAp();
    startHttpServer();
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
  if (localAudioWsStarted) {
    localAudioWs.loop();
  }
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
