#include <Arduino.h>
#include <Bluepad32.h>
// #include <DFPlayerMini_Fast.h>
#include <ESP32Servo.h>
// #include <EEPROM.h>
#include <esp_log.h>
#include <driver/ledc.h>

static auto MAIN_TAG = "RC_TANK";

// 핀 정의
#define DFPLAYER_RX 20
#define DFPLAYER_TX 21
#define LEFT_TRACK_IN1 6
#define LEFT_TRACK_IN2 5
#define RIGHT_TRACK_IN1 4
#define RIGHT_TRACK_IN2 3
#define CANNON_LED_PIN 1
#define HEADLIGHT_PIN 0
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
  int *prevSpeed;
} MotorConfig;

// EEPROM 주소
// #define EEPROM_LEFT_SPEED_ADDR 0
// #define EEPROM_RIGHT_SPEED_ADDR 1
// #define EEPROM_BUTTON_SWAP_FLAG_ADDR 2
// #define EEPROM_VOLUME_ADDR 4

// EEPROM 초기화 플래그
// #define EEPROM_INIT_FLAG_ADDR 5

// 게임패드 관련 변수
ControllerPtr myControllers[BP32_MAX_GAMEPADS];
bool gamepadConnected = false;

// DFPlayer 관련 변수
// DFPlayerMini_Fast myDFPlayer;
// HardwareSerial DFPlayerSerial(2); // UART2 사용
// unsigned long lastIdleSoundTime = 0;
// constexpr unsigned long idleSoundInterval = 13000; // 13초마다 효과음 1 재생

// 터렛 서보 객체
Servo turretServo;

// 모터 제어 변수
float leftTrackMultiplier = 1.0; // 좌측 트랙 속도 배율 (0.1~2.0)
float rightTrackMultiplier = 1.0; // 우측 트랙 속도 배율 (0.1~2.0)
int leftTrackPWM = 0; // 미사용 변수 (추후 필요 없으면 제거 가능)
int rightTrackPWM = 0; // 미사용 변수 (추후 필요 없으면 제거 가능)
int turretAngle = 90; // 터렛 기본 각도

// 버튼 스왑 설정
bool buttonSwapEnabled = false; // A/B, X/Y 버튼 스왑 여부

// 볼륨 제어 변수
int currentVolume = 20; // 현재 볼륨 (1-30)
int tempVolume = 20; // 임시 볼륨 (버튼을 누르고 있는 동안 사용)
bool volumeChanged = false; // 볼륨이 변경되었는지 확인

// DC 모터 이전 속도 값 저장 변수
int prevLeftTrackSpeed = 0;
int prevRightTrackSpeed = 0;

// 모터 설정 구조체 인스턴스
MotorConfig leftTrackMotor = {
  .in1Pin = LEFT_TRACK_IN1,
  .in2Pin = LEFT_TRACK_IN2,
  .channelA = LEDC_CH_LEFT_IN1,
  .channelB = LEDC_CH_LEFT_IN2,
  .prevSpeed = &prevLeftTrackSpeed
};

MotorConfig rightTrackMotor = {
  .in1Pin = RIGHT_TRACK_IN1,
  .in2Pin = RIGHT_TRACK_IN2,
  .channelA = LEDC_CH_RIGHT_IN1,
  .channelB = LEDC_CH_RIGHT_IN2,
  .prevSpeed = &prevRightTrackSpeed
};

// LED 상태
bool headlightOn = false;
bool ledBlinking = false;
unsigned long lastBlinkTime = 0;
constexpr unsigned long blinkInterval = 100; // 100ms 간격으로 깜빡임

// 포신 발사 관련 변수
bool cannonFiring = false;
unsigned long cannonStartTime = 0;
constexpr unsigned long cannonDuration = 200; // 200ms 동안 포신 당김

// 기관총 발사 관련 변수
bool machineGunFiring = false;
unsigned long machineGunStartTime = 0;
constexpr unsigned long machineGunDuration = 1000; // 1초간 기관총 발사

// 효과음 파일 번호
// #define SOUND_IDLE 1
// #define SOUND_CANNON 2
// #define SOUND_MACHINEGUN 3
// #define SOUND_CONNECTED 4

