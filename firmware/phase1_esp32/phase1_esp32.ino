// ============================================================
//  RC Car Firmware  –  XIAO ESP32-S3 Sense
//  단순·안정 버전 (복잡한 복구 상태머신 제거)
// ============================================================
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

// ============================================================
//  핀
// ============================================================
namespace {

constexpr int PIN_CAM_PWDN=-1, PIN_CAM_RESET=-1;
constexpr int PIN_CAM_XCLK=10, PIN_CAM_SIOD=40, PIN_CAM_SIOC=39;
constexpr int PIN_CAM_D7=48,  PIN_CAM_D6=11, PIN_CAM_D5=12, PIN_CAM_D4=14;
constexpr int PIN_CAM_D3=16,  PIN_CAM_D2=18, PIN_CAM_D1=17, PIN_CAM_D0=15;
constexpr int PIN_CAM_VSYNC=38, PIN_CAM_HREF=47, PIN_CAM_PCLK=13;
constexpr int PIN_MIC_CLK=42, PIN_MIC_DATA=41;
constexpr int PIN_AMP_BCLK=7, PIN_AMP_WS=8, PIN_AMP_DIN=4;
constexpr int PIN_MOTOR_PWM=2, PIN_SERVO=3;
constexpr int PIN_MOTOR_A=5,   PIN_MOTOR_B=6;
constexpr int PIN_LED=43,      PIN_BAT=1;
constexpr uint8_t  MOTOR_CH   = 2;
constexpr uint32_t MOTOR_FREQ = 200;
constexpr uint8_t  MOTOR_BITS = 12;

// ============================================================
//  타이밍 상수
// ============================================================
constexpr unsigned long WIFI_RETRY_MS    = 5000;
constexpr unsigned long WIFI_TIMEOUT_MS  = 15000;
constexpr unsigned long WS_TIMEOUT_MS    = 10000;
constexpr unsigned long WS_RETRY_MIN     = 2000;
constexpr unsigned long WS_RETRY_MAX     = 30000;
constexpr unsigned long AP_DELAY_MS      = 600;
constexpr unsigned long STATUS_MS        = 2000;
constexpr unsigned long PING_MS          = 10000;
constexpr unsigned long CMD_TIMEOUT_MS   = 1000;
constexpr unsigned long VIDEO_MS         = 220;   // ~4.5fps
constexpr unsigned long AUDIO_MS         = 80;
constexpr unsigned long AUDIO_DELAY_MS   = 5000;  // 클라이언트 접속 후 오디오 시작 대기
constexpr unsigned long VIDEO_GRACE_MS   = 3000;  // WS 연결 직후 그레이스

// ============================================================
//  오디오 상수
// ============================================================
constexpr uint32_t SRATE        = 16000;
constexpr size_t   CHUNK        = 640;
constexpr size_t   CHUNK_BYTES  = CHUNK * 2;
constexpr size_t   ADPCM_HDR    = 4;
constexpr size_t   ADPCM_BYTES  = ADPCM_HDR + (CHUNK / 2);
constexpr size_t   B64_BYTES    = 433;
constexpr int32_t  HPF_N        = 995, HPF_D = 1000;
constexpr int32_t  NGATE        = 32, GAIN = 10;
constexpr int      DRAIN_LIMIT  = 200;  // 버퍼 비우기 최대 블록

// NVS 키
constexpr char NVS_NS[]   = "rc-car";
constexpr char NVS_SSID[] = "wifi_ssid", NVS_PASS[] = "wifi_pass";
constexpr char NVS_HOST[] = "broker_host", NVS_PORT[] = "broker_port";
constexpr char NVS_ID[]   = "device_id",  NVS_MEM[]  = "wifi_memory";
constexpr char AP_SSID[]  = "RC-Car-Setup", AP_PASS[] = "12345678";
constexpr char DEF_ID[]   = "rc-car-01";
constexpr uint8_t WIFI_MEM_MAX = 5;
constexpr uint8_t WIFI_FAIL_BEFORE_AP = 3;

// IMA-ADPCM 테이블
constexpr int8_t IMA_IDX[16] = {-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};
constexpr int16_t IMA_STP[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,
    50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,
    253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,
    1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,
    3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,
    10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,
    27086,29794,32767
};

// ============================================================
//  전역 객체
// ============================================================
WebSocketsClient  brokerWs;
WebServer         httpSrv(80);
WebSocketsServer  localWs(81);
I2SClass          mic;
I2SClass          spk;
Preferences       nvs;
Servo             steerServo;
SemaphoreHandle_t frameLock = nullptr;
SemaphoreHandle_t wsLock    = nullptr;

// ============================================================
//  상태 (최소화)
// ============================================================
bool apMode      = false;
bool wsReady     = false;
bool wsConfigured= false;
bool camReady    = false, micReady = false, spkReady = false;
bool httpReady   = false, lwsReady = false;

bool driveMode   = true;
bool ledOn       = false;
bool talkOn      = false;
int  curThr = 0, curStr = 0;

// WiFi 연결
bool     wifiFlying  = false;
unsigned long wifiAt = 0, wifiStartAt = 0;
uint8_t  wifiFails   = 0;
bool     hasWifi     = false;

// WS 연결
bool     wsFlying    = false;
unsigned long wsAt   = 0;
unsigned long wsConnectedAt = 0;  // 0 = 미연결
unsigned long wsNextAt      = 0;
unsigned long wsRetryMs     = WS_RETRY_MIN;

// 브로커 클라이언트 유무
bool     clientOnline = false;
uint16_t clientCount  = 0;

// 타임스탬프
unsigned long vidAt  = 0, audAt  = 0;
unsigned long pingAt = 0, statAt = 0, cmdAt = 0;
unsigned long transAt = 0;  // AP→브로커 전환 예약

// WiFi 설정
String cfgSsid, cfgPass, cfgHost, cfgId;
uint16_t cfgPort = BROKER_PORT;
String memSsid[WIFI_MEM_MAX], memPass[WIFI_MEM_MAX];
uint8_t memCnt = 0;

// JPEG 프레임
uint8_t* latestJpeg    = nullptr;
size_t   latestJpegLen = 0;

// 오디오 버퍼 (전역 → 스택 절약)
int16_t capBuf[CHUNK]       = {};
uint8_t adpcmBuf[ADPCM_BYTES]= {};
char    b64Buf[B64_BYTES]    = {};
uint8_t decBuf[ADPCM_BYTES]  = {};
int16_t playBuf[CHUNK]       = {};
uint8_t spkBuf[CHUNK * 4]    = {};

// ADPCM / HPF 상태
int32_t hpfState = 0, hpfPrev = 0;
int8_t  adpcmStep = 0;
uint32_t audSeq   = 0;

// ============================================================
//  뮤텍스 / WS 송신 헬퍼
// ============================================================
bool lockWs(TickType_t t = pdMS_TO_TICKS(100)) {
  return wsLock && xSemaphoreTakeRecursive(wsLock, t) == pdTRUE;
}
void unlockWs() { if (wsLock) xSemaphoreGiveRecursive(wsLock); }

bool wsSendText(String& p) {
  if (!wsReady || !lockWs()) return false;
  bool ok = brokerWs.sendTXT(p);
  unlockWs();
  return ok;
}
bool wsSendBinary(const uint8_t* d, size_t n) {
  if (!wsReady || !lockWs()) return false;
  bool ok = brokerWs.sendBIN(d, n);
  unlockWs();
  return ok;
}
void wsDisconnect() { if (lockWs()) { brokerWs.disconnect(); unlockWs(); } }
void wsPoll()       { if (lockWs(pdMS_TO_TICKS(20))) { brokerWs.loop(); unlockWs(); } }

// ============================================================
//  모터 / 서보
// ============================================================
void motorWrite(int v) {
  v = map(constrain(v,-100,100),-100,100,-90,90);
  int pwm = map(v,-90,90,-2000,2000);
  uint32_t maxD = (1u<<MOTOR_BITS)-1u, duty=0;
  if      (pwm >  600) { digitalWrite(PIN_MOTOR_A,HIGH); digitalWrite(PIN_MOTOR_B,LOW);  duty=map(pwm,0,2000,0,(int)maxD); }
  else if (pwm < -600) { digitalWrite(PIN_MOTOR_A,LOW);  digitalWrite(PIN_MOTOR_B,HIGH); duty=map(-pwm,0,2000,0,(int)maxD); }
  else                 { digitalWrite(PIN_MOTOR_A,LOW);  digitalWrite(PIN_MOTOR_B,LOW); }
  ledcWriteChannel(MOTOR_CH, duty);
}
void servoWrite(int v) {
  steerServo.write(constrain(map(v,-100,100,180,35),0,180));
}
void safeStop() {
  curThr=0; curStr=0;
  digitalWrite(PIN_MOTOR_A,LOW); digitalWrite(PIN_MOTOR_B,LOW);
  ledcWriteChannel(MOTOR_CH,0);
  steerServo.write(90);
}
void initActuators() {
  pinMode(PIN_MOTOR_A,OUTPUT); pinMode(PIN_MOTOR_B,OUTPUT);
  digitalWrite(PIN_MOTOR_A,LOW); digitalWrite(PIN_MOTOR_B,LOW);
  ledcAttachChannel(PIN_MOTOR_PWM, MOTOR_FREQ, MOTOR_BITS, MOTOR_CH);
  ledcWriteChannel(MOTOR_CH,0);
  steerServo.setPeriodHertz(50);
  steerServo.attach(PIN_SERVO,500,2500);
  if (PIN_LED>=0){ pinMode(PIN_LED,OUTPUT); digitalWrite(PIN_LED,LOW); }
  analogReadResolution(12);
}

// ============================================================
//  카메라 초기화
// ============================================================
bool initCamera() {
  camera_config_t c={};
  c.ledc_channel=LEDC_CHANNEL_0; c.ledc_timer=LEDC_TIMER_0;
  c.pin_d0=PIN_CAM_D0; c.pin_d1=PIN_CAM_D1; c.pin_d2=PIN_CAM_D2; c.pin_d3=PIN_CAM_D3;
  c.pin_d4=PIN_CAM_D4; c.pin_d5=PIN_CAM_D5; c.pin_d6=PIN_CAM_D6; c.pin_d7=PIN_CAM_D7;
  c.pin_xclk=PIN_CAM_XCLK; c.pin_pclk=PIN_CAM_PCLK;
  c.pin_vsync=PIN_CAM_VSYNC; c.pin_href=PIN_CAM_HREF;
  c.pin_sccb_sda=PIN_CAM_SIOD; c.pin_sccb_scl=PIN_CAM_SIOC;
  c.pin_pwdn=PIN_CAM_PWDN; c.pin_reset=PIN_CAM_RESET;
  c.xclk_freq_hz=20000000; c.pixel_format=PIXFORMAT_JPEG;
  c.frame_size=FRAMESIZE_QVGA; c.jpeg_quality=14; c.fb_count=2;
  c.grab_mode=CAMERA_GRAB_LATEST;
  c.fb_location=psramFound()?CAMERA_FB_IN_PSRAM:CAMERA_FB_IN_DRAM;
  Serial.printf("[CAM] psram=%s\n",psramFound()?"yes":"no");
  if (esp_camera_init(&c)!=ESP_OK) {
    c.frame_size=FRAMESIZE_QQVGA; c.jpeg_quality=20;
    c.fb_count=1; c.fb_location=CAMERA_FB_IN_DRAM;
    if (esp_camera_init(&c)!=ESP_OK) { Serial.println("[CAM] FAIL"); return false; }
  }
  sensor_t* s=esp_camera_sensor_get();
  if (s){ s->set_framesize(s,FRAMESIZE_QVGA); s->set_quality(s,14);
          s->set_brightness(s,0); s->set_saturation(s,0);
          s->set_hmirror(s,0); s->set_vflip(s,1); }
  Serial.println("[CAM] OK"); return true;
}
void setCamQuality(const char* q) {
  sensor_t* s=esp_camera_sensor_get(); if(!s) return;
  if      (!strcmp(q,"VGA"))  { s->set_framesize(s,FRAMESIZE_VGA);  s->set_quality(s,12); }
  else if (!strcmp(q,"SVGA")) { s->set_framesize(s,FRAMESIZE_SVGA); s->set_quality(s,12); }
  else if (!strcmp(q,"UXGA")) { s->set_framesize(s,FRAMESIZE_UXGA); s->set_quality(s,10); }
  else                        { s->set_framesize(s,FRAMESIZE_QVGA); s->set_quality(s,14); }
  Serial.printf("[CAM] quality=%s\n",q);
}

// 카메라 캡처 태스크 (Core 0)
void taskCamCapture(void*) {
  for(;;) {
    if (!camReady) { vTaskDelay(pdMS_TO_TICKS(250)); continue; }
    camera_fb_t* fb=esp_camera_fb_get();
    if (!fb)       { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
    auto buf=std::make_unique<uint8_t[]>(fb->len);
    if (buf && frameLock && xSemaphoreTake(frameLock,pdMS_TO_TICKS(20))==pdTRUE) {
      memcpy(buf.get(),fb->buf,fb->len);
      delete[] latestJpeg;
      latestJpeg=buf.release(); latestJpegLen=fb->len;
      xSemaphoreGive(frameLock);
    }
    esp_camera_fb_return(fb);
    vTaskDelay(pdMS_TO_TICKS(80));
  }
}

bool copyFrame(std::unique_ptr<uint8_t[]>& out, size_t& len) {
  len=0;
  if (!frameLock||xSemaphoreTake(frameLock,pdMS_TO_TICKS(20))!=pdTRUE) return false;
  if (!latestJpeg||!latestJpegLen) { xSemaphoreGive(frameLock); return false; }
  len=latestJpegLen;
  out=std::make_unique<uint8_t[]>(len);
  if (out) memcpy(out.get(),latestJpeg,len); else len=0;
  xSemaphoreGive(frameLock);
  return len>0;
}

// 비디오 업로드 태스크 (Core 1)
void taskVidUpload(void*) {
  for(;;) {
    // 조건 미충족 → 슬립
    if (!camReady||!wsReady||apMode||WiFi.status()!=WL_CONNECTED) {
      vTaskDelay(pdMS_TO_TICKS(20)); continue;
    }
    unsigned long now=millis();
    if (now-vidAt<VIDEO_MS)                                { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
    if (wsConnectedAt==0||(now-wsConnectedAt)<VIDEO_GRACE_MS){ vTaskDelay(pdMS_TO_TICKS(10)); continue; }

    std::unique_ptr<uint8_t[]> f; size_t n=0;
    if (copyFrame(f,n) && wsSendBinary(f.get(),n)) vidAt=now;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ============================================================
//  마이크 / 스피커
// ============================================================
bool initMic() {
  mic.setPinsPdmRx(PIN_MIC_CLK,PIN_MIC_DATA);
  bool ok=mic.begin(I2S_MODE_PDM_RX,SRATE,I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_MONO);
  if (!ok){ Serial.printf("[MIC] FAIL %d\n",mic.lastError()); return false; }
  Serial.println("[MIC] OK"); return true;
}
bool initSpk() {
  spk.setPins(PIN_AMP_BCLK,PIN_AMP_WS,PIN_AMP_DIN);
  bool ok=spk.begin(I2S_MODE_STD,SRATE,I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO);
  if (!ok){ Serial.printf("[SPK] FAIL %d\n",spk.lastError()); return false; }
  Serial.println("[SPK] OK"); return true;
}

// AP모드 대기 중 쌓인 마이크 버퍼 제거
void drainMic() {
  if (!micReady) return;
  int n=0;
  while (n<DRAIN_LIMIT && mic.available()>=(int)CHUNK_BYTES)
    { mic.readBytes((char*)capBuf,CHUNK_BYTES); n++; }
  if (n) Serial.printf("[MIC] drained=%d\n",n);
}

// HPF + 노이즈게이트 + 소프트게인
int16_t filterSample(int16_t raw) {
  int32_t in=raw;
  hpfState=(HPF_N*(hpfState+in-hpfPrev))/HPF_D; hpfPrev=in;
  int32_t v=hpfState;
  if (abs(v)<NGATE) v=0;
  return (int16_t)constrain(v*GAIN,-32768,32767);
}

// IMA-ADPCM 인코더
uint8_t adpcmNibble(int16_t s, int16_t& p, int8_t& si) {
  int step=IMA_STP[si], diff=s-p;
  uint8_t n=(diff<0)?(diff=-diff,8):0;
  int delta=step>>3;
  if (diff>=step)       { n|=4; diff-=step;     delta+=step;    }
  if (diff>=(step>>1))  { n|=2; diff-=step>>1;  delta+=step>>1; }
  if (diff>=(step>>2))  { n|=1; delta+=step>>2; }
  p=constrain(p+((n&8)?-delta:delta),-32768,32767);
  si=constrain(si+IMA_IDX[n&0xF],0,88);
  return n&0xF;
}
size_t encodeAdpcm(const int16_t* in, size_t n, uint8_t* out) {
  if (!n) return 0;
  int16_t p=in[0]; int8_t si=adpcmStep;
  out[0]=p&0xFF; out[1]=(p>>8)&0xFF; out[2]=(uint8_t)si; out[3]=0;
  size_t i=ADPCM_HDR;
  for (size_t k=1;k<n;k+=2) {
    uint8_t lo=adpcmNibble(in[k],p,si);
    uint8_t hi=(k+1<n)?adpcmNibble(in[k+1],p,si):0;
    out[i++]=lo|(hi<<4);
  }
  adpcmStep=si; return i;
}

// IMA-ADPCM 디코더
size_t decodeAdpcm(const uint8_t* in, size_t inN, int16_t* out, size_t maxN) {
  if (!in||!out||inN<ADPCM_HDR||!maxN) return 0;
  int16_t p=(int16_t)(in[0]|((uint16_t)in[1]<<8));
  int8_t  si=(int8_t)constrain((int)in[2],0,88);
  size_t oi=0; out[oi++]=p;
  for (size_t i=ADPCM_HDR;i<inN&&oi<maxN;i++) {
    for (int nb=0;nb<2&&oi<maxN;nb++) {
      uint8_t n=(nb==0)?(in[i]&0xF):((in[i]>>4)&0xF);
      int step=IMA_STP[si], diff=step>>3;
      if (n&1) diff+=step>>2; if (n&2) diff+=step>>1; if (n&4) diff+=step;
      p=constrain(p+((n&8)?-diff:diff),-32768,32767);
      si=constrain(si+IMA_IDX[n],0,88);
      out[oi++]=p;
    }
  }
  return oi;
}

size_t b64Decode(const char* enc, uint8_t* out, size_t sz) {
  if (!enc||!out||!sz) return 0;
  size_t dl=0;
  return mbedtls_base64_decode(out,sz,&dl,(const unsigned char*)enc,strlen(enc))==0?dl:0;
}

void playSamples(const int16_t* s, size_t n) {
  if (!spkReady||!s||!n) return;
  for (size_t i=0;i<n;i++){
    spkBuf[i*4]=spkBuf[i*4+2]=s[i]&0xFF;
    spkBuf[i*4+1]=spkBuf[i*4+3]=(s[i]>>8)&0xFF;
  }
  spk.write(spkBuf,n*4);
}

// 부팅/전환 효과음
void playTone(uint32_t hz, int16_t amp, size_t cnt) {
  uint32_t half=SRATE/(hz*2); if (!half) half=1;
  int16_t v=amp;
  for (size_t i=0;i<cnt;i++) {
    if (i%half==0) v=-v;
    uint8_t f[4]={(uint8_t)(v&0xFF),(uint8_t)((v>>8)&0xFF),(uint8_t)(v&0xFF),(uint8_t)((v>>8)&0xFF)};
    spk.write(f,4);
  }
}
void playSilence(size_t cnt){ uint8_t f[4]={0}; for(size_t i=0;i<cnt;i++) spk.write(f,4); }
void bootTone()       { if (spkReady) playTone(880,2800,SRATE/12); }
void transitionTone() {
  if (!spkReady) return;
  playTone(880,2600,SRATE/20);  playSilence(SRATE/80);
  playTone(1320,2600,SRATE/20);
}

// ============================================================
//  배터리
// ============================================================
uint32_t batMv()  { return analogReadMilliVolts(PIN_BAT)*2U; }
uint8_t  batPct(uint32_t mv) {
  return (uint8_t)constrain(map((long)constrain(mv,6400UL,8400UL),6400L,8400L,0L,100L),0L,100L);
}

// ============================================================
//  NVS 설정
// ============================================================
bool loadConfig() {
  nvs.begin(NVS_NS,true);
  cfgSsid=nvs.getString(NVS_SSID,""); cfgPass=nvs.getString(NVS_PASS,"");
  cfgHost=nvs.getString(NVS_HOST,""); cfgPort=nvs.getUShort(NVS_PORT,0);
  cfgId  =nvs.getString(NVS_ID,"");
  nvs.end();
  if (cfgSsid.isEmpty()&&strlen(WIFI_SSID)>0){cfgSsid=WIFI_SSID;cfgPass=WIFI_PASSWORD;}
  if (cfgHost.isEmpty()&&strlen(BROKER_HOST)>0) cfgHost=BROKER_HOST;
  if (!cfgPort)  cfgPort=(uint16_t)BROKER_PORT;
  if (cfgId.isEmpty()&&strlen(DEVICE_ID)>0) cfgId=DEVICE_ID;
  if (cfgId.isEmpty()) cfgId=DEF_ID;
  hasWifi=!cfgSsid.isEmpty();
  Serial.printf("[CFG] ssid=%s broker=%s:%u id=%s\n",
    cfgSsid.c_str(),cfgHost.c_str(),cfgPort,cfgId.c_str());
  return hasWifi;
}
void saveConfig(const String& ss,const String& ps,
                const String& bh,uint16_t bp,const String& id) {
  nvs.begin(NVS_NS,false);
  nvs.putString(NVS_SSID,ss); nvs.putString(NVS_PASS,ps);
  nvs.putString(NVS_HOST,bh); nvs.putUShort(NVS_PORT,bp);
  nvs.putString(NVS_ID,id);
  nvs.end();
  cfgSsid=ss;cfgPass=ps;cfgHost=bh;cfgPort=bp;cfgId=id;hasWifi=true;
  Serial.printf("[CFG] saved %s -> %s:%u\n",ss.c_str(),bh.c_str(),bp);
}

void loadMemWifi() {
  memCnt=0; nvs.begin(NVS_NS,true); String j=nvs.getString(NVS_MEM,""); nvs.end();
  if (j.isEmpty()){ if(!cfgSsid.isEmpty()){memSsid[0]=cfgSsid;memPass[0]=cfgPass;memCnt=1;} return; }
  StaticJsonDocument<512> doc; if (deserializeJson(doc,j)) return;
  for (JsonObject o:doc["items"].as<JsonArray>()) {
    if (memCnt>=WIFI_MEM_MAX) break;
    String s=o["ssid"]|""; if(s.isEmpty()) continue;
    memSsid[memCnt]=s; memPass[memCnt]=String(o["password"]|""); memCnt++;
  }
}
void saveMemWifi() {
  StaticJsonDocument<512> doc; JsonArray a=doc.createNestedArray("items");
  for (uint8_t i=0;i<memCnt;i++){JsonObject o=a.createNestedObject();o["ssid"]=memSsid[i];o["password"]=memPass[i];}
  String j; serializeJson(doc,j);
  nvs.begin(NVS_NS,false); nvs.putString(NVS_MEM,j); nvs.end();
}
void rememberWifi(const String& ss,const String& ps) {
  if (ss.isEmpty()) return;
  int idx=-1;
  for (uint8_t i=0;i<memCnt;i++) if (memSsid[i]==ss){idx=i;break;}
  if (idx==0){memPass[0]=ps;saveMemWifi();return;}
  int top=(idx>0)?idx:(memCnt<WIFI_MEM_MAX?(int)memCnt++:WIFI_MEM_MAX-1);
  for (int i=top;i>0;i--){memSsid[i]=memSsid[i-1];memPass[i]=memPass[i-1];}
  memSsid[0]=ss; memPass[0]=ps; saveMemWifi();
}
String recallPass(const String& ss) {
  for (uint8_t i=0;i<memCnt;i++) if(memSsid[i]==ss) return memPass[i]; return "";
}

// ============================================================
//  AP 모드 / 브로커 모드 전환
// ============================================================
void startAP() {
  if (apMode) return;
  safeStop();
  wsDisconnect(); wsReady=false; wsConnectedAt=0; wsFlying=false;
  clientOnline=false; clientCount=0;
  WiFi.disconnect(true,true); delay(100);
  WiFi.mode(WIFI_AP); WiFi.softAP(AP_SSID,AP_PASS);
  apMode=true; wifiFlying=false; drainMic();
  Serial.printf("[AP] up ip=%s\n",WiFi.softAPIP().toString().c_str());
}

void enterBrokerMode() {
  if (!apMode) { transAt=0; return; }
  Serial.println("[CFG] -> broker mode");
  transitionTone(); transAt=0;
  safeStop();
  wsReady=false; wsConnectedAt=0; wsFlying=false; wifiFlying=false;
  clientOnline=false; clientCount=0;
  wsNextAt=0; wifiAt=0; audAt=0; wsRetryMs=WS_RETRY_MIN;
  wsDisconnect(); drainMic();
  WiFi.softAPdisconnect(true); delay(100);
  WiFi.mode(WIFI_STA); apMode=false;
}

// ============================================================
//  WebSocket 이벤트 핸들러
// ============================================================
void onWsEvent(WStype_t type, uint8_t* payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED:
      wsReady=true; wsFlying=false; wsConnectedAt=millis();
      clientOnline=false; clientCount=0; wsRetryMs=WS_RETRY_MIN;
      drainMic();
      Serial.println("[WS] connected");
      break;

    case WStype_DISCONNECTED:
      wsReady=false; wsFlying=false; wsConnectedAt=0;
      clientOnline=false; clientCount=0;
      safeStop();
      wsNextAt=millis()+wsRetryMs;
      wsRetryMs=min(wsRetryMs*2UL, WS_RETRY_MAX);
      Serial.printf("[WS] disconnected, retry %lums\n", wsRetryMs/2);
      break;

    case WStype_TEXT: {
      StaticJsonDocument<256> doc;
      if (deserializeJson(doc,payload,len)) return;
      const char* t=doc["type"]|"";

      if (!strcmp(t,"pong")) { /* OK */ }

      else if (!strcmp(t,"hello")) {
        Serial.println("[WS] hello from broker");
      }
      else if (!strcmp(t,"client_state")) {
        clientCount=(uint16_t)(doc["clients"]|0);
        clientOnline=clientCount>0;
        if (clientOnline) {
          wsConnectedAt=millis();  // 오디오 딜레이 타이머 리셋
          Serial.printf("[WS] clients=%u\n",clientCount);
        } else {
          drainMic();
          Serial.println("[WS] no clients");
        }
      }
      else if (!strcmp(t,"ctrl")) {
        if (driveMode) {
          curThr=constrain((int)(doc["throttle"]|0),-100,100);
          curStr=constrain((int)(doc["steering"]|0),-100,100);
          cmdAt=millis();
          motorWrite(curThr); servoWrite(curStr);
        }
      }
      else if (!strcmp(t,"led")) {
        ledOn=doc["enabled"]|false;
        if (PIN_LED>=0) digitalWrite(PIN_LED,ledOn?HIGH:LOW);
      }
      else if (!strcmp(t,"mode")) {
        driveMode=strcmp(doc["mode"]|"drive","monitor")!=0;
        if (!driveMode) safeStop();
        Serial.printf("[MODE] %s\n",driveMode?"drive":"monitor");
      }
      else if (!strcmp(t,"talk")) {
        talkOn=doc["enabled"]|false;
        Serial.printf("[TALK] %s\n",talkOn?"on":"off");
      }
      else if (!strcmp(t,"talk_audio")) {
        if (!spkReady) return;
        size_t dl=b64Decode(doc["payload"]|"",decBuf,sizeof(decBuf));
        if (!dl) return;
        size_t sc=min((size_t)(doc["samples"]|(int)CHUNK),CHUNK);
        size_t pcm=decodeAdpcm(decBuf,dl,playBuf,sc);
        if (pcm) playSamples(playBuf,pcm);
      }
      else if (!strcmp(t,"camera_quality")) {
        setCamQuality(doc["quality"]|"QVGA");
      }
      break;
    }
    case WStype_ERROR:
      Serial.println("[WS] error");
      break;
    default: break;
  }
}

// ============================================================
//  WiFi / WS 연결 관리 (루프에서 호출)
// ============================================================
void manageWifi() {
  if (apMode) return;
  if (!hasWifi) { startAP(); return; }
  if (WiFi.status()==WL_CONNECTED) { wifiFlying=false; wifiFails=0; return; }
  unsigned long now=millis();
  if (wifiFlying) {
    if (now-wifiStartAt>=WIFI_TIMEOUT_MS) {
      Serial.println("[WIFI] timeout");
      WiFi.disconnect(true,true); wifiFlying=false; wifiAt=now;
      if (++wifiFails>=WIFI_FAIL_BEFORE_AP) { Serial.println("[WIFI] -> AP"); startAP(); }
    }
    return;
  }
  if (now-wifiAt<WIFI_RETRY_MS) return;
  wifiAt=now; wifiStartAt=now; wifiFlying=true;
  Serial.printf("[WIFI] connecting %s\n",cfgSsid.c_str());
  WiFi.mode(WIFI_STA);
  if (cfgPass.isEmpty()) WiFi.begin(cfgSsid.c_str());
  else                   WiFi.begin(cfgSsid.c_str(),cfgPass.c_str());
}

void manageWs() {
  if (apMode||!WiFi.isConnected()) { wsReady=false; wsFlying=false; return; }
  if (wsReady) return;
  if (wsFlying) {
    if (millis()-wsAt>=WS_TIMEOUT_MS) {
      Serial.println("[WS] connect timeout");
      safeStop(); wsDisconnect(); wsFlying=false; wsConnectedAt=0;
      wsNextAt=millis()+wsRetryMs;
      wsRetryMs=min(wsRetryMs*2UL,WS_RETRY_MAX);
    }
    return;
  }
  if (millis()<wsNextAt) return;
  if (!wsConfigured) { brokerWs.onEvent(onWsEvent); brokerWs.setReconnectInterval(0); wsConfigured=true; }
  Serial.printf("[WS] -> %s:%u\n",cfgHost.c_str(),cfgPort);
  if (lockWs()) { brokerWs.begin(cfgHost.c_str(),cfgPort,"/device?deviceId="+cfgId); unlockWs(); }
  wsAt=millis(); wsFlying=true;
}

// ============================================================
//  주기적 송신
// ============================================================
void sendPing() {
  if (!wsReady||millis()-pingAt<PING_MS) return;
  pingAt=millis();
  String p="{\"type\":\"ping\"}"; wsSendText(p);
}

void sendStatus() {
  if (!wsReady||millis()-statAt<STATUS_MS) return;
  statAt=millis();
  StaticJsonDocument<160> doc;
  doc["type"]="status"; doc["rssi"]=WiFi.RSSI(); doc["uptime"]=millis();
  doc["throttle"]=curThr; doc["steering"]=curStr;
  doc["cameraReady"]=camReady; doc["audioReady"]=micReady;
  doc["audioCodec"]="adpcm_ima"; doc["audioSampleRate"]=SRATE;
  doc["mode"]=driveMode?"drive":"monitor";
  doc["ledEnabled"]=ledOn; doc["talkEnabled"]=talkOn;
  uint32_t mv=batMv(); doc["batteryMv"]=mv; doc["batteryPct"]=batPct(mv);
  doc["apMode"]=apMode; doc["localIp"]=WiFi.localIP().toString();
  String p; serializeJson(doc,p); wsSendText(p);
}

void sendAudio() {
  unsigned long now=millis();
  bool hasLocal=lwsReady&&localWs.connectedClients()>0;
  bool doBroker=wsReady&&clientOnline&&wsConnectedAt!=0&&(now-wsConnectedAt)>=AUDIO_DELAY_MS;

  if (!micReady||talkOn||(!doBroker&&!hasLocal)) {
    if (!doBroker&&!hasLocal) drainMic();
    return;
  }
  if (now-audAt<AUDIO_MS) return;
  if (mic.available()<(int)CHUNK_BYTES) return;
  if (mic.readBytes((char*)capBuf,CHUNK_BYTES)!=CHUNK_BYTES) return;

  for (size_t i=0;i<CHUNK;i++) capBuf[i]=filterSample(capBuf[i]);
  size_t aLen=encodeAdpcm(capBuf,CHUNK,adpcmBuf);
  int bLen=base64_encode_chars((const char*)adpcmBuf,aLen,b64Buf);
  if (bLen<=0||bLen>=(int)B64_BYTES) return;
  b64Buf[bLen]='\0';

  StaticJsonDocument<512> doc;
  doc["type"]="audio"; doc["codec"]="adpcm_ima";
  doc["sampleRate"]=SRATE; doc["samples"]=CHUNK;
  doc["seq"]=audSeq++; doc["payload"]=b64Buf;
  String p; serializeJson(doc,p);

  bool sent=false;
  if (doBroker)  sent=wsSendText(p);
  if (hasLocal) { localWs.broadcastTXT(p); sent=true; }
  // 성공/실패 모두 타임스탬프 갱신 → 인터벌 유지
  audAt=now;
}

// ============================================================
//  HTTP 핸들러
// ============================================================
void hRoot() {
  httpSrv.sendHeader("Cache-Control","no-cache,no-store,must-revalidate");
  httpSrv.send_P(200,"text/html; charset=utf-8",WEB_INDEX_HTML);
}

void hStatus() {
  httpSrv.sendHeader("Access-Control-Allow-Origin","*");
  StaticJsonDocument<192> doc;
  doc["apMode"]=apMode;
  doc["ip"]=apMode?WiFi.softAPIP().toString():WiFi.localIP().toString();
  doc["rssi"]=WiFi.isConnected()?WiFi.RSSI():0;
  uint32_t mv=batMv(); doc["batteryMv"]=mv; doc["batteryPct"]=batPct(mv);
  doc["mode"]=driveMode?"drive":"monitor"; doc["ledEnabled"]=ledOn;
  doc["throttle"]=curThr; doc["steering"]=curStr; doc["wsConnected"]=wsReady;
  String p; serializeJson(doc,p); httpSrv.send(200,"application/json",p);
}

void hControl() {
  httpSrv.sendHeader("Access-Control-Allow-Origin","*");
  if (apMode){httpSrv.send(403,"application/json","{\"ok\":false}");return;}
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc,httpSrv.arg("plain"))){httpSrv.send(400,"application/json","{\"ok\":false}");return;}
  if (doc.containsKey("ledEnabled")){ledOn=doc["ledEnabled"]|false;if(PIN_LED>=0)digitalWrite(PIN_LED,ledOn?HIGH:LOW);}
  if ((doc.containsKey("throttle")||doc.containsKey("steering"))&&driveMode){
    curThr=constrain((int)(doc["throttle"]|curThr),-100,100);
    curStr=constrain((int)(doc["steering"]|curStr),-100,100);
    cmdAt=millis(); motorWrite(curThr); servoWrite(curStr);
  }
  if (doc.containsKey("quality")) setCamQuality(doc["quality"]|"QVGA");
  httpSrv.send(200,"application/json","{\"ok\":true}");
}

void hConfigSave() {
  String ss=httpSrv.arg("ssid");       ss.trim();
  String ps=httpSrv.arg("password");   ps.trim();
  String bh=httpSrv.arg("brokerHost"); bh.trim();
  String bp=httpSrv.arg("brokerPort"); bp.trim();
  String id=httpSrv.arg("deviceId");   id.trim();
  if (ss.isEmpty()) ss=cfgSsid;
  if (bh.isEmpty()) bh=cfgHost;
  if (id.isEmpty()) id=cfgId;
  if (ps.isEmpty()&&ss!=cfgSsid){ String r=recallPass(ss); if(!r.isEmpty()) ps=r; }
  uint16_t port=cfgPort;
  if (!bp.isEmpty()){ long v=bp.toInt(); if(v>0&&v<=65535) port=(uint16_t)v; }
  if (ss.isEmpty()||bh.isEmpty()||id.isEmpty()||!port){httpSrv.send(400,"text/plain","invalid");return;}
  saveConfig(ss,ps,bh,port,id); rememberWifi(ss,ps);
  httpSrv.send(200,"text/html","<!doctype html><html><body><h1>Saved</h1><p>Connecting...</p></body></html>");
  transAt=millis()+AP_DELAY_MS;
}

void hJpeg() {
  if (apMode){httpSrv.send(410,"text/plain","ap mode");return;}
  std::unique_ptr<uint8_t[]> f; size_t n=0;
  if (!copyFrame(f,n)){httpSrv.send(503,"text/plain","no frame");return;}
  WiFiClient c=httpSrv.client();
  c.printf("HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nCache-Control: no-cache\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",(unsigned)n);
  c.write(f.get(),n);
}

void hStream() { httpSrv.send(410,"text/plain","use /jpg"); }

void hTalkAudio() {
  httpSrv.sendHeader("Access-Control-Allow-Origin","*");
  if (apMode||!spkReady){httpSrv.send(503,"application/json","{\"ok\":false}");return;}
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc,httpSrv.arg("plain"))){httpSrv.send(400,"application/json","{\"ok\":false}");return;}
  size_t dl=b64Decode(doc["payload"]|"",decBuf,sizeof(decBuf));
  if (!dl){httpSrv.send(400,"application/json","{\"ok\":false}");return;}
  size_t sc=min((size_t)(doc["samples"]|(int)CHUNK),CHUNK);
  size_t pcm=decodeAdpcm(decBuf,dl,playBuf,sc); if(pcm) playSamples(playBuf,pcm);
  httpSrv.send(200,"application/json","{\"ok\":true}");
}

