#include <Arduino.h>
#include <Bluepad32.h>
#include <DFPlayerMini_Fast.h>
#include <ESP32Servo.h>
#include <Preferences.h>
#include <esp_log.h>
#include <driver/ledc.h>
#include <WiFi.h>

static auto MAIN_TAG = "RC_TANK";

// 핀 정의
#define DFPLAYER_RX 20 // Unused
#define DFPLAYER_TX 10 // dfplayer RX
#define LEFT_TRACK_IN1 4
#define LEFT_TRACK_IN2 3
#define RIGHT_TRACK_IN1 0
#define RIGHT_TRACK_IN2 5
#define CANNON_LED_PIN 1
#define HEADLIGHT_PIN 6
#define TURRET_SERVO_PIN 7   // 터렛 회전 SG90 서보 핀

// LEDC 설정 (트랙 모터용)
#define LEDC_FREQ 5000
#define LEDC_RESOLUTION 8
#define LEDC_MAX_VAL 256

// LEDC 채널 매핑 (필요 시 겹치지 않게 조정 가능)
#define LEDC_CH_LEFT_IN1 4
#define LEDC_CH_LEFT_IN2 5
#define LEDC_CH_RIGHT_IN1 3
#define LEDC_CH_RIGHT_IN2 2

// DC 모터 최소 속도 임계값 (80 미만은 처리하지 않음)
#define MOTOR_MIN_SPEED_THRESHOLD 80

// 모터 설정 구조체
typedef struct {
  int in1Pin;
  int in2Pin;
  int channelA; // IN1에 매핑된 LEDC 채널
  int channelB; // IN2에 매핑된 LEDC 채널
} MotorConfig;

// 게임패드 관련 변수
ControllerPtr myControllers[BP32_MAX_GAMEPADS];
bool gamepadConnected = false;

// DFPlayer 관련 변수
DFPlayerMini_Fast myDFPlayer;
HardwareSerial DFPlayerSerial(1); // UART2 사용
Preferences prefs;

constexpr auto NVS_NAMESPACE = "rc_tank";
constexpr auto NVS_KEY_VOLUME = "volume";
unsigned long lastIdleSoundTime = 0;
constexpr unsigned long idleSoundInterval = 13000; // 13초마다 효과음 1 재생

// 터렛 서보 객체
Servo turretServo;

// 모터 제어 변수
int turretAngle = 90; // 터렛 기본 각도

// 볼륨 제어 변수
constexpr int initialVolume = 15;
int currentVolume = 20; // 현재 볼륨 (1-30)
int tempVolume = 20; // 임시 볼륨 (버튼을 누르고 있는 동안 사용)
bool volumeChanged = false; // 볼륨이 변경되었는지 확인

// 모터 설정 구조체 인스턴스
MotorConfig leftTrackMotor = {
  .in1Pin = LEFT_TRACK_IN1,
  .in2Pin = LEFT_TRACK_IN2,
  .channelA = LEDC_CH_LEFT_IN1,
  .channelB = LEDC_CH_LEFT_IN2,
};

MotorConfig rightTrackMotor = {
  .in1Pin = RIGHT_TRACK_IN1,
  .in2Pin = RIGHT_TRACK_IN2,
  .channelA = LEDC_CH_RIGHT_IN1,
  .channelB = LEDC_CH_RIGHT_IN2,
};

// LED 상태
bool headlightOn = false;
unsigned long lastBlinkTime = 0;
constexpr unsigned long blinkInterval = 100; // 100ms 간격으로 깜빡임

// 포신 발사 관련 변수
bool cannonFiring = false;
unsigned long cannonStartTime = 0;
constexpr unsigned long cannonDuration = 1000; // 200ms 동안 포신 당김

// 리코일(발사 반동) 관련 변수
bool recoilActive = false;
unsigned long recoilStartTime = 0;
constexpr unsigned long recoilBackDuration = 100; // 강한 후진 구간
constexpr unsigned long recoilSettleDuration = 80; // 정지 후 안정화 구간
int recoilBackSpeed = 400; // 후진 강도 (-512 ~ 0)