// 게임패드 연결 콜백
void onConnectedController(const ControllerPtr ctl) {
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
      // myDFPlayer.play(SOUND_CONNECTED);
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
    // 모든 게임패드가 연결 해제되면 효과음 1 재생 시작
    // myDFPlayer.play(SOUND_IDLE);
    // lastIdleSoundTime = millis();
  }

  if (!foundController) {
    ESP_LOGW(MAIN_TAG, "Gamepad disconnected, but not found in myControllers");
  }
}

// DC 모터 제어 함수 (LEDC 사용, 속도 변화가 없으면 호출 무시)
void setMotorSpeed(const MotorConfig *motor, int speed) {
  // 최소 속도 임계값 적용
  if (abs(speed) < MOTOR_MIN_SPEED_THRESHOLD) {
    speed = 0;
  }

  // 속도 변화가 없으면 호출 무시
  if (speed == *(motor->prevSpeed)) {
    return;
  }

  ESP_LOGD(MAIN_TAG,
           "setMotorSpeed IN1:%d IN2:%d ChA:%d ChB:%d Speed:%d (prev:%d)",
           motor->in1Pin,
           motor->in2Pin,
           motor->channelA,
           motor->channelB,
           speed,
           *(motor->prevSpeed));

  // LEDC는 8비트 해상도 사용: 듀티 0~255
  if (speed > 0) {
    // 정방향 회전
    ledcWrite(motor->channelA, 255 /* speed */);
    ledcWrite(motor->channelB, 0);
  } else if (speed < 0) {
    // 역방향 회전
    ledcWrite(motor->channelA, 0);
    ledcWrite(motor->channelB, 255 /* speed */);
  } else {
    // 정지
    ledcWrite(motor->channelA, 0);
    ledcWrite(motor->channelB, 0);
  }

  // 현재 속도를 이전 속도로 저장
  *(motor->prevSpeed) = speed;
}

// 터렛 서보 관련 함수 없음: 간단히 write 사용

// EEPROM에서 속도 배율 값 읽기
void loadSpeedSettings() {
  // EEPROM에서 배율 값을 읽기 (0.1~2.0 범위를 10~200으로 저장)
  // int leftMultiplierInt = EEPROM.read(EEPROM_LEFT_SPEED_ADDR);
  // int rightMultiplierInt = EEPROM.read(EEPROM_RIGHT_SPEED_ADDR);

  // 기본값 설정 (EEPROM이 초기화되지 않은 경우)
  // if (leftMultiplierInt == 0 || leftMultiplierInt > 200) {
  //   leftMultiplierInt = 100; // 1.0을 100으로 저장
  //   EEPROM.write(EEPROM_LEFT_SPEED_ADDR, leftMultiplierInt);
  // }
  // if (rightMultiplierInt == 0 || rightMultiplierInt > 200) {
  //   rightMultiplierInt = 100; // 1.0을 100으로 저장
  //   EEPROM.write(EEPROM_RIGHT_SPEED_ADDR, rightMultiplierInt);
  // }
  // EEPROM.commit();

  // 정수 값을 배율로 변환 (100 = 1.0)
  // leftTrackMultiplier = leftMultiplierInt / 100.0;
  // rightTrackMultiplier = rightMultiplierInt / 100.0;

  // ESP_LOGI(MAIN_TAG, "Loaded speed multipliers: left=%.1f, right=%.1f", leftTrackMultiplier, rightTrackMultiplier);
}

// EEPROM에 속도 배율 값 저장
void saveSpeedSettings() {
  // 배율을 정수로 변환하여 저장 (1.0 = 100)
  // const int leftMultiplierInt = static_cast<int>(leftTrackMultiplier * 100);
  // const int rightMultiplierInt = static_cast<int>(rightTrackMultiplier * 100);

  // EEPROM.write(EEPROM_LEFT_SPEED_ADDR, leftMultiplierInt);
  // EEPROM.write(EEPROM_RIGHT_SPEED_ADDR, rightMultiplierInt);
  // EEPROM.commit();
  // ESP_LOGI(MAIN_TAG, "Speed multipliers saved: left=%.1f, right=%.1f", leftTrackMultiplier, rightTrackMultiplier);
}

