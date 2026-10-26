#include <Arduino.h>
#include <PhoneControlPage.h>
#include <HistoryStore.h>
#include <DualScaleBleProtocol.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <Wire.h>
#include <WebServer.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <math.h>

#if !defined(CONFIG_BT_ENABLED)
#error Bluetooth is not enabled for this ESP32 build.
#endif

namespace {

// Guition JC2432W328C display and touch connections.
constexpr int TFT_BACKLIGHT_PIN = 27;
constexpr int TOUCH_SDA_PIN = 33;
constexpr int TOUCH_SCL_PIN = 32;
constexpr int TOUCH_RESET_PIN = 25;
constexpr int TOUCH_INTERRUPT_PIN = 21;
constexpr uint8_t CST820_ADDRESS = 0x15;

constexpr int SCREEN_WIDTH = 240;
constexpr int SCREEN_HEIGHT = 320;
constexpr int WEIGHT_SPRITE_WIDTH = 145;
constexpr int WEIGHT_SPRITE_HEIGHT = 42;
constexpr int DASHBOARD_WEIGHT_X = 83;
constexpr int DASHBOARD_SCALE_A_WEIGHT_Y = 57;
constexpr int DASHBOARD_SCALE_B_WEIGHT_Y = 122;
constexpr int DASHBOARD_TOTAL_WEIGHT_Y = 184;
constexpr int16_t DASHBOARD_SCALE_PANEL_Y[] = {47, 112};
constexpr uint8_t BACKLIGHT_PWM_CHANNEL = 0;
constexpr uint32_t BACKLIGHT_PWM_FREQUENCY_HZ = 5000;
constexpr uint8_t BACKLIGHT_PWM_RESOLUTION_BITS = 8;
constexpr uint8_t MIN_BRIGHTNESS_PERCENT = 10;
constexpr uint8_t MAX_BRIGHTNESS_PERCENT = 100;
constexpr uint8_t BRIGHTNESS_STEP_PERCENT = 10;
constexpr uint8_t DEFAULT_BRIGHTNESS_PERCENT = 100;
constexpr uint16_t DEFAULT_CPU_FREQUENCY_MHZ = 240;
constexpr uint8_t WEB_MODE_MAX_BRIGHTNESS_PERCENT = 20;

// The phone connects directly to this access point. Internet and a router are
// not required; the touchscreen remains the only BLE master for both scales.
constexpr char PHONE_WIFI_SSID[] = "WEIGHING_SCALE";
constexpr char PHONE_WIFI_PASSWORD[] = "ScaleControl";
constexpr uint8_t PHONE_WIFI_CHANNEL = 6;
constexpr uint8_t PHONE_WIFI_MAX_CLIENTS = 2;
constexpr uint32_t PHONE_CLIENT_CHECK_INTERVAL_MS = 1000;
constexpr uint32_t PHONE_ACTION_MIN_INTERVAL_MS = 250;
constexpr uint32_t PHONE_WEB_ACTIVITY_LIVE_MS = 5000;
constexpr uint32_t PHONE_WIFI_STOP_DELAY_MS = 1500;
constexpr uint8_t HISTORY_PAGE_DEFAULT_LIMIT = 10;
constexpr uint8_t HISTORY_PAGE_MAX_LIMIT = 20;
constexpr uint64_t HISTORY_MIN_TIMESTAMP_MS = 1577836800000ULL;
constexpr uint64_t HISTORY_MAX_TIMESTAMP_MS = 4102444800000ULL;
constexpr int16_t HISTORY_MIN_TIMEZONE_OFFSET_MINUTES = -840;
constexpr int16_t HISTORY_MAX_TIMEZONE_OFFSET_MINUTES = 840;

constexpr char LOCAL_BLUETOOTH_NAME[] = "LOAD_CELL_DISPLAY";
constexpr char SETTINGS_NAMESPACE[] = "display_ui";
constexpr char BRIGHTNESS_KEY[] = "brightness";
constexpr char CPU_FREQUENCY_KEY[] = "cpu_mhz";
constexpr char OPERATING_MODE_KEY[] = "op_mode";
constexpr uint32_t RECONNECT_INTERVAL_MS = 3000;
constexpr uint32_t BLE_CONNECT_TIMEOUT_MS = 5000;
constexpr uint32_t BLE_SCAN_WINDOW_MS = 4000;
constexpr uint32_t BLE_SCAN_PAUSE_MS = 6000;
constexpr uint8_t MAX_ADDRESS_CONNECT_FAILURES = 3;
constexpr uint32_t TELEMETRY_STALE_MS = 2500;
constexpr uint32_t TELEMETRY_RECONNECT_MS = 8000;
constexpr uint32_t DUAL_SAMPLE_MAX_SKEW_MS = 1000;
constexpr uint32_t COMMAND_ACK_TIMEOUT_MS = 12000;
constexpr uint8_t REQUIRED_GOOD_PACKETS = 2;
constexpr uint32_t SCREEN_TIMEOUT_MS = 300000;
constexpr uint32_t WEB_MODE_SCREEN_TIMEOUT_MS = 300000;
constexpr uint32_t SCREEN_IDLE_WARNING_MS = 3000;
constexpr int32_t WEIGHT_LOCK_THRESHOLD_GRAMS = 3000;
constexpr uint32_t WEIGHT_LOCK_SETTLE_MS = 2500;
constexpr uint32_t WEIGHT_LOCK_SAMPLE_INTERVAL_MS = 500;
constexpr uint8_t WEIGHT_LOCK_MIN_SAMPLES = 5;
constexpr uint8_t WEIGHT_LOCK_SAMPLE_CAPACITY = 8;
// Allow an inclusive 1.00 kg peak-to-peak noise window while settling.
// For example, samples from 5.50 kg through 6.50 kg may lock; a sample
// outside that accumulated window restarts the complete settling timer.
constexpr int32_t WEIGHT_LOCK_NOISE_WINDOW_GRAMS = 1000;
constexpr uint32_t WEIGHT_LOCK_SYNC_RETRY_MS = 500;
constexpr uint32_t SCALE_BORDER_SERVICE_INTERVAL_MS = 50;
constexpr uint32_t SCALE_BORDER_SEARCH_BLINK_MS = 700;
constexpr uint32_t SCALE_BORDER_ATTENTION_BLINK_MS = 900;
constexpr uint32_t SCALE_BORDER_ERROR_BLINK_MS = 800;
constexpr uint32_t CALIBRATION_REPEAT_DELAY_MS = 500;
constexpr uint32_t CALIBRATION_REPEAT_INTERVAL_MS = 150;

constexpr uint16_t COLOR_BACKGROUND = 0x0083;
constexpr uint16_t COLOR_PANEL = 0x08E5;
constexpr uint16_t COLOR_PANEL_LIGHT = 0x1188;
constexpr uint16_t COLOR_PRIMARY = 0x047E;
constexpr uint16_t COLOR_SUCCESS = 0x3EEE;
constexpr uint16_t COLOR_WARNING = 0xF5A6;
constexpr uint16_t COLOR_DANGER = 0xF228;
constexpr uint16_t COLOR_MUTED = 0x8D16;

struct RemoteScaleDefinition {
  const char *bluetoothName;
};

constexpr RemoteScaleDefinition REMOTE_SCALES[] = {
    {"LOAD_CELL_1"},
    {"LOAD_CELL_2"},
};
constexpr uint8_t REMOTE_SCALE_COUNT =
    sizeof(REMOTE_SCALES) / sizeof(REMOTE_SCALES[0]);
static_assert(NIMBLE_MAX_CONNECTIONS >= REMOTE_SCALE_COUNT,
              "NimBLE must support both load-cell connections.");

TFT_eSPI tft;
TFT_eSprite weightSprite(&tft);
Preferences settingsPreferences;
WebServer phoneWebServer(80);
ScaleHistory::Store historyStore;

struct TouchSample {
  uint16_t x = 0;
  uint16_t y = 0;
};

struct CalibrationAdjustButtonDefinition {
  int16_t x;
  int16_t y;
  int16_t width;
  int16_t height;
  float adjustmentKg;
  const char *label;
};

constexpr CalibrationAdjustButtonDefinition CALIBRATION_ADJUST_BUTTONS[] = {
    {4, 194, 55, 32, -1.0f, "-1"},
    {63, 194, 55, 32, -0.1f, "-0.1"},
    {122, 194, 55, 32, 0.1f, "+0.1"},
    {181, 194, 55, 32, 1.0f, "+1"},
};
constexpr uint8_t CALIBRATION_ADJUST_BUTTON_COUNT =
    sizeof(CALIBRATION_ADJUST_BUTTONS) /
    sizeof(CALIBRATION_ADJUST_BUTTONS[0]);
constexpr uint8_t NO_CALIBRATION_ADJUST_BUTTON = UINT8_MAX;

enum class Page {
  Dashboard,
  Calibration,
  Settings,
  Connectivity,
  ModeSelection,
  WebStatus,
};

enum class OperatingMode : uint8_t {
  Local = 0,
  Web = 1,
  Hybrid = 2,
};

enum class CalibrationRepeatPhase : uint8_t {
  Idle,
  Waiting,
  Repeating,
  CancelledUntilRelease,
};

bool touchReady = false;
bool touchWasDown = false;
volatile bool screenSleeping = false;
bool idleWarningActive = false;
bool backlightPwmReady = false;
bool settingsPreferencesReady = false;
bool bluetoothReady = false;
bool phoneControlReady = false;
bool phoneRoutesConfigured = false;
bool phoneWifiStopPending = false;
bool weightValid = false;
bool pageDirty = true;
bool uiDirty = true;
bool weightSpriteReady = false;
bool dashboardWeightDirty = true;
bool dashboardLinkDirty = true;
bool dashboardStatusDirty = true;
bool dashboardPrimaryActionDirty = true;
bool dashboardScaleDirty[REMOTE_SCALE_COUNT] = {true, true};
bool dashboardScaleWeightDirty[REMOTE_SCALE_COUNT] = {true, true};
uint16_t dashboardScaleBorderDrawnColor[REMOTE_SCALE_COUNT] = {
    UINT16_MAX,
    UINT16_MAX,
};
uint32_t lastScaleBorderServiceMs = 0;
CalibrationRepeatPhase calibrationRepeatPhase =
    CalibrationRepeatPhase::Idle;
uint8_t activeCalibrationAdjustButton = NO_CALIBRATION_ADJUST_BUTTON;
uint32_t nextCalibrationRepeatAtMs = 0;

Page currentPage = Page::Dashboard;
OperatingMode operatingMode = OperatingMode::Hybrid;
float latestWeightKg = 0.0f;
float calibrationWeightKg = 1.0f;
uint8_t brightnessPercent = DEFAULT_BRIGHTNESS_PERCENT;
uint8_t calibrationTargetIndex = 0;
uint16_t cpuFrequencyMhz = DEFAULT_CPU_FREQUENCY_MHZ;
String noticeMessage;

uint32_t lastTouchActionMs = 0;
uint32_t lastUserActivityMs = 0;
uint32_t noticeUntilMs = 0;
uint32_t lastConnectivityRedrawMs = 0;
uint32_t lastPhoneClientCheckMs = 0;
uint32_t lastPhoneActionMs = 0;
uint32_t lastPhoneWebActivityMs = 0;
uint32_t phoneWifiStopAtMs = 0;
uint32_t lastWebStatusRedrawMs = 0;
uint8_t phoneClientCount = 0;
uint8_t idleWarningSecondsShown = 0;
uint32_t currentWeightLockToken = 0;
bool currentWeightLockSaved = false;
uint32_t currentWeightLockRecordId = 0;

void configurePhoneRoutes();
bool beginPhoneControl();
void stopPhoneControl();
void applyOperatingMode(OperatingMode mode, bool requestedFromPhone = false);

enum class CombinedMode : uint8_t {
  None,
  Dual,
  Scale1Only,
  Scale2Only,
};

enum class WeightLockPhase : uint8_t {
  Unlocked,
  Settling,
  Locked,
};

enum class ScaleBorderMode : uint8_t {
  Searching,
  Ready,
  Attention,
  Error,
  Locked,
  Inactive,
};

struct ControlActionResult {
  bool accepted = false;
  uint16_t httpStatus = 409;
  String message;
};

struct WeightSnapshot {
  CombinedMode mode = CombinedMode::None;
  int32_t totalGrams = 0;
  int32_t scaleGrams[REMOTE_SCALE_COUNT] = {};
  bool scaleIncluded[REMOTE_SCALE_COUNT] = {};
};

struct ScaleState {
  bool discovered = false;
  bool connecting = false;
  bool connected = false;
  bool identityVerified = false;
  bool subscribed = false;
  bool reconnectRequested = false;
  uint8_t goodPacketCount = 0;
  uint8_t flags = 0;
  uint8_t commandResult =
      static_cast<uint8_t>(DualScaleBle::CommandResult::None);
  uint16_t acknowledgedToken = 0;
  uint16_t pendingToken = 0;
  uint32_t pendingCommandAtMs = 0;
  DualScaleBle::CommandType pendingCommand =
      DualScaleBle::CommandType::Status;
  DualScaleBle::Status status = DualScaleBle::Status::Starting;
  int32_t weightGrams = 0;
  uint32_t sequence = 0;
  uint32_t connectedAtMs = 0;
  uint32_t lastTelemetryMs = 0;
  uint8_t unlockRequestGeneration = 0;
  uint8_t handledUnlockRequestGeneration = 0;
  bool unlockRequestHandled = false;
};

ScaleState scaleStates[REMOTE_SCALE_COUNT];
CombinedMode combinedMode = CombinedMode::None;
int32_t latestCombinedGrams = 0;
WeightLockPhase weightLockPhase = WeightLockPhase::Unlocked;
WeightSnapshot lockedWeightSnapshot;
WeightSnapshot weightLockSamples[WEIGHT_LOCK_SAMPLE_CAPACITY];
uint8_t weightLockSampleCount = 0;
uint32_t weightLockSettlingStartedMs = 0;
uint32_t lastWeightLockSampleMs = 0;
uint32_t lastWeightLockSampleSequence[REMOTE_SCALE_COUNT] = {};

enum class BleManagerMessageType : uint8_t {
  Discovered,
  Disconnected,
  SendCommand,
  ForceCheck,
  Reconnect,
};

struct BleManagerMessage {
  BleManagerMessageType type = BleManagerMessageType::ForceCheck;
  uint8_t scaleIndex = 0;
  uint8_t addressType = 0;
  uint64_t addressValue = 0;
  DualScaleBle::CommandPacket command = {};
};

enum class BleUiEventType : uint8_t {
  RadioReady,
  Discovered,
  Connecting,
  Connected,
  Disconnected,
  Telemetry,
  CommandWriteFailed,
};

struct BleUiEvent {
  BleUiEventType type = BleUiEventType::RadioReady;
  uint8_t scaleIndex = 0;
  int reason = 0;
  DualScaleBle::TelemetryPacket telemetry = {};
};

struct BleTransportSlot {
  NimBLEAddress address;
  bool addressKnown = false;
  bool connected = false;
  uint8_t consecutiveConnectFailures = 0;
  uint32_t nextAttemptMs = 0;
  NimBLEClient *client = nullptr;
  NimBLERemoteCharacteristic *telemetryCharacteristic = nullptr;
  NimBLERemoteCharacteristic *commandCharacteristic = nullptr;
};

BleTransportSlot bleTransport[REMOTE_SCALE_COUNT];
QueueHandle_t bleManagerQueue = nullptr;
QueueHandle_t bleUiQueue = nullptr;
uint16_t nextCommandToken = 1;
bool bleForceScanRequested = false;
bool peerStatusSyncPending[REMOTE_SCALE_COUNT] = {true, true};
bool weightLockSyncPending[REMOTE_SCALE_COUNT] = {true, true};
uint32_t weightLockSyncRetryAtMs[REMOTE_SCALE_COUNT] = {};

bool readTouchRegisters(uint8_t firstRegister, uint8_t *buffer, size_t length) {
  Wire.beginTransmission(CST820_ADDRESS);
  Wire.write(firstRegister);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  const uint8_t requested = static_cast<uint8_t>(length);
  const uint8_t received = Wire.requestFrom(CST820_ADDRESS, requested);
  if (received != requested) {
    while (Wire.available()) {
      Wire.read();
    }
    return false;
  }

  for (size_t i = 0; i < length; ++i) {
    buffer[i] = static_cast<uint8_t>(Wire.read());
  }
  return true;
}

bool writeTouchRegister(uint8_t registerAddress, uint8_t value) {
  Wire.beginTransmission(CST820_ADDRESS);
  Wire.write(registerAddress);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool touchControllerPresent() {
  Wire.beginTransmission(CST820_ADDRESS);
  return Wire.endTransmission() == 0;
}

bool beginTouchController() {
  Wire.begin(TOUCH_SDA_PIN, TOUCH_SCL_PIN);
  Wire.setClock(400000);

  // CST820 wake/reset sequence used by this board.
  pinMode(TOUCH_INTERRUPT_PIN, OUTPUT);
  digitalWrite(TOUCH_INTERRUPT_PIN, HIGH);
  delay(1);
  digitalWrite(TOUCH_INTERRUPT_PIN, LOW);
  delay(1);

  pinMode(TOUCH_RESET_PIN, OUTPUT);
  digitalWrite(TOUCH_RESET_PIN, LOW);
  delay(10);
  digitalWrite(TOUCH_RESET_PIN, HIGH);
  delay(300);
  pinMode(TOUCH_INTERRUPT_PIN, INPUT_PULLUP);

  if (!touchControllerPresent()) {
    Serial.println("ERROR: CST820 touch controller not found at 0x15.");
    return false;
  }

  // Keep the controller awake.
  writeTouchRegister(0xFE, 0xFF);
  Serial.println("CST820 touch controller found at I2C address 0x15.");
  return true;
}

bool readTouch(TouchSample &sample) {
  // Registers 0x01..0x06 contain gesture, point count, X, and Y.
  uint8_t data[6] = {};
  if (!readTouchRegisters(0x01, data, sizeof(data))) {
    return false;
  }

  if ((data[1] & 0x0F) == 0) {
    return false;
  }

  const uint16_t rawX =
      static_cast<uint16_t>(((data[2] & 0x0F) << 8) | data[3]);
  const uint16_t rawY =
      static_cast<uint16_t>(((data[4] & 0x0F) << 8) | data[5]);

  // Rotation 0 is portrait and matches the CST820 axes on this board.
  sample.x = constrain(rawX, 0, SCREEN_WIDTH - 1);
  sample.y = constrain(rawY, 0, SCREEN_HEIGHT - 1);
  return true;
}

void setCenteredText(const String &text,
                     int16_t centerX,
                     int16_t centerY,
                     uint8_t size,
                     uint16_t foreground,
                     uint16_t background) {
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(size);
  tft.setTextColor(foreground, background);
  tft.drawString(text, centerX, centerY);
  tft.setTextDatum(TL_DATUM);
}

void drawButton(int16_t x,
                int16_t y,
                int16_t width,
                int16_t height,
                const String &label,
                uint16_t fill,
                uint16_t border = TFT_WHITE,
                uint8_t textSize = 2) {
  tft.fillRoundRect(x, y, width, height, 7, fill);
  tft.drawRoundRect(x, y, width, height, 7, border);
  setCenteredText(label,
                  x + width / 2,
                  y + height / 2,
                  textSize,
                  TFT_WHITE,
                  fill);
}

void drawCalibrationAdjustButton(uint8_t index, bool pressed) {
  if (index >= CALIBRATION_ADJUST_BUTTON_COUNT) {
    return;
  }

  const CalibrationAdjustButtonDefinition &button =
      CALIBRATION_ADJUST_BUTTONS[index];
  drawButton(button.x,
             button.y,
             button.width,
             button.height,
             button.label,
             pressed ? COLOR_PRIMARY : COLOR_PANEL_LIGHT,
             pressed ? COLOR_PRIMARY : COLOR_MUTED,
             1);
}

void beginWeightSprite() {
  weightSprite.setColorDepth(16);
  weightSpriteReady =
      weightSprite.createSprite(WEIGHT_SPRITE_WIDTH,
                                WEIGHT_SPRITE_HEIGHT) != nullptr;
  if (weightSpriteReady) {
    Serial.printf("Weight sprite ready: %d x %d, %lu bytes.\n",
                  WEIGHT_SPRITE_WIDTH,
                  WEIGHT_SPRITE_HEIGHT,
                  static_cast<unsigned long>(WEIGHT_SPRITE_WIDTH) *
                      WEIGHT_SPRITE_HEIGHT * 2UL);
  } else {
    Serial.println(
        "Warning: weight sprite allocation failed; using direct updates.");
  }
}

bool pointInRect(uint16_t x,
                 uint16_t y,
                 int16_t left,
                 int16_t top,
                 int16_t width,
                 int16_t height) {
  return x >= left && x < left + width && y >= top && y < top + height;
}

int8_t calibrationAdjustButtonAt(const TouchSample &sample) {
  for (uint8_t index = 0;
       index < CALIBRATION_ADJUST_BUTTON_COUNT;
       ++index) {
    const CalibrationAdjustButtonDefinition &button =
        CALIBRATION_ADJUST_BUTTONS[index];
    if (pointInRect(sample.x,
                    sample.y,
                    button.x,
                    button.y,
                    button.width,
                    button.height)) {
      return static_cast<int8_t>(index);
    }
  }
  return -1;
}

bool touchRemainsOnCalibrationAdjustButton(const TouchSample &sample) {
  if (activeCalibrationAdjustButton >=
      CALIBRATION_ADJUST_BUTTON_COUNT) {
    return false;
  }

  const CalibrationAdjustButtonDefinition &button =
      CALIBRATION_ADJUST_BUTTONS[activeCalibrationAdjustButton];
  return pointInRect(sample.x,
                     sample.y,
                     button.x,
                     button.y,
                     button.width,
                     button.height);
}

uint8_t connectedScaleCount() {
  uint8_t count = 0;
  for (const ScaleState &state : scaleStates) {
    if (state.connected) {
      ++count;
    }
  }
  return count;
}

String connectionLabel() {
  if (!bluetoothReady) {
    return "BT ERROR";
  }
  return String(connectedScaleCount()) + "/2";
}

uint16_t connectionColor() {
  const uint8_t count = connectedScaleCount();
  return count == 2 ? COLOR_SUCCESS
                    : (bluetoothReady ? COLOR_WARNING : COLOR_DANGER);
}

bool requiredTelemetryFlagsPresent(const ScaleState &state) {
  constexpr uint8_t REQUIRED_FLAGS =
      DualScaleBle::TelemetryFlag::SensorPresent |
      DualScaleBle::TelemetryFlag::AdcResponding |
      DualScaleBle::TelemetryFlag::Tared |
      DualScaleBle::TelemetryFlag::Calibrated |
      DualScaleBle::TelemetryFlag::WeightValid;
  return (state.flags & REQUIRED_FLAGS) == REQUIRED_FLAGS;
}

bool scaleUsable(uint8_t index, uint32_t now = millis()) {
  const ScaleState &state = scaleStates[index];
  return state.connected &&
         state.identityVerified &&
         state.subscribed &&
         state.status == DualScaleBle::Status::Ready &&
         requiredTelemetryFlagsPresent(state) &&
         state.goodPacketCount >= REQUIRED_GOOD_PACKETS &&
         state.lastTelemetryMs != 0 &&
         now - state.lastTelemetryMs <= TELEMETRY_STALE_MS;
}

void recomputeCombinedWeight() {
  const uint32_t now = millis();
  const bool scale1Usable = scaleUsable(0, now);
  const bool scale2Usable = scaleUsable(1, now);

  CombinedMode newMode = CombinedMode::None;
  int64_t combinedGrams = 0;

  if (scale1Usable && scale2Usable) {
    const uint32_t age1 = now - scaleStates[0].lastTelemetryMs;
    const uint32_t age2 = now - scaleStates[1].lastTelemetryMs;
    const uint32_t skew = age1 > age2 ? age1 - age2 : age2 - age1;
    if (skew <= DUAL_SAMPLE_MAX_SKEW_MS) {
      newMode = CombinedMode::Dual;
      combinedGrams =
          static_cast<int64_t>(scaleStates[0].weightGrams) +
          static_cast<int64_t>(scaleStates[1].weightGrams);
    } else if (age1 < age2) {
      newMode = CombinedMode::Scale1Only;
      combinedGrams = scaleStates[0].weightGrams;
    } else {
      newMode = CombinedMode::Scale2Only;
      combinedGrams = scaleStates[1].weightGrams;
    }
  } else if (scale1Usable) {
    newMode = CombinedMode::Scale1Only;
    combinedGrams = scaleStates[0].weightGrams;
  } else if (scale2Usable) {
    newMode = CombinedMode::Scale2Only;
    combinedGrams = scaleStates[1].weightGrams;
  }

  const bool newValid = newMode != CombinedMode::None;
  const int32_t newCombinedGrams =
      !newValid
          ? 0
          : combinedGrams > INT32_MAX
                ? INT32_MAX
                : combinedGrams < INT32_MIN
                      ? INT32_MIN
                      : static_cast<int32_t>(combinedGrams);
  const float newWeightKg =
      static_cast<float>(newCombinedGrams) / 1000.0f;
  const bool modeChanged = newMode != combinedMode;
  const bool validityChanged = newValid != weightValid;
  const bool weightChanged =
      newCombinedGrams != latestCombinedGrams;
  if (modeChanged || validityChanged || weightChanged) {
    combinedMode = newMode;
    weightValid = newValid;
    latestCombinedGrams = newCombinedGrams;
    latestWeightKg = newWeightKg;
    if (weightLockPhase != WeightLockPhase::Locked) {
      dashboardWeightDirty = true;
    }
    if (modeChanged || validityChanged) {
      if (weightLockPhase != WeightLockPhase::Locked) {
        dashboardStatusDirty = true;
      }
      for (uint8_t index = 0; index < REMOTE_SCALE_COUNT; ++index) {
        dashboardScaleDirty[index] = true;
      }
    }
    if (currentPage == Page::Dashboard) {
      uiDirty = true;
    }
  }
}

bool combinedModeIncludesScale(CombinedMode mode, uint8_t scaleIndex) {
  return mode == CombinedMode::Dual ||
         (mode == CombinedMode::Scale1Only && scaleIndex == 0) ||
         (mode == CombinedMode::Scale2Only && scaleIndex == 1);
}

WeightSnapshot currentLiveWeightSnapshot() {
  WeightSnapshot snapshot;
  if (!weightValid || combinedMode == CombinedMode::None) {
    return snapshot;
  }

  snapshot.mode = combinedMode;
  snapshot.totalGrams = latestCombinedGrams;
  for (uint8_t index = 0; index < REMOTE_SCALE_COUNT; ++index) {
    snapshot.scaleIncluded[index] =
        combinedModeIncludesScale(combinedMode, index);
    if (snapshot.scaleIncluded[index]) {
      snapshot.scaleGrams[index] = scaleStates[index].weightGrams;
    }
  }
  return snapshot;
}

void markWeightLockUiDirty() {
  dashboardWeightDirty = true;
  dashboardStatusDirty = true;
  dashboardPrimaryActionDirty = true;
  for (bool &dirty : dashboardScaleDirty) {
    dirty = true;
  }
  if (currentPage == Page::Dashboard) {
    uiDirty = true;
  }
}

void markWeightLockSyncPending() {
  for (uint8_t index = 0; index < REMOTE_SCALE_COUNT; ++index) {
    weightLockSyncPending[index] = true;
    weightLockSyncRetryAtMs[index] = 0;
  }
}

void clearWeightLockSamples() {
  weightLockSampleCount = 0;
  weightLockSettlingStartedMs = 0;
  lastWeightLockSampleMs = 0;
  for (uint32_t &sequence : lastWeightLockSampleSequence) {
    sequence = 0;
  }
}

void appendWeightLockSample(const WeightSnapshot &snapshot) {
  if (weightLockSampleCount < WEIGHT_LOCK_SAMPLE_CAPACITY) {
    weightLockSamples[weightLockSampleCount++] = snapshot;
    return;
  }

  for (uint8_t index = 1;
       index < WEIGHT_LOCK_SAMPLE_CAPACITY;
       ++index) {
    weightLockSamples[index - 1] = weightLockSamples[index];
  }
  weightLockSamples[WEIGHT_LOCK_SAMPLE_CAPACITY - 1] = snapshot;
}

WeightSnapshot medianWeightLockSample() {
  uint8_t order[WEIGHT_LOCK_SAMPLE_CAPACITY] = {};
  for (uint8_t index = 0; index < weightLockSampleCount; ++index) {
    order[index] = index;
  }

  for (uint8_t index = 1; index < weightLockSampleCount; ++index) {
    const uint8_t selected = order[index];
    int8_t insertAt = static_cast<int8_t>(index) - 1;
    while (insertAt >= 0 &&
           weightLockSamples[order[insertAt]].totalGrams >
               weightLockSamples[selected].totalGrams) {
      order[insertAt + 1] = order[insertAt];
      --insertAt;
    }
    order[insertAt + 1] = selected;
  }

  return weightLockSamples[order[weightLockSampleCount / 2]];
}

void rememberWeightLockSampleSequences(CombinedMode mode) {
  for (uint8_t index = 0; index < REMOTE_SCALE_COUNT; ++index) {
    if (combinedModeIncludesScale(mode, index)) {
      lastWeightLockSampleSequence[index] =
          scaleStates[index].sequence;
    }
  }
}

bool weightLockContributorsHaveFreshTelemetry(CombinedMode mode) {
  for (uint8_t index = 0; index < REMOTE_SCALE_COUNT; ++index) {
    if (combinedModeIncludesScale(mode, index) &&
        scaleStates[index].sequence ==
            lastWeightLockSampleSequence[index]) {
      return false;
    }
  }
  return true;
}

void beginWeightLockSettling(const WeightSnapshot &firstSample,
                             const char *reason) {
  clearWeightLockSamples();
  weightLockPhase = WeightLockPhase::Settling;
  weightLockSettlingStartedMs = millis();
  lastWeightLockSampleMs = millis();
  appendWeightLockSample(firstSample);
  rememberWeightLockSampleSequences(firstSample.mode);
  dashboardStatusDirty = true;
  if (currentPage == Page::Dashboard) {
    uiDirty = true;
  }
  Serial.print("Weight lock settling started");
  if (reason != nullptr && reason[0] != '\0') {
    Serial.print(" (");
    Serial.print(reason);
    Serial.print(")");
  }
  Serial.println(".");
}

void unlockCombinedWeight(const char *reason) {
  const bool stateWasVisible =
      weightLockPhase != WeightLockPhase::Unlocked;
  weightLockPhase = WeightLockPhase::Unlocked;
  clearWeightLockSamples();
  markWeightLockSyncPending();
  if (stateWasVisible) {
    markWeightLockUiDirty();
  } else {
    dashboardStatusDirty = true;
    if (currentPage == Page::Dashboard) {
      uiDirty = true;
    }
  }

  Serial.print("Combined weight unlocked and re-armed");
  if (reason != nullptr && reason[0] != '\0') {
    Serial.print(" (");
    Serial.print(reason);
    Serial.print(")");
  }
  Serial.println(".");
}

void lockCombinedWeight(const WeightSnapshot &snapshot) {
  lockedWeightSnapshot = snapshot;
  weightLockPhase = WeightLockPhase::Locked;
  ++currentWeightLockToken;
  if (currentWeightLockToken == 0) {
    currentWeightLockToken = 1;
  }
  currentWeightLockSaved = false;
  currentWeightLockRecordId = 0;
  clearWeightLockSamples();
  markWeightLockSyncPending();
  markWeightLockUiDirty();
  Serial.printf("Combined weight LOCKED at %.2f kg (%s).\n",
                static_cast<float>(snapshot.totalGrams) / 1000.0f,
                snapshot.mode == CombinedMode::Dual
                    ? "A+B"
                    : snapshot.mode == CombinedMode::Scale1Only
                          ? "A only"
                          : "B only");
}

bool scaleOperationCommandPending() {
  for (const ScaleState &state : scaleStates) {
    if (state.pendingToken != 0 &&
        (state.pendingCommand == DualScaleBle::CommandType::Tare ||
         state.pendingCommand ==
             DualScaleBle::CommandType::Calibrate)) {
      return true;
    }
  }
  return false;
}

bool weightLockContributorsReadyToArm() {
  constexpr uint8_t BLOCKING_FLAGS =
      DualScaleBle::TelemetryFlag::WeightLocked |
      DualScaleBle::TelemetryFlag::UnlockRequested;
  for (uint8_t index = 0; index < REMOTE_SCALE_COUNT; ++index) {
    if (combinedModeIncludesScale(combinedMode, index) &&
        (scaleStates[index].flags & BLOCKING_FLAGS) != 0) {
      return false;
    }
  }
  return true;
}

void serviceAutomaticWeightLock() {
  if (weightLockPhase == WeightLockPhase::Locked) {
    return;
  }

  const bool eligible =
      weightValid &&
      combinedMode != CombinedMode::None &&
      latestCombinedGrams > WEIGHT_LOCK_THRESHOLD_GRAMS &&
      !scaleOperationCommandPending() &&
      weightLockContributorsReadyToArm();
  if (!eligible) {
    if (weightLockPhase == WeightLockPhase::Settling) {
      weightLockPhase = WeightLockPhase::Unlocked;
      clearWeightLockSamples();
      dashboardStatusDirty = true;
      if (currentPage == Page::Dashboard) {
        uiDirty = true;
      }
      Serial.println(
          "Weight lock settling cancelled: eligibility was lost.");
    }
    return;
  }

  const WeightSnapshot liveSnapshot = currentLiveWeightSnapshot();
  if (weightLockPhase == WeightLockPhase::Unlocked) {
    beginWeightLockSettling(liveSnapshot, "weight above 3 kg");
    return;
  }

  if (weightLockSampleCount == 0 ||
      liveSnapshot.mode != weightLockSamples[0].mode) {
    beginWeightLockSettling(liveSnapshot, "contributing scales changed");
    return;
  }

  const uint32_t now = millis();
  if (now - lastWeightLockSampleMs <
      WEIGHT_LOCK_SAMPLE_INTERVAL_MS) {
    return;
  }
  if (!weightLockContributorsHaveFreshTelemetry(liveSnapshot.mode)) {
    return;
  }
  lastWeightLockSampleMs = now;
  rememberWeightLockSampleSequences(liveSnapshot.mode);

  int32_t minimumGrams = liveSnapshot.totalGrams;
  int32_t maximumGrams = liveSnapshot.totalGrams;
  for (uint8_t index = 0; index < weightLockSampleCount; ++index) {
    minimumGrams =
        min(minimumGrams, weightLockSamples[index].totalGrams);
    maximumGrams =
        max(maximumGrams, weightLockSamples[index].totalGrams);
  }

  const int64_t candidateRangeGrams =
      static_cast<int64_t>(maximumGrams) - minimumGrams;
  if (candidateRangeGrams > WEIGHT_LOCK_NOISE_WINDOW_GRAMS) {
    beginWeightLockSettling(liveSnapshot,
                            "noise exceeded 1 kg window");
    return;
  }

  appendWeightLockSample(liveSnapshot);
  if (now - weightLockSettlingStartedMs < WEIGHT_LOCK_SETTLE_MS ||
      weightLockSampleCount < WEIGHT_LOCK_MIN_SAMPLES) {
    return;
  }

  const WeightSnapshot lockedSample = medianWeightLockSample();
  lockCombinedWeight(lockedSample);
}

bool noticeActive() {
  return !noticeMessage.isEmpty() &&
         static_cast<int32_t>(noticeUntilMs - millis()) > 0;
}

void showNotice(const String &message, uint32_t durationMs = 2500) {
  noticeMessage = message;
  noticeUntilMs = millis() + durationMs;
  dashboardStatusDirty = true;
  uiDirty = true;
}

void markConnectionUiDirty(int8_t scaleIndex = -1) {
  dashboardLinkDirty = true;
  dashboardStatusDirty = true;
  if (scaleIndex >= 0 && scaleIndex < REMOTE_SCALE_COUNT) {
    dashboardScaleDirty[scaleIndex] = true;
  } else {
    for (uint8_t index = 0; index < REMOTE_SCALE_COUNT; ++index) {
      dashboardScaleDirty[index] = true;
    }
  }
  // Calibration controls also depend on each scale's live connection state.
  if (currentPage != Page::Settings) {
    uiDirty = true;
  }
}

void changePage(Page page) {
  if (currentPage == page) {
    return;
  }
  currentPage = page;
  pageDirty = true;
  uiDirty = true;
}

bool supportedOperatingMode(uint8_t value) {
  return value <= static_cast<uint8_t>(OperatingMode::Hybrid);
}

const char *operatingModeName(OperatingMode mode) {
  switch (mode) {
  case OperatingMode::Local:
    return "LOCAL";
  case OperatingMode::Web:
    return "WEB";
  case OperatingMode::Hybrid:
  default:
    return "HYBRID";
  }
}

const char *operatingModeApiName(OperatingMode mode) {
  switch (mode) {
  case OperatingMode::Local:
    return "local";
  case OperatingMode::Web:
    return "web";
  case OperatingMode::Hybrid:
  default:
    return "hybrid";
  }
}

bool operatingModeUsesWifi(OperatingMode mode) {
  return mode != OperatingMode::Local;
}

uint32_t activeScreenTimeoutMs() {
  return operatingMode == OperatingMode::Web
             ? WEB_MODE_SCREEN_TIMEOUT_MS
             : SCREEN_TIMEOUT_MS;
}

bool supportedCpuFrequency(uint16_t frequencyMhz) {
  return frequencyMhz == 160 || frequencyMhz == 240;
}

uint8_t effectiveBrightnessPercent() {
  return operatingMode == OperatingMode::Web
             ? min(brightnessPercent, WEB_MODE_MAX_BRIGHTNESS_PERCENT)
             : brightnessPercent;
}

uint8_t brightnessDutyCycle() {
  const uint8_t effectiveBrightness = effectiveBrightnessPercent();
  return static_cast<uint8_t>(
      (static_cast<uint16_t>(effectiveBrightness) * 255U + 50U) / 100U);
}

void applyBacklightOutput(bool enabled) {
  const uint8_t duty = enabled ? brightnessDutyCycle() : 0;
  if (backlightPwmReady) {
    ledcWrite(BACKLIGHT_PWM_CHANNEL, duty);
  } else {
    digitalWrite(TFT_BACKLIGHT_PIN, enabled ? HIGH : LOW);
  }
}

void beginBacklightControl() {
  const uint32_t actualFrequency =
      ledcSetup(BACKLIGHT_PWM_CHANNEL,
                BACKLIGHT_PWM_FREQUENCY_HZ,
                BACKLIGHT_PWM_RESOLUTION_BITS);
  if (actualFrequency == 0) {
    Serial.println("Warning: backlight PWM setup failed; using on/off control.");
    return;
  }

  ledcAttachPin(TFT_BACKLIGHT_PIN, BACKLIGHT_PWM_CHANNEL);
  backlightPwmReady = true;
  ledcWrite(BACKLIGHT_PWM_CHANNEL, 0);
}

void loadDisplaySettings() {
  settingsPreferencesReady =
      settingsPreferences.begin(SETTINGS_NAMESPACE, false);
  if (!settingsPreferencesReady) {
    Serial.println("Warning: display settings storage is unavailable.");
  } else {
    brightnessPercent =
        settingsPreferences.getUChar(BRIGHTNESS_KEY,
                                     DEFAULT_BRIGHTNESS_PERCENT);
    brightnessPercent = static_cast<uint8_t>(
        constrain(static_cast<int>(brightnessPercent),
                  static_cast<int>(MIN_BRIGHTNESS_PERCENT),
                  static_cast<int>(MAX_BRIGHTNESS_PERCENT)));

    const uint16_t storedCpuFrequency =
        settingsPreferences.getUShort(CPU_FREQUENCY_KEY,
                                      DEFAULT_CPU_FREQUENCY_MHZ);
    cpuFrequencyMhz = supportedCpuFrequency(storedCpuFrequency)
                          ? storedCpuFrequency
                          : DEFAULT_CPU_FREQUENCY_MHZ;

    const uint8_t storedOperatingMode =
        settingsPreferences.getUChar(
            OPERATING_MODE_KEY,
            static_cast<uint8_t>(OperatingMode::Hybrid));
    operatingMode =
        supportedOperatingMode(storedOperatingMode)
            ? static_cast<OperatingMode>(storedOperatingMode)
            : OperatingMode::Hybrid;

  }

  if (!setCpuFrequencyMhz(cpuFrequencyMhz)) {
    Serial.println("Warning: saved CPU frequency could not be applied.");
  }
  cpuFrequencyMhz = static_cast<uint16_t>(getCpuFrequencyMhz());

  Serial.printf("Display brightness: %u%%\n", brightnessPercent);
  Serial.printf("CPU frequency: %u MHz\n", cpuFrequencyMhz);
  Serial.print("Operating mode: ");
  Serial.println(operatingModeName(operatingMode));
}

void adjustBrightness(int amountPercent) {
  const int adjustedPercent =
      constrain(static_cast<int>(brightnessPercent) + amountPercent,
                static_cast<int>(MIN_BRIGHTNESS_PERCENT),
                static_cast<int>(MAX_BRIGHTNESS_PERCENT));
  if (adjustedPercent == brightnessPercent) {
    return;
  }

  brightnessPercent = static_cast<uint8_t>(adjustedPercent);
  applyBacklightOutput(!screenSleeping);
  if (settingsPreferencesReady) {
    settingsPreferences.putUChar(BRIGHTNESS_KEY, brightnessPercent);
  }
  Serial.printf("Display brightness changed to %u%%.\n", brightnessPercent);
  uiDirty = true;
}

void selectCpuFrequency(uint16_t requestedFrequencyMhz) {
  if (!supportedCpuFrequency(requestedFrequencyMhz) ||
      requestedFrequencyMhz == cpuFrequencyMhz) {
    return;
  }

  if (!setCpuFrequencyMhz(requestedFrequencyMhz)) {
    Serial.printf("ERROR: Could not set CPU frequency to %u MHz.\n",
                  requestedFrequencyMhz);
    showNotice("CPU clock change failed", 4000);
    return;
  }

  cpuFrequencyMhz = static_cast<uint16_t>(getCpuFrequencyMhz());
  if (settingsPreferencesReady) {
    settingsPreferences.putUShort(CPU_FREQUENCY_KEY, cpuFrequencyMhz);
  }
  Serial.printf("CPU frequency changed to %u MHz.\n", cpuFrequencyMhz);
  uiDirty = true;
}

void drawHeader(const String &title,
                bool backButton,
                bool dashboardButtons = false) {
  tft.fillRect(0, 0, SCREEN_WIDTH, 42, COLOR_PANEL);

  if (backButton) {
    drawButton(5, 7, 55, 28, "BACK", COLOR_PANEL_LIGHT, COLOR_MUTED, 1);
    tft.setTextColor(TFT_WHITE, COLOR_PANEL);
    tft.setTextSize(2);
    tft.setCursor(70, 12);
    tft.print(title);
  } else {
    tft.setTextColor(TFT_WHITE, COLOR_PANEL);
    // Dashboard title uses the smaller size-1 font so it does not collide
    // with the LINK button (which starts at x=123) and leaves room for the
    // battery indicator at the top-right.
    tft.setTextSize(dashboardButtons ? 1 : 2);
    tft.setCursor(8, dashboardButtons ? 17 : 12);
    tft.print(title);

    if (dashboardButtons) {
      drawButton(123,
                 7,
                 51,
                 28,
                 "LINK",
                 COLOR_PANEL_LIGHT,
                 connectionColor(),
                 1);
      drawButton(181,
                 7,
                 54,
                 28,
                 "SET",
                 COLOR_PANEL_LIGHT,
                 COLOR_MUTED,
                 1);
    } else {
      const uint16_t color = connectionColor();
      tft.fillCircle(153, 21, 5, color);
      tft.setTextColor(color, COLOR_PANEL);
      tft.setTextSize(1);
      tft.setCursor(163, 17);
      tft.print(connectionLabel());
    }
  }
}

void drawStartupScreen(const String &stage, uint8_t progressPercent) {
  progressPercent = constrain(progressPercent, 0, 100);
  tft.fillScreen(COLOR_BACKGROUND);

  tft.setTextColor(COLOR_MUTED, COLOR_BACKGROUND);
  tft.setTextSize(1);
  tft.setCursor(33, 32);
  tft.print("SCHOOL OF ENGINEERING");
  tft.setCursor(54, 47);
  tft.print("TEMASEK POLYTECHNIC");
  tft.drawFastHLine(26, 67, 188, COLOR_PRIMARY);

  setCenteredText("DUAL LOAD CELL", 120, 111, 2, TFT_WHITE,
                  COLOR_BACKGROUND);
  setCenteredText("SCALE", 120, 135, 2, TFT_WHITE, COLOR_BACKGROUND);
  setCenteredText("SYSTEM STARTUP", 120, 161, 1, COLOR_PRIMARY,
                  COLOR_BACKGROUND);

  tft.fillRoundRect(18, 187, 204, 108, 8, COLOR_PANEL);
  tft.setTextColor(COLOR_MUTED, COLOR_PANEL);
  tft.setTextSize(1);
  tft.setCursor(29, 197);
  tft.print(stage);

  char progressText[8];
  snprintf(progressText, sizeof(progressText), "%u%%", progressPercent);
  tft.setTextColor(TFT_WHITE, COLOR_PANEL);
  tft.setCursor(187, 197);
  tft.print(progressText);

  tft.drawRoundRect(29, 215, 182, 12, 4, COLOR_PANEL_LIGHT);
  const int16_t progressWidth =
      static_cast<int16_t>((176L * progressPercent) / 100L);
  if (progressWidth > 0) {
    tft.fillRoundRect(32, 218, progressWidth, 6, 2, COLOR_PRIMARY);
  }

  const char *startupLabels[] = {"TOUCHSCREEN", "STORAGE", "BLE MANAGER"};
  const uint8_t readyAt[] = {55, 75, 95};
  for (uint8_t index = 0; index < 3; ++index) {
    const int16_t y = 237 + index * 17;
    const bool ready = progressPercent >= readyAt[index];
    tft.setTextColor(COLOR_MUTED, COLOR_PANEL);
    tft.setCursor(29, y);
    tft.print(startupLabels[index]);
    tft.setTextColor(ready ? COLOR_SUCCESS : COLOR_WARNING, COLOR_PANEL);
    tft.setCursor(178, y);
    tft.print(ready ? "OK" : "WAIT");
  }

  setCenteredText("Starting weighing controller...", 120, 307, 1,
                  COLOR_MUTED, COLOR_PANEL);
}

String dashboardScaleStatus(uint8_t index) {
  const ScaleState &state = scaleStates[index];
  if (weightLockPhase == WeightLockPhase::Locked &&
      !combinedModeIncludesScale(lockedWeightSnapshot.mode, index)) {
    return "NOT IN LOCK";
  }
  if (!bluetoothReady) {
    return "BT ERROR";
  }
  if (!state.connected) {
    return state.connecting ? "CONNECTING"
                            : (state.discovered ? "RETRYING" : "SEARCHING");
  }
  if (state.status == DualScaleBle::Status::NoSensor) {
    return "NO SENSOR";
  }
  if (state.status == DualScaleBle::Status::SensorError) {
    return "ADC ERROR";
  }
  if (state.status == DualScaleBle::Status::Taring) {
    return "TARING";
  }
  if (state.status == DualScaleBle::Status::Calibrating) {
    return "CALIBRATING";
  }
  if (state.status == DualScaleBle::Status::NeedsTare) {
    return "NEEDS TARE";
  }
  if (state.status == DualScaleBle::Status::NeedsCalibration) {
    return "NEEDS CAL";
  }
  if (state.status == DualScaleBle::Status::ShuttingDown) {
    return "SHUTDOWN";
  }
  if (!state.identityVerified || !state.subscribed ||
      state.lastTelemetryMs == 0) {
    return "SYNCING";
  }
  if (millis() - state.lastTelemetryMs > TELEMETRY_STALE_MS) {
    return "STALE DATA";
  }
  if (weightLockPhase == WeightLockPhase::Locked) {
    return "LOCKED";
  }
  if (state.goodPacketCount < REQUIRED_GOOD_PACKETS) {
    return "STABILIZING";
  }
  return scaleUsable(index) ? "READY" : "NOT READY";
}

uint16_t dashboardScaleColor(uint8_t index) {
  const String status = dashboardScaleStatus(index);
  if (status == "READY" || status == "LOCKED") {
    return COLOR_SUCCESS;
  }
  if (status == "BT ERROR" || status == "NO SENSOR" ||
      status == "ADC ERROR" || status == "STALE DATA") {
    return COLOR_DANGER;
  }
  return COLOR_WARNING;
}

uint16_t blendRgb565(uint16_t foreground,
                     uint16_t background,
                     uint8_t foregroundWeight) {
  const uint16_t backgroundWeight = 255U - foregroundWeight;
  const uint16_t foregroundRed = (foreground >> 11) & 0x1F;
  const uint16_t foregroundGreen = (foreground >> 5) & 0x3F;
  const uint16_t foregroundBlue = foreground & 0x1F;
  const uint16_t backgroundRed = (background >> 11) & 0x1F;
  const uint16_t backgroundGreen = (background >> 5) & 0x3F;
  const uint16_t backgroundBlue = background & 0x1F;

  const uint16_t red =
      (foregroundRed * foregroundWeight +
       backgroundRed * backgroundWeight + 127U) /
      255U;
  const uint16_t green =
      (foregroundGreen * foregroundWeight +
       backgroundGreen * backgroundWeight + 127U) /
      255U;
  const uint16_t blue =
      (foregroundBlue * foregroundWeight +
       backgroundBlue * backgroundWeight + 127U) /
      255U;
  return static_cast<uint16_t>((red << 11) | (green << 5) | blue);
}

ScaleBorderMode dashboardScaleBorderMode(uint8_t index) {
  const ScaleState &state = scaleStates[index];

  // A scale that did not contribute to the held result stays visually muted.
  if (weightLockPhase == WeightLockPhase::Locked &&
      !combinedModeIncludesScale(lockedWeightSnapshot.mode, index)) {
    return ScaleBorderMode::Inactive;
  }
  if (!bluetoothReady) {
    return ScaleBorderMode::Error;
  }
  if (!state.connected) {
    return ScaleBorderMode::Searching;
  }
  if (state.status == DualScaleBle::Status::NoSensor ||
      state.status == DualScaleBle::Status::SensorError) {
    return ScaleBorderMode::Error;
  }
  if (state.identityVerified && state.lastTelemetryMs != 0) {
    constexpr uint8_t SENSOR_HEALTH_FLAGS =
        DualScaleBle::TelemetryFlag::SensorPresent |
        DualScaleBle::TelemetryFlag::AdcResponding;
    if ((state.flags & SENSOR_HEALTH_FLAGS) != SENSOR_HEALTH_FLAGS ||
        millis() - state.lastTelemetryMs > TELEMETRY_STALE_MS) {
      return ScaleBorderMode::Error;
    }
  }
  if (state.status == DualScaleBle::Status::ShuttingDown) {
    return ScaleBorderMode::Inactive;
  }
  if (!state.identityVerified || !state.subscribed ||
      state.lastTelemetryMs == 0) {
    return ScaleBorderMode::Searching;
  }
  if (state.status == DualScaleBle::Status::NeedsTare ||
      state.status == DualScaleBle::Status::NeedsCalibration ||
      state.status == DualScaleBle::Status::Taring ||
      state.status == DualScaleBle::Status::Calibrating) {
    return ScaleBorderMode::Attention;
  }
  if (state.identityVerified && state.lastTelemetryMs != 0 &&
      ((state.flags & DualScaleBle::TelemetryFlag::Tared) == 0 ||
       (state.flags & DualScaleBle::TelemetryFlag::Calibrated) == 0)) {
    return ScaleBorderMode::Attention;
  }
  if (weightLockPhase == WeightLockPhase::Locked) {
    return ScaleBorderMode::Locked;
  }
  return scaleUsable(index) ? ScaleBorderMode::Ready
                            : ScaleBorderMode::Searching;
}

uint16_t animatedScaleBorderColor(uint16_t brightColor,
                                  uint32_t phaseDurationMs) {
  const bool brightPhase =
      ((millis() / phaseDurationMs) & 1U) == 0;
  return brightPhase
             ? brightColor
             : blendRgb565(brightColor, COLOR_PANEL, 72);
}

uint16_t dashboardScaleBorderColor(uint8_t index) {
  switch (dashboardScaleBorderMode(index)) {
  case ScaleBorderMode::Ready:
    return COLOR_SUCCESS;
  case ScaleBorderMode::Attention:
    return animatedScaleBorderColor(
        COLOR_WARNING, SCALE_BORDER_ATTENTION_BLINK_MS);
  case ScaleBorderMode::Error:
    return animatedScaleBorderColor(
        COLOR_DANGER, SCALE_BORDER_ERROR_BLINK_MS);
  case ScaleBorderMode::Locked:
    return COLOR_PRIMARY;
  case ScaleBorderMode::Inactive:
    return COLOR_MUTED;
  case ScaleBorderMode::Searching:
  default:
    return animatedScaleBorderColor(
        COLOR_WARNING, SCALE_BORDER_SEARCH_BLINK_MS);
  }
}

void drawDashboardScaleBorder(uint8_t index, bool force = false) {
  const uint16_t color = dashboardScaleBorderColor(index);
  if (!force && dashboardScaleBorderDrawnColor[index] == color) {
    return;
  }

  tft.drawRoundRect(6,
                    DASHBOARD_SCALE_PANEL_Y[index],
                    228,
                    61,
                    8,
                    color);
  dashboardScaleBorderDrawnColor[index] = color;
}

void serviceDashboardScaleBorderAnimation() {
  if (currentPage != Page::Dashboard || screenSleeping ||
      idleWarningActive) {
    return;
  }

  const uint32_t now = millis();
  if (now - lastScaleBorderServiceMs <
      SCALE_BORDER_SERVICE_INTERVAL_MS) {
    return;
  }
  lastScaleBorderServiceMs = now;

  for (uint8_t index = 0; index < REMOTE_SCALE_COUNT; ++index) {
    drawDashboardScaleBorder(index);
  }
}

void drawWeightValue(float weightKg,
                     bool valid,
                     int16_t x,
                     int16_t y,
                     bool largeText) {
  char weightText[20];
  const uint16_t textColor = valid ? TFT_WHITE : COLOR_MUTED;
  if (valid) {
    snprintf(weightText, sizeof(weightText), "%.2f", weightKg);
  } else {
    snprintf(weightText, sizeof(weightText), "--.--");
  }

  const uint8_t textSize = largeText && strlen(weightText) <= 6 ? 3 : 2;
  if (weightSpriteReady) {
    weightSprite.fillSprite(COLOR_PANEL);
    weightSprite.setTextDatum(MR_DATUM);
    weightSprite.setTextSize(textSize);
    weightSprite.setTextColor(textColor, COLOR_PANEL);
    weightSprite.drawString(weightText, 112, WEIGHT_SPRITE_HEIGHT / 2);
    weightSprite.setTextDatum(ML_DATUM);
    weightSprite.setTextSize(1);
    weightSprite.drawString("kg", 120, WEIGHT_SPRITE_HEIGHT / 2);
    weightSprite.setTextDatum(TL_DATUM);
    weightSprite.pushSprite(x, y);
    return;
  }

  // Safe fallback if heap allocation for the sprite ever fails.
  tft.fillRect(x,
               y,
               WEIGHT_SPRITE_WIDTH,
               WEIGHT_SPRITE_HEIGHT,
               COLOR_PANEL);
  tft.setTextDatum(MR_DATUM);
  tft.setTextSize(textSize);
  tft.setTextColor(textColor, COLOR_PANEL);
  tft.drawString(weightText, x + 112, y + WEIGHT_SPRITE_HEIGHT / 2);
  tft.setTextDatum(ML_DATUM);
  tft.setTextSize(1);
  tft.drawString("kg", x + 120, y + WEIGHT_SPRITE_HEIGHT / 2);
  tft.setTextDatum(TL_DATUM);
}

void drawDashboardScaleWeight(uint8_t index, int16_t weightY) {
  const bool globalLockActive =
      weightLockPhase == WeightLockPhase::Locked;
  const bool locked =
      globalLockActive &&
      lockedWeightSnapshot.scaleIncluded[index];
  // A scale that was not a contributor when the snapshot locked is shown as
  // unavailable, not as a changing live value that would no longer add up to
  // the frozen total.
  const bool valid = locked || (!globalLockActive && scaleUsable(index));
  const float weightKg =
      locked
          ? static_cast<float>(
                lockedWeightSnapshot.scaleGrams[index]) /
                1000.0f
          : valid
                ? static_cast<float>(
                      scaleStates[index].weightGrams) /
                      1000.0f
                : 0.0f;
  drawWeightValue(weightKg, valid, DASHBOARD_WEIGHT_X, weightY, false);
}

void drawDashboardScale(uint8_t index, int16_t panelY, int16_t weightY) {
  const String status = dashboardScaleStatus(index);
  const uint16_t statusColor = dashboardScaleColor(index);

  tft.fillRect(12, panelY + 30, 68, 16, COLOR_PANEL);
  tft.setTextColor(statusColor, COLOR_PANEL);
  tft.setTextSize(1);
  tft.setCursor(14, panelY + 34);
  tft.print(status);
  drawDashboardScaleBorder(index, true);
  drawDashboardScaleWeight(index, weightY);
}

void drawDashboardStatus() {
  String statusLine;
  uint16_t statusColor = TFT_WHITE;
  if (weightLockPhase == WeightLockPhase::Locked) {
    statusLine = "WEIGHT LOCKED - TAP UNLOCK";
    statusColor = COLOR_SUCCESS;
  } else if (weightLockPhase == WeightLockPhase::Settling) {
    statusLine = "HOLD STILL - LOCKING...";
    statusColor = COLOR_WARNING;
  } else if (noticeActive()) {
    statusLine = noticeMessage;
    statusColor = COLOR_WARNING;
  } else if (combinedMode == CombinedMode::Dual) {
    statusLine = "A + B COMBINED";
    statusColor = COLOR_SUCCESS;
  } else if (combinedMode == CombinedMode::Scale1Only) {
    statusLine = "PARTIAL: LOAD CELL A";
    statusColor = COLOR_WARNING;
  } else if (combinedMode == CombinedMode::Scale2Only) {
    statusLine = "PARTIAL: LOAD CELL B";
    statusColor = COLOR_WARNING;
  } else {
    statusLine = bluetoothReady
                     ? "WAITING FOR VALID DATA"
                     : "BLE RADIO ERROR";
    statusColor = connectedScaleCount() == 0 ? COLOR_DANGER : COLOR_WARNING;
  }

  tft.fillRect(12, 229, 216, 15, COLOR_PANEL);
  tft.setTextColor(statusColor, COLOR_PANEL);
  tft.setTextSize(1);
  tft.setCursor(14, 233);
  tft.print(statusLine);
  tft.drawRoundRect(6, 177, 228, 72, 8, statusColor);
}

void drawDashboardPrimaryAction() {
  const bool locked = weightLockPhase == WeightLockPhase::Locked;
  drawButton(6,
             256,
             108,
             56,
             locked ? "UNLOCK" : "TARE ALL",
             locked ? COLOR_WARNING : COLOR_PRIMARY,
             locked ? COLOR_WARNING : COLOR_PRIMARY,
             2);
}

void drawDashboardDynamic(bool force = false) {
  // Each live region is independent. Weight packets only push the sprite;
  // unchanged connection and status areas are left completely untouched.
  if (force || dashboardLinkDirty) {
    tft.drawRoundRect(123, 7, 51, 28, 7, connectionColor());
    dashboardLinkDirty = false;
  }

  if (force || dashboardScaleDirty[0]) {
    drawDashboardScale(0, 47, DASHBOARD_SCALE_A_WEIGHT_Y);
    dashboardScaleDirty[0] = false;
    dashboardScaleWeightDirty[0] = false;
  } else if (dashboardScaleWeightDirty[0]) {
    drawDashboardScaleWeight(0, DASHBOARD_SCALE_A_WEIGHT_Y);
    dashboardScaleWeightDirty[0] = false;
  }

  if (force || dashboardScaleDirty[1]) {
    drawDashboardScale(1, 112, DASHBOARD_SCALE_B_WEIGHT_Y);
    dashboardScaleDirty[1] = false;
    dashboardScaleWeightDirty[1] = false;
  } else if (dashboardScaleWeightDirty[1]) {
    drawDashboardScaleWeight(1, DASHBOARD_SCALE_B_WEIGHT_Y);
    dashboardScaleWeightDirty[1] = false;
  }

  if (force || dashboardWeightDirty) {
    const bool locked =
        weightLockPhase == WeightLockPhase::Locked;
    const float displayedWeightKg =
        locked
            ? static_cast<float>(
                  lockedWeightSnapshot.totalGrams) /
                  1000.0f
            : latestWeightKg;
    drawWeightValue(displayedWeightKg,
                    locked || weightValid,
                    DASHBOARD_WEIGHT_X,
                    DASHBOARD_TOTAL_WEIGHT_Y,
                    true);
    dashboardWeightDirty = false;
  }

  if (force || dashboardStatusDirty) {
    drawDashboardStatus();
    dashboardStatusDirty = false;
  }

  if (force || dashboardPrimaryActionDirty) {
    drawDashboardPrimaryAction();
    dashboardPrimaryActionDirty = false;
  }
}

void drawDashboard() {
  tft.fillScreen(COLOR_BACKGROUND);
  drawHeader("DUAL SCALE", false, true);

  tft.fillRoundRect(6, 47, 228, 61, 8, COLOR_PANEL);
  tft.fillRoundRect(6, 112, 228, 61, 8, COLOR_PANEL);
  tft.fillRoundRect(6, 177, 228, 72, 8, COLOR_PANEL);
  tft.setTextColor(TFT_WHITE, COLOR_PANEL);
  tft.setTextSize(1);
  tft.setCursor(14, 54);
  tft.print("LOAD CELL A");
  tft.setCursor(14, 119);
  tft.print("LOAD CELL B");
  tft.setCursor(14, 184);
  tft.print("TOTAL");

  drawButton(126,
             256,
             108,
             56,
             "CALIBRATE",
             COLOR_PANEL_LIGHT,
             COLOR_WARNING,
             1);

  drawDashboardDynamic(true);

  if (!touchReady) {
    tft.fillRect(0, 311, SCREEN_WIDTH, 9, COLOR_DANGER);
    setCenteredText("TOUCH NOT FOUND", 120, 315, 1, TFT_WHITE, COLOR_DANGER);
  }
}

void drawCalibrationWeightValue() {
  // Update only the number area so auto-repeat does not flash the page.
  tft.fillRect(10, 108, 220, 43, COLOR_PANEL);
  char knownWeightText[24];
  snprintf(knownWeightText,
           sizeof(knownWeightText),
           "%.1f kg",
           calibrationWeightKg);
  setCenteredText(knownWeightText, 120, 130, 3, TFT_WHITE, COLOR_PANEL);
}

void drawCalibrationDynamic() {
  tft.fillRoundRect(6, 84, 228, 72, 9, COLOR_PANEL);
  tft.setTextColor(COLOR_MUTED, COLOR_PANEL);
  tft.setTextSize(1);
  tft.setCursor(16, 94);
  tft.print("KNOWN WEIGHT - LOAD CELL ");
  tft.print(calibrationTargetIndex == 0 ? 'A' : 'B');
  drawCalibrationWeightValue();

  drawButton(6,
             158,
             111,
             30,
             "LOAD CELL A",
             calibrationTargetIndex == 0 ? COLOR_PRIMARY : COLOR_PANEL_LIGHT,
             calibrationTargetIndex == 0 ? COLOR_PRIMARY : COLOR_MUTED,
             1);
  drawButton(123,
             158,
             111,
             30,
             "LOAD CELL B",
             calibrationTargetIndex == 1 ? COLOR_PRIMARY : COLOR_PANEL_LIGHT,
             calibrationTargetIndex == 1 ? COLOR_PRIMARY : COLOR_MUTED,
             1);

  drawButton(6,
             232,
             228,
             36,
             "TARE SCALE " + String(calibrationTargetIndex + 1),
             COLOR_PANEL_LIGHT,
             COLOR_PRIMARY,
             1);
  drawButton(6,
             276,
             228,
             37,
             "CALIBRATE SCALE " + String(calibrationTargetIndex + 1),
             COLOR_PRIMARY,
             COLOR_PRIMARY,
             1);

  tft.fillRect(6, 268, 228, 8, COLOR_BACKGROUND);
  if (noticeActive()) {
    setCenteredText(noticeMessage,
                    120,
                    272,
                    1,
                    COLOR_WARNING,
                    COLOR_BACKGROUND);
  }
}

void drawCalibrationPage() {
  tft.fillScreen(COLOR_BACKGROUND);
  drawHeader("CALIBRATE", true);

  tft.setTextColor(COLOR_MUTED, COLOR_BACKGROUND);
  tft.setTextSize(1);
  tft.setCursor(8, 50);
  tft.println("1. Select a scale and tare it empty");
  tft.setCursor(8, 65);
  tft.println("2. Load selected cell only, then CAL");

  for (uint8_t index = 0;
       index < CALIBRATION_ADJUST_BUTTON_COUNT;
       ++index) {
    const bool pressed =
        index == activeCalibrationAdjustButton &&
        (calibrationRepeatPhase == CalibrationRepeatPhase::Waiting ||
         calibrationRepeatPhase == CalibrationRepeatPhase::Repeating);
    drawCalibrationAdjustButton(index, pressed);
  }

  drawCalibrationDynamic();
}

void drawSettingsDynamic() {
  tft.fillRect(20, 75, 200, 33, COLOR_PANEL);
  setCenteredText(String(brightnessPercent) + "%",
                  SCREEN_WIDTH / 2,
                  91,
                  3,
                  TFT_WHITE,
                  COLOR_PANEL);

  tft.fillRect(20, 191, 200, 33, COLOR_PANEL);
  setCenteredText(String(cpuFrequencyMhz) + " MHz",
                  SCREEN_WIDTH / 2,
                  207,
                  2,
                  TFT_WHITE,
                  COLOR_PANEL);

  drawButton(12,
             230,
             102,
             40,
             "160 MHz",
             cpuFrequencyMhz == 160 ? COLOR_PRIMARY : COLOR_PANEL_LIGHT,
             cpuFrequencyMhz == 160 ? COLOR_PRIMARY : COLOR_MUTED,
             1);
  drawButton(126,
             230,
             102,
             40,
             "240 MHz",
             cpuFrequencyMhz == 240 ? COLOR_PRIMARY : COLOR_PANEL_LIGHT,
             cpuFrequencyMhz == 240 ? COLOR_PRIMARY : COLOR_MUTED,
             1);

  tft.fillRect(0, 282, SCREEN_WIDTH, 38, COLOR_BACKGROUND);
  drawButton(6,
             284,
             228,
             31,
             "MODE: " + String(operatingModeName(operatingMode)),
             COLOR_PANEL_LIGHT,
             COLOR_PRIMARY,
             1);
}

void drawSettingsPage() {
  tft.fillScreen(COLOR_BACKGROUND);
  drawHeader("SETTINGS", true);

  tft.fillRoundRect(6, 51, 228, 106, 9, COLOR_PANEL);
  tft.setTextColor(COLOR_MUTED, COLOR_PANEL);
  tft.setTextSize(1);
  tft.setCursor(16, 61);
  tft.print("DISPLAY BRIGHTNESS");
  drawButton(12,
             116,
             92,
             34,
             "-10%",
             COLOR_PANEL_LIGHT,
             COLOR_MUTED,
             1);
  drawButton(136,
             116,
             92,
             34,
             "+10%",
             COLOR_PANEL_LIGHT,
             COLOR_MUTED,
             1);

  tft.fillRoundRect(6, 167, 228, 113, 9, COLOR_PANEL);
  tft.setTextColor(COLOR_MUTED, COLOR_PANEL);
  tft.setTextSize(1);
  tft.setCursor(16, 177);
  tft.print("ESP32 CPU CLOCK");

  drawSettingsDynamic();
}

void drawOperatingModeChoice(OperatingMode mode,
                             int16_t y,
                             const String &title,
                             const String &description) {
  const bool selected = operatingMode == mode;
  const uint16_t fill = selected ? COLOR_PRIMARY : COLOR_PANEL;
  const uint16_t border = selected ? COLOR_PRIMARY : COLOR_PANEL_LIGHT;
  tft.fillRoundRect(6, y, 228, 68, 9, fill);
  tft.drawRoundRect(6, y, 228, 68, 9, border);
  setCenteredText(title,
                  SCREEN_WIDTH / 2,
                  y + 23,
                  2,
                  TFT_WHITE,
                  fill);
  setCenteredText(description,
                  SCREEN_WIDTH / 2,
                  y + 49,
                  1,
                  selected ? TFT_WHITE : COLOR_MUTED,
                  fill);
}

void drawOperatingModeSelectionPage() {
  tft.fillScreen(COLOR_BACKGROUND);
  drawHeader("SELECT MODE", true);
  drawOperatingModeChoice(OperatingMode::Local,
                          51,
                          "LOCAL",
                          "Touchscreen only - WiFi off");
  drawOperatingModeChoice(OperatingMode::Web,
                          127,
                          "WEB APP",
                          "Phone control - status screen");
  drawOperatingModeChoice(OperatingMode::Hybrid,
                          203,
                          "HYBRID",
                          "Touchscreen and phone control");
  const String footer =
      noticeActive() ? noticeMessage : "BLE scales stay active in every mode";
  setCenteredText(footer,
                  SCREEN_WIDTH / 2,
                  292,
                  1,
                  noticeActive() ? COLOR_WARNING : COLOR_MUTED,
                  COLOR_BACKGROUND);
  setCenteredText("Selection saves automatically",
                  SCREEN_WIDTH / 2,
                  308,
                  1,
                  COLOR_MUTED,
                  COLOR_BACKGROUND);
}

String scaleLinkLabel(uint8_t index) {
  const ScaleState &state = scaleStates[index];
  if (!bluetoothReady) {
    return "BT ERROR";
  }
  if (state.connected) {
    return state.identityVerified ? "CONNECTED" : "VERIFYING";
  }
  if (state.connecting) {
    return "CONNECTING";
  }
  return state.discovered ? "RETRYING" : "SEARCHING";
}

uint16_t scaleLinkColor(uint8_t index) {
  const ScaleState &state = scaleStates[index];
  if (state.connected && state.identityVerified) {
    return COLOR_SUCCESS;
  }
  if (bluetoothReady && (state.connecting || state.discovered)) {
    return COLOR_WARNING;
  }
  return COLOR_DANGER;
}

String scaleAdcLabel(uint8_t index) {
  const ScaleState &state = scaleStates[index];
  if (!state.connected || state.lastTelemetryMs == 0) {
    return "UNKNOWN";
  }
  if (state.status == DualScaleBle::Status::NoSensor ||
      !(state.flags & DualScaleBle::SensorPresent)) {
    return "NOT FOUND";
  }
  if (state.status == DualScaleBle::Status::SensorError ||
      !(state.flags & DualScaleBle::AdcResponding)) {
    return "READ ERROR";
  }
  return "CONNECTED";
}

uint16_t scaleAdcColor(uint8_t index) {
  const String label = scaleAdcLabel(index);
  return label == "CONNECTED"
             ? COLOR_SUCCESS
             : (label == "UNKNOWN" ? COLOR_WARNING : COLOR_DANGER);
}

String scaleOledLabel(uint8_t index) {
  const ScaleState &state = scaleStates[index];
  if (!state.connected || state.lastTelemetryMs == 0) {
    return "UNKNOWN";
  }
  return state.flags & DualScaleBle::OledPresent ? "CONNECTED" : "NOT FOUND";
}

uint16_t scaleOledColor(uint8_t index) {
  const String label = scaleOledLabel(index);
  return label == "CONNECTED"
             ? COLOR_SUCCESS
             : (label == "UNKNOWN" ? COLOR_WARNING : COLOR_DANGER);
}

String scaleDataLabel(uint8_t index) {
  const ScaleState &state = scaleStates[index];
  if (!state.connected) {
    return "NO LINK";
  }
  if (scaleUsable(index)) {
    return "LIVE";
  }
  if (state.lastTelemetryMs != 0 &&
      millis() - state.lastTelemetryMs > TELEMETRY_STALE_MS) {
    return "STALE";
  }
  if (state.goodPacketCount > 0 &&
      state.goodPacketCount < REQUIRED_GOOD_PACKETS) {
    return "VALIDATING";
  }
  return "NOT READY";
}

uint16_t scaleDataColor(uint8_t index) {
  const String label = scaleDataLabel(index);
  return label == "LIVE"
             ? COLOR_SUCCESS
             : (label == "VALIDATING" || label == "NOT READY"
                    ? COLOR_WARNING
                    : COLOR_DANGER);
}

String scaleAgeLabel(uint8_t index) {
  const ScaleState &state = scaleStates[index];
  if (state.lastTelemetryMs == 0) {
    return "--";
  }
  const uint32_t ageMs = millis() - state.lastTelemetryMs;
  if (ageMs < 1000) {
    return String(ageMs) + " ms";
  }
  return String(ageMs / 1000.0f, 1) + " s";
}

uint16_t scaleAgeColor(uint8_t index) {
  if (scaleStates[index].lastTelemetryMs == 0) {
    return COLOR_DANGER;
  }
  return millis() - scaleStates[index].lastTelemetryMs <= TELEMETRY_STALE_MS
             ? COLOR_SUCCESS
             : COLOR_DANGER;
}

bool phoneWebAppActive() {
  return phoneControlReady && phoneClientCount > 0 &&
         lastPhoneWebActivityMs != 0 &&
         millis() - lastPhoneWebActivityMs <= PHONE_WEB_ACTIVITY_LIVE_MS;
}

void drawWebStatusRow(int16_t y,
                      const String &label,
                      const String &value,
                      uint16_t valueColor) {
  tft.fillRect(14, y - 7, 212, 15, COLOR_PANEL);
  tft.setTextSize(1);
  tft.setTextColor(COLOR_MUTED, COLOR_PANEL);
  tft.setCursor(16, y - 4);
  tft.print(label);
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(valueColor, COLOR_PANEL);
  tft.drawString(value, 224, y, 1);
  tft.setTextDatum(TL_DATUM);
}

void drawWebStatusDynamic() {
  const String phoneValue =
      !phoneControlReady
          ? "WIFI ERROR"
          : phoneClientCount == 0
                ? "WAITING"
                : String(phoneClientCount) + " CONNECTED";
  const uint16_t phoneColor =
      !phoneControlReady
          ? COLOR_DANGER
          : phoneClientCount > 0 ? COLOR_SUCCESS : COLOR_WARNING;
  drawWebStatusRow(190, "PHONES", phoneValue, phoneColor);

  const bool appActive = phoneWebAppActive();
  drawWebStatusRow(210,
                   "WEB APP",
                   appActive ? "DATA ACTIVE" : "NOT OPEN",
                   appActive ? COLOR_SUCCESS : COLOR_WARNING);

  for (uint8_t index = 0; index < REMOTE_SCALE_COUNT; ++index) {
    String value = scaleDataLabel(index);
    if (value == "LIVE") {
      value += "  ";
      value += scaleAgeLabel(index);
    }
    drawWebStatusRow(230 + index * 20,
                     "LOAD CELL " + String(index == 0 ? "A" : "B"),
                     value,
                     scaleDataColor(index));
  }
}

void drawWebStatusPage() {
  tft.fillScreen(COLOR_BACKGROUND);
  drawHeader("WEB MODE", false);

  tft.fillRoundRect(6, 51, 228, 106, 9, COLOR_PANEL);
  setCenteredText("CONNECT YOUR PHONE",
                  SCREEN_WIDTH / 2,
                  64,
                  1,
                  COLOR_MUTED,
                  COLOR_PANEL);
  setCenteredText(PHONE_WIFI_SSID,
                  SCREEN_WIDTH / 2,
                  85,
                  2,
                  TFT_WHITE,
                  COLOR_PANEL);
  String webAddress = "http://";
  webAddress += phoneControlReady
                    ? WiFi.softAPIP().toString()
                    : String("192.168.4.1");
  setCenteredText(webAddress,
                  SCREEN_WIDTH / 2,
                  112,
                  1,
                  COLOR_PRIMARY,
                  COLOR_PANEL);
  setCenteredText("Password: " + String(PHONE_WIFI_PASSWORD),
                  SCREEN_WIDTH / 2,
                  137,
                  1,
                  COLOR_MUTED,
                  COLOR_PANEL);

  tft.fillRoundRect(6, 166, 228, 96, 9, COLOR_PANEL);
  tft.setTextSize(1);
  tft.setTextColor(COLOR_MUTED, COLOR_PANEL);
  tft.setCursor(16, 174);
  tft.print("LIVE STATUS");
  drawWebStatusDynamic();

  drawButton(6,
             271,
             108,
             42,
             "MODE",
             COLOR_PANEL_LIGHT,
             COLOR_PRIMARY,
             1);
  drawButton(126,
             271,
             108,
             42,
             "SCREEN OFF",
             COLOR_PANEL_LIGHT,
             COLOR_MUTED,
             1);
  lastWebStatusRedrawMs = millis();
}

void drawConnectivityItem(int16_t centerX,
                          int16_t labelY,
                          const String &label,
                          const String &value,
                          uint16_t valueColor) {
  setCenteredText(label,
                  centerX,
                  labelY,
                  1,
                  COLOR_MUTED,
                  COLOR_PANEL);
  tft.fillRect(centerX - 51,
               labelY + 9,
               102,
               14,
               COLOR_PANEL);
  setCenteredText(value,
                  centerX,
                  labelY + 15,
                  1,
                  valueColor,
                  COLOR_PANEL);
}

void drawConnectivityDynamic() {
  constexpr int16_t LEFT_CENTER_X = 61;
  constexpr int16_t RIGHT_CENTER_X = 178;

  drawConnectivityItem(LEFT_CENTER_X,
                       101,
                       "BLUETOOTH",
                       scaleLinkLabel(0),
                       scaleLinkColor(0));
  drawConnectivityItem(LEFT_CENTER_X,
                       131,
                       "LOAD CELL ADC",
                       scaleAdcLabel(0),
                       scaleAdcColor(0));
  drawConnectivityItem(LEFT_CENTER_X,
                       161,
                       "OLED SCREEN",
                       scaleOledLabel(0),
                       scaleOledColor(0));
  drawConnectivityItem(LEFT_CENTER_X,
                       191,
                       "WEIGHT DATA",
                       scaleDataLabel(0),
                       scaleDataColor(0));
  drawConnectivityItem(LEFT_CENTER_X,
                       221,
                       "DATA AGE",
                       scaleAgeLabel(0),
                       scaleAgeColor(0));

  drawConnectivityItem(RIGHT_CENTER_X,
                       101,
                       "BLUETOOTH",
                       scaleLinkLabel(1),
                       scaleLinkColor(1));
  drawConnectivityItem(RIGHT_CENTER_X,
                       131,
                       "LOAD CELL ADC",
                       scaleAdcLabel(1),
                       scaleAdcColor(1));
  drawConnectivityItem(RIGHT_CENTER_X,
                       161,
                       "OLED SCREEN",
                       scaleOledLabel(1),
                       scaleOledColor(1));
  drawConnectivityItem(RIGHT_CENTER_X,
                       191,
                       "WEIGHT DATA",
                       scaleDataLabel(1),
                       scaleDataColor(1));
  drawConnectivityItem(RIGHT_CENTER_X,
                       221,
                       "DATA AGE",
                       scaleAgeLabel(1),
                       scaleAgeColor(1));

  // The 8-pixel strip between the panels and button gives CHECK BOTH
  // immediate feedback without redrawing the whole Link Status page.
  tft.fillRect(6, 270, 228, 8, COLOR_BACKGROUND);
  if (noticeActive()) {
    setCenteredText(noticeMessage,
                    SCREEN_WIDTH / 2,
                    274,
                    1,
                    COLOR_WARNING,
                    COLOR_BACKGROUND);
  }
}

void drawConnectivityPage() {
  tft.fillScreen(COLOR_BACKGROUND);
  drawHeader("LINK STATUS", true);

  constexpr int16_t LEFT_PANEL_X = 6;
  constexpr int16_t RIGHT_PANEL_X = 123;
  constexpr int16_t PANEL_Y = 51;
  constexpr int16_t PANEL_WIDTH = 111;
  constexpr int16_t PANEL_HEIGHT = 219;
  constexpr int16_t LEFT_CENTER_X = 61;
  constexpr int16_t RIGHT_CENTER_X = 178;

  tft.fillRoundRect(LEFT_PANEL_X,
                    PANEL_Y,
                    PANEL_WIDTH,
                    PANEL_HEIGHT,
                    9,
                    COLOR_PANEL);
  tft.fillRoundRect(RIGHT_PANEL_X,
                    PANEL_Y,
                    PANEL_WIDTH,
                    PANEL_HEIGHT,
                    9,
                    COLOR_PANEL);

  setCenteredText("SCALE 1",
                  LEFT_CENTER_X,
                  66,
                  2,
                  TFT_WHITE,
                  COLOR_PANEL);
  setCenteredText("LOAD-CELL ESP32",
                  LEFT_CENTER_X,
                  84,
                  1,
                  COLOR_MUTED,
                  COLOR_PANEL);
  setCenteredText("SCALE 2",
                  RIGHT_CENTER_X,
                  66,
                  2,
                  TFT_WHITE,
                  COLOR_PANEL);
  setCenteredText("LOAD-CELL ESP32",
                  RIGHT_CENTER_X,
                  84,
                  1,
                  COLOR_MUTED,
                  COLOR_PANEL);

  tft.drawFastHLine(14, 94, 95, COLOR_PANEL_LIGHT);
  tft.drawFastHLine(131, 94, 95, COLOR_PANEL_LIGHT);

  drawConnectivityDynamic();

  drawButton(6,
             278,
             228,
             35,
             "CHECK BOTH",
             COLOR_PRIMARY,
             COLOR_PRIMARY,
             1);
}

void drawCurrentPage() {
  if (currentPage == Page::Dashboard) {
    drawDashboard();
  } else if (currentPage == Page::Calibration) {
    drawCalibrationPage();
  } else if (currentPage == Page::Settings) {
    drawSettingsPage();
  } else if (currentPage == Page::Connectivity) {
    drawConnectivityPage();
  } else if (currentPage == Page::ModeSelection) {
    drawOperatingModeSelectionPage();
  } else if (currentPage == Page::WebStatus) {
    drawWebStatusPage();
  }
  pageDirty = false;
  uiDirty = false;
}

void updateCurrentPage() {
  if (currentPage == Page::Dashboard) {
    drawDashboardDynamic();
  } else if (currentPage == Page::Calibration) {
    drawCalibrationDynamic();
  } else if (currentPage == Page::Settings) {
    drawSettingsDynamic();
  } else if (currentPage == Page::Connectivity) {
    drawConnectivityDynamic();
  } else if (currentPage == Page::ModeSelection) {
    drawOperatingModeSelectionPage();
  } else if (currentPage == Page::WebStatus) {
    drawWebStatusDynamic();
    lastWebStatusRedrawMs = millis();
  }
  uiDirty = false;
}

void drawIdleWarningCountdown(uint8_t secondsRemaining) {
  tft.fillRect(20, 174, 200, 20, COLOR_BACKGROUND);
  setCenteredText("Screen off in " + String(secondsRemaining) + " sec",
                  SCREEN_WIDTH / 2,
                  184,
                  1,
                  COLOR_WARNING,
                  COLOR_BACKGROUND);
}

void drawIdleWarning(uint8_t secondsRemaining) {
  tft.fillScreen(COLOR_BACKGROUND);
  setCenteredText("SNOOZING",
                  SCREEN_WIDTH / 2,
                  116,
                  3,
                  TFT_WHITE,
                  COLOR_BACKGROUND);
  setCenteredText("No recent touchscreen activity",
                  SCREEN_WIDTH / 2,
                  158,
                  1,
                  COLOR_MUTED,
                  COLOR_BACKGROUND);
  drawIdleWarningCountdown(secondsRemaining);
  setCenteredText("Touch screen to stay awake",
                  SCREEN_WIDTH / 2,
                  210,
                  1,
                  TFT_WHITE,
                  COLOR_BACKGROUND);
  uiDirty = false;
}

void setScreenSleeping(bool sleeping) {
  if (screenSleeping == sleeping) {
    return;
  }

  idleWarningActive = false;
  idleWarningSecondsShown = 0;

  if (sleeping) {
    Serial.println("Screen standby: LCD and backlight off; touch stays awake.");
    screenSleeping = true;
    touchWasDown = false;

    tft.writecommand(TFT_DISPOFF);
    delay(20);
    tft.writecommand(TFT_SLPIN);
    delay(120);
    applyBacklightOutput(false);
    return;
  }

  Serial.println("Screen standby ended.");
  tft.writecommand(TFT_SLPOUT);
  delay(120);
  tft.writecommand(TFT_DISPON);

  if (!touchReady) {
    touchReady = beginTouchController();
  }
  screenSleeping = false;
  lastUserActivityMs = millis();
  pageDirty = true;
  uiDirty = true;
  // Restore a complete frame while the backlight is still dark, then reveal
  // it in one step so wake-up never exposes stale framebuffer contents.
  drawCurrentPage();
  applyBacklightOutput(true);
}

bool queueBleManagerMessage(const BleManagerMessage &message) {
  return bleManagerQueue != nullptr &&
         xQueueSend(bleManagerQueue, &message, 0) == pdTRUE;
}

void queueBleUiEvent(const BleUiEvent &event) {
  if (bleUiQueue != nullptr) {
    xQueueSend(bleUiQueue, &event, 0);
  }
}

void queueSimpleBleUiEvent(BleUiEventType type,
                           uint8_t scaleIndex,
                           int reason = 0) {
  BleUiEvent event;
  event.type = type;
  event.scaleIndex = scaleIndex;
  event.reason = reason;
  queueBleUiEvent(event);
}

class DisplayBleScanCallbacks : public NimBLEScanCallbacks {
public:
  void onResult(const NimBLEAdvertisedDevice *device) override {
    static uint32_t lastQueuedMs[REMOTE_SCALE_COUNT] = {};
    const NimBLEUUID serviceUuid(DualScaleBle::SERVICE_UUID);
    if (!device->isAdvertisingService(serviceUuid) || !device->haveName()) {
      return;
    }

    const std::string name = device->getName();
    for (uint8_t index = 0; index < REMOTE_SCALE_COUNT; ++index) {
      if (name != REMOTE_SCALES[index].bluetoothName ||
          millis() - lastQueuedMs[index] < 1000) {
        continue;
      }

      BleManagerMessage message;
      message.type = BleManagerMessageType::Discovered;
      message.scaleIndex = index;
      message.addressType = device->getAddress().getType();
      // uint64_t preserves NimBLE's native byte order. Reconstructing from a
      // raw uint8_t[6] would invoke the Bluedroid-compatibility constructor
      // and reverse the address.
      message.addressValue =
          static_cast<uint64_t>(device->getAddress());
      if (queueBleManagerMessage(message)) {
        lastQueuedMs[index] = millis();
      }
      return;
    }
  }
};

class DisplayBleClientCallbacks : public NimBLEClientCallbacks {
public:
  explicit DisplayBleClientCallbacks(uint8_t scaleIndex)
      : scaleIndex_(scaleIndex) {}

  void onDisconnect(NimBLEClient *, int reason) override {
    BleManagerMessage message;
    message.type = BleManagerMessageType::Disconnected;
    message.scaleIndex = scaleIndex_;
    message.command.valueGrams = reason;
    queueBleManagerMessage(message);
  }

private:
  uint8_t scaleIndex_;
};

DisplayBleScanCallbacks bleScanCallbacks;
DisplayBleClientCallbacks bleClientCallbacks[REMOTE_SCALE_COUNT] = {
    DisplayBleClientCallbacks(0),
    DisplayBleClientCallbacks(1),
};

void onBleTelemetry(uint8_t scaleIndex,
                    uint8_t *data,
                    size_t length) {
  if (length != sizeof(DualScaleBle::TelemetryPacket)) {
    return;
  }

  BleUiEvent event;
  event.type = BleUiEventType::Telemetry;
  event.scaleIndex = scaleIndex;
  memcpy(&event.telemetry, data, sizeof(event.telemetry));
  queueBleUiEvent(event);
}

void markTransportDisconnected(uint8_t scaleIndex,
                               int reason,
                               bool notifyUi = true) {
  BleTransportSlot &slot = bleTransport[scaleIndex];
  slot.connected = false;
  slot.telemetryCharacteristic = nullptr;
  slot.commandCharacteristic = nullptr;
  slot.nextAttemptMs = millis() + RECONNECT_INTERVAL_MS;
  if (notifyUi) {
    queueSimpleBleUiEvent(BleUiEventType::Disconnected,
                          scaleIndex,
                          reason);
  }
}

void recordBleConnectFailure(uint8_t scaleIndex, int reason) {
  BleTransportSlot &slot = bleTransport[scaleIndex];
  markTransportDisconnected(scaleIndex, reason);
  if (slot.consecutiveConnectFailures < UINT8_MAX) {
    ++slot.consecutiveConnectFailures;
  }

  if (slot.consecutiveConnectFailures >=
      MAX_ADDRESS_CONNECT_FAILURES) {
    Serial.printf(
        "BLE Scale %u address failed repeatedly; rediscovering.\n",
        scaleIndex + 1);
    slot.addressKnown = false;
    slot.consecutiveConnectFailures = 0;
    bleForceScanRequested = true;
  }
}

bool connectBleScale(uint8_t scaleIndex) {
  BleTransportSlot &slot = bleTransport[scaleIndex];
  if (!slot.addressKnown) {
    return false;
  }

  NimBLEScan *scan = NimBLEDevice::getScan();
  if (scan->isScanning()) {
    scan->stop();
  }

  queueSimpleBleUiEvent(BleUiEventType::Connecting, scaleIndex);

  if (slot.client == nullptr) {
    slot.client = NimBLEDevice::createClient(slot.address);
    if (slot.client == nullptr) {
      recordBleConnectFailure(scaleIndex, -1);
      return false;
    }
    slot.client->setClientCallbacks(&bleClientCallbacks[scaleIndex], false);
    slot.client->setConnectionParams(24, 40, 0, 400);
    slot.client->setConnectTimeout(BLE_CONNECT_TIMEOUT_MS);
  } else {
    slot.client->setPeerAddress(slot.address);
  }

  // Attribute discovery is deliberately refreshed after every reconnect.
  // Sender firmware can be updated independently without leaving stale handles.
  if (!slot.client->connect(slot.address, true, false, false)) {
    const int error = slot.client->getLastError();
    Serial.printf("BLE Scale %u connect failed: %d\n",
                  scaleIndex + 1,
                  error);
    recordBleConnectFailure(scaleIndex, error);
    return false;
  }

  NimBLERemoteService *service =
      slot.client->getService(DualScaleBle::SERVICE_UUID);
  if (service == nullptr) {
    Serial.printf("BLE Scale %u service not found.\n", scaleIndex + 1);
    slot.client->disconnect();
    recordBleConnectFailure(scaleIndex, -2);
    return false;
  }

  slot.telemetryCharacteristic =
      service->getCharacteristic(DualScaleBle::TELEMETRY_UUID);
  slot.commandCharacteristic =
      service->getCharacteristic(DualScaleBle::COMMAND_UUID);

  if (slot.telemetryCharacteristic == nullptr ||
      slot.commandCharacteristic == nullptr ||
      !slot.telemetryCharacteristic->canNotify() ||
      (!slot.commandCharacteristic->canWrite() &&
       !slot.commandCharacteristic->canWriteNoResponse())) {
    Serial.printf("BLE Scale %u characteristics are incompatible.\n",
                  scaleIndex + 1);
    slot.client->disconnect();
    recordBleConnectFailure(scaleIndex, -3);
    return false;
  }

  const bool subscribed = slot.telemetryCharacteristic->subscribe(
      true,
      [scaleIndex](NimBLERemoteCharacteristic *,
                   uint8_t *data,
                   size_t length,
                   bool) { onBleTelemetry(scaleIndex, data, length); },
      true);
  if (!subscribed) {
    Serial.printf("BLE Scale %u notification subscribe failed.\n",
                  scaleIndex + 1);
    slot.client->disconnect();
    recordBleConnectFailure(scaleIndex, -4);
    return false;
  }

  slot.connected = true;
  slot.consecutiveConnectFailures = 0;
  slot.nextAttemptMs = 0;
  Serial.printf("BLE Scale %u connected and subscribed.\n", scaleIndex + 1);
  queueSimpleBleUiEvent(BleUiEventType::Connected, scaleIndex);
  return true;
}

bool writeBleCommand(uint8_t scaleIndex,
                     const DualScaleBle::CommandPacket &command) {
  BleTransportSlot &slot = bleTransport[scaleIndex];
  if (!slot.connected ||
      slot.client == nullptr ||
      !slot.client->isConnected() ||
      slot.commandCharacteristic == nullptr) {
    return false;
  }

  const bool useResponse = slot.commandCharacteristic->canWrite();
  return slot.commandCharacteristic->writeValue(
      reinterpret_cast<const uint8_t *>(&command),
      sizeof(command),
      useResponse);
}

void handleBleManagerMessage(const BleManagerMessage &message) {
  if (message.scaleIndex >= REMOTE_SCALE_COUNT &&
      message.type != BleManagerMessageType::ForceCheck) {
    return;
  }

  if (message.type == BleManagerMessageType::Discovered) {
    BleTransportSlot &slot = bleTransport[message.scaleIndex];
    const NimBLEAddress address(message.addressValue,
                                message.addressType);
    if (!slot.addressKnown || slot.address != address) {
      slot.address = address;
      slot.addressKnown = true;
      slot.nextAttemptMs = 0;
      Serial.printf("Discovered BLE Scale %u at %s.\n",
                    message.scaleIndex + 1,
                    address.toString().c_str());
      queueSimpleBleUiEvent(BleUiEventType::Discovered,
                            message.scaleIndex);
    }
    return;
  }

  if (message.type == BleManagerMessageType::Disconnected) {
    // A disconnect callback from the previous link can arrive after a fast
    // reconnect. Do not let that stale callback tear down the new session.
    BleTransportSlot &slot = bleTransport[message.scaleIndex];
    if (slot.client != nullptr && slot.client->isConnected()) {
      return;
    }
    markTransportDisconnected(message.scaleIndex,
                              message.command.valueGrams);
    return;
  }

  if (message.type == BleManagerMessageType::SendCommand) {
    if (!writeBleCommand(message.scaleIndex, message.command)) {
      queueSimpleBleUiEvent(BleUiEventType::CommandWriteFailed,
                            message.scaleIndex);
    }
    return;
  }

  if (message.type == BleManagerMessageType::Reconnect) {
    BleTransportSlot &slot = bleTransport[message.scaleIndex];
    if (slot.client != nullptr && slot.client->isConnected()) {
      slot.client->disconnect();
    }
    markTransportDisconnected(message.scaleIndex, -5);
    slot.nextAttemptMs = 0;
    return;
  }

  if (message.type == BleManagerMessageType::ForceCheck) {
    for (BleTransportSlot &slot : bleTransport) {
      if (!slot.connected) {
        // The offline board may have been replaced and therefore have a new
        // BLE address. Rediscover it by service UUID and unit name.
        slot.addressKnown = false;
        slot.consecutiveConnectFailures = 0;
        slot.nextAttemptMs = 0;
      }
    }
    bleForceScanRequested = true;
  }
}

void bleManagerTask(void *) {
  if (!NimBLEDevice::init(LOCAL_BLUETOOTH_NAME)) {
    Serial.println("ERROR: NimBLE initialization failed.");
    queueSimpleBleUiEvent(BleUiEventType::RadioReady, 0, -1);
    vTaskDelete(nullptr);
  }

  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&bleScanCallbacks, true);
  scan->setActiveScan(true);
  scan->setInterval(60);
  scan->setWindow(30);
  // Callback-only scanning cannot fill up in a BLE-dense room. The callback
  // copies each matching address before NimBLE releases its temporary result.
  scan->setMaxResults(0);
  queueSimpleBleUiEvent(BleUiEventType::RadioReady, 0, 1);
  uint32_t nextScanAtMs = 0;

  while (true) {
    BleManagerMessage message;
    while (xQueueReceive(bleManagerQueue, &message, 0) == pdTRUE) {
      handleBleManagerMessage(message);
    }

    for (uint8_t index = 0; index < REMOTE_SCALE_COUNT; ++index) {
      BleTransportSlot &slot = bleTransport[index];
      if (slot.connected &&
          (slot.client == nullptr || !slot.client->isConnected())) {
        markTransportDisconnected(index, slot.client
                                             ? slot.client->getLastError()
                                             : -6);
      }
    }

    if (bleForceScanRequested) {
      bleForceScanRequested = false;
      nextScanAtMs = 0;
    }

    bool attemptedConnection = false;
    // LCD standby is display-only. BLE discovery and reconnect must continue
    // so weighing, locking, and phone control remain available with the panel
    // dark.
    for (uint8_t index = 0; index < REMOTE_SCALE_COUNT; ++index) {
      BleTransportSlot &slot = bleTransport[index];
      if (!slot.connected &&
          slot.addressKnown &&
          static_cast<int32_t>(millis() - slot.nextAttemptMs) >= 0) {
        connectBleScale(index);
        attemptedConnection = true;
        break;
      }
    }
    if (attemptedConnection) {
      nextScanAtMs = millis() + BLE_SCAN_PAUSE_MS;
    }

    bool scanNeeded = false;
    for (const BleTransportSlot &slot : bleTransport) {
      scanNeeded |= !slot.addressKnown;
    }

    if (scanNeeded &&
        !scan->isScanning() &&
        !attemptedConnection &&
        (nextScanAtMs == 0 ||
         static_cast<int32_t>(millis() - nextScanAtMs) >= 0)) {
      if (scan->start(BLE_SCAN_WINDOW_MS, false, true)) {
        nextScanAtMs =
            millis() + BLE_SCAN_WINDOW_MS + BLE_SCAN_PAUSE_MS;
      } else {
        nextScanAtMs = millis() + BLE_SCAN_PAUSE_MS;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

bool startBleManager() {
  bleManagerQueue =
      xQueueCreate(16, sizeof(BleManagerMessage));
  bleUiQueue = xQueueCreate(24, sizeof(BleUiEvent));
  if (bleManagerQueue == nullptr || bleUiQueue == nullptr) {
    Serial.println("ERROR: Could not create BLE queues.");
    return false;
  }

  const BaseType_t created = xTaskCreate(bleManagerTask,
                                         "ble-manager",
                                         8192,
                                         nullptr,
                                         1,
                                         nullptr);
  if (created != pdPASS) {
    Serial.println("ERROR: Could not create BLE manager task.");
    return false;
  }
  return true;
}

bool requestBleReconnect(uint8_t scaleIndex) {
  if (scaleIndex >= REMOTE_SCALE_COUNT ||
      scaleStates[scaleIndex].reconnectRequested) {
    return false;
  }

  BleManagerMessage message;
  message.type = BleManagerMessageType::Reconnect;
  message.scaleIndex = scaleIndex;
  if (queueBleManagerMessage(message)) {
    scaleStates[scaleIndex].reconnectRequested = true;
    return true;
  }
  return false;
}

bool queueScaleCommand(uint8_t scaleIndex,
                       DualScaleBle::CommandType commandType,
                       int32_t valueGrams = 0) {
  if (scaleIndex >= REMOTE_SCALE_COUNT ||
      !scaleStates[scaleIndex].connected ||
      scaleStates[scaleIndex].pendingToken != 0) {
    return false;
  }

  uint16_t token = nextCommandToken++;
  if (token == 0) {
    token = nextCommandToken++;
  }

  BleManagerMessage message;
  message.type = BleManagerMessageType::SendCommand;
  message.scaleIndex = scaleIndex;
  message.command.magic = DualScaleBle::COMMAND_MAGIC;
  message.command.version = DualScaleBle::PROTOCOL_VERSION;
  message.command.targetUnit = scaleIndex + 1;
  message.command.token = token;
  message.command.command = static_cast<uint8_t>(commandType);
  message.command.unlockRequestGeneration =
      scaleStates[scaleIndex].unlockRequestGeneration;
  message.command.valueGrams = valueGrams;

  if (!queueBleManagerMessage(message)) {
    return false;
  }

  scaleStates[scaleIndex].pendingToken = token;
  scaleStates[scaleIndex].pendingCommand = commandType;
  scaleStates[scaleIndex].pendingCommandAtMs = millis();
  return true;
}

void markPeerStatusSyncPending() {
  for (bool &pending : peerStatusSyncPending) {
    pending = true;
  }
}

bool peerScaleConnectedFor(uint8_t scaleIndex) {
  const uint8_t peerIndex = scaleIndex == 0 ? 1 : 0;
  const ScaleState &peer = scaleStates[peerIndex];
  return peer.connected && peer.identityVerified;
}

void servicePeerStatusSync() {
  for (uint8_t index = 0; index < REMOTE_SCALE_COUNT; ++index) {
    ScaleState &state = scaleStates[index];
    if (!peerStatusSyncPending[index] ||
        !state.connected ||
        !state.identityVerified ||
        state.pendingToken != 0) {
      continue;
    }

    // PeerStatus reuses valueGrams as a compact boolean: 1 means the other
    // verified scale is connected to this touchscreen; 0 means disconnected.
    const int32_t peerConnected =
        peerScaleConnectedFor(index) ? 1 : 0;
    if (queueScaleCommand(index,
                          DualScaleBle::CommandType::PeerStatus,
                          peerConnected)) {
      peerStatusSyncPending[index] = false;
      Serial.printf("Reported peer %s to Scale %u.\n",
                    peerConnected ? "connected" : "disconnected",
                    index + 1);
    }
  }
}

bool scaleShouldDisplayLocked(uint8_t scaleIndex) {
  return weightLockPhase == WeightLockPhase::Locked &&
         lockedWeightSnapshot.scaleIncluded[scaleIndex];
}

bool scaleReportsDesiredWeightLock(uint8_t scaleIndex) {
  const ScaleState &state = scaleStates[scaleIndex];
  const bool reportsLocked =
      (state.flags & DualScaleBle::TelemetryFlag::WeightLocked) != 0;
  const bool reportsUnlockRequest =
      (state.flags & DualScaleBle::TelemetryFlag::UnlockRequested) != 0;
  return scaleShouldDisplayLocked(scaleIndex)
             ? reportsLocked && !reportsUnlockRequest
             : !reportsLocked && !reportsUnlockRequest;
}

void serviceWeightLockSync() {
  const uint32_t now = millis();
  for (uint8_t index = 0; index < REMOTE_SCALE_COUNT; ++index) {
    ScaleState &state = scaleStates[index];
    if (!state.connected || !state.identityVerified) {
      continue;
    }

    if (!weightLockSyncPending[index] &&
        !scaleReportsDesiredWeightLock(index)) {
      weightLockSyncPending[index] = true;
      weightLockSyncRetryAtMs[index] = 0;
    }

    if (!weightLockSyncPending[index] ||
        state.pendingToken != 0 ||
        (weightLockSyncRetryAtMs[index] != 0 &&
         static_cast<int32_t>(
             now - weightLockSyncRetryAtMs[index]) < 0)) {
      continue;
    }

    // A latched physical-button request always wins over a desired lock. The
    // touchscreen must clear/acknowledge it before any new settling window.
    const bool unlockRequestPending =
        (state.flags &
         DualScaleBle::TelemetryFlag::UnlockRequested) != 0;
    const bool shouldLock =
        scaleShouldDisplayLocked(index) && !unlockRequestPending;
    const auto command =
        shouldLock
            ? DualScaleBle::CommandType::SetWeightLock
            : DualScaleBle::CommandType::ClearWeightLock;
    const int32_t valueGrams =
        shouldLock ? lockedWeightSnapshot.scaleGrams[index] : 0;
    if (queueScaleCommand(index, command, valueGrams)) {
      weightLockSyncRetryAtMs[index] =
          now + WEIGHT_LOCK_SYNC_RETRY_MS;
      Serial.printf("Synchronizing Scale %u OLED: %s %.2f kg.\n",
                    index + 1,
                    shouldLock ? "LOCK" : "UNLOCK",
                    static_cast<float>(valueGrams) / 1000.0f);
    } else {
      weightLockSyncRetryAtMs[index] =
          now + WEIGHT_LOCK_SYNC_RETRY_MS;
    }
  }
}

void triggerConnectivityCheck() {
  BleManagerMessage forceMessage;
  forceMessage.type = BleManagerMessageType::ForceCheck;
  queueBleManagerMessage(forceMessage);

  uint8_t commandCount = 0;
  for (uint8_t index = 0; index < REMOTE_SCALE_COUNT; ++index) {
    if (queueScaleCommand(index, DualScaleBle::CommandType::Status)) {
      ++commandCount;
    }
  }
  showNotice(commandCount ? "Checking both scales..."
                          : (connectedScaleCount()
                                 ? "Scale command already running"
                                 : "Searching for both scales..."),
             1800);
}

void processBleUiEvents() {
  if (bleUiQueue == nullptr) {
    return;
  }

  BleUiEvent event;
  while (xQueueReceive(bleUiQueue, &event, 0) == pdTRUE) {
    if (event.type == BleUiEventType::RadioReady) {
      bluetoothReady = event.reason > 0;
      if (!bluetoothReady) {
        showNotice("BLE radio failed", 5000);
      }
      markConnectionUiDirty();
      continue;
    }

    if (event.scaleIndex >= REMOTE_SCALE_COUNT) {
      continue;
    }
    ScaleState &state = scaleStates[event.scaleIndex];

    if (event.type == BleUiEventType::Discovered) {
      state.discovered = true;
      markConnectionUiDirty(event.scaleIndex);
      continue;
    }
    if (event.type == BleUiEventType::Connecting) {
      state.connecting = true;
      state.reconnectRequested = false;
      markConnectionUiDirty(event.scaleIndex);
      continue;
    }
    if (event.type == BleUiEventType::Connected) {
      state.connected = true;
      state.connecting = false;
      state.subscribed = true;
      state.reconnectRequested = false;
      state.identityVerified = false;
      state.goodPacketCount = 0;
      state.connectedAtMs = millis();
      state.lastTelemetryMs = 0;
      state.pendingToken = 0;
      state.pendingCommandAtMs = 0;
      state.unlockRequestGeneration = 0;
      state.handledUnlockRequestGeneration = 0;
      state.unlockRequestHandled = false;
      weightLockSyncPending[event.scaleIndex] = true;
      weightLockSyncRetryAtMs[event.scaleIndex] = 0;
      markPeerStatusSyncPending();
      markConnectionUiDirty(event.scaleIndex);
      continue;
    }
    if (event.type == BleUiEventType::Disconnected) {
      state.connected = false;
      state.connecting = false;
      state.subscribed = false;
      state.identityVerified = false;
      state.goodPacketCount = 0;
      state.flags = 0;
      state.reconnectRequested = false;
      state.connectedAtMs = 0;
      state.pendingToken = 0;
      state.pendingCommandAtMs = 0;
      state.unlockRequestGeneration = 0;
      state.handledUnlockRequestGeneration = 0;
      state.unlockRequestHandled = false;
      weightLockSyncPending[event.scaleIndex] = true;
      weightLockSyncRetryAtMs[event.scaleIndex] = 0;
      markPeerStatusSyncPending();
      markConnectionUiDirty(event.scaleIndex);
      continue;
    }
    if (event.type == BleUiEventType::CommandWriteFailed) {
      const bool peerStatusCommand =
          state.pendingCommand == DualScaleBle::CommandType::PeerStatus;
      const bool weightLockCommand =
          state.pendingCommand == DualScaleBle::CommandType::SetWeightLock ||
          state.pendingCommand ==
              DualScaleBle::CommandType::ClearWeightLock;
      state.pendingToken = 0;
      state.pendingCommandAtMs = 0;
      if (peerStatusCommand) {
        peerStatusSyncPending[event.scaleIndex] = true;
      } else if (weightLockCommand) {
        weightLockSyncPending[event.scaleIndex] = true;
        weightLockSyncRetryAtMs[event.scaleIndex] =
            millis() + WEIGHT_LOCK_SYNC_RETRY_MS;
        Serial.printf("Scale %u weight-lock sync write failed; retrying.\n",
                      event.scaleIndex + 1);
      } else {
        showNotice("Scale " + String(event.scaleIndex + 1) +
                       " command failed",
                   3500);
      }
      continue;
    }
    if (event.type != BleUiEventType::Telemetry) {
      continue;
    }

    const DualScaleBle::TelemetryPacket &packet = event.telemetry;
    const bool identityValid =
        packet.magic == DualScaleBle::TELEMETRY_MAGIC &&
        packet.version == DualScaleBle::PROTOCOL_VERSION &&
        packet.unit == event.scaleIndex + 1;
    const bool statusValid =
        packet.status <=
        static_cast<uint8_t>(DualScaleBle::Status::ShuttingDown);
    if (!identityValid || !statusValid) {
      state.identityVerified = false;
      state.goodPacketCount = 0;
      markPeerStatusSyncPending();
      markConnectionUiDirty(event.scaleIndex);
      requestBleReconnect(event.scaleIndex);
      continue;
    }

    const bool identityWasVerified = state.identityVerified;
    const bool wasUsable = scaleUsable(event.scaleIndex);
    const int32_t previousWeightGrams = state.weightGrams;
    const DualScaleBle::Status previousStatus = state.status;
    const uint8_t previousFlags = state.flags;
    const bool newSequence =
        state.lastTelemetryMs == 0 || packet.sequence != state.sequence;
    state.identityVerified = true;
    state.connected = true;
    state.subscribed = true;
    state.sequence = packet.sequence;
    state.weightGrams = packet.weightGrams;
    state.status = static_cast<DualScaleBle::Status>(packet.status);
    state.flags = packet.flags;
    state.commandResult = packet.commandResult;
    state.acknowledgedToken = packet.acknowledgedToken;
    state.unlockRequestGeneration =
        packet.unlockRequestGeneration;
    state.lastTelemetryMs = millis();
    state.reconnectRequested = false;
    if (!identityWasVerified) {
      markPeerStatusSyncPending();
    }

    const bool packetUsable =
        state.status == DualScaleBle::Status::Ready &&
        requiredTelemetryFlagsPresent(state);
    if (!packetUsable) {
      state.goodPacketCount = 0;
    } else if (newSequence) {
      state.goodPacketCount =
          static_cast<uint8_t>(
              min<int>(state.goodPacketCount + 1, 255));
    }

    const bool unlockRequested =
        (state.flags &
         DualScaleBle::TelemetryFlag::UnlockRequested) != 0;
    if (unlockRequested &&
        (!state.unlockRequestHandled ||
         state.handledUnlockRequestGeneration !=
             state.unlockRequestGeneration)) {
      state.unlockRequestHandled = true;
      state.handledUnlockRequestGeneration =
          state.unlockRequestGeneration;
      unlockCombinedWeight(
          event.scaleIndex == 0
              ? "Scale A button"
              : "Scale B button");
    } else if (!unlockRequested) {
      state.unlockRequestHandled = false;
    }

    const bool nonWeightStateChanged =
        previousStatus != state.status ||
        previousFlags != state.flags ||
        wasUsable != scaleUsable(event.scaleIndex);
    if (nonWeightStateChanged) {
      dashboardScaleDirty[event.scaleIndex] = true;
      if (currentPage == Page::Dashboard) {
        uiDirty = true;
      }
    }
    if (weightLockPhase != WeightLockPhase::Locked &&
        previousWeightGrams != state.weightGrams) {
      dashboardScaleWeightDirty[event.scaleIndex] = true;
      if (currentPage == Page::Dashboard) {
        uiDirty = true;
      }
    }

    if (state.pendingToken != 0 &&
        packet.acknowledgedToken == state.pendingToken &&
        packet.commandResult !=
            static_cast<uint8_t>(DualScaleBle::CommandResult::None) &&
        packet.commandResult !=
            static_cast<uint8_t>(DualScaleBle::CommandResult::Accepted)) {
      const bool success =
          packet.commandResult ==
          static_cast<uint8_t>(DualScaleBle::CommandResult::Success);
      const bool setWeightLockCommand =
          state.pendingCommand ==
          DualScaleBle::CommandType::SetWeightLock;
      const bool clearWeightLockCommand =
          state.pendingCommand ==
          DualScaleBle::CommandType::ClearWeightLock;
      if (state.pendingCommand == DualScaleBle::CommandType::PeerStatus) {
        if (!success) {
          peerStatusSyncPending[event.scaleIndex] = true;
        }
      } else if (setWeightLockCommand ||
                 clearWeightLockCommand) {
        const bool desiredSetCommand =
            weightLockPhase == WeightLockPhase::Locked &&
            lockedWeightSnapshot.scaleIncluded[event.scaleIndex];
        const bool commandStillMatches =
            (setWeightLockCommand && desiredSetCommand) ||
            (clearWeightLockCommand && !desiredSetCommand);
        weightLockSyncPending[event.scaleIndex] =
            !success || !commandStillMatches;
        if (weightLockSyncPending[event.scaleIndex]) {
          weightLockSyncRetryAtMs[event.scaleIndex] =
              millis() + WEIGHT_LOCK_SYNC_RETRY_MS;
        }
        Serial.printf("Scale %u weight-lock sync %s.\n",
                      event.scaleIndex + 1,
                      success && commandStillMatches
                          ? "complete"
                          : "will retry");
      } else if (state.pendingCommand != DualScaleBle::CommandType::Status) {
        showNotice("Scale " + String(event.scaleIndex + 1) +
                       (success ? " command complete" : " command failed"),
                   3000);
      }
      state.pendingToken = 0;
      state.pendingCommandAtMs = 0;
    }
  }

  recomputeCombinedWeight();
}

bool scaleReadyForTareCommand(uint8_t scaleIndex) {
  if (scaleIndex >= REMOTE_SCALE_COUNT) {
    return false;
  }
  constexpr uint8_t HEALTH_FLAGS =
      DualScaleBle::TelemetryFlag::SensorPresent |
      DualScaleBle::TelemetryFlag::AdcResponding;
  const ScaleState &state = scaleStates[scaleIndex];
  return state.connected && state.identityVerified &&
         state.lastTelemetryMs != 0 &&
         millis() - state.lastTelemetryMs <= TELEMETRY_STALE_MS &&
         (state.flags & HEALTH_FLAGS) == HEALTH_FLAGS &&
         state.pendingToken == 0 &&
         state.status != DualScaleBle::Status::Taring &&
         state.status != DualScaleBle::Status::Calibrating &&
         state.status != DualScaleBle::Status::ShuttingDown;
}

bool scaleSensorHealthyForCalibration(uint8_t scaleIndex) {
  if (scaleIndex >= REMOTE_SCALE_COUNT) {
    return false;
  }
  constexpr uint8_t HEALTH_FLAGS =
      DualScaleBle::TelemetryFlag::SensorPresent |
      DualScaleBle::TelemetryFlag::AdcResponding;
  const ScaleState &state = scaleStates[scaleIndex];
  return state.connected && state.identityVerified &&
         state.lastTelemetryMs != 0 &&
         millis() - state.lastTelemetryMs <= TELEMETRY_STALE_MS &&
         (state.flags & HEALTH_FLAGS) == HEALTH_FLAGS &&
         state.status != DualScaleBle::Status::NoSensor &&
         state.status != DualScaleBle::Status::SensorError &&
         state.status != DualScaleBle::Status::ShuttingDown;
}

ControlActionResult requestControllerTare(uint8_t targetMask,
                                          const char *source) {
  ControlActionResult result;
  if ((targetMask & 0x03U) == 0 || (targetMask & ~0x03U) != 0) {
    result.httpStatus = 400;
    result.message = "Invalid tare target";
    return result;
  }

  uint8_t requestedCount = 0;
  for (uint8_t index = 0; index < REMOTE_SCALE_COUNT; ++index) {
    if ((targetMask & (1U << index)) == 0) {
      continue;
    }
    ++requestedCount;
  }

  // Preflight every selected scale before queueing any command. In
  // particular, TARE BOTH must be all-or-nothing so the two zero offsets
  // cannot silently become mismatched.
  for (uint8_t index = 0; index < REMOTE_SCALE_COUNT; ++index) {
    if ((targetMask & (1U << index)) == 0 ||
        scaleReadyForTareCommand(index)) {
      continue;
    }
    result.httpStatus = 409;
    result.message = scaleStates[index].connected
                         ? "Load Cell " + String(index == 0 ? "A" : "B") +
                               " is busy or not ready"
                         : "Load Cell " + String(index == 0 ? "A" : "B") +
                               " is not connected";
    showNotice(result.message, 3500);
    return result;
  }

  if (bleManagerQueue == nullptr ||
      uxQueueSpacesAvailable(bleManagerQueue) < requestedCount) {
    result.httpStatus = 503;
    result.message = "BLE command queue is busy; try again";
    showNotice(result.message, 3000);
    return result;
  }

  bool queued[REMOTE_SCALE_COUNT] = {};
  uint8_t queuedCount = 0;
  for (uint8_t index = 0; index < REMOTE_SCALE_COUNT; ++index) {
    if ((targetMask & (1U << index)) == 0) {
      continue;
    }
    queued[index] =
        queueScaleCommand(index, DualScaleBle::CommandType::Tare);
    queuedCount += static_cast<uint8_t>(queued[index]);
  }

  if (queuedCount != requestedCount) {
    result.httpStatus = 503;
    result.message = "Could not queue every tare command; try again";
    showNotice(result.message, 3500);
    return result;
  }

  // Unlock only after at least one tare command is safely in the BLE queue.
  unlockCombinedWeight(source);
  result.accepted = true;
  result.httpStatus = 202;
  if (queued[0] && queued[1]) {
    result.message = "Taring both scales...";
  } else if (queued[0]) {
    result.message = "Taring Load Cell A...";
  } else {
    result.message = "Taring Load Cell B...";
  }
  showNotice(result.message, 3000);
  Serial.printf("Controller tare accepted from %s (mask=0x%02X).\n",
                source ? source : "unknown",
                targetMask);
  return result;
}

ControlActionResult requestControllerCalibration(uint8_t scaleIndex,
                                                 int32_t valueGrams,
                                                 const char *source) {
  ControlActionResult result;
  if (scaleIndex >= REMOTE_SCALE_COUNT ||
      valueGrams < 100 || valueGrams > 100000) {
    result.httpStatus = 400;
    result.message = "Calibration weight must be 0.1 to 100 kg";
    return result;
  }

  ScaleState &state = scaleStates[scaleIndex];
  if (!state.connected || !state.identityVerified) {
    result.message = "Selected scale is not connected";
    showNotice(result.message, 3500);
    return result;
  }
  if (scaleOperationCommandPending() || state.pendingToken != 0 ||
      state.status == DualScaleBle::Status::Taring ||
      state.status == DualScaleBle::Status::Calibrating) {
    result.message = "A scale operation is already running";
    showNotice(result.message, 3500);
    return result;
  }
  if (!scaleSensorHealthyForCalibration(scaleIndex)) {
    result.message = "Selected load-cell ADC is not ready";
    showNotice(result.message, 3500);
    return result;
  }
  if ((state.flags & DualScaleBle::TelemetryFlag::Tared) == 0) {
    result.message = "Tare the selected empty scale first";
    showNotice(result.message, 4000);
    return result;
  }
  if (!queueScaleCommand(scaleIndex,
                         DualScaleBle::CommandType::Calibrate,
                         valueGrams)) {
    result.message = "Could not queue calibration command";
    showNotice(result.message, 3500);
    return result;
  }

  // Unlock only after calibration has been accepted by the manager queue.
  unlockCombinedWeight(source);
  result.accepted = true;
  result.httpStatus = 202;
  result.message = "Calibrating Load Cell " +
                   String(scaleIndex == 0 ? "A" : "B") + " with " +
                   String(valueGrams / 1000.0f, 3) + " kg...";
  showNotice(result.message, 4500);
  Serial.printf("Controller calibration accepted from %s for Scale %u.\n",
                source ? source : "unknown",
                scaleIndex + 1);
  return result;
}

ControlActionResult requestControllerUnlock(const char *source) {
  ControlActionResult result;
  const bool wasLocked = weightLockPhase == WeightLockPhase::Locked;
  const bool wasSettling = weightLockPhase == WeightLockPhase::Settling;
  unlockCombinedWeight(source);
  result.accepted = true;
  result.httpStatus = 200;
  result.message = wasLocked
                       ? "Weight unlocked; automatic locking re-armed"
                       : wasSettling
                             ? "Lock timer cancelled and re-armed"
                             : "Automatic weight lock re-armed";
  showNotice(result.message, 2200);
  return result;
}

void adjustCalibrationWeight(float amount) {
  const float adjustedWeightKg =
      roundf(constrain(calibrationWeightKg + amount, 0.1f, 100.0f) *
             10.0f) /
      10.0f;
  if (fabsf(adjustedWeightKg - calibrationWeightKg) < 0.001f) {
    return;
  }

  calibrationWeightKg = adjustedWeightKg;
  if (currentPage == Page::Calibration && !screenSleeping &&
      !idleWarningActive && !pageDirty) {
    drawCalibrationWeightValue();
  } else {
    uiDirty = true;
  }
}

void finishCalibrationWeightRepeat(bool redrawButton) {
  if (redrawButton &&
      activeCalibrationAdjustButton <
          CALIBRATION_ADJUST_BUTTON_COUNT &&
      currentPage == Page::Calibration && !screenSleeping &&
      !idleWarningActive && !pageDirty) {
    drawCalibrationAdjustButton(activeCalibrationAdjustButton, false);
  }

  activeCalibrationAdjustButton = NO_CALIBRATION_ADJUST_BUTTON;
  calibrationRepeatPhase = CalibrationRepeatPhase::Idle;
  nextCalibrationRepeatAtMs = 0;
}

void cancelCalibrationWeightRepeatUntilRelease(bool redrawButton) {
  if (redrawButton &&
      activeCalibrationAdjustButton <
          CALIBRATION_ADJUST_BUTTON_COUNT &&
      currentPage == Page::Calibration && !screenSleeping &&
      !idleWarningActive && !pageDirty) {
    drawCalibrationAdjustButton(activeCalibrationAdjustButton, false);
  }

  activeCalibrationAdjustButton = NO_CALIBRATION_ADJUST_BUTTON;
  calibrationRepeatPhase =
      CalibrationRepeatPhase::CancelledUntilRelease;
  nextCalibrationRepeatAtMs = 0;
}

void beginCalibrationWeightRepeat(uint8_t buttonIndex) {
  if (buttonIndex >= CALIBRATION_ADJUST_BUTTON_COUNT ||
      calibrationRepeatPhase != CalibrationRepeatPhase::Idle) {
    return;
  }

  activeCalibrationAdjustButton = buttonIndex;
  calibrationRepeatPhase = CalibrationRepeatPhase::Waiting;
  nextCalibrationRepeatAtMs =
      millis() + CALIBRATION_REPEAT_DELAY_MS;

  adjustCalibrationWeight(
      CALIBRATION_ADJUST_BUTTONS[buttonIndex].adjustmentKg);
  if (!screenSleeping && !idleWarningActive && !pageDirty) {
    drawCalibrationAdjustButton(buttonIndex, true);
  }
}

void serviceCalibrationWeightRepeat(const TouchSample *sample) {
  if (sample == nullptr) {
    finishCalibrationWeightRepeat(true);
    return;
  }

  if (calibrationRepeatPhase == CalibrationRepeatPhase::Idle ||
      calibrationRepeatPhase ==
          CalibrationRepeatPhase::CancelledUntilRelease) {
    return;
  }

  if (currentPage != Page::Calibration || screenSleeping ||
      idleWarningActive ||
      !touchRemainsOnCalibrationAdjustButton(*sample)) {
    cancelCalibrationWeightRepeatUntilRelease(true);
    return;
  }

  const uint32_t now = millis();
  if (static_cast<int32_t>(now - nextCalibrationRepeatAtMs) < 0) {
    return;
  }

  adjustCalibrationWeight(
      CALIBRATION_ADJUST_BUTTONS[activeCalibrationAdjustButton]
          .adjustmentKg);
  calibrationRepeatPhase = CalibrationRepeatPhase::Repeating;
  // Schedule from now rather than catching up after a slow display update.
  nextCalibrationRepeatAtMs =
      now + CALIBRATION_REPEAT_INTERVAL_MS;
}

void handleDashboardTouch(const TouchSample &sample) {
  if (pointInRect(sample.x, sample.y, 123, 7, 51, 28)) {
    changePage(Page::Connectivity);
    triggerConnectivityCheck();
    return;
  }

  if (pointInRect(sample.x, sample.y, 181, 7, 54, 28)) {
    changePage(Page::Settings);
    return;
  }

  if (pointInRect(sample.x, sample.y, 6, 256, 108, 56)) {
    if (weightLockPhase == WeightLockPhase::Locked) {
      requestControllerUnlock("touchscreen UNLOCK button");
      return;
    }

    requestControllerTare(0x03, "touchscreen TARE ALL");
    return;
  }

  if (pointInRect(sample.x, sample.y, 126, 256, 108, 56)) {
    changePage(Page::Calibration);
  }
}

void handleCalibrationTouch(const TouchSample &sample) {
  if (pointInRect(sample.x, sample.y, 5, 7, 55, 28)) {
    changePage(Page::Dashboard);
    return;
  }

  if (pointInRect(sample.x, sample.y, 6, 158, 111, 30)) {
    calibrationTargetIndex = 0;
    uiDirty = true;
    return;
  }

  if (pointInRect(sample.x, sample.y, 123, 158, 111, 30)) {
    calibrationTargetIndex = 1;
    uiDirty = true;
    return;
  }

  const int8_t adjustButtonIndex =
      calibrationAdjustButtonAt(sample);
  if (adjustButtonIndex >= 0) {
    beginCalibrationWeightRepeat(
        static_cast<uint8_t>(adjustButtonIndex));
    return;
  }

  if (pointInRect(sample.x, sample.y, 6, 232, 228, 36)) {
    requestControllerTare(
        static_cast<uint8_t>(1U << calibrationTargetIndex),
        "touchscreen tare command");
    return;
  }

  if (pointInRect(sample.x, sample.y, 6, 276, 228, 37)) {
    const int32_t valueGrams =
        static_cast<int32_t>(lroundf(calibrationWeightKg * 1000.0f));
    const ControlActionResult result =
        requestControllerCalibration(calibrationTargetIndex,
                                     valueGrams,
                                     "touchscreen calibration command");
    if (result.accepted) {
      changePage(Page::Dashboard);
    }
  }
}

void handleSettingsTouch(const TouchSample &sample) {
  if (pointInRect(sample.x, sample.y, 5, 7, 55, 28)) {
    changePage(Page::Dashboard);
    return;
  }

  if (pointInRect(sample.x, sample.y, 12, 116, 92, 34)) {
    adjustBrightness(-BRIGHTNESS_STEP_PERCENT);
    return;
  }

  if (pointInRect(sample.x, sample.y, 136, 116, 92, 34)) {
    adjustBrightness(BRIGHTNESS_STEP_PERCENT);
    return;
  }

  if (pointInRect(sample.x, sample.y, 12, 230, 102, 40)) {
    selectCpuFrequency(160);
    return;
  }

  if (pointInRect(sample.x, sample.y, 126, 230, 102, 40)) {
    selectCpuFrequency(240);
    return;
  }

  if (pointInRect(sample.x, sample.y, 6, 284, 228, 31)) {
    changePage(Page::ModeSelection);
  }
}

void handleConnectivityTouch(const TouchSample &sample) {
  if (pointInRect(sample.x, sample.y, 5, 7, 55, 28)) {
    changePage(Page::Dashboard);
    return;
  }

  if (pointInRect(sample.x, sample.y, 6, 278, 228, 35)) {
    triggerConnectivityCheck();
  }
}

void handleOperatingModeTouch(const TouchSample &sample) {
  if (pointInRect(sample.x, sample.y, 5, 7, 55, 28)) {
    changePage(operatingMode == OperatingMode::Web
                   ? Page::WebStatus
                   : Page::Settings);
    return;
  }
  if (pointInRect(sample.x, sample.y, 6, 51, 228, 68)) {
    applyOperatingMode(OperatingMode::Local);
    return;
  }
  if (pointInRect(sample.x, sample.y, 6, 127, 228, 68)) {
    applyOperatingMode(OperatingMode::Web);
    return;
  }
  if (pointInRect(sample.x, sample.y, 6, 203, 228, 68)) {
    applyOperatingMode(OperatingMode::Hybrid);
  }
}

void handleWebStatusTouch(const TouchSample &sample) {
  if (pointInRect(sample.x, sample.y, 6, 271, 108, 42)) {
    changePage(Page::ModeSelection);
    return;
  }
  if (pointInRect(sample.x, sample.y, 126, 271, 108, 42)) {
    setScreenSleeping(true);
  }
}

void processTouch(const TouchSample &sample, bool newTouch) {
  if (!newTouch || millis() - lastTouchActionMs < 180) {
    return;
  }
  lastTouchActionMs = millis();

  Serial.printf("Touch: x=%u y=%u\n", sample.x, sample.y);
  if (currentPage == Page::Dashboard) {
    handleDashboardTouch(sample);
  } else if (currentPage == Page::Calibration) {
    handleCalibrationTouch(sample);
  } else if (currentPage == Page::Settings) {
    handleSettingsTouch(sample);
  } else if (currentPage == Page::Connectivity) {
    handleConnectivityTouch(sample);
  } else if (currentPage == Page::ModeSelection) {
    handleOperatingModeTouch(sample);
  } else if (currentPage == Page::WebStatus) {
    handleWebStatusTouch(sample);
  }
}

const char *phoneLockLabel() {
  if (weightLockPhase == WeightLockPhase::Locked) {
    return "LOCKED";
  }
  if (weightLockPhase == WeightLockPhase::Settling) {
    return "LOCKING";
  }
  return "LIVE";
}

const char *phonePendingCommandLabel(DualScaleBle::CommandType command) {
  switch (command) {
  case DualScaleBle::CommandType::Tare:
    return "TARE";
  case DualScaleBle::CommandType::Calibrate:
    return "CALIBRATE";
  case DualScaleBle::CommandType::PeerStatus:
    return "PEER SYNC";
  case DualScaleBle::CommandType::SetWeightLock:
    return "LOCK SYNC";
  case DualScaleBle::CommandType::ClearWeightLock:
    return "UNLOCK SYNC";
  case DualScaleBle::CommandType::Status:
  default:
    return "STATUS";
  }
}

String phonePendingLabel(uint8_t scaleIndex) {
  const ScaleState &state = scaleStates[scaleIndex];
  if (state.pendingToken != 0) {
    return String(phonePendingCommandLabel(state.pendingCommand)) + " #" +
           String(state.pendingToken);
  }
  if (state.status == DualScaleBle::Status::Taring) {
    return "TARING";
  }
  if (state.status == DualScaleBle::Status::Calibrating) {
    return "CALIBRATING";
  }
  return "IDLE";
}

void appendJsonString(String &json, const String &value) {
  json += '"';
  for (size_t index = 0; index < value.length(); ++index) {
    const char character = value.charAt(index);
    switch (character) {
    case '"':
      json += "\\\"";
      break;
    case '\\':
      json += "\\\\";
      break;
    case '\n':
      json += "\\n";
      break;
    case '\r':
      json += "\\r";
      break;
    case '\t':
      json += "\\t";
      break;
    default:
      json += (static_cast<uint8_t>(character) < 0x20 ? ' ' : character);
      break;
    }
  }
  json += '"';
}

void appendJsonBool(String &json, bool value) {
  json += value ? "true" : "false";
}

void appendJsonUint64(String &json, uint64_t value) {
  char buffer[24];
  snprintf(buffer,
           sizeof(buffer),
           "%llu",
           static_cast<unsigned long long>(value));
  json += buffer;
}

void sendPhoneJson(int statusCode, const String &json) {
  phoneWebServer.sendHeader("Cache-Control", "no-store, max-age=0");
  phoneWebServer.sendHeader("X-Content-Type-Options", "nosniff");
  phoneWebServer.send(statusCode, "application/json", json);
}

void sendPhoneMessage(int statusCode,
                      bool accepted,
                      const String &message) {
  String json;
  json.reserve(message.length() + 48);
  json = "{\"ok\":";
  appendJsonBool(json, accepted);
  json += ",\"message\":";
  appendJsonString(json, message);
  json += '}';
  sendPhoneJson(statusCode, json);
}

void notePhoneWebActivity() {
  lastPhoneWebActivityMs = millis();
  if (currentPage == Page::WebStatus) {
    uiDirty = true;
  }
}

bool validatePhonePost() {
  if (phoneWebServer.header("X-Scale-Control") != "1") {
    sendPhoneMessage(403, false, "Control request was not authorized");
    return false;
  }
  notePhoneWebActivity();

  const uint32_t now = millis();
  if (lastPhoneActionMs != 0 &&
      now - lastPhoneActionMs < PHONE_ACTION_MIN_INTERVAL_MS) {
    sendPhoneMessage(429, false, "Please wait before sending another command");
    return false;
  }
  lastPhoneActionMs = now;
  return true;
}

bool parsePhoneTarget(const String &argument,
                      uint8_t &targetMask,
                      bool allowBoth) {
  String target = argument;
  target.trim();
  target.toLowerCase();
  if (target == "a" || target == "1") {
    targetMask = 0x01;
    return true;
  }
  if (target == "b" || target == "2") {
    targetMask = 0x02;
    return true;
  }
  if (allowBoth && (target == "all" || target == "both")) {
    targetMask = 0x03;
    return true;
  }
  return false;
}

bool parsePhoneLong(const String &argument, long &value) {
  String text = argument;
  text.trim();
  if (text.isEmpty() || text.length() > 12) {
    return false;
  }
  char *end = nullptr;
  value = strtol(text.c_str(), &end, 10);
  return end != text.c_str() && end != nullptr && *end == '\0';
}

bool parsePhoneUint64(const String &argument, uint64_t &value) {
  String text = argument;
  text.trim();
  if (text.isEmpty() || text.length() > 20) {
    return false;
  }

  uint64_t parsed = 0;
  for (size_t index = 0; index < text.length(); ++index) {
    const char character = text.charAt(index);
    if (character < '0' || character > '9') {
      return false;
    }
    const uint8_t digit = static_cast<uint8_t>(character - '0');
    if (parsed > (UINT64_MAX - digit) / 10ULL) {
      return false;
    }
    parsed = parsed * 10ULL + digit;
  }
  value = parsed;
  return true;
}

bool parsePhoneUint32(const String &argument, uint32_t &value) {
  uint64_t parsed = 0;
  if (!parsePhoneUint64(argument, parsed) || parsed == 0 ||
      parsed > UINT32_MAX) {
    return false;
  }
  value = static_cast<uint32_t>(parsed);
  return true;
}

bool validUtf8Text(const String &value,
                   size_t maximumBytes,
                   bool allowEmpty,
                   bool allowLineBreaks) {
  const size_t length = value.length();
  if ((!allowEmpty && length == 0) || length > maximumBytes) {
    return false;
  }

  const uint8_t *bytes =
      reinterpret_cast<const uint8_t *>(value.c_str());
  for (size_t index = 0; index < length;) {
    const uint8_t first = bytes[index];
    if (first < 0x80) {
      if (first == 0 ||
          (first < 0x20 &&
           !(allowLineBreaks && (first == '\n' || first == '\t')))) {
        return false;
      }
      ++index;
      continue;
    }

    size_t continuationCount = 0;
    if (first >= 0xC2 && first <= 0xDF) {
      continuationCount = 1;
    } else if (first >= 0xE0 && first <= 0xEF) {
      continuationCount = 2;
    } else if (first >= 0xF0 && first <= 0xF4) {
      continuationCount = 3;
    } else {
      return false;
    }
    if (index + continuationCount >= length) {
      return false;
    }
    for (size_t offset = 1; offset <= continuationCount; ++offset) {
      if ((bytes[index + offset] & 0xC0U) != 0x80U) {
        return false;
      }
    }
    if ((first == 0xE0 && bytes[index + 1] < 0xA0) ||
        (first == 0xED && bytes[index + 1] >= 0xA0) ||
        (first == 0xF0 && bytes[index + 1] < 0x90) ||
        (first == 0xF4 && bytes[index + 1] >= 0x90)) {
      return false;
    }
    index += continuationCount + 1;
  }
  return true;
}

String historyDisplayId(uint32_t id) {
  char buffer[20];
  snprintf(buffer,
           sizeof(buffer),
           "LOG-%06lu",
           static_cast<unsigned long>(id));
  return String(buffer);
}

const char *historyModeLabel(uint8_t mode) {
  switch (static_cast<CombinedMode>(mode)) {
  case CombinedMode::Dual:
    return "A + B combined";
  case CombinedMode::Scale1Only:
    return "Load Cell A only";
  case CombinedMode::Scale2Only:
    return "Load Cell B only";
  case CombinedMode::None:
  default:
    return "No valid scale";
  }
}

const char *historyScaleStatusLabel(uint8_t status) {
  switch (static_cast<DualScaleBle::Status>(status)) {
  case DualScaleBle::Status::Ready:
    return "Ready";
  case DualScaleBle::Status::NeedsCalibration:
    return "Needs calibration";
  case DualScaleBle::Status::NeedsTare:
    return "Needs tare";
  case DualScaleBle::Status::Taring:
    return "Taring";
  case DualScaleBle::Status::Calibrating:
    return "Calibrating";
  case DualScaleBle::Status::NoSensor:
    return "No sensor";
  case DualScaleBle::Status::SensorError:
    return "Sensor error";
  case DualScaleBle::Status::ShuttingDown:
    return "Shutting down";
  case DualScaleBle::Status::Starting:
  default:
    return "Starting";
  }
}

void appendHistoryScaleJson(String &json,
                            const ScaleHistory::Record &record,
                            uint8_t index) {
  const uint8_t bit = 1U << index;
  const uint8_t flags = record.scaleFlags[index];
  json += "{\"included\":";
  appendJsonBool(json, (record.includedMask & bit) != 0);
  json += ",\"connected\":";
  appendJsonBool(json, (record.connectionMask & bit) != 0);
  json += ",\"grams\":";
  if ((record.includedMask & bit) != 0) {
    json += record.scaleGrams[index];
  } else {
    json += "null";
  }
  json += ",\"status\":";
  appendJsonString(json, historyScaleStatusLabel(record.scaleStatus[index]));
  json += ",\"sensor\":";
  appendJsonBool(json,
                 (flags & DualScaleBle::TelemetryFlag::SensorPresent) != 0);
  json += ",\"adc\":";
  appendJsonBool(json,
                 (flags & DualScaleBle::TelemetryFlag::AdcResponding) != 0);
  json += ",\"oled\":";
  appendJsonBool(json,
                 (flags & DualScaleBle::TelemetryFlag::OledPresent) != 0);
  json += ",\"tared\":";
  appendJsonBool(json,
                 (flags & DualScaleBle::TelemetryFlag::Tared) != 0);
  json += ",\"calibrated\":";
  appendJsonBool(json,
                 (flags & DualScaleBle::TelemetryFlag::Calibrated) != 0);
  json += '}';
}

void appendHistoryRecordJson(String &json,
                             const ScaleHistory::Record &record,
                             bool includeDetails) {
  json += "{\"id\":";
  json += static_cast<unsigned long>(record.id);
  json += ",\"displayId\":";
  appendJsonString(json, historyDisplayId(record.id));
  json += ",\"revision\":";
  json += static_cast<unsigned int>(record.revision);
  json += ",\"name\":";
  appendJsonString(json, String(record.name));
  json += ",\"loggedAtMs\":";
  appendJsonUint64(json, record.loggedAtMs);
  json += ",\"updatedAtMs\":";
  appendJsonUint64(json, record.updatedAtMs);
  json += ",\"timezoneOffsetMinutes\":";
  json += record.timezoneOffsetMinutes;
  json += ",\"timezone\":";
  appendJsonString(json, String(record.timezone));
  json += ",\"totalGrams\":";
  json += record.totalGrams;
  json += ",\"mode\":";
  appendJsonString(json, historyModeLabel(record.combinedMode));
  json += ",\"includedMask\":";
  json += static_cast<unsigned int>(record.includedMask);
  if (includeDetails) {
    json += ",\"notes\":";
    appendJsonString(json, String(record.notes));
    json += ",\"timeSource\":\"iPhone browser\",\"scales\":[";
    appendHistoryScaleJson(json, record, 0);
    json += ',';
    appendHistoryScaleJson(json, record, 1);
    json += ']';
  }
  json += '}';
}

bool parsePhoneCalibrationGrams(const String &argument,
                                int32_t &valueGrams) {
  String text = argument;
  text.trim();
  if (text.isEmpty() || text.length() > 16) {
    return false;
  }
  char *end = nullptr;
  const double valueKg = strtod(text.c_str(), &end);
  if (end == text.c_str() || end == nullptr || *end != '\0' ||
      !isfinite(valueKg) || valueKg < 0.1 || valueKg > 100.0) {
    return false;
  }
  valueGrams = static_cast<int32_t>(lround(valueKg * 1000.0));
  return valueGrams >= 100 && valueGrams <= 100000;
}

void handlePhoneRoot() {
  notePhoneWebActivity();
  phoneWebServer.sendHeader("Cache-Control", "no-store, max-age=0");
  phoneWebServer.sendHeader("X-Content-Type-Options", "nosniff");
  phoneWebServer.sendHeader(
      "Content-Security-Policy",
      "default-src 'self'; style-src 'self' 'unsafe-inline'; "
      "script-src 'self' 'unsafe-inline'; connect-src 'self'");
  phoneWebServer.send_P(200, "text/html", PHONE_CONTROL_PAGE_HTML);
}

void handlePhoneStatus() {
  notePhoneWebActivity();
  static String json;
  // reserve() is a no-op after the retained String buffer reaches this size.
  json.reserve(3200);
  json = "{";
  json += "\"version\":1,\"uptimeMs\":";
  json += static_cast<unsigned long>(millis());
  json += ",\"freeHeap\":";
  json += static_cast<unsigned long>(ESP.getFreeHeap());
  json += ",\"operatingMode\":";
  appendJsonString(json, operatingModeApiName(operatingMode));
  json += ",\"wifi\":{\"enabled\":";
  appendJsonBool(json, phoneControlReady);
  json += ",\"ssid\":";
  appendJsonString(json, PHONE_WIFI_SSID);
  json += ",\"ip\":";
  appendJsonString(json, WiFi.softAPIP().toString());
  json += ",\"clients\":";
  json += static_cast<unsigned int>(phoneClientCount);
  json += "},\"bleReady\":";
  appendJsonBool(json, bluetoothReady);
  json += ",\"lock\":";
  appendJsonString(json, phoneLockLabel());

  const CombinedMode displayedMode =
      weightLockPhase == WeightLockPhase::Locked
          ? lockedWeightSnapshot.mode
          : combinedMode;
  String modeLabel;
  switch (displayedMode) {
  case CombinedMode::Dual:
    modeLabel = "A + B combined";
    break;
  case CombinedMode::Scale1Only:
    modeLabel = "Partial reading: Load Cell A only";
    break;
  case CombinedMode::Scale2Only:
    modeLabel = "Partial reading: Load Cell B only";
    break;
  case CombinedMode::None:
  default:
    modeLabel = "Waiting for valid scale data";
    break;
  }
  json += ",\"modeLabel\":";
  appendJsonString(json, modeLabel);

  const bool totalValid =
      weightLockPhase == WeightLockPhase::Locked
          ? lockedWeightSnapshot.mode != CombinedMode::None
          : weightValid;
  const int32_t displayedTotalGrams =
      weightLockPhase == WeightLockPhase::Locked
          ? lockedWeightSnapshot.totalGrams
          : latestCombinedGrams;
  json += ",\"totalGrams\":";
  if (totalValid) {
    json += displayedTotalGrams;
  } else {
    json += "null";
  }
  json += ",\"totalValid\":";
  appendJsonBool(json, totalValid);

  const bool canTareBoth = scaleReadyForTareCommand(0) &&
                           scaleReadyForTareCommand(1);
  json += ",\"canTareBoth\":";
  appendJsonBool(json, canTareBoth);
  json += ",\"operationBusy\":";
  appendJsonBool(json, scaleOperationCommandPending());
  json += ",\"notice\":";
  appendJsonString(json, noticeActive() ? noticeMessage : String());

  const bool canSaveCurrentLock =
      historyStore.ready() &&
      weightLockPhase == WeightLockPhase::Locked &&
      lockedWeightSnapshot.mode != CombinedMode::None &&
      !currentWeightLockSaved;
  json += ",\"history\":{\"ready\":";
  appendJsonBool(json, historyStore.ready());
  json += ",\"count\":";
  json += static_cast<unsigned int>(historyStore.count());
  json += ",\"capacity\":";
  json += static_cast<unsigned int>(historyStore.capacity());
  json += ",\"canSaveCurrentLock\":";
  appendJsonBool(json, canSaveCurrentLock);
  json += ",\"currentLockSaved\":";
  appendJsonBool(json, currentWeightLockSaved);
  json += ",\"lockToken\":";
  json += static_cast<unsigned long>(currentWeightLockToken);
  json += '}';

  json += ",\"display\":{\"brightness\":";
  json += static_cast<unsigned int>(brightnessPercent);
  json += ",\"effectiveBrightness\":";
  json += static_cast<unsigned int>(effectiveBrightnessPercent());
  json += ",\"cpuMHz\":";
  json += cpuFrequencyMhz;
  json += ",\"sleeping\":";
  appendJsonBool(json, screenSleeping);
  json += "},\"scales\":[";

  for (uint8_t index = 0; index < REMOTE_SCALE_COUNT; ++index) {
    if (index != 0) {
      json += ',';
    }
    const ScaleState &state = scaleStates[index];
    const bool locked = weightLockPhase == WeightLockPhase::Locked;
    const bool lockedContributor =
        locked && lockedWeightSnapshot.scaleIncluded[index];
    const bool displayedWeightValid =
        locked ? lockedContributor : scaleUsable(index);
    const int32_t displayedScaleGrams =
        lockedContributor ? lockedWeightSnapshot.scaleGrams[index]
                          : state.weightGrams;
    const bool busy = state.pendingToken != 0 ||
                      state.status == DualScaleBle::Status::Taring ||
                      state.status == DualScaleBle::Status::Calibrating;
    const bool canCalibrate =
        !scaleOperationCommandPending() && !busy &&
        scaleSensorHealthyForCalibration(index) &&
        (state.flags & DualScaleBle::TelemetryFlag::Tared) != 0;

    json += "{\"unit\":";
    json += index + 1;
    json += ",\"connected\":";
    appendJsonBool(json, state.connected && state.identityVerified);
    json += ",\"usable\":";
    appendJsonBool(json, scaleUsable(index));
    json += ",\"busy\":";
    appendJsonBool(json, busy);
    json += ",\"canTare\":";
    appendJsonBool(json, scaleReadyForTareCommand(index));
    json += ",\"canCalibrate\":";
    appendJsonBool(json, canCalibrate);
    json += ",\"weightGrams\":";
    if (displayedWeightValid) {
      json += displayedScaleGrams;
    } else {
      json += "null";
    }
    json += ",\"weightValid\":";
    appendJsonBool(json, displayedWeightValid);
    json += ",\"system\":";
    appendJsonString(json, dashboardScaleStatus(index));
    json += ",\"link\":";
    appendJsonString(json, scaleLinkLabel(index));
    json += ",\"adc\":";
    appendJsonString(json, scaleAdcLabel(index));
    json += ",\"oled\":";
    appendJsonString(json, scaleOledLabel(index));
    json += ",\"data\":";
    appendJsonString(json, scaleDataLabel(index));
    json += ",\"age\":";
    appendJsonString(json, scaleAgeLabel(index));
    json += ",\"pending\":";
    appendJsonString(json, phonePendingLabel(index));
    json += '}';
  }
  json += "]}";
  sendPhoneJson(200, json);
}

bool readHistoryTextFields(String &name, String &notes) {
  if (!phoneWebServer.hasArg("name") ||
      !phoneWebServer.hasArg("notes")) {
    sendPhoneMessage(400, false, "Name and notes fields are required");
    return false;
  }
  name = phoneWebServer.arg("name");
  notes = phoneWebServer.arg("notes");
  name.trim();
  notes.replace("\r\n", "\n");
  notes.replace("\r", "\n");
  notes.trim();
  if (!validUtf8Text(name,
                     ScaleHistory::NAME_CAPACITY - 1,
                     false,
                     false)) {
    sendPhoneMessage(400,
                     false,
                     "Name must be 1 to 50 valid text bytes");
    return false;
  }
  if (!validUtf8Text(notes,
                     ScaleHistory::NOTES_CAPACITY - 1,
                     true,
                     true)) {
    sendPhoneMessage(400,
                     false,
                     "Notes must be no more than 500 valid text bytes");
    return false;
  }
  return true;
}

bool readHistoryTimestamp(uint64_t &timestampMs) {
  if (!phoneWebServer.hasArg("timestampMs") ||
      !parsePhoneUint64(phoneWebServer.arg("timestampMs"), timestampMs) ||
      timestampMs < HISTORY_MIN_TIMESTAMP_MS ||
      timestampMs > HISTORY_MAX_TIMESTAMP_MS) {
    sendPhoneMessage(400,
                     false,
                     "Phone date/time must be between 2020 and 2100");
    return false;
  }
  return true;
}

void sendHistoryStoreFailure(ScaleHistory::Result result) {
  if (result == ScaleHistory::Result::NotFound) {
    sendPhoneMessage(404, false, "History entry was not found");
  } else if (result == ScaleHistory::Result::Conflict) {
    sendPhoneMessage(409,
                     false,
                     "This entry changed on another phone; reopen Details");
  } else if (result == ScaleHistory::Result::NotReady) {
    sendPhoneMessage(503, false, "History storage is unavailable");
  } else {
    sendPhoneMessage(500, false, "History storage operation failed");
  }
}

void handlePhoneHistorySave() {
  if (!validatePhonePost()) {
    return;
  }
  if (!historyStore.ready()) {
    sendPhoneMessage(503, false, "History storage is unavailable");
    return;
  }

  uint32_t lockToken = 0;
  uint64_t timestampMs = 0;
  long timezoneOffset = 0;
  String name;
  String notes;
  String timezone;
  if (!phoneWebServer.hasArg("lockToken") ||
      !parsePhoneUint32(phoneWebServer.arg("lockToken"), lockToken)) {
    sendPhoneMessage(400, false, "Weight-lock token is invalid");
    return;
  }
  if (!readHistoryTimestamp(timestampMs)) {
    return;
  }
  if (!phoneWebServer.hasArg("timezoneOffsetMinutes") ||
      !parsePhoneLong(phoneWebServer.arg("timezoneOffsetMinutes"),
                      timezoneOffset) ||
      timezoneOffset < HISTORY_MIN_TIMEZONE_OFFSET_MINUTES ||
      timezoneOffset > HISTORY_MAX_TIMEZONE_OFFSET_MINUTES) {
    sendPhoneMessage(400, false, "Phone timezone offset is invalid");
    return;
  }
  if (!readHistoryTextFields(name, notes)) {
    return;
  }
  timezone = phoneWebServer.hasArg("timezone")
                 ? phoneWebServer.arg("timezone")
                 : String("Unknown");
  timezone.trim();
  if (!validUtf8Text(timezone,
                     ScaleHistory::TIMEZONE_CAPACITY - 1,
                     false,
                     false)) {
    sendPhoneMessage(400, false, "Phone timezone is invalid or too long");
    return;
  }

  if (weightLockPhase != WeightLockPhase::Locked ||
      lockedWeightSnapshot.mode == CombinedMode::None ||
      lockToken != currentWeightLockToken) {
    sendPhoneMessage(409,
                     false,
                     "Locked weight changed; reopen Save Log");
    return;
  }
  if (currentWeightLockSaved) {
    sendPhoneMessage(409,
                     false,
                     "This locked weight is already in history");
    return;
  }

  // Copy the authoritative lock and health snapshot before touching flash.
  const WeightSnapshot snapshot = lockedWeightSnapshot;
  ScaleHistory::Record record;
  record.loggedAtMs = timestampMs;
  record.updatedAtMs = timestampMs;
  record.timezoneOffsetMinutes = static_cast<int16_t>(timezoneOffset);
  record.combinedMode = static_cast<uint8_t>(snapshot.mode);
  record.totalGrams = snapshot.totalGrams;
  strlcpy(record.timezone, timezone.c_str(), sizeof(record.timezone));
  strlcpy(record.name, name.c_str(), sizeof(record.name));
  strlcpy(record.notes, notes.c_str(), sizeof(record.notes));
  for (uint8_t index = 0; index < REMOTE_SCALE_COUNT; ++index) {
    const uint8_t bit = 1U << index;
    if (snapshot.scaleIncluded[index]) {
      record.includedMask |= bit;
      record.scaleGrams[index] = snapshot.scaleGrams[index];
    }
    if (scaleStates[index].connected &&
        scaleStates[index].identityVerified) {
      record.connectionMask |= bit;
    }
    record.scaleStatus[index] =
        static_cast<uint8_t>(scaleStates[index].status);
    record.scaleFlags[index] = scaleStates[index].flags;
  }

  const bool wasFull = historyStore.count() >= historyStore.capacity();
  const ScaleHistory::Result result = historyStore.add(record);
  if (result != ScaleHistory::Result::Ok) {
    sendHistoryStoreFailure(result);
    return;
  }
  currentWeightLockSaved = true;
  currentWeightLockRecordId = record.id;
  const String displayId = historyDisplayId(record.id);
  showNotice("Saved " + displayId + " to history", 3000);

  String json;
  json.reserve(180);
  json = "{\"ok\":true,\"id\":";
  json += static_cast<unsigned long>(record.id);
  json += ",\"displayId\":";
  appendJsonString(json, displayId);
  json += ",\"message\":";
  appendJsonString(json,
                   wasFull
                       ? displayId +
                             " saved; the oldest entry was removed"
                       : displayId + " saved to history");
  json += '}';
  sendPhoneJson(201, json);
}

struct HistoryListVisitContext {
  String *json = nullptr;
  const String *query = nullptr;
  size_t offset = 0;
  size_t limit = 0;
  size_t matchingCount = 0;
  size_t returnedCount = 0;
};

bool appendHistoryListRecord(const ScaleHistory::Record &record,
                             void *opaqueContext) {
  HistoryListVisitContext &context =
      *static_cast<HistoryListVisitContext *>(opaqueContext);
  if (context.query != nullptr && !context.query->isEmpty()) {
    String searchable = historyDisplayId(record.id) + " " + record.name;
    searchable.toLowerCase();
    if (searchable.indexOf(*context.query) < 0) {
      return true;
    }
  }

  const size_t matchIndex = context.matchingCount++;
  if (matchIndex < context.offset ||
      context.returnedCount >= context.limit) {
    return true;
  }
  if (context.returnedCount++ != 0) {
    *context.json += ',';
  }
  appendHistoryRecordJson(*context.json, record, false);
  return true;
}

void handlePhoneHistoryList() {
  if (!historyStore.ready()) {
    sendPhoneMessage(503, false, "History storage is unavailable");
    return;
  }

  long requestedOffset = 0;
  long requestedLimit = HISTORY_PAGE_DEFAULT_LIMIT;
  if (phoneWebServer.hasArg("offset") &&
      (!parsePhoneLong(phoneWebServer.arg("offset"), requestedOffset) ||
       requestedOffset < 0 || requestedOffset > ScaleHistory::MAX_RECORDS)) {
    sendPhoneMessage(400, false, "History offset is invalid");
    return;
  }
  if (phoneWebServer.hasArg("limit") &&
      (!parsePhoneLong(phoneWebServer.arg("limit"), requestedLimit) ||
       requestedLimit < 1 || requestedLimit > HISTORY_PAGE_MAX_LIMIT)) {
    sendPhoneMessage(400, false, "History limit must be 1 to 20");
    return;
  }

  String query = phoneWebServer.hasArg("q")
                     ? phoneWebServer.arg("q")
                     : String();
  query.trim();
  if (!validUtf8Text(query, 50, true, false)) {
    sendPhoneMessage(400, false, "History search is invalid");
    return;
  }
  query.toLowerCase();

  String json;
  json.reserve(512 + requestedLimit * 260);
  json = "{\"ok\":true,\"count\":";
  json += static_cast<unsigned int>(historyStore.count());
  json += ",\"capacity\":";
  json += static_cast<unsigned int>(historyStore.capacity());
  json += ",\"offset\":";
  json += requestedOffset;
  json += ",\"records\":[";

  HistoryListVisitContext visitContext;
  visitContext.json = &json;
  visitContext.query = &query;
  visitContext.offset = static_cast<size_t>(requestedOffset);
  visitContext.limit = static_cast<size_t>(requestedLimit);
  const ScaleHistory::Result visitResult =
      historyStore.visitNewest(appendHistoryListRecord, &visitContext);
  if (visitResult != ScaleHistory::Result::Ok) {
    sendHistoryStoreFailure(visitResult);
    return;
  }
  json += "],\"matchingCount\":";
  json += static_cast<unsigned int>(visitContext.matchingCount);
  json += ",\"hasMore\":";
  appendJsonBool(json,
                 visitContext.offset + visitContext.returnedCount <
                     visitContext.matchingCount);
  json += '}';
  sendPhoneJson(200, json);
}

void handlePhoneHistoryDetail() {
  uint32_t id = 0;
  if (!phoneWebServer.hasArg("id") ||
      !parsePhoneUint32(phoneWebServer.arg("id"), id)) {
    sendPhoneMessage(400, false, "History ID is invalid");
    return;
  }
  ScaleHistory::Record record;
  const ScaleHistory::Result result = historyStore.get(id, record);
  if (result != ScaleHistory::Result::Ok) {
    sendHistoryStoreFailure(result);
    return;
  }
  String json;
  json.reserve(1500);
  json = "{\"ok\":true,\"record\":";
  appendHistoryRecordJson(json, record, true);
  json += '}';
  sendPhoneJson(200, json);
}

void handlePhoneHistoryUpdate() {
  if (!validatePhonePost()) {
    return;
  }
  uint32_t id = 0;
  uint32_t revision = 0;
  uint64_t timestampMs = 0;
  String name;
  String notes;
  if (!phoneWebServer.hasArg("id") ||
      !parsePhoneUint32(phoneWebServer.arg("id"), id)) {
    sendPhoneMessage(400, false, "History ID is invalid");
    return;
  }
  if (!phoneWebServer.hasArg("revision") ||
      !parsePhoneUint32(phoneWebServer.arg("revision"), revision) ||
      revision > UINT16_MAX) {
    sendPhoneMessage(400, false, "History revision is invalid");
    return;
  }
  if (!readHistoryTimestamp(timestampMs) ||
      !readHistoryTextFields(name, notes)) {
    return;
  }

  const ScaleHistory::Result result =
      historyStore.edit(id,
                        static_cast<uint16_t>(revision),
                        name.c_str(),
                        notes.c_str(),
                        timestampMs);
  if (result != ScaleHistory::Result::Ok) {
    sendHistoryStoreFailure(result);
    return;
  }
  ScaleHistory::Record updated;
  if (historyStore.get(id, updated) != ScaleHistory::Result::Ok) {
    sendPhoneMessage(500, false, "Entry saved but could not be reloaded");
    return;
  }
  String json;
  json.reserve(1400);
  json = "{\"ok\":true,\"message\":\"Details updated\",\"record\":";
  appendHistoryRecordJson(json, updated, true);
  json += '}';
  sendPhoneJson(200, json);
}

void handlePhoneHistoryDelete() {
  if (!validatePhonePost()) {
    return;
  }
  uint32_t id = 0;
  if (!phoneWebServer.hasArg("id") ||
      !parsePhoneUint32(phoneWebServer.arg("id"), id)) {
    sendPhoneMessage(400, false, "History ID is invalid");
    return;
  }
  const ScaleHistory::Result result = historyStore.remove(id);
  if (result != ScaleHistory::Result::Ok) {
    sendHistoryStoreFailure(result);
    return;
  }
  if (id == currentWeightLockRecordId) {
    currentWeightLockSaved = false;
    currentWeightLockRecordId = 0;
  }
  sendPhoneMessage(200, true, historyDisplayId(id) + " deleted");
}

void handlePhoneHistoryClear() {
  if (!validatePhonePost()) {
    return;
  }
  if (!phoneWebServer.hasArg("confirm") ||
      phoneWebServer.arg("confirm") != "DELETE_ALL") {
    sendPhoneMessage(400, false, "Clear confirmation was not accepted");
    return;
  }
  const ScaleHistory::Result result = historyStore.clear();
  if (result != ScaleHistory::Result::Ok) {
    sendHistoryStoreFailure(result);
    return;
  }
  currentWeightLockSaved = false;
  currentWeightLockRecordId = 0;
  sendPhoneMessage(200, true, "All history entries deleted");
}

void appendCsvField(String &line,
                    const String &value,
                    bool protectFromFormula = true) {
  String safeValue = value;
  if (protectFromFormula) {
    size_t firstVisible = 0;
    while (firstVisible < safeValue.length() &&
           (safeValue.charAt(firstVisible) == ' ' ||
            safeValue.charAt(firstVisible) == '\t')) {
      ++firstVisible;
    }
    if (firstVisible < safeValue.length()) {
      const char first = safeValue.charAt(firstVisible);
      if (first == '=' || first == '+' || first == '-' || first == '@') {
        safeValue = "'" + safeValue;
      }
    }
  }
  line += '"';
  for (size_t index = 0; index < safeValue.length(); ++index) {
    const char character = safeValue.charAt(index);
    if (character == '"') {
      line += "\"\"";
    } else {
      line += character;
    }
  }
  line += '"';
}

String historyUtcIso(uint64_t timestampMs) {
  const time_t seconds = static_cast<time_t>(timestampMs / 1000ULL);
  struct tm utcTime = {};
  if (gmtime_r(&seconds, &utcTime) == nullptr) {
    return String();
  }
  char buffer[24];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utcTime);
  return String(buffer);
}

struct HistoryCsvVisitContext {
  uint16_t recordsSent = 0;
};

bool sendHistoryCsvRecord(const ScaleHistory::Record &record,
                          void *opaqueContext) {
  HistoryCsvVisitContext &context =
      *static_cast<HistoryCsvVisitContext *>(opaqueContext);
  String line;
  line.reserve(900);
  appendCsvField(line, historyDisplayId(record.id), false);
  line += ',';
  appendCsvField(line, String(record.name));
  line += ',';
  appendCsvField(line, String(record.notes));
  line += ',';
  appendCsvField(line, historyUtcIso(record.loggedAtMs), false);
  line += ',';
  char epochBuffer[24];
  snprintf(epochBuffer,
           sizeof(epochBuffer),
           "%llu",
           static_cast<unsigned long long>(record.loggedAtMs));
  appendCsvField(line, String(epochBuffer), false);
  line += ',';
  appendCsvField(line, String(record.timezone));
  line += ',';
  appendCsvField(line, String(record.timezoneOffsetMinutes), false);
  line += ',';
  appendCsvField(line, String(record.totalGrams / 1000.0f, 3), false);
  for (uint8_t scaleIndex = 0; scaleIndex < REMOTE_SCALE_COUNT;
       ++scaleIndex) {
    line += ',';
    appendCsvField(
        line,
        (record.includedMask & (1U << scaleIndex)) != 0
            ? String(record.scaleGrams[scaleIndex] / 1000.0f, 3)
            : String(),
        false);
  }
  line += ',';
  appendCsvField(line, historyModeLabel(record.combinedMode), false);
  for (uint8_t scaleIndex = 0; scaleIndex < REMOTE_SCALE_COUNT;
       ++scaleIndex) {
    const uint8_t bit = 1U << scaleIndex;
    const uint8_t flags = record.scaleFlags[scaleIndex];
    line += ',';
    appendCsvField(line,
                   (record.connectionMask & bit) != 0 ? "Yes" : "No",
                   false);
    line += ',';
    appendCsvField(line,
                   historyScaleStatusLabel(record.scaleStatus[scaleIndex]),
                   false);
    line += ',';
    appendCsvField(
        line,
        (flags & DualScaleBle::TelemetryFlag::SensorPresent) != 0
            ? "Yes"
            : "No",
        false);
    line += ',';
    appendCsvField(
        line,
        (flags & DualScaleBle::TelemetryFlag::AdcResponding) != 0
            ? "Yes"
            : "No",
        false);
    line += ',';
    appendCsvField(
        line,
        (flags & DualScaleBle::TelemetryFlag::OledPresent) != 0
            ? "Yes"
            : "No",
        false);
  }
  line += "\r\n";
  phoneWebServer.sendContent(line);
  ++context.recordsSent;
  if ((context.recordsSent % 5U) == 0U) {
    processBleUiEvents();
  }
  delay(0);
  return true;
}

void handlePhoneHistoryCsv() {
  if (!historyStore.ready()) {
    sendPhoneMessage(503, false, "History storage is unavailable");
    return;
  }
  phoneWebServer.sendHeader("Cache-Control", "no-store, max-age=0");
  phoneWebServer.sendHeader(
      "Content-Disposition",
      "attachment; filename=weighing-scale-history.csv");
  phoneWebServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  phoneWebServer.send(200, "text/csv; charset=utf-8", "");
  phoneWebServer.sendContent(
      "ID,Name,Notes,Logged UTC,Epoch ms,Timezone,UTC-minus-local minutes,"
      "Total kg,Load Cell A kg,Load Cell B kg,Mode,A connected,A status,"
      "A sensor,A ADC,A OLED,B connected,B status,B sensor,B ADC,B OLED\r\n");

  HistoryCsvVisitContext visitContext;
  const ScaleHistory::Result visitResult =
      historyStore.visitNewest(sendHistoryCsvRecord, &visitContext);
  if (visitResult != ScaleHistory::Result::Ok) {
    Serial.println("WARNING: History CSV export stopped after a storage error.");
  }
  phoneWebServer.sendContent("");
}

void handlePhoneTare() {
  if (!validatePhonePost()) {
    return;
  }
  uint8_t targetMask = 0;
  if (!phoneWebServer.hasArg("target") ||
      !parsePhoneTarget(phoneWebServer.arg("target"), targetMask, true)) {
    sendPhoneMessage(400, false, "Tare target must be A, B, or both");
    return;
  }
  const ControlActionResult result =
      requestControllerTare(targetMask, "phone tare command");
  sendPhoneMessage(result.httpStatus, result.accepted, result.message);
}

void handlePhoneCalibration() {
  if (!validatePhonePost()) {
    return;
  }
  uint8_t targetMask = 0;
  int32_t valueGrams = 0;
  if (!phoneWebServer.hasArg("target") ||
      !parsePhoneTarget(phoneWebServer.arg("target"), targetMask, false)) {
    sendPhoneMessage(400, false, "Calibration target must be A or B");
    return;
  }
  if (!phoneWebServer.hasArg("kg") ||
      !parsePhoneCalibrationGrams(phoneWebServer.arg("kg"), valueGrams)) {
    sendPhoneMessage(400,
                     false,
                     "Calibration weight must be 0.1 to 100 kg");
    return;
  }
  const uint8_t scaleIndex = targetMask == 0x01 ? 0 : 1;
  const ControlActionResult result =
      requestControllerCalibration(scaleIndex,
                                   valueGrams,
                                   "phone calibration command");
  sendPhoneMessage(result.httpStatus, result.accepted, result.message);
}

void handlePhoneUnlock() {
  if (!validatePhonePost()) {
    return;
  }
  const ControlActionResult result =
      requestControllerUnlock("phone UNLOCK button");
  sendPhoneMessage(result.httpStatus, result.accepted, result.message);
}

void handlePhoneReconnect() {
  if (!validatePhonePost()) {
    return;
  }
  uint8_t targetMask = 0;
  if (!phoneWebServer.hasArg("target") ||
      !parsePhoneTarget(phoneWebServer.arg("target"), targetMask, true)) {
    sendPhoneMessage(400, false, "Connection target must be A, B, or both");
    return;
  }

  bool actionQueued = false;
  bool needsDiscovery = false;
  const uint32_t now = millis();
  for (uint8_t index = 0; index < REMOTE_SCALE_COUNT; ++index) {
    if ((targetMask & (1U << index)) == 0) {
      continue;
    }

    ScaleState &state = scaleStates[index];
    const bool telemetryFresh =
        state.lastTelemetryMs != 0 &&
        now - state.lastTelemetryMs <= TELEMETRY_STALE_MS;
    if (state.connected && state.identityVerified && telemetryFresh) {
      if (state.pendingToken == 0) {
        actionQueued |=
            queueScaleCommand(index, DualScaleBle::CommandType::Status);
      }
    } else if (state.connected) {
      // A nominal connection with no fresh verified telemetry is not useful.
      // Restart it so CHECK LINKS can recover a stuck GATT session.
      actionQueued |= requestBleReconnect(index);
    } else {
      needsDiscovery = true;
    }
  }

  if (needsDiscovery) {
    BleManagerMessage forceMessage;
    forceMessage.type = BleManagerMessageType::ForceCheck;
    actionQueued |= queueBleManagerMessage(forceMessage);
  }
  if (!actionQueued) {
    sendPhoneMessage(409,
                     false,
                     "Link check is already running; please wait");
    return;
  }
  showNotice(targetMask == 0x03 ? "Checking both scales..."
                                : targetMask == 0x01
                                      ? "Checking Load Cell A..."
                                      : "Checking Load Cell B...",
             1800);
  sendPhoneMessage(202, true, "Connection check started");
}

void handlePhoneBrightness() {
  if (!validatePhonePost()) {
    return;
  }
  long requestedPercent = 0;
  if (!phoneWebServer.hasArg("percent") ||
      !parsePhoneLong(phoneWebServer.arg("percent"), requestedPercent) ||
      requestedPercent < MIN_BRIGHTNESS_PERCENT ||
      requestedPercent > MAX_BRIGHTNESS_PERCENT ||
      requestedPercent % BRIGHTNESS_STEP_PERCENT != 0) {
    sendPhoneMessage(400, false, "Brightness must be 10 to 100 percent");
    return;
  }
  adjustBrightness(static_cast<int>(requestedPercent) -
                   static_cast<int>(brightnessPercent));
  sendPhoneMessage(200,
                   true,
                   "Brightness set to " + String(brightnessPercent) + "%");
}

void handlePhoneCpuFrequency() {
  if (!validatePhonePost()) {
    return;
  }
  long requestedMhz = 0;
  if (!phoneWebServer.hasArg("mhz") ||
      !parsePhoneLong(phoneWebServer.arg("mhz"), requestedMhz) ||
      !supportedCpuFrequency(static_cast<uint16_t>(requestedMhz))) {
    sendPhoneMessage(400, false, "CPU clock must be 160 or 240 MHz");
    return;
  }
  selectCpuFrequency(static_cast<uint16_t>(requestedMhz));
  const bool applied = cpuFrequencyMhz == requestedMhz;
  sendPhoneMessage(applied ? 200 : 500,
                   applied,
                   applied ? "CPU clock set to " + String(cpuFrequencyMhz) +
                                 " MHz"
                           : "CPU clock change failed");
}

bool parsePhoneOperatingMode(const String &argument,
                             OperatingMode &mode) {
  String value = argument;
  value.trim();
  value.toLowerCase();
  if (value == "local") {
    mode = OperatingMode::Local;
    return true;
  }
  if (value == "web") {
    mode = OperatingMode::Web;
    return true;
  }
  if (value == "hybrid") {
    mode = OperatingMode::Hybrid;
    return true;
  }
  return false;
}

void handlePhoneOperatingMode() {
  if (!validatePhonePost()) {
    return;
  }
  OperatingMode requestedMode = operatingMode;
  if (!phoneWebServer.hasArg("mode") ||
      !parsePhoneOperatingMode(phoneWebServer.arg("mode"),
                               requestedMode)) {
    sendPhoneMessage(400,
                     false,
                     "Mode must be local, web, or hybrid");
    return;
  }

  applyOperatingMode(requestedMode, true);
  if (operatingMode != requestedMode) {
    sendPhoneMessage(500, false, "Mode change failed");
    return;
  }
  sendPhoneMessage(
      requestedMode == OperatingMode::Local ? 202 : 200,
      true,
      requestedMode == OperatingMode::Local
          ? "Local mode enabled; WiFi will turn off now"
          : String(operatingModeName(requestedMode)) + " mode enabled");
}

void handlePhoneNotFound() {
  if (phoneWebServer.uri().startsWith("/api/")) {
    sendPhoneMessage(404, false, "API endpoint not found");
    return;
  }
  phoneWebServer.sendHeader(
      "Location", "http://" + WiFi.softAPIP().toString() + "/", true);
  phoneWebServer.send(302, "text/plain", "Open the weighing-scale control page");
}

void configurePhoneRoutes() {
  if (phoneRoutesConfigured) {
    return;
  }
  const char *collectedHeaders[] = {"X-Scale-Control"};
  phoneWebServer.collectHeaders(collectedHeaders, 1);
  phoneWebServer.on("/", HTTP_GET, handlePhoneRoot);
  phoneWebServer.on("/api/v1/status", HTTP_GET, handlePhoneStatus);
  phoneWebServer.on("/api/v1/history", HTTP_GET, handlePhoneHistoryList);
  phoneWebServer.on("/api/v1/history/detail",
                    HTTP_GET,
                    handlePhoneHistoryDetail);
  phoneWebServer.on("/api/v1/history/export.csv",
                    HTTP_GET,
                    handlePhoneHistoryCsv);
  phoneWebServer.on("/api/v1/history/save",
                    HTTP_POST,
                    handlePhoneHistorySave);
  phoneWebServer.on("/api/v1/history/update",
                    HTTP_POST,
                    handlePhoneHistoryUpdate);
  phoneWebServer.on("/api/v1/history/delete",
                    HTTP_POST,
                    handlePhoneHistoryDelete);
  phoneWebServer.on("/api/v1/history/clear",
                    HTTP_POST,
                    handlePhoneHistoryClear);
  phoneWebServer.on("/api/v1/tare", HTTP_POST, handlePhoneTare);
  phoneWebServer.on("/api/v1/calibrate", HTTP_POST, handlePhoneCalibration);
  phoneWebServer.on("/api/v1/unlock", HTTP_POST, handlePhoneUnlock);
  phoneWebServer.on("/api/v1/reconnect", HTTP_POST, handlePhoneReconnect);
  phoneWebServer.on("/api/v1/display/brightness",
                    HTTP_POST,
                    handlePhoneBrightness);
  phoneWebServer.on("/api/v1/display/cpu",
                    HTTP_POST,
                    handlePhoneCpuFrequency);
  phoneWebServer.on("/api/v1/mode",
                    HTTP_POST,
                    handlePhoneOperatingMode);
  phoneWebServer.on("/favicon.ico", HTTP_GET, []() {
    phoneWebServer.send(204, "image/x-icon", "");
  });
  phoneWebServer.onNotFound(handlePhoneNotFound);
  phoneRoutesConfigured = true;
}

bool beginPhoneControl() {
  if (phoneControlReady) {
    return true;
  }
  configurePhoneRoutes();
  WiFi.persistent(false);
  if (!WiFi.mode(WIFI_AP)) {
    Serial.println("ERROR: Could not enable Wi-Fi AP mode.");
    return false;
  }
  const IPAddress address(192, 168, 4, 1);
  const IPAddress gateway(192, 168, 4, 1);
  const IPAddress subnet(255, 255, 255, 0);
  if (!WiFi.softAPConfig(address, gateway, subnet)) {
    Serial.println("Warning: phone-control SoftAP IP configuration failed.");
  }
  if (!WiFi.softAP(PHONE_WIFI_SSID,
                   PHONE_WIFI_PASSWORD,
                   PHONE_WIFI_CHANNEL,
                   false,
                   PHONE_WIFI_MAX_CLIENTS)) {
    Serial.println("ERROR: Phone-control Wi-Fi access point failed to start.");
    phoneControlReady = false;
    WiFi.mode(WIFI_OFF);
    return false;
  }
  phoneWebServer.begin();
  phoneControlReady = true;
  phoneClientCount = 0;
  lastPhoneClientCheckMs = 0;
  lastPhoneWebActivityMs = 0;
  lastPhoneActionMs = 0;

  Serial.println("Phone control Wi-Fi ready.");
  Serial.print("  Network: ");
  Serial.println(PHONE_WIFI_SSID);
  Serial.print("  Password: ");
  Serial.println(PHONE_WIFI_PASSWORD);
  Serial.print("  Open: http://");
  Serial.println(WiFi.softAPIP());
  return true;
}

void stopPhoneControl() {
  phoneWifiStopPending = false;
  phoneWifiStopAtMs = 0;
  if (phoneControlReady) {
    phoneWebServer.stop();
  }
  phoneControlReady = false;
  phoneClientCount = 0;
  lastPhoneClientCheckMs = 0;
  lastPhoneWebActivityMs = 0;
  lastPhoneActionMs = 0;
  WiFi.softAPdisconnect(true);
  if (WiFi.getMode() != WIFI_OFF) {
    WiFi.mode(WIFI_OFF);
  }
  Serial.println("Phone control Wi-Fi stopped; BLE scales remain active.");
}

void applyOperatingMode(OperatingMode requestedMode,
                        bool requestedFromPhone) {
  const OperatingMode previousMode = operatingMode;
  phoneWifiStopPending = false;
  phoneWifiStopAtMs = 0;

  if (operatingModeUsesWifi(requestedMode) &&
      !beginPhoneControl()) {
    operatingMode = previousMode;
    showNotice("WiFi could not start", 4500);
    return;
  }

  operatingMode = requestedMode;
  if (settingsPreferencesReady &&
      settingsPreferences.putUChar(
          OPERATING_MODE_KEY,
          static_cast<uint8_t>(operatingMode)) != 1) {
    Serial.println("Warning: operating mode could not be saved.");
  }

  if (operatingMode == OperatingMode::Local) {
    if (requestedFromPhone && phoneControlReady) {
      phoneWifiStopPending = true;
      phoneWifiStopAtMs = millis() + PHONE_WIFI_STOP_DELAY_MS;
    } else {
      stopPhoneControl();
    }
    changePage(Page::Dashboard);
  } else if (operatingMode == OperatingMode::Web) {
    changePage(Page::WebStatus);
  } else {
    changePage(Page::Dashboard);
  }

  lastUserActivityMs = millis();
  idleWarningActive = false;
  idleWarningSecondsShown = 0;
  pageDirty = true;
  uiDirty = true;
  applyBacklightOutput(!screenSleeping);
  Serial.print("Operating mode changed to ");
  Serial.println(operatingModeName(operatingMode));
}

void servicePhoneControl() {
  if (phoneWifiStopPending &&
      static_cast<int32_t>(millis() - phoneWifiStopAtMs) >= 0) {
    stopPhoneControl();
    return;
  }
  if (!phoneControlReady) {
    return;
  }

  const uint32_t now = millis();
  if (lastPhoneClientCheckMs == 0 ||
      now - lastPhoneClientCheckMs >= PHONE_CLIENT_CHECK_INTERVAL_MS) {
    lastPhoneClientCheckMs = now;
    const uint8_t connectedClients = WiFi.softAPgetStationNum();
    if (connectedClients != phoneClientCount) {
      phoneClientCount = connectedClients;
      Serial.printf("Phone-control clients: %u.\n", phoneClientCount);
      if (currentPage == Page::WebStatus) {
        uiDirty = true;
      }
    }
  }
  phoneWebServer.handleClient();
}

} // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  noticeMessage.reserve(64);

  Serial.println();
  Serial.println("Guition JC2432W328C dual BLE load-cell display");
  Serial.println("LCD: ST7789 240x320; touch: CST820 at 0x15");
  loadDisplaySettings();

  pinMode(TFT_BACKLIGHT_PIN, OUTPUT);
  digitalWrite(TFT_BACKLIGHT_PIN, LOW);
  pinMode(TOUCH_RESET_PIN, OUTPUT);
  digitalWrite(TOUCH_RESET_PIN, LOW);

  // GPIO holds survive deep sleep. Prepare LOW first, then release the holds
  // before reinitializing the LCD and touch controller.
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis(GPIO_NUM_27);
  gpio_hold_dis(GPIO_NUM_25);
  beginBacklightControl();

  tft.init();
  tft.setRotation(0);
  beginWeightSprite();

  drawStartupScreen("INITIALIZING DISPLAY", 20);
  applyBacklightOutput(true);

  touchReady = beginTouchController();
  drawStartupScreen(touchReady ? "TOUCHSCREEN READY" : "TOUCHSCREEN ERROR",
                    55);
  if (!touchReady && operatingMode == OperatingMode::Local) {
    // Recovery path: a saved Wi-Fi-off mode must never leave the product with
    // neither a working touchscreen nor a reachable phone interface.
    operatingMode = OperatingMode::Hybrid;
    Serial.println(
        "Touch unavailable in Local mode; enabling Hybrid for this boot.");
  }
  currentPage = operatingMode == OperatingMode::Web
                    ? Page::WebStatus
                    : Page::Dashboard;
  drawStartupScreen("LOADING INTERFACE", 85);
  drawCurrentPage();
  if (historyStore.begin()) {
    Serial.printf("History storage ready: %u/%u entries.\n",
                  historyStore.count(),
                  historyStore.capacity());
  } else {
    Serial.println(
        "WARNING: History storage unavailable; existing data was not erased.");
  }
  // A fresh random starting token prevents a Save dialog left open on a phone
  // across a display reboot from matching a different, newly locked weight.
  currentWeightLockToken = esp_random();
  if (currentWeightLockToken == 0) {
    currentWeightLockToken = 1;
  }
  if (operatingModeUsesWifi(operatingMode)) {
    if (!beginPhoneControl()) {
      showNotice("Phone WiFi failed", 5000);
    }
  } else {
    WiFi.persistent(false);
    WiFi.mode(WIFI_OFF);
    Serial.println("Local mode: phone-control Wi-Fi is off.");
  }
  pageDirty = true;
  uiDirty = true;
  if (!startBleManager()) {
    showNotice("BLE manager failed", 5000);
  }
  lastUserActivityMs = millis();
}

void loop() {
  processBleUiEvents();
  servicePhoneControl();

  for (uint8_t index = 0; index < REMOTE_SCALE_COUNT; ++index) {
    ScaleState &state = scaleStates[index];
    const uint32_t latestActivityMs =
        state.lastTelemetryMs != 0
            ? state.lastTelemetryMs
            : state.connectedAtMs;
    if (state.connected && latestActivityMs != 0 &&
        millis() - latestActivityMs > TELEMETRY_RECONNECT_MS) {
      requestBleReconnect(index);
    }

    if (state.pendingToken != 0 &&
        state.pendingCommandAtMs != 0 &&
        millis() - state.pendingCommandAtMs > COMMAND_ACK_TIMEOUT_MS) {
      const bool peerStatusCommand =
          state.pendingCommand == DualScaleBle::CommandType::PeerStatus;
      const bool weightLockCommand =
          state.pendingCommand == DualScaleBle::CommandType::SetWeightLock ||
          state.pendingCommand ==
              DualScaleBle::CommandType::ClearWeightLock;
      state.pendingToken = 0;
      state.pendingCommandAtMs = 0;
      if (peerStatusCommand) {
        peerStatusSyncPending[index] = true;
      } else if (weightLockCommand) {
        weightLockSyncPending[index] = true;
        weightLockSyncRetryAtMs[index] =
            millis() + WEIGHT_LOCK_SYNC_RETRY_MS;
        Serial.printf("Scale %u weight-lock sync timed out; retrying.\n",
                      index + 1);
      } else {
        showNotice("Scale " + String(index + 1) +
                       " command timed out",
                   4000);
      }
      requestBleReconnect(index);
    }
  }
  recomputeCombinedWeight();
  serviceAutomaticWeightLock();
  serviceWeightLockSync();
  servicePeerStatusSync();

  if (!noticeMessage.isEmpty() && !noticeActive()) {
    noticeMessage = "";
    dashboardStatusDirty = true;
    uiDirty = true;
  }

  if (touchReady) {
    TouchSample sample;
    const bool touched = readTouch(sample);
    if (touched) {
      lastUserActivityMs = millis();
      if (screenSleeping) {
        setScreenSleeping(false);
        Serial.println("Screen standby ended by touchscreen activity.");
      } else if (idleWarningActive) {
        idleWarningActive = false;
        idleWarningSecondsShown = 0;
        pageDirty = true;
        uiDirty = true;
        Serial.println("Idle standby cancelled by touchscreen activity.");
      } else {
        processTouch(sample, !touchWasDown);
      }
    }
    serviceCalibrationWeightRepeat(touched ? &sample : nullptr);
    touchWasDown = touched;
  } else {
    finishCalibrationWeightRepeat(false);
  }

  if (currentPage == Page::Connectivity && !screenSleeping &&
      millis() - lastConnectivityRedrawMs >= 1000) {
    lastConnectivityRedrawMs = millis();
    uiDirty = true;
  }
  if (currentPage == Page::WebStatus && !screenSleeping &&
      millis() - lastWebStatusRedrawMs >= 1000) {
    lastWebStatusRedrawMs = millis();
    uiDirty = true;
  }

  if (!screenSleeping) {
    const uint32_t screenTimeoutMs = activeScreenTimeoutMs();
    const uint32_t idleTimeMs = millis() - lastUserActivityMs;
    if (idleTimeMs >= screenTimeoutMs) {
      setScreenSleeping(true);
    } else if (idleTimeMs >= screenTimeoutMs - SCREEN_IDLE_WARNING_MS) {
      const uint32_t remainingMs = screenTimeoutMs - idleTimeMs;
      const uint8_t secondsRemaining =
          static_cast<uint8_t>((remainingMs + 999) / 1000);

      if (!idleWarningActive ||
          secondsRemaining != idleWarningSecondsShown) {
        const bool enteringIdleWarning = !idleWarningActive;
        if (enteringIdleWarning) {
          Serial.println("Screen idle: showing standby warning.");
        }
        idleWarningActive = true;
        idleWarningSecondsShown = secondsRemaining;
        if (enteringIdleWarning) {
          drawIdleWarning(secondsRemaining);
        } else {
          drawIdleWarningCountdown(secondsRemaining);
        }
      }
    }
  }

  if (!screenSleeping && !idleWarningActive) {
    if (pageDirty) {
      drawCurrentPage();
    } else if (uiDirty) {
      updateCurrentPage();
    }
    serviceDashboardScaleBorderAnimation();
  }

  delay(12);
}