// 기관총 발사 관련 변수
bool machineGunFiring = false;
unsigned long machineGunStartTime = 0;
constexpr unsigned long machineGunDuration = 500; // 1초간 기관총 발사

// 효과음 파일 번호
#define SOUND_IDLE 1
#define SOUND_CANNON 2
#define SOUND_MACHINEGUN 3
#define SOUND_CONNECTED 4

// DC 모터 제어 함수 (LEDC 사용, 속도 변화가 없으면 호출 무시)
void setMotorSpeed(const MotorConfig *motor, int speed) {
  // 최소 속도 임계값 적용
  if (abs(speed) < MOTOR_MIN_SPEED_THRESHOLD) {
    speed = 0;
  }

  ESP_LOGD(MAIN_TAG,
           "setMotorSpeed IN1:%d IN2:%d ChA:%d ChB:%d Speed:%d (prev:%d)",
           motor->in1Pin,
           motor->in2Pin,
           motor->channelA,
           motor->channelB,
           speed,
           *(motor->prevSpeed));

  // LEDC는 8비트 해상도 사용: 듀티 0~512
  if (speed > 0) {
    // 정방향 회전
    ledcWrite(motor->channelA, 512 /* speed */);
    ledcWrite(motor->channelB, 0);
  } else if (speed < 0) {
    // 역방향 회전
    ledcWrite(motor->channelA, 0);
    ledcWrite(motor->channelB, 512 /* speed */);
  } else {
    // 정지
    ledcWrite(motor->channelA, 0);
    ledcWrite(motor->channelB, 0);
  }
}

// 게임패드 연결 콜백
void onConnectedController(const ControllerPtr ctl) {

  myDFPlayer.volume(currentVolume);

  bool foundEmptySlot = false;
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
    if (myControllers[i] == nullptr) {
      ESP_LOGI(MAIN_TAG, "Gamepad connected, index=%d", i);
      ControllerProperties properties = ctl->getProperties();
      ESP_LOGI(MAIN_TAG,
               "Controller model: %s, VID=0x%04x, PID=0x%04x",
               ctl->getModelName().c_str(),
               properties.vendor_id,
               properties.product_id);
      myControllers[i] = ctl;
      foundEmptySlot = true;
      gamepadConnected = true;

      // 게임패드 연결 시 효과음 4 재생
      myDFPlayer.play(SOUND_CONNECTED);
      break;
    }
  }
  if (!foundEmptySlot) {
    ESP_LOGW(MAIN_TAG, "Gamepad connected, but no empty slot found");
  }
}

// 게임패드 연결 해제 콜백
void onDisconnectedController(ControllerPtr ctl) {
  bool foundController = false;
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
    if (myControllers[i] == ctl) {
      ESP_LOGI(MAIN_TAG, "Gamepad disconnected, index=%d", i);
      myControllers[i] = nullptr;
      foundController = true;
      break;
    }
  }

  // 모든 게임패드가 연결 해제되었는지 확인
  gamepadConnected = false;
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
    if (myControllers[i] != nullptr) {
      gamepadConnected = true;
      break;
    }
  }

  if (!gamepadConnected) {
    myDFPlayer.volume(initialVolume);

    // 모든 게임패드가 연결 해제되면 효과음 1 재생 시작
    myDFPlayer.play(SOUND_IDLE);
    lastIdleSoundTime = millis();

    setMotorSpeed(&leftTrackMotor, 0);
    setMotorSpeed(&rightTrackMotor, 0);

    digitalWrite(HEADLIGHT_PIN, LOW);
  }

  if (!foundController) {
    ESP_LOGW(MAIN_TAG, "Gamepad disconnected, but not found in myControllers");
  }
}

// NVS에서 볼륨 불러오기 (없으면 기본값 20, 범위 11~30로 클램프)
void loadVolumeFromNVS() {
  int stored = prefs.getInt(NVS_KEY_VOLUME, -1);
  if (stored < 11 || stored > 30) {
    stored = 20;
    ESP_LOGI(MAIN_TAG, "Volume not found in NVS. Using default: %d", stored);
  } else {
    ESP_LOGI(MAIN_TAG, "Volume loaded from NVS: %d", stored);
  }
  currentVolume = stored;
  tempVolume = stored;
}