// 버튼 스왑 설정 저장
void saveButtonSwapSettings() {
  // EEPROM.write(EEPROM_BUTTON_SWAP_FLAG_ADDR, buttonSwapEnabled ? 1 : 0);
  // EEPROM.commit();
  // ESP_LOGI(MAIN_TAG, "Button swap setting saved: %s", buttonSwapEnabled ? "enabled" : "disabled");
}

// 볼륨 설정 저장
void saveVolumeSettings() {
  // EEPROM.write(EEPROM_VOLUME_ADDR, currentVolume);
  // EEPROM.commit();
  // ESP_LOGI(MAIN_TAG, "Volume setting saved: %d", currentVolume);
}

// 볼륨 설정 로드
void loadVolumeSettings() {
  // int volume = EEPROM.read(EEPROM_VOLUME_ADDR);

  // 기본값 설정 (EEPROM이 초기화되지 않은 경우)
  // if (volume < 1 || volume > 30) {
  //   volume = 20; // 기본 볼륨 20
  //   EEPROM.write(EEPROM_VOLUME_ADDR, volume);
  //   EEPROM.commit();
  // }

  // currentVolume = volume;
  // tempVolume = volume;
  // myDFPlayer.volume(currentVolume);
  // ESP_LOGI(MAIN_TAG, "Volume setting loaded: %d", currentVolume);
}

// 버튼 스왑 설정 로드
void loadButtonSwapSettings() {
  // const int swapFlag = EEPROM.read(EEPROM_BUTTON_SWAP_FLAG_ADDR);
  // buttonSwapEnabled = (swapFlag == 1);
  // ESP_LOGI(MAIN_TAG, "Button swap setting loaded: %s", buttonSwapEnabled ? "enabled" : "disabled");
}

// EEPROM 초기화 및 ESP32 재시작
void resetEEPROMAndRestart() {
  ESP_LOGI(MAIN_TAG, "EEPROM 초기화 및 재시작 시작...");

  // 모든 EEPROM 데이터 초기화
  // for (int i = 0; i < 512; i++) {
  //   EEPROM.write(i, 0);
  // }
  // EEPROM.commit();

  ESP_LOGI(MAIN_TAG, "EEPROM 초기화 완료. 3초 후 재시작합니다.");

  // 3초 대기 후 재시작
  delay(3000);
  esp_restart();
}