void hWifiScan() {
  httpSrv.sendHeader("Access-Control-Allow-Origin","*");
  int r=WiFi.scanComplete();
  if (r==WIFI_SCAN_FAILED){WiFi.scanNetworks(true,false,false,300);httpSrv.send(202,"application/json","{\"scanning\":true}");return;}
  if (r==WIFI_SCAN_RUNNING){httpSrv.send(202,"application/json","{\"scanning\":true}");return;}
  DynamicJsonDocument doc(2048); doc["scanning"]=false;
  JsonArray arr=doc.createNestedArray("networks");
  for (int i=0;i<r&&i<20;i++){
    JsonObject o=arr.createNestedObject();
    o["ssid"]=WiFi.SSID(i); o["rssi"]=WiFi.RSSI(i);
    o["secure"]=(WiFi.encryptionType(i)!=WIFI_AUTH_OPEN);
  }
  WiFi.scanDelete();
  JsonObject sv=doc.createNestedObject("saved");
  for (uint8_t i=0;i<memCnt;i++) sv[memSsid[i]]=memPass[i];
  String p; serializeJson(doc,p); httpSrv.send(200,"application/json",p);
}

void hWifiConnect() {
  httpSrv.sendHeader("Access-Control-Allow-Origin","*");
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc,httpSrv.arg("plain"))){httpSrv.send(400,"application/json","{\"ok\":false}");return;}
  String ss=String(doc["ssid"]|""); ss.trim();
  String ps=String(doc["password"]|""); ps.trim();
  bool sec=doc["secure"].isNull()?true:(bool)doc["secure"];
  if (ss.isEmpty()){httpSrv.send(400,"application/json","{\"ok\":false,\"reason\":\"ssid_required\"}");return;}
  if (sec&&ps.isEmpty()){String r=recallPass(ss);if(!r.isEmpty())ps=r;}
  saveConfig(ss,ps,cfgHost,cfgPort,cfgId); rememberWifi(ss,ps);
  httpSrv.send(200,"application/json","{\"ok\":true}");
  transAt=millis()+AP_DELAY_MS;
}