// NVS에 볼륨 저장 (범위 11~30로 클램프)
void saveVolumeToNVS(int volume) {
  const int clamped = constrain(volume, 11, 30);
  prefs.putInt(NVS_KEY_VOLUME, clamped);
  ESP_LOGI(MAIN_TAG, "Volume saved to NVS: %d", clamped);
}

void dumpGamepad(ControllerPtr ctl) {
  ESP_LOGD(MAIN_TAG,
           "0x%02x %s %s %s %s %s %s %s %s %s %s %s %s %s %s misc: 0x%02x LY:%3d RY:%3d",
           ctl->dpad(),
           ctl->a() ? "A" : "-",
           ctl->b() ? "B" : "-",
           ctl->x() ? "X" : "-",
           ctl->y() ? "Y" : "-",
           ctl->l1() ? "L1" : "--",
           ctl->r1() ? "R1" : "--",
           ctl->l2() ? "L2" : "--",
           ctl->r2() ? "R2" : "--",
           ctl->thumbL() ? "ThumbL" : "------",
           ctl->thumbR() ? "ThumbR" : "------",
           ctl->miscStart() ? "Start" : "------",
           ctl->miscSelect() ? "Select" : "------",
           ctl->miscSystem() ? "System" : "------",
           ctl->miscCapture() ? "Capture" : "------",
           ctl->miscButtons(),
           ctl->axisY(),
           ctl->axisRY()
  );
}