void dumpGamepad(ControllerPtr ctl) {
  ESP_LOGD(MAIN_TAG,
      "%s %s %s %s %s %s %s %s %s %s %s %s %s %s misc: 0x%02x",
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
      ctl->miscButtons()
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
  int leftTrackSpeed = map(leftStickY, -255, 255, -255, 255); // TODO. 입력 기기마다 다른 입렵 범위가 들어오는지 확인

  // 우측 스틱 Y축으로 우측 트랙 전후진 제어
  int rightTrackSpeed = map(rightStickY, -255, 255, -255, 255); // TODO. 입력 기기마다 다른 입렵 범위가 들어오는지 확인

  // 속도 제한
  leftTrackSpeed = constrain(leftTrackSpeed, -255, 255);
  rightTrackSpeed = constrain(rightTrackSpeed, -255, 255);

  // 배율 적용
  leftTrackSpeed = static_cast<int>(leftTrackSpeed * leftTrackMultiplier);
  rightTrackSpeed = static_cast<int>(rightTrackSpeed * rightTrackMultiplier);

  // 최종 속도 제한
  leftTrackSpeed = constrain(leftTrackSpeed, -255, 255);
  rightTrackSpeed = constrain(rightTrackSpeed, -255, 255);

  // 모터 제어
  setMotorSpeed(&leftTrackMotor, leftTrackSpeed);
  setMotorSpeed(&rightTrackMotor, rightTrackSpeed);

  // D-PAD로 터렛과 포 마운트 제어
  // D-PAD 좌우로 터렛 제어
  if (ctl->dpad() == DPAD_LEFT) {
    turretAngle = constrain(turretAngle - 2, 0, 180);
    turretServo.write(turretAngle);
  } else if (ctl->dpad() == DPAD_RIGHT) {
    turretAngle = constrain(turretAngle + 2, 0, 180);
    turretServo.write(turretAngle);
  }

  // 터렛(포 마운트) 각도 제어 제거됨

  // 버튼 스왑 적용: A/B 버튼 처리
  const bool buttonA = buttonSwapEnabled ? ctl->b() : ctl->a();
  const bool buttonB = buttonSwapEnabled ? ctl->a() : ctl->b();

  // B 버튼으로 포신 발사
  if (buttonB && !cannonFiring && !machineGunFiring) {
    cannonFiring = true;
    cannonStartTime = millis();
    ledBlinking = true;

    // 게임 패드 진동
    ctl->playDualRumble(0, 400, 0xFF, 0x0);

    // 서보 제거됨

    // 효과음 2 재생
    // myDFPlayer.play(SOUND_CANNON);
  }

  // A 버튼으로 기관총 발사
  if (buttonA && !machineGunFiring && !cannonFiring) {
    machineGunFiring = true;
    machineGunStartTime = millis();
    ledBlinking = true;

    // 게임 패드 진동
    ctl->playDualRumble(0, 300, 0xFF, 0x0);

    // 효과음 3 재생
    // myDFPlayer.play(SOUND_MACHINEGUN);
  }

  // L1 + (X 또는 Y) 버튼으로 볼륨 조절 (둔감하게 처리)
  static bool l1ButtonPressed = false;
  static bool r1ButtonPressed = false;
  static unsigned long l1LastChangeTime = 0;
  static unsigned long r1LastChangeTime = 0;
  constexpr unsigned long volumeChangeInterval = 100; // 100ms 간격으로 볼륨 변경

  // L1 + (X 또는 Y) 버튼으로 볼륨 감소
  if (ctl->l1() && (ctl->x() || ctl->y())) {
    if (!l1ButtonPressed) {
      l1ButtonPressed = true;
      tempVolume = currentVolume; // 현재 볼륨을 임시 볼륨으로 복사
      l1LastChangeTime = millis();
    }

    // 볼륨 감소 (1-30 범위, 100ms 간격으로만 변경)
    if (tempVolume > 1 && (millis() - l1LastChangeTime >= volumeChangeInterval)) {
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
        // myDFPlayer.volume(currentVolume); // DFPlayer 볼륨 적용
        volumeChanged = true;
        ESP_LOGI(MAIN_TAG, "Volume change confirmed: %d", currentVolume);
      }
    }
  }

  // R1 + (X 또는 Y) 버튼으로 볼륨 증가
  if (ctl->r1() && (ctl->x() || ctl->y())) {
    if (!r1ButtonPressed) {
      r1ButtonPressed = true;
      tempVolume = currentVolume; // 현재 볼륨을 임시 볼륨으로 복사
      r1LastChangeTime = millis();
    }

    // 볼륨 증가 (1-30 범위, 100ms 간격으로만 변경)
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
        // myDFPlayer.volume(currentVolume); // DFPlayer 볼륨 적용
        volumeChanged = true;
        ESP_LOGI(MAIN_TAG, "Volume change confirmed: %d", currentVolume);
      }
    }
  }

  // 볼륨이 변경되었으면 EEPROM에 저장
  if (volumeChanged) {
    saveVolumeSettings();
    volumeChanged = false;
  }

  // 헤드라이트 토글 (L2 + R2 버튼으로 변경, 단일 클릭)
  static bool l2r2ButtonPressed = false;
  if (ctl->l2() && ctl->r2() && !l2r2ButtonPressed) {
    headlightOn = !headlightOn;
    digitalWrite(HEADLIGHT_PIN, headlightOn ? HIGH : LOW);
    l2r2ButtonPressed = true;
  } else if (!ctl->l2() || !ctl->r2()) {
    l2r2ButtonPressed = false;
  }

  // 버튼 스왑 적용: X/Y 버튼 처리
  const bool buttonX = buttonSwapEnabled ? ctl->y() : ctl->x();
  const bool buttonY = buttonSwapEnabled ? ctl->x() : ctl->y();

  // X 버튼 + D-PAD Y축으로 좌측 트랙 속도 배율 설정
  static bool xButtonPressed = false;
  if (buttonX) {
    if (!xButtonPressed) {
      xButtonPressed = true;
    }

    // D-PAD 상하로 좌측 트랙 속도 배율 조절 (0.1~2.0)
    if (ctl->dpad() == DPAD_UP) {
      leftTrackMultiplier = constrain(leftTrackMultiplier + 0.02, 0.1, 2.0);
      saveSpeedSettings();
    } else if (ctl->dpad() == DPAD_DOWN) {
      leftTrackMultiplier = constrain(leftTrackMultiplier - 0.02, 0.1, 2.0);
      saveSpeedSettings();
    }
  } else {
    xButtonPressed = false;
  }

  // Y 버튼 + D-PAD Y축으로 우측 트랙 속도 배율 설정
  static bool yButtonPressed = false;
  if (buttonY) {
    if (!yButtonPressed) {
      yButtonPressed = true;
    }

    // D-PAD 상하로 우측 트랙 속도 배율 조절 (0.1~2.0)
    if (ctl->dpad() == DPAD_UP) {
      rightTrackMultiplier = constrain(rightTrackMultiplier + 0.02, 0.1, 2.0);
      saveSpeedSettings();
    } else if (ctl->dpad() == DPAD_DOWN) {
      rightTrackMultiplier = constrain(rightTrackMultiplier - 0.02, 0.1, 2.0);
      saveSpeedSettings();
    }
  } else {
    yButtonPressed = false;
  }

  // L1 + R1 버튼 3초 이상 동시 누름으로 버튼 스왑 토글
  static bool l1r1Pressed = false;
  static unsigned long l1r1StartTime = 0;

  if (ctl->l1() && ctl->r1()) {
    if (!l1r1Pressed) {
      l1r1Pressed = true;
      l1r1StartTime = millis();
      ESP_LOGI(MAIN_TAG, "L1 + R1 버튼이 눌렸습니다. 3초간 유지하면 버튼 스왑이 변경됩니다.");
    } else {
      constexpr unsigned long l1r1HoldDuration = 3000;
      // 버튼이 계속 눌려있는 상태에서 3초 경과 확인
      if (millis() - l1r1StartTime >= l1r1HoldDuration) {
        buttonSwapEnabled = !buttonSwapEnabled;

        ESP_LOGI(MAIN_TAG, "L1 + R1 버튼을 3초간 누르셨습니다. 버튼 스왑: %s",
                 buttonSwapEnabled ? "활성화" : "비활성화");

        // 게임패드 진동으로 확인 신호
        ctl->playDualRumble(0, 600, 0xFF, 0x0);

        // 설정 저장
        saveButtonSwapSettings();

        // 플래그 리셋하여 중복 실행 방지
        l1r1Pressed = false;
      }
    }
  } else {
    l1r1Pressed = false;
  }

  // Select + Start 버튼 3초 이상 동시 누름으로 EEPROM 초기화 및 재시작
  static bool selectStartPressed = false;
  static unsigned long selectStartStartTime = 0;

  if (ctl->miscSelect() && ctl->miscStart()) {
    if (!selectStartPressed) {
      selectStartPressed = true;
      selectStartStartTime = millis();
      ESP_LOGI(MAIN_TAG, "Select + Start 버튼이 눌렸습니다. 3초간 유지하면 EEPROM 초기화가 시작됩니다.");
    } else {
      constexpr unsigned long selectStartHoldDuration = 3000;
      // 버튼이 계속 눌려있는 상태에서 3초 경과 확인
      if (millis() - selectStartStartTime >= selectStartHoldDuration) {
        ESP_LOGI(MAIN_TAG, "Select + Start 버튼을 3초간 누르셨습니다. EEPROM 초기화를 시작합니다.");

        // 게임패드 진동으로 확인 신호
        ctl->playDualRumble(0, 800, 0xFF, 0x0);

        // EEPROM 초기화 및 재시작
        resetEEPROMAndRestart();
      }
    }
  } else {
    selectStartPressed = false;
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
        ledBlinking = false;
        digitalWrite(CANNON_LED_PIN, LOW);
      }

      // 서보 제거됨

      // 효과음 1 재생 재개 (게임패드가 연결되어 있지 않은 경우)
      if (!gamepadConnected && !machineGunFiring) {
        // myDFPlayer.play(SOUND_IDLE);
        // lastIdleSoundTime = millis();
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
        ledBlinking = false;
        digitalWrite(CANNON_LED_PIN, LOW);
      }

      // 효과음 1 재생 재개 (게임패드가 연결되어 있지 않은 경우)
      if (!gamepadConnected && !cannonFiring) {
        // myDFPlayer.play(SOUND_IDLE);
        // lastIdleSoundTime = millis();
      }
    }
  }
}