void startHTTP() {
  if (httpReady) return;
  httpSrv.on("/",             HTTP_GET,  hRoot);
  httpSrv.on("/api/status",   HTTP_GET,  hStatus);
  httpSrv.on("/api/control",  HTTP_POST, hControl);
  httpSrv.on("/api/config",   HTTP_POST, hConfigSave);
  httpSrv.on("/api/wifi-scan",HTTP_GET,  hWifiScan);
  httpSrv.on("/api/wifi-connect",HTTP_POST,hWifiConnect);
  httpSrv.on("/api/talk-audio",HTTP_POST,hTalkAudio);
  httpSrv.on("/jpg",          HTTP_GET,  hJpeg);
  httpSrv.on("/stream",       HTTP_GET,  hStream);
  httpSrv.onNotFound([]{httpSrv.send(404,"text/plain","not found");});
  httpSrv.begin(); httpReady=true;

  if (!apMode&&!lwsReady) {
    localWs.begin();
    localWs.onEvent([](uint8_t cn,WStype_t t,uint8_t* p,size_t l){
      if (t==WStype_TEXT&&l==4&&!memcmp(p,"ping",4)) localWs.sendTXT(cn,"pong");
    });
    lwsReady=true; Serial.println("[WS-LOCAL] :81");
  }
  Serial.println("[HTTP] :80");
}

} // namespace