// 게임패드 처리 함수
void processGamepad(const ControllerPtr ctl) {
  dumpGamepad(ctl);

  // 좌측 스틱 Y축으로 좌측 트랙 제어, 우측 스틱 Y축으로 우측 트랙 제어
  int leftStickY = ctl->axisY();
  int rightStickY = ctl->axisRY();

  // 데드존 설정
  if (abs(leftStickY) < 50) leftStickY = 0;
  if (abs(rightStickY) < 50) rightStickY = 0;

  // 좌측 스틱 Y축으로 좌측 트랙 전후진 제어
  int leftTrackSpeed = map(leftStickY, -512, 512, -512, 512); // TODO. 입력 기기마다 다른 입렵 범위가 들어오는지 확인

  // 우측 스틱 Y축으로 우측 트랙 전후진 제어
  int rightTrackSpeed = map(rightStickY, -512, 512, -512, 512); // TODO. 입력 기기마다 다른 입렵 범위가 들어오는지 확인

  // 속도 제한
  leftTrackSpeed = constrain(leftTrackSpeed, -512, 512);
  rightTrackSpeed = constrain(rightTrackSpeed, -512, 512);

  // 최종 속도 제한
  leftTrackSpeed = constrain(leftTrackSpeed, -512, 512);
  rightTrackSpeed = constrain(rightTrackSpeed, -512, 512);

  // 모터 제어 (리코일 중에는 사용자 입력에 의한 모터 제어를 잠시 무시)
  if (!recoilActive) {
    setMotorSpeed(&leftTrackMotor, leftTrackSpeed);
    setMotorSpeed(&rightTrackMotor, rightTrackSpeed);
  }

  // D-PAD 좌우로 터렛 제어
  if (ctl->dpad() == DPAD_LEFT) {
    turretAngle = constrain(turretAngle - 1, 0, 180);
    ESP_LOGD(MAIN_TAG, "Turret - Left(%3d)", turretAngle);
    turretServo.write(turretAngle);
  } else if (ctl->dpad() == DPAD_RIGHT) {
    turretAngle = constrain(turretAngle + 1, 0, 180);
    ESP_LOGD(MAIN_TAG, "Turret - Right(%3d)", turretAngle);
    turretServo.write(turretAngle);
  }

  // B 버튼으로 포신 발사
  if (ctl->b() && !cannonFiring && !machineGunFiring && !recoilActive) {
    cannonFiring = true;
    cannonStartTime = millis();

    digitalWrite(CANNON_LED_PIN, HIGH);

    // 게임 패드 진동
    // ctl->playDualRumble(0, 400, 0xFF, 0x0);

    // 리코일 시작: 현재 속도 저장 후 강한 후진 적용
    recoilActive = true;
    recoilStartTime = cannonStartTime;
    setMotorSpeed(&leftTrackMotor, recoilBackSpeed);
    setMotorSpeed(&rightTrackMotor, recoilBackSpeed);

    delay(100);
    digitalWrite(CANNON_LED_PIN, LOW);

    setMotorSpeed(&leftTrackMotor, 0);
    setMotorSpeed(&rightTrackMotor, 0);

    // 효과음 2 재생
    myDFPlayer.play(SOUND_CANNON);
  }

  // A 버튼으로 기관총 발사
  if (ctl->a() && !machineGunFiring && !cannonFiring) {
    machineGunFiring = true;
    machineGunStartTime = millis();

    // 게임 패드 진동
    // ctl->playDualRumble(0, 300, 0xFF, 0x0);

    // 효과음 3 재생
    myDFPlayer.play(SOUND_MACHINEGUN);
  }

  // L1/R1 버튼으로 볼륨 조절 (둔감하게 처리)
  static bool l1ButtonPressed = false;
  static bool r1ButtonPressed = false;
  static unsigned long l1LastChangeTime = 0;
  static unsigned long r1LastChangeTime = 0;
  constexpr unsigned long volumeChangeInterval = 100; // 100ms 간격으로 볼륨 변경

  // L1 버튼으로 볼륨 감소
  if (ctl->l1()) {
    if (!l1ButtonPressed) {
      l1ButtonPressed = true;
      tempVolume = currentVolume; // 현재 볼륨을 임시 볼륨으로 복사
      l1LastChangeTime = millis();
    }

    // 볼륨 감소 (10-30 범위, 100ms 간격으로만 변경)
    if (tempVolume > 11 && (millis() - l1LastChangeTime >= volumeChangeInterval)) {
      tempVolume--;
      l1LastChangeTime = millis();
      ESP_LOGI(MAIN_TAG, "Volume decreased to: %d", tempVolume);
    }
  } else {
    if (l1ButtonPressed) {
      l1ButtonPressed = false;
      // L1 버튼을 뗐을 때 볼륨 변경사항 저장
      if (tempVolume != currentVolume) {
        currentVolume = tempVolume;
        myDFPlayer.volume(currentVolume); // DFPlayer 볼륨 적용
        volumeChanged = true;
        ESP_LOGI(MAIN_TAG, "Volume change confirmed: %d", currentVolume);
      }
    }
  }

  // R1 버튼으로 볼륨 증가
  if (ctl->r1()) {
    if (!r1ButtonPressed) {
      r1ButtonPressed = true;
      tempVolume = currentVolume; // 현재 볼륨을 임시 볼륨으로 복사
      r1LastChangeTime = millis();
    }

    // 볼륨 증가 (10-30 범위, 100ms 간격으로만 변경)
    if (tempVolume < 30 && (millis() - r1LastChangeTime >= volumeChangeInterval)) {
      tempVolume++;
      r1LastChangeTime = millis();
      ESP_LOGI(MAIN_TAG, "Volume increased to: %d", tempVolume);
    }
  } else {
    if (r1ButtonPressed) {
      r1ButtonPressed = false;
      // R1 버튼을 뗐을 때 볼륨 변경사항 저장
      if (tempVolume != currentVolume) {
        currentVolume = tempVolume;
        myDFPlayer.volume(currentVolume); // DFPlayer 볼륨 적용
        volumeChanged = true;
        ESP_LOGI(MAIN_TAG, "Volume change confirmed: %d", currentVolume);
      }
    }
  }

  // 볼륨 변경 시 NVS에 즉시 저장
  if (volumeChanged) {
    saveVolumeToNVS(currentVolume);
    volumeChanged = false;
  }

  // 헤드라이트 토글: Y 버튼을 200ms 이상 누르고 뗄 때 적용
  static bool yPressing = false;
  static unsigned long yPressStart = 0;
  constexpr unsigned long headlightHoldMs = 200;

  if (ctl->y()) {
    if (!yPressing) {
      yPressing = true;
      yPressStart = millis();
    }
  } else {
    if (yPressing) {
      yPressing = false;
      const unsigned long held = millis() - yPressStart;
      if (held >= headlightHoldMs) {
        headlightOn = !headlightOn;
        digitalWrite(HEADLIGHT_PIN, headlightOn ? HIGH : LOW);
      }
    }
  }
}