// LED 깜빡임 처리
void processLEDBlinking() {
  if (ledBlinking) {
    const unsigned long currentTime = millis();

    // 기관총 발사 중일 때는 500ms 간격으로 점멸
    if (machineGunFiring) {
      if (currentTime - lastBlinkTime >= 500) {
        digitalWrite(CANNON_LED_PIN, !digitalRead(CANNON_LED_PIN));
        lastBlinkTime = currentTime;
      }
    } else {
      // 포신 발사 중일 때는 기존 100ms 간격으로 점멸
      if (currentTime - lastBlinkTime >= blinkInterval) {
        digitalWrite(CANNON_LED_PIN, !digitalRead(CANNON_LED_PIN));
        lastBlinkTime = currentTime;
      }
    }
  }
}

// 효과음 반복 재생 처리
void processIdleSound() {
  // if (!gamepadConnected && !cannonFiring && !machineGunFiring) {
  //   const unsigned long currentTime = millis();
  //   if (currentTime - lastIdleSoundTime >= idleSoundInterval) {
  //     myDFPlayer.play(SOUND_IDLE);
  //     lastIdleSoundTime = currentTime;
  //   }
  // }
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
  Serial.begin(115200);
  delay(2000); // USB CDC 초기화를 위한 충분한 대기 시간

  // ESP-IDF 로그를 Serial 객체로 출력하도록 설정
  Serial.setDebugOutput(true);

  // (선택 사항) 런타임에 특정 태그의 로그 레벨 설정
  esp_log_level_set("*", ESP_LOG_DEBUG);

  ESP_LOGI(MAIN_TAG, "RC Tank Initialization...");

  Serial.println("Hello World");

  // Brownout을 피하기 위해 CPU 클록을 160 MHz로 낮춤
  setCpuFrequencyMhz(160);

  // EEPROM 초기화
  // EEPROM.begin(512);

  // 핀 모드 설정
  pinMode(LEFT_TRACK_IN1, OUTPUT);
  pinMode(LEFT_TRACK_IN2, OUTPUT);
  pinMode(RIGHT_TRACK_IN1, OUTPUT);
  pinMode(RIGHT_TRACK_IN2, OUTPUT);
  pinMode(CANNON_LED_PIN, OUTPUT);
  pinMode(HEADLIGHT_PIN, OUTPUT);

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
  // 터렛 제어 제거됨
  // 터렛 서보 초기화 및 초기 각도 설정
  turretServo.attach(TURRET_SERVO_PIN);
  turretServo.write(turretAngle);

  // DFPlayer 초기화
  // DFPlayerSerial.begin(9600, SERIAL_8N1, DFPLAYER_RX, DFPLAYER_TX);
  // myDFPlayer.begin(DFPlayerSerial);
  // 볼륨은 loadVolumeSettings()에서 설정됨

  // EEPROM에서 설정 로드
  // loadSpeedSettings();
  // loadButtonSwapSettings();
  // loadVolumeSettings();

  // 효과음 1 재생 시작
  // myDFPlayer.play(SOUND_IDLE);
  // lastIdleSoundTime = millis();

  // EEPROM 초기화 플래그 확인 (첫 실행 시)
  // const int initFlag = EEPROM.read(EEPROM_INIT_FLAG_ADDR);
  // if (initFlag != 0xAA) {
  //   ESP_LOGI(MAIN_TAG, "EEPROM이 초기화되지 않았습니다. 초기화 플래그를 설정합니다.");
  //   EEPROM.write(EEPROM_INIT_FLAG_ADDR, 0xAA);
  //   EEPROM.commit();
  // }

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

unsigned long lastCheckTime = 0;
// 메인 루프
void loop() {
  if (millis() - lastCheckTime >= 1000 * 10) {
    lastCheckTime = millis();
    ESP_LOGI(MAIN_TAG, "LOOP : Hello World");
  }

  // Bluepad32 업데이트
  const bool dataUpdated = BP32.update();
  if (dataUpdated) {
    processControllers();
  }

  // // 포신 발사 처리
  // processCannonFiring();

  // // 기관총 발사 처리
  // processMachineGunFiring();

  // // LED 깜빡임 처리
  // processLEDBlinking();

  // // 효과음 반복 재생 처리
  // processIdleSound();

  delay(10); // 10ms 딜레이
}