// ============================================================
//  setup / loop
// ============================================================
void setup() {
  Serial.begin(115200); delay(500);
  Serial.println("\n[BOOT] RC Car");

  wsLock    = xSemaphoreCreateRecursiveMutex();
  frameLock = xSemaphoreCreateMutex();

  safeStop();
  camReady = initCamera();

  if (camReady) {
    xTaskCreatePinnedToCore(taskCamCapture,"cam",6144,nullptr,1,nullptr,0);
    xTaskCreatePinnedToCore(taskVidUpload, "vid",8192,nullptr,1,nullptr,1);
  }

  initActuators();
  micReady = initMic();
  spkReady = initSpk();
  bootTone();

  loadConfig();
  loadMemWifi();

  startAP();
  startHTTP();
}

void loop() {
  // 브로커 모드 전환 예약 처리
  if (transAt && millis()>=transAt) enterBrokerMode();

  manageWifi();
  if (!httpReady) startHTTP();
  manageWs();
  wsPoll();

  if (lwsReady)   localWs.loop();
  if (httpReady)  httpSrv.handleClient();

  sendPing();
  sendStatus();
  sendAudio();

  // 명령 타임아웃 → 자동 정지
  if (cmdAt>0 && millis()-cmdAt>CMD_TIMEOUT_MS) { safeStop(); cmdAt=0; }
}