// 포신 발사 처리
void processCannonFiring() {
  if (cannonFiring) {
    const unsigned long currentTime = millis();
    if (currentTime - cannonStartTime >= cannonDuration) {
      // 포신 발사 완료
      cannonFiring = false;

      // 기관총이 발사 중이 아닌 경우에만 LED 점멸 중단
      if (!machineGunFiring) {
        digitalWrite(CANNON_LED_PIN, LOW);
      }

      // 효과음 1 재생 재개 (게임패드가 연결되어 있지 않은 경우)
      if (!gamepadConnected && !machineGunFiring) {
        myDFPlayer.play(SOUND_IDLE);
        lastIdleSoundTime = millis();
      }
    }
  }
}

// 기관총 발사 처리
void processMachineGunFiring() {
  if (machineGunFiring) {
    const unsigned long currentTime = millis();
    if (currentTime - machineGunStartTime >= machineGunDuration) {
      // 기관총 발사 완료
      machineGunFiring = false;

      // 포신이 발사 중이 아닌 경우에만 LED 점멸 중단
      if (!cannonFiring) {
        digitalWrite(CANNON_LED_PIN, LOW);
      }

      // 효과음 1 재생 재개 (게임패드가 연결되어 있지 않은 경우)
      if (!gamepadConnected && !cannonFiring) {
        myDFPlayer.play(SOUND_IDLE);
        lastIdleSoundTime = millis();
      }
    }
  }
}

// 리코일 처리
void processRecoil() {
  if (!recoilActive) return;

  const unsigned long now = millis();
  const unsigned long elapsed = now - recoilStartTime;

  if (elapsed < recoilBackDuration) {
    // 강한 후진 유지
    setMotorSpeed(&leftTrackMotor, recoilBackSpeed);
    setMotorSpeed(&rightTrackMotor, recoilBackSpeed);
    return;
  }

  if (elapsed < recoilBackDuration + recoilSettleDuration) {
    // 정지 상태로 안정화
    setMotorSpeed(&leftTrackMotor, 0);
    setMotorSpeed(&rightTrackMotor, 0);
    return;
  }

  // 리코일 종료: 원래 속도로 복원
  recoilActive = false;
}

// 효과음 반복 재생 처리
void processIdleSound() {
  if (!gamepadConnected && !cannonFiring && !machineGunFiring) {
    const unsigned long currentTime = millis();
    if (currentTime - lastIdleSoundTime >= idleSoundInterval) {
      myDFPlayer.play(SOUND_IDLE);
      lastIdleSoundTime = currentTime;
    }
  }
}

// 모든 컨트롤러 처리
void processControllers() {
  for (const auto myController : myControllers) {
    if (myController && myController->isConnected() && myController->hasData()) {
      if (myController->isGamepad()) {
        processGamepad(myController);
      }
    }
  }
}

