# Preserve List

## Keep As-Is

- `web/index.html`
  - AP/브로커 공용 UI 레이아웃
  - `WS binary -> canvas` 영상 렌더링
  - PTT용 JS ADPCM 인코더/송신 코드
  - `MediaRecorder` 기반 녹화
  - HUD 구성과 상태 표시 방식

## Reuse With Minimal Changes

- `broker/src/server.js`
  - `/device`, `/client` WebSocket 기본 구조
  - JPEG binary frame 래핑 포맷
  - `audio`, `ctrl`, `talk_audio` relay 로직
  - `device_online`, `device_offline`, `broker_status` 이벤트 구조

- `firmware/phase1_esp32/phase1_esp32.ino`
  - 카메라 초기화 코드
  - `cameraCaptureTask`
  - JPEG 최신 프레임 캐시 구조
  - ADPCM encode/decode 함수
  - 마이크 입력 처리
  - 스피커 출력 처리
  - Wi-Fi 스캔/저장 유틸
  - `enterBrokerMode()`의 무재부팅 전환 아이디어

## Keep As Reference Only

- 브로커 binary video packet 형식
  - `type(1 byte) + deviceIdLen(2 bytes) + deviceId + jpeg bytes`

- AP/브로커 공용 UI가 한 HTML에서 모드 분기되는 방식

- 로컬 오디오 WebSocket `:81` 사용 예시

## Do Not Carry Forward Directly

- 현재 `loop()` 중심 상태 전이 구조
- 복구 카운터와 재부팅 스케줄링 로직
- `connectOnBoot` 기반 흐름 제어
- AP 모드와 브로커 모드가 한 파일에서 강하게 뒤엉킨 제어 흐름
- 구형 `/stream` MJPEG 경로

## Rebuild First

1. 펌웨어 상태기계
   - `BOOT`
   - `AP_CONFIG`
   - `TRANSITIONING`
   - `STA_CONNECTING`
   - `BROKER_CONNECTING`
   - `BROKER_ONLINE`
   - `RECOVERY`

2. 모드별 책임 분리
   - AP 모드: 설정, 로컬 미리보기, 로컬 talk
   - 브로커 모드: control, video uplink, audio uplink/downlink

3. 복구 정책 단순화
   - 정상 전환은 무재부팅
   - 복구는 명시적 재시작

## Suggested Extraction Order

1. `web/index.html`
2. 브로커의 WebSocket 기본 구조
3. 펌웨어 카메라/오디오 유틸
4. 새 상태기계
5. 마지막으로 AP/브로커 연결 흐름