// 설정 함수
void setup() {
  WiFi.mode(WIFI_OFF);

  // 캐패시터 충전 시간?
  delay(5000);

  // 핀 모드 설정
  pinMode(LEFT_TRACK_IN1, OUTPUT);
  pinMode(LEFT_TRACK_IN2, OUTPUT);
  pinMode(RIGHT_TRACK_IN1, OUTPUT);
  pinMode(RIGHT_TRACK_IN2, OUTPUT);

  digitalWrite(LEFT_TRACK_IN1, LOW);
  digitalWrite(LEFT_TRACK_IN2, LOW);
  digitalWrite(RIGHT_TRACK_IN1, LOW);
  digitalWrite(RIGHT_TRACK_IN2, LOW);

  pinMode(CANNON_LED_PIN, OUTPUT);
  pinMode(HEADLIGHT_PIN, OUTPUT);

  // NVS 초기화 및 볼륨 로드
  prefs.begin(NVS_NAMESPACE, false);
  loadVolumeFromNVS();

  // LEDC 초기화 (트랙 모터용)
  ledcSetup(LEDC_CH_LEFT_IN1, LEDC_FREQ, LEDC_RESOLUTION);
  ledcAttachPin(LEFT_TRACK_IN1, LEDC_CH_LEFT_IN1);
  ledcSetup(LEDC_CH_LEFT_IN2, LEDC_FREQ, LEDC_RESOLUTION);
  ledcAttachPin(LEFT_TRACK_IN2, LEDC_CH_LEFT_IN2);

  ledcSetup(LEDC_CH_RIGHT_IN1, LEDC_FREQ, LEDC_RESOLUTION);
  ledcAttachPin(RIGHT_TRACK_IN1, LEDC_CH_RIGHT_IN1);
  ledcSetup(LEDC_CH_RIGHT_IN2, LEDC_FREQ, LEDC_RESOLUTION);
  ledcAttachPin(RIGHT_TRACK_IN2, LEDC_CH_RIGHT_IN2);

  // 모터 정지
  setMotorSpeed(&leftTrackMotor, 0);
  setMotorSpeed(&rightTrackMotor, 0);

#if ARDUINO_USB_CDC_ON_BOOT
  Serial.begin(115200);
  delay(2000); // USB CDC 초기화를 위한 충분한 대기 시간

  // ESP-IDF 로그를 Serial 객체로 출력하도록 설정
  Serial.setDebugOutput(true);

  // (선택 사항) 런타임에 특정 태그의 로그 레벨 설정
  esp_log_level_set("*", ESP_LOG_DEBUG);

  ESP_LOGI(MAIN_TAG, "RC Tank Initialization...");

  Serial.println("Hello World");
#endif

  // Brownout을 피하기 위해 CPU 클록을 80 MHz로 낮춤
  setCpuFrequencyMhz(80);

  // 터렛 서보 초기화 및 초기 각도 설정
  turretServo.attach(TURRET_SERVO_PIN);
  turretServo.write(turretAngle);

  // DFPlayer 초기화
  DFPlayerSerial.begin(9600, SERIAL_8N1, DFPLAYER_RX, DFPLAYER_TX);
  myDFPlayer.begin(DFPlayerSerial);
  myDFPlayer.volume(initialVolume);

  // 효과음 1 재생 시작
  myDFPlayer.play(SOUND_IDLE);
  lastIdleSoundTime = millis();

  // Bluepad32 설정
  BP32.setup(&onConnectedController, &onDisconnectedController);
  BP32.forgetBluetoothKeys();
  BP32.enableVirtualDevice(false);

  ESP_LOGI(MAIN_TAG, "Firmware version: %s", BP32.firmwareVersion());
  const uint8_t *addr = BP32.localBdAddress();
  ESP_LOGI(MAIN_TAG,
           "BD address: %2X:%2X:%2X:%2X:%2X:%2X",
           addr[0],
           addr[1],
           addr[2],
           addr[3],
           addr[4],
           addr[5]);

  ESP_LOGI(MAIN_TAG, "RC Tank Initialization Complete!");
}

// 메인 루프
void loop() {
  // Bluepad32 업데이트
  const bool dataUpdated = BP32.update();
  if (dataUpdated) {
    processControllers();
  }

  // 포신 발사 처리
  processCannonFiring();

  // 기관총 발사 처리
  processMachineGunFiring();

  // 리코일 처리
  processRecoil();

  // 효과음 반복 재생 처리
  processIdleSound();

  delay(10); // 10ms 딜레이
}
