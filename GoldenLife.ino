#include <HTTPUpdateServer.h>

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <EEPROM.h>
#include <Adafruit_NeoPixel.h>
#include <SPIFFS.h>       // For log storage in flash
#include <Arduino.h>      // For basic Arduino functions like millis(), Serial, etc.
#include <esp_system.h>   // חשוב: ודא ששורה זו קיימת ולא ממוקמת בתוך בלוק #ifdef ESP32
#include <esp_ota_ops.h>  // חדש: עבור פונקציות OTA

#ifdef ESP32
#include <esp_sleep.h>  // For deep sleep functions
#endif

// --- Global Constants and Pin Definitions ---
const int buttonPin = 3;
const int vibPin = 6;
#ifdef ESP32
const int ledBuiltin = 15;  // Built-in LED pin
#endif
#define LED_PIN 8                        // Pin connected to the NeoPixel LED
#define NUM_PIXELS 8                     // Number of LEDs in the strip
const unsigned long debounceDelay = 20;  // Debounce delay reduced to 20ms

// --- Sleep time constants ---
#define MIN_DELAY_MINUTES 10                  // Minimum required delay (in minutes)
#define IDLE_SLEEP_TIMEOUT_MS 60000           // 1 minute of idle time before deep sleep
#define WAKEUP_BEFORE_NEXT_SCENARIO_MS 60000  // Wake up 1 minute before next scenario

#define LONG_PRESS_DURATION 3000  // Long press duration (not used)

#define SERVICE_UUID "069f372b-274b-4198-a099-74f2791734a5"  // FIXED: Changed '7' to '9' to match HTML error
#define CHAR_UUID_VIBRATE_POWER "84a054be-6a59-402c-9f49-cf3a5a0c230b"
#define CHAR_UUID_PULSE_INTERVAL "53843d2f-2a09-4614-8776-fc8a68fee4b9"
#define CHAR_UUID_PULSENUMBER "8c29fc39-27ae-4e90-a7de-eb3eb4f820bf"
#define CHAR_UUID_VIBRATION_DURATION "7737e412-351e-4245-9e79-b2a5673a5abf"
#define CHAR_UUID_START "66ccf7cf-6a9e-4eda-b868-515dd5b9eb8b"
#define CHAR_UUID_VIBRATION_STATUS "12cfcb54-2b8a-4da4-a737-f040b6f79fc7"

// --- חדש: הגדרות UUIDs עבור BLE OTA ---
#define OTA_SERVICE_UUID "501A0001-08EE-4DEE-AE83-3A19C0E28B02"
#define OTA_CHAR_DATA_UUID "501A0002-08EE-4DEE-AE83-3A19C0E28B02"     // Write_No_Rsp
#define OTA_CHAR_CONTROL_UUID "501A0003-08EE-4DEE-AE83-3A19C0E28B02"  // Write
#define OTA_CHAR_STATUS_UUID "501A0004-08EE-4DEE-AE83-3A19C0E28B02"   // Notify

// --- Global Variables ---
Adafruit_NeoPixel strip = Adafruit_NeoPixel(NUM_PIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);
esp_sleep_wakeup_cause_t wakeupReason;

// Button variables
static bool lastButtonState = LOW;
static unsigned long buttonPressStart = 0;
static bool buttonHeld = false;
bool wakeupDetected = false;
bool waitingForReactivation = false;
bool deviceAwakeAndReady = false;  // New flag for "awake and waiting for second press"

// Vibration variables
int vibState = LOW;
int vibratePower = 195;
int pulseInterval = 1000;
int pulsenumber = 60;
int vibrationDuration = 400;
int totalVibrations = 0;

#ifdef ESP32
const int ledChannel = 0;
const int freq = 5000;
const int resolution = 8;
#endif

unsigned long previousMillis = 0;
bool scenarioActive = false;
bool scenarioCompleted = false;
unsigned long scenarioEndTime = 0;
unsigned long lastActivityTime = 0;
unsigned long waitStartTime = 0;
bool hasPrintedSleepMessage = false;
unsigned long previousMillisVib = 0;
// --- משתנה גלובלי לאחסון כתובת ה-MAC ---
char storedMacAddress[13];  // 12 תווים לכתובת MAC + תו סיום NULL
uint16_t savedDelay = 35;
uint8_t savedRepeat = 2;
uint8_t scenarioRepeatMax = 0;
uint8_t scenarioRepeatCount = 0;
bool waitingForNextScenario = false;

// --- Logging state (session id + source) ---
String currentSessionId = "";
String currentSessionSource = "";  // "BUTTON" או "WEB"

// המשתנים isCharging ו-fullCharge הוסרו כפי שביקשת.
float previousBatteryVoltage = 0.0;
const float fullChargeVoltage = 4.2;        // נשאר כקבוע, אך לא משמש ללוגיקת טעינה
const float chargingThreshold = 3.9;        // נשאר כקבוע, אך לא משמש ללוגיקת טעינה
const float LOW_BATTERY_THRESHOLD_V = 3.6;  // New: 3.6V for low battery (approx 30%)

// --- EEPROM Magic Number for Initialization ---
const int EEPROM_MAGIC_ADDRESS = 0;
const uint8_t EEPROM_MAGIC_NUMBER = 0xA5;  // A unique byte to indicate EEPROM is initialized

// --- Charging Detection Pin (REMOVED as requested) ---
// const int CHARGE_DETECT_PIN = 0; // Placeholder for a pin connected to charger status (e.g., from TP4056)
// bool isCharging = false; // Flag to indicate if the device is currently charging (REMOVED)

BLECharacteristic *vibratePowerChar;
BLECharacteristic *pulseIntervalChar;
BLECharacteristic *pulseNumberChar;
BLECharacteristic *vibrationDurationChar;
BLECharacteristic *startStopChar;
BLECharacteristic *vibrationStatusChar;

// --- חדש: משתנים גלובליים עבור OTA ---
BLECharacteristic *otaDataChar;
BLECharacteristic *otaControlChar;
BLECharacteristic *otaStatusChar;

esp_ota_handle_t ota_handle = 0;
const esp_partition_t *update_partition = NULL;
unsigned int ota_firmware_offset = 0;  // מונה כמה בייטים כבר נכתבו

// סטטוס OTA
enum OtaStatus {
  OTA_IDLE = 0,
  OTA_STARTING,
  OTA_IN_PROGRESS,  // הוחזר: חשוב למעקב התקדמות
  OTA_VERIFYING,
  OTA_COMPLETE,
  OTA_FAILED  // הוחזר: חשוב לדיווח שגיאות
};
OtaStatus currentOtaStatus = OTA_IDLE;

// --- Forward Declarations ---
void updateVibrationStatusBLE(int remainingVibrations);
float readBatteryVoltage();
void printSettings();
void saveSettings();
void saveScenarioParams(uint16_t delayBetweenScenarios, uint8_t repeatCount);
void loadSettings();
void loadScenarioParams();
void updateStartStopCharacteristic(uint8_t command, uint16_t delay, uint8_t repeat, uint8_t remainingScenarios);
void startScenarioWithParams(uint16_t delayMinutes, uint8_t repeatCount);
void enterDeepSleep(bool onlyButtonWakeup = false, unsigned long timerDurationMs = 0);
void stopScenario(bool manualStop = false);  // Modified to accept manualStop parameter
bool hasScenarioSaved();
void savePostDelayAndSleep(uint16_t postDelayMinutes);
void checkButtonPress();
const char *getStoredMacAddress();
void initializeEEPROMDefaults();  // New: Function to initialize EEPROM

// --- חדש: הצהרות קדימה לפונקציות OTA ---
void sendOtaStatus(OtaStatus status, uint32_t progress = 0);

// --- Logging helpers (declarations) ---
void initLogStorage();
void appendLogEvent(const String &sessionId,
                    const String &eventType,
                    const String &source,
                    int vibratePowerValue,
                    int pulseIntervalMs,
                    int pulseNumberValue,
                    int vibrationDurationMs,
                    int pauseMinutes,
                    int repeats,
                    const String &stopReason,
                    int batteryPercent,
                    const String &rawData);
void clearLog();

// --- Function Definitions ---
const char *getStoredMacAddress() {
  return storedMacAddress;
}

bool hasScenarioSaved() {
  return (savedDelay > 0 && savedRepeat > 0);
}

void updateVibrationStatusBLE(int remainingVibrations) {
  if (vibrationStatusChar) {
    uint8_t buffer[2];
    buffer[0] = remainingVibrations & 0xFF;
    buffer[1] = (remainingVibrations >> 8) & 0xFF;
    vibrationStatusChar->setValue(buffer, 2);
    vibrationStatusChar->notify();
    Serial.print("רטטים נותרים נשלחו ב-BLE: ");
    Serial.println(remainingVibrations);
  }
}

float readBatteryVoltage() {
  const int numReadings = 10;  // קרא 10 פעמים
  long sum = 0;
  for (int i = 0; i < numReadings; i++) {
    sum += analogRead(0);
    delay(1);  // השהיה קצרה בין קריאות (אופציונלי, אך יכול לעזור)
  }
  int raw = sum / numReadings;  // חשב ממוצע

  const float ADC_MAX_RAW_VALUE = 4095.0;
  const float ADC_REFERENCE_VOLTAGE = 3.3;
  const float VOLTAGE_DIVIDER_FACTOR_CALIBRATED = 2.5076;

  float voltageAtAdcPin = (static_cast<float>(raw) / ADC_MAX_RAW_VALUE) * ADC_REFERENCE_VOLTAGE;
  float batteryVoltage = voltageAtAdcPin * VOLTAGE_DIVIDER_FACTOR_CALIBRATED;

  Serial.print("ADC Raw (Avg): ");
  Serial.println(raw);
  Serial.print("Calculated Battery Voltage (Avg): ");
  Serial.println(batteryVoltage, 2);

  return batteryVoltage;
}

void printSettings() {
  Serial.println("הגדרות נוכחיות:");
  Serial.print("עוצמת רטט: ");
  Serial.println(vibratePower);
  Serial.print("מרווח פולסים: ");
  Serial.println(pulseInterval);
  Serial.print("מספר פולסים: ");
  Serial.println(pulsenumber);
  Serial.print("משך רטט: ");
  Serial.println(vibrationDuration);
}

void saveSettings() {
  EEPROM.write(1, vibratePower);  // Adjusted address to avoid magic number
  EEPROM.write(2, pulseInterval >> 8);
  EEPROM.write(3, pulseInterval & 0xFF);
  EEPROM.write(4, pulsenumber >> 8);
  EEPROM.write(5, pulsenumber & 0xFF);
  EEPROM.write(6, vibrationDuration >> 8);
  EEPROM.write(7, vibrationDuration & 0xFF);
  EEPROM.commit();
  printSettings();
  Serial.println("הגדרות נשמרו ב-EEPROM.");
}

void saveScenarioParams(uint16_t delayBetweenScenarios, uint8_t repeatCount) {
  if (delayBetweenScenarios < MIN_DELAY_MINUTES) {
    Serial.printf("אזהרה: delayBetweenScenarios (%d) נמוך מהמינימום (%d). מגדיר למינימום.\n", delayBetweenScenarios, MIN_DELAY_MINUTES);
    delayBetweenScenarios = MIN_DELAY_MINUTES;
  }
  EEPROM.write(10, delayBetweenScenarios & 0xFF);
  EEPROM.write(11, (delayBetweenScenarios >> 8) & 0xFF);
  EEPROM.write(12, repeatCount);  // This writes repeatCount
  EEPROM.write(13, 0);
  EEPROM.write(14, 0);
  EEPROM.write(15, 0);
  EEPROM.commit();
  Serial.printf("קיבלתי עיכוב: %d דקות\n", delayBetweenScenarios);
  Serial.println("פרמטרי תרחיש נשמרו ב-EEPROM.");
}

void loadSettings() {
  vibratePower = EEPROM.read(1);  // Adjusted address
  pulseInterval = (EEPROM.read(2) << 8) | EEPROM.read(3);
  pulsenumber = (EEPROM.read(4) << 8) | EEPROM.read(5);
  vibrationDuration = (EEPROM.read(6) << 8) | EEPROM.read(7);
  if (vibratePower == 0xFF) vibratePower = 195;
  if (pulseInterval == 0xFFFF) pulseInterval = 1000;
  if (pulsenumber == 0xFFFF) pulsenumber = 60;
  if (vibrationDuration == 0xFFFF) vibrationDuration = 400;
  if (vibratePowerChar) vibratePowerChar->setValue(vibratePower);
  if (pulseIntervalChar) pulseIntervalChar->setValue(pulseInterval);
  if (pulseNumberChar) pulseNumberChar->setValue(pulsenumber);
  if (vibrationDurationChar) vibrationDurationChar->setValue(vibrationDuration);
  printSettings();
  Serial.println("הגדרות נטענו מ-EEPROM.");
}

void loadScenarioParams() {
  uint16_t delay = (EEPROM.read(11) << 8) | EEPROM.read(10);
  uint8_t repeat = EEPROM.read(12);
  uint8_t count = EEPROM.read(14);
  uint8_t reactivationFlag = EEPROM.read(15);
  if (delay >= MIN_DELAY_MINUTES && delay <= 600 && repeat > 0 && repeat <= 255) {
    savedDelay = delay;
    savedRepeat = repeat;
    scenarioRepeatCount = count;
    waitingForReactivation = (reactivationFlag == 1);
  } else {
    // This block is now primarily for when EEPROM is corrupted or not initialized
    // Initial defaults are handled by initializeEEPROMDefaults()
    Serial.println("⚠ נתוני EEPROM לא תקינים עבור פרמטרי תרחיש – משתמש בברירות מחדל זמניות.");
    savedDelay = MIN_DELAY_MINUTES;
    savedRepeat = 2;
    scenarioRepeatCount = 0;
    waitingForReactivation = false;
  }
  Serial.println("📥 נטען תרחיש מ-EEPROM:");
  Serial.printf("⏱ עיכוב: %d דקות\n", savedDelay);
  Serial.printf("🔁 מספר חזרות מרבי: %d\n", savedRepeat);
  Serial.printf("📊 מונה חזרות נוכחי (מ-EEPROM): %d\n", scenarioRepeatCount);
  Serial.printf("🔄 ממתין להפעלה מחדש: %s\n", waitingForReactivation ? "כן" : "לא");
  scenarioRepeatMax = savedRepeat;
}

// --- Logging implementation ---

// Initialize flash file system for log storage
void initLogStorage() {
  if (!SPIFFS.begin(true)) {
    Serial.println("⚠ SPIFFS init failed – log storage disabled.");
  } else {
    Serial.println("✅ SPIFFS initialized for log storage.");
  }
}

// Very simple voltage→battery% estimate (3.3V=0%, 4.2V=100%)
static int estimateBatteryPercent(float voltage) {
  const float minV = 3.3f;
  const float maxV = 4.2f;
  if (voltage <= minV) return 0;
  if (voltage >= maxV) return 100;
  float pct = (voltage - minV) * 100.0f / (maxV - minV);
  if (pct < 0.0f) pct = 0.0f;
  if (pct > 100.0f) pct = 100.0f;
  return static_cast<int>(pct + 0.5f);
}

// Append one CSV log line both to Serial (for debug) and to /goldenlife_log.csv on SPIFFS
void appendLogEvent(const String &sessionId,
                    const String &eventType,
                    const String &source,
                    int vibratePowerValue,
                    int pulseIntervalMs,
                    int pulseNumberValue,
                    int vibrationDurationMs,
                    int pauseMinutes,
                    int repeats,
                    const String &stopReason,
                    int batteryPercent,
                    const String &rawData) {
  String line;
  line.reserve(256);

  // Order matches Google Sheets columns (בעברית) כדי להקל מיפוי
  line += String(millis());          line += ",";  // תאריך ושעה (כעת millis – אפשר להחליף לזמן אמיתי)
  line += sessionId;                 line += ",";  // מזהה סשן
  line += eventType;                 line += ",";  // סוג אירוע
  line += source;                    line += ",";  // מקור הפעלה
  line += String(vibratePowerValue); line += ",";  // עוצמת ויברציה
  line += String(pulseIntervalMs);   line += ",";  // מרווח בין פולסים
  line += String(pulseNumberValue);  line += ",";  // מספר פולסים
  line += String(vibrationDurationMs); line += ",";  // משך ויברציה
  line += String(pauseMinutes);      line += ",";  // זמן השהייה בין תרחישים
  line += String(repeats);           line += ",";  // מספר חזרות
  line += stopReason;                line += ",";  // סיבת סיום
  line += String(batteryPercent);    line += ",";  // אחוז סוללה
  line += rawData;                              // טקסט גולמי

  // Debug to Serial
  Serial.print("LOG: ");
  Serial.println(line);

  // Persist to flash (best-effort)
  if (SPIFFS.begin(false)) {
    File f = SPIFFS.open("/goldenlife_log.csv", FILE_APPEND);
    if (f) {
      f.println(line);
      f.close();
    } else {
      Serial.println("⚠ Failed to open log file for append.");
    }
  }
}

void clearLog() {
  if (SPIFFS.begin(false)) {
    if (SPIFFS.exists("/goldenlife_log.csv")) {
      SPIFFS.remove("/goldenlife_log.csv");
      Serial.println("🧹 Log file cleared from SPIFFS.");
    }
  }
}

void updateStartStopCharacteristic(uint8_t command, uint16_t delay, uint8_t repeat, uint8_t remainingScenarios) {
  float voltage = readBatteryVoltage();
  uint16_t voltage100 = (uint16_t)(voltage * 100);
  uint8_t data[7];
  data[0] = command;
  data[1] = delay & 0xFF;
  data[2] = (delay >> 8) & 0xFF;
  data[3] = repeat;
  data[4] = voltage100 & 0xFF;
  data[5] = (voltage100 >> 8) & 0xFF;
  // Ensure remainingScenarios is correctly cast and within bounds of uint8_t
  data[6] = (uint8_t)min((int)remainingScenarios, 255);  // Cast to int for min, then to uint8_t
  if (startStopChar) {
    startStopChar->setValue(data, 7);
    startStopChar->notify();
  }
  Serial.printf("? נשלח StartStop BLE: command=%d delay=%d repeat=%d voltage=%.2fV RemainingScenarios=%d\n",
                command, delay, repeat, voltage, remainingScenarios);
}

class GeneralSettingsCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) override {
    String value = pCharacteristic->getValue();
    if (value.length() > 0) {
      if (pCharacteristic->getUUID().equals(BLEUUID(CHAR_UUID_VIBRATE_POWER)) && value.length() >= 1) {
        vibratePower = static_cast<uint8_t>(value.charAt(0));
        Serial.print("עוצמת רטט עודכנה: ");
        Serial.println(vibratePower);
      } else if (pCharacteristic->getUUID().equals(BLEUUID(CHAR_UUID_PULSE_INTERVAL)) && value.length() >= 2) {
        pulseInterval = (static_cast<uint8_t>(value.charAt(1)) << 8) | static_cast<uint8_t>(value.charAt(0));
        Serial.print("מרווח פולסים עודכן: ");
        Serial.println(pulseInterval);
      } else if (pCharacteristic->getUUID().equals(BLEUUID(CHAR_UUID_PULSENUMBER)) && value.length() >= 2) {
        pulsenumber = (static_cast<uint8_t>(value.charAt(1)) << 8) | static_cast<uint8_t>(value.charAt(0));
        Serial.print("מספר פולסים עודכן: ");
        Serial.println(pulsenumber);
      } else if (pCharacteristic->getUUID().equals(BLEUUID(CHAR_UUID_VIBRATION_DURATION)) && value.length() >= 2) {
        vibrationDuration = (static_cast<uint8_t>(value.charAt(1)) << 8) | static_cast<uint8_t>(value.charAt(0));
        Serial.print("משך רטט עודכן: ");
        Serial.println(vibrationDuration);
      } else {
        Serial.println("פעולת כתיבה לא תקינה ב-BLE: אורך לא מתאים או מאפיין לא נתמך.");
      }
      saveSettings();
      // lastActivityTime = millis(); // Removed: Don't reset idle timer on every BLE write
    } else {
      Serial.println("ערך ריק התקבל במאפיין BLE!");
    }
  }
};

class StartStopCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) override {
    String value = pCharacteristic->getValue();
    if (value.length() > 0) {
      uint8_t command = static_cast<uint8_t>(value.charAt(0));
      if (waitingForReactivation && command == 1) {
        Serial.println("✅ BLE - איפוס והפעלה מחדש.");
        waitingForReactivation = false;
        scenarioRepeatCount = 0;
        EEPROM.write(15, 0);
        EEPROM.write(14, 0);
        EEPROM.commit();
        // After BLE reactivation, go to "ready" state, waiting for second press or BLE start
        deviceAwakeAndReady = true;
        strip.setPixelColor(0, strip.Color(255, 255, 255));  // White for ready
        strip.show();
        lastActivityTime = millis();  // Reset idle timer on significant BLE command
      }
      if (command == 1) {
        Serial.println("תרחיש הופעל דרך BLE.");
        uint16_t delay = savedDelay;
        uint8_t repeat = savedRepeat;
        if (value.length() >= 7) {
          delay = (static_cast<uint8_t>(value.charAt(1)) | (static_cast<uint8_t>(value.charAt(2)) << 8));
          repeat = static_cast<uint8_t>(value.charAt(3));
          if (delay < MIN_DELAY_MINUTES) {
            Serial.printf("אזהרה: עיכוב BLE (%d) נמוך מהמינימום (%d). מגדיר למינימום.\n", delay, MIN_DELAY_MINUTES);
            delay = MIN_DELAY_MINUTES;
          }
          Serial.printf("פרמטרים אופציונליים מ-BLE → עיכוב: %d דקות, חזרות: %d\n", delay, repeat);
          savedDelay = delay;
          savedRepeat = repeat;
          saveScenarioParams(delay, repeat);

        } else {
          Serial.println("אין פרמטרים אופציונליים מ-BLE, מפעיל עם ערכים שמורים.");
        }
        // When starting via BLE, we are no longer just "awake and ready"
        deviceAwakeAndReady = false;
        currentSessionSource = "WEB";
        startScenario();              // Always start scenario after parameters are set/confirmed
        lastActivityTime = millis();  // Reset idle timer on significant BLE command
      } else if (command == 0) {
        Serial.println("תרחיש נעצר דרך BLE.");
        stopScenario(true);           // Call stopScenario with manualStop = true
        lastActivityTime = millis();  // Reset idle timer on significant BLE command
      } else if (command == 2) {
        // Calculate remaining scenarios safely
        uint8_t currentRemaining = 0;
        if (scenarioActive) {
          if (scenarioRepeatMax > scenarioRepeatCount) {  // Ensure no negative result
            currentRemaining = scenarioRepeatMax - scenarioRepeatCount;
          }
        } else if (waitingForReactivation) {
          currentRemaining = 0;  // If waiting for reactivation, the previous sequence is considered complete
        } else if (deviceAwakeAndReady && hasScenarioSaved()) {
          // If device is awake and ready, and a scenario is saved, show the full saved repeat count.
          currentRemaining = savedRepeat;
        } else {
          // Default case: no active scenario, not waiting for reactivation, not awake and ready with saved scenario
          currentRemaining = 0;
        }

        updateStartStopCharacteristic(0, savedDelay, savedRepeat, currentRemaining);
        float voltage = readBatteryVoltage();
        Serial.printf("🔋 מתח התבקש דרך BLE: %0.2f V\n", voltage);
        // lastActivityTime = millis(); // Removed: Don't reset idle timer on status request
      } else {
        Serial.print("פקודה לא תקינה התקבלה ב-Start/Stop: ");
        Serial.println(command);
      }
    } else {
      Serial.println("ערך ריק התקבל בפקודת Start/Stop!");
    }
  }
};

class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) override {
    Serial.println("📲 BLE מחובר");
    if (startStopChar) {
      uint8_t initialRemaining;
      if (scenarioActive) {
        if (scenarioRepeatMax > scenarioRepeatCount) {
          initialRemaining = scenarioRepeatMax - scenarioRepeatCount;
        } else {
          initialRemaining = 0;
        }
      } else if (waitingForReactivation) {
        initialRemaining = 0;  // If waiting for reactivation, the previous sequence is considered complete
      } else if (deviceAwakeAndReady && hasScenarioSaved()) {
        // If device is awake and ready, and a scenario is saved, show the full saved repeat count.
        initialRemaining = savedRepeat;
      } else {
        initialRemaining = 0;
      }
      updateStartStopCharacteristic(0, savedDelay, savedRepeat, initialRemaining);
    }
  }
  void onDisconnect(BLEServer *pServer) override {
    Serial.println("🔌 BLE מנותק, מפעיל מחדש פרסום...");
    BLEDevice::startAdvertising();
  }
};

// --- חדש: פונקציית עזר לשליחת סטטוס OTA ---
void sendOtaStatus(OtaStatus status, uint32_t progress) {
  if (otaStatusChar) {
    uint8_t buffer[5];  // Status (1 byte) + Progress (4 bytes)
    buffer[0] = status;
    buffer[1] = (progress >> 24) & 0xFF;
    buffer[2] = (progress >> 16) & 0xFF;
    buffer[3] = (progress >> 8) & 0xFF;
    buffer[4] = progress & 0xFF;
    otaStatusChar->setValue(buffer, 5);
    otaStatusChar->notify();
    currentOtaStatus = status;  // עדכן את הסטטוס הפנימי
    Serial.printf("OTA Status: %d, Progress: %u\n", status, progress);
  }
}

// --- חדש: Callbacks למאפייני ה-OTA ---
class OtaControlCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) override {
    uint8_t *data = pCharacteristic->getData();
    size_t len = pCharacteristic->getLength();
    uint8_t command = data[0];
    Serial.printf("OTA Control Command: 0x%02X, Length: %u\n", command, len);

    switch (command) {
      case 0x01:
        {                  // Start OTA
          if (len >= 5) {  // Command (1 byte) + Firmware Size (4 bytes)
            uint32_t firmwareSize;
            memcpy(&firmwareSize, data + 1, 4);  // קריאת גודל הקושחה
            Serial.printf("OTA Start command received. Firmware size: %u bytes\n", firmwareSize);

            sendOtaStatus(OTA_STARTING, 0);

            update_partition = esp_ota_get_next_update_partition(NULL);
            if (update_partition == NULL) {
              Serial.println("Fatal Error: No OTA partition found!");
              sendOtaStatus(OTA_FAILED, 0);
              return;
            }
            Serial.printf("Writing to partition subtype %d at offset 0x%x\n", update_partition->subtype, update_partition->address);

            esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
            if (err != ESP_OK) {
              Serial.printf("esp_ota_begin failed (%s)\n", esp_err_to_name(err));
              esp_ota_abort(ota_handle);
              sendOtaStatus(OTA_FAILED, 0);
              return;
            }
            ota_firmware_offset = 0;
            sendOtaStatus(OTA_IN_PROGRESS, ota_firmware_offset);  // עדכון הסטטוס ל-IN_PROGRESS
            Serial.println("OTA process started.");
          } else {
            Serial.println("OTA Start command invalid length.");
            sendOtaStatus(OTA_FAILED, 0);
          }
          break;
        }
      case 0x02:
        {  // End OTA / Verify
          Serial.println("OTA End command received.");
          if (currentOtaStatus != OTA_IN_PROGRESS) {  // בדיקה שה-OTA אכן בתהליך
            Serial.println("OTA not in progress, cannot end.");
            sendOtaStatus(OTA_FAILED, ota_firmware_offset);
            return;
          }
          sendOtaStatus(OTA_VERIFYING, ota_firmware_offset);

          esp_err_t err = esp_ota_end(ota_handle);
          if (err != ESP_OK) {
            if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
              Serial.println("Image validation failed, image is corrupted");
            } else {
              Serial.printf("esp_ota_end failed (%s)!\n", esp_err_to_name(err));
            }
            sendOtaStatus(OTA_FAILED, ota_firmware_offset);
            return;
          }

          err = esp_ota_set_boot_partition(update_partition);
          if (err != ESP_OK) {
            Serial.printf("esp_ota_set_boot_partition failed (%s)!\n", esp_err_to_name(err));
            sendOtaStatus(OTA_FAILED, ota_firmware_offset);
            return;
          }
          Serial.println("OTA update complete. Restarting...");
          sendOtaStatus(OTA_COMPLETE, ota_firmware_offset);
          delay(100);     // Give time for BLE notification to send
          esp_restart();  // אתחול מחדש עם הקושחה החדשה
          break;
        }
      case 0x03:
        {  // Cancel OTA
          Serial.println("OTA Cancel command received.");
          if (currentOtaStatus == OTA_STARTING || currentOtaStatus == OTA_IN_PROGRESS) {  // בדיקה שה-OTA אכן בתהליך
            esp_ota_abort(ota_handle);
            Serial.println("OTA process cancelled.");
            sendOtaStatus(OTA_IDLE, 0);
          } else {
            Serial.println("No active OTA process to cancel.");
          }
          break;
        }
      case 0x04:
        {  // Get current OTA Status
          Serial.println("OTA Get Status command received.");
          sendOtaStatus(currentOtaStatus, ota_firmware_offset);
          break;
        }
      default:
        {
          Serial.printf("Unknown OTA Control Command: 0x%02X\n", command);
          sendOtaStatus(OTA_FAILED, ota_firmware_offset);  // או סטטוס מתאים אחר
          break;
        }
    }
  }
};

class OtaDataCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) override {
    // קבלת נתונים מותרת אם ה-OTA במצב התחלה או בתהליך
    if (currentOtaStatus != OTA_STARTING && currentOtaStatus != OTA_IN_PROGRESS) {
      Serial.println("Received OTA data, but OTA not in starting/in-progress state. Ignoring.");
      sendOtaStatus(OTA_FAILED, ota_firmware_offset);
      return;
    }

    uint8_t *data = pCharacteristic->getData();
    size_t len = pCharacteristic->getLength();

    esp_err_t err = esp_ota_write(ota_handle, data, len);
    if (err != ESP_OK) {
      Serial.printf("esp_ota_write failed (%s)\n", esp_err_to_name(err));
      esp_ota_abort(ota_handle);
      sendOtaStatus(OTA_FAILED, ota_firmware_offset);
      return;
    }
    ota_firmware_offset += len;
    // Serial.printf("OTA data written: %u bytes. Total: %u bytes\n", len, ota_firmware_offset); // ניתן להפעיל לדיבוג מפורט

    // שלח עדכון התקדמות בכל פעם שנתונים נכתבים
    sendOtaStatus(OTA_IN_PROGRESS, ota_firmware_offset);  // השתמש ב-OTA_IN_PROGRESS להתקדמות
  }
};

void startScenarioWithParams(uint16_t delayMinutes, uint8_t repeatCount) {
  if (delayMinutes < MIN_DELAY_MINUTES) {
    Serial.printf("אזהרה: delayMinutes (%d) נמוך מהמינימום (%d). מגדיר למינימום.\n", delayMinutes, MIN_DELAY_MINUTES);
    delayMinutes = MIN_DELAY_MINUTES;
  }
  savedDelay = delayMinutes;
  scenarioRepeatMax = repeatCount;
  EEPROM.write(10, savedDelay & 0xFF);
  EEPROM.write(11, (savedDelay >> 8) & 0xFF);
  EEPROM.write(12, repeatCount);
  EEPROM.write(14, 0);  // Reset scenario count in EEPROM for new sequence
  EEPROM.write(15, 0);  // Clear reactivation flag
  EEPROM.commit();
  Serial.print("✅ מפעיל עם פרמטרים - עיכוב: ");
  Serial.print(savedDelay);
  Serial.print(" דקות, חזרות: ");
  Serial.println(scenarioRepeatMax);
  waitingForNextScenario = false;
  deviceAwakeAndReady = false;  // No longer just "ready", now active
  scenarioActive = true;
  startScenario();
}

void enterDeepSleep(bool onlyButtonWakeup, unsigned long timerDurationMs) {
  // כיבוי NeoPixel
  strip.clear();
  strip.show();

  // בדיקת מצב הכפתור לפני שינה
  bool currentButtonState = digitalRead(buttonPin);
  Serial.print("מצב כפתור לפני שינה (פין 3): ");
  Serial.println(currentButtonState == HIGH ? "HIGH" : "LOW");
  if (currentButtonState == HIGH) {
    Serial.println("⚠ אזהרה: הכפתור במצב HIGH – בדוק נגד משיכה למטה או חיבורי חומרה.");
    // המתנה לשחרור הכפתור
    unsigned long waitStart = millis();
    while (digitalRead(buttonPin) == HIGH && (millis() - waitStart < 1000)) {
      delay(10);  // המתנה עד שהכפתור משתחרר או עד 1 שנייה
    }
    if (digitalRead(buttonPin) == HIGH) {
      Serial.println("🚫 הכפתור עדיין במצב HIGH – לא נכנס לשינה. בדוק חומרה.");
      return;  // לא נכנס לשינה אם הכפתור תקוע
    }
  }

  // ביטול כל מקורות ההתעוררות
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

  // הגדרת התעוררות מכפתור
  esp_sleep_enable_ext1_wakeup((1ULL << buttonPin), ESP_EXT1_WAKEUP_ANY_HIGH);
  Serial.println("הפעלת התעוררות מכפתור (פין 3 HIGH).");

  // הגדרת טיימר רק אם onlyButtonWakeup הוא false
  if (!onlyButtonWakeup) {
    uint64_t sleepTimeUs;
    if (timerDurationMs > 0) {
      sleepTimeUs = (uint64_t)timerDurationMs * 1000ULL;
      Serial.printf("הפעלת טיימר לשינה עבור %lu מילישניות.\n", timerDurationMs);
    } else {
      int delayMinutes = savedDelay;
      if (delayMinutes <= 0) {
        delayMinutes = 3;
        Serial.println("אין עיכוב מוגדר – משתמש בברירת מחדל של 3 דקות.");
      }
      sleepTimeUs = (uint64_t)delayMinutes * 60ULL * 1000000ULL;
      Serial.printf("הפעלת טיימר לשינה עבור %d דקות.\n", delayMinutes);
    }
    esp_sleep_enable_timer_wakeup(sleepTimeUs);
  } else {
    Serial.println("טיימר התעוררות מבוטל – התעוררות מכפתור בלבד.");
  }

  Serial.println("נכנס לשינה עמוקה...");
  Serial.flush();
  esp_deep_sleep_start();
}

void startScenario() {
  if (waitingForReactivation) {
    Serial.println("❌ לא ניתן להתחיל – ממתין להפעלה מחדש דרך כפתור או BLE.");
    return;
  }
  waitingForNextScenario = false;
  deviceAwakeAndReady = false;  // No longer just "ready", now active
  scenarioActive = true;
  scenarioCompleted = false;
  totalVibrations = 0;
  previousMillis = millis();
  previousMillisVib = millis();
  scenarioRepeatCount++;  // Increment count for the current repetition
  Serial.println("תרחיש הופעל.");
  lastActivityTime = millis();  // Reset idle timer when scenario starts
  float voltage = readBatteryVoltage();
  Serial.print("מתח סוללה בתחילת התרחיש: ");
  Serial.println(voltage);
  if (currentSessionId.isEmpty()) {
    currentSessionId = String(millis());
  }
  int batteryPercent = estimateBatteryPercent(voltage);
  appendLogEvent(
      currentSessionId,
      "SESSION_START",
      currentSessionSource.isEmpty() ? String("UNKNOWN") : currentSessionSource,
      vibratePower,
      pulseInterval,
      pulsenumber,
      vibrationDuration,
      static_cast<int>(savedDelay),
      static_cast<int>(savedRepeat),
      "",
      batteryPercent,
      ""
  );
#ifdef ESP32
  ledcWrite(ledChannel, vibratePower);
#endif
}

// Modified stopScenario function
void stopScenario(bool manualStop) {
  scenarioActive = false;
  totalVibrations = 0;
  vibState = LOW;

#ifdef ESP32
  ledcWrite(vibPin, 0);  // כיבוי מלא של הוויברטור
#endif

  float voltage = readBatteryVoltage();
  Serial.print("🔋 מתח סוללה בסיום: ");
  Serial.println(voltage);

  Serial.print("⛔ התרחיש הסתיים ");
  Serial.print(scenarioRepeatCount);
  Serial.print(" מתוך ");
  Serial.println(scenarioRepeatMax);
  Serial.printf("תרחישים נותרים: %d\n", scenarioRepeatMax - scenarioRepeatCount);

  scenarioCompleted = true;  // Mark current repetition as completed
  scenarioEndTime = millis();
  Serial.print("⏱ זמן סיום: ");
  Serial.print(scenarioEndTime);
  Serial.println(" ms");

  int batteryPercent = estimateBatteryPercent(voltage);
  String stopReason = manualStop ? String("MANUAL_STOP") : String("AUTO_STOP");
  appendLogEvent(
      currentSessionId,
      "SESSION_END",
      currentSessionSource.isEmpty() ? String("UNKNOWN") : currentSessionSource,
      vibratePower,
      pulseInterval,
      pulsenumber,
      vibrationDuration,
      static_cast<int>(savedDelay),
      static_cast<int>(savedRepeat),
      stopReason,
      batteryPercent,
      ""
  );

  // Reset session id after logging end
  currentSessionId = "";

  // This is the point where we decide the next state based on completion or manual stop
  if (manualStop) {
    Serial.println("DEBUG: Scenario stopped manually.");
    waitingForReactivation = false;  // Manual stop means not waiting for full sequence reactivation
    scenarioRepeatCount = 0;         // Reset count for a fresh start
    EEPROM.write(15, 0);             // Clear reactivation flag in EEPROM
    EEPROM.write(14, 0);             // Clear scenario count in EEPROM
    EEPROM.commit();
    updateStartStopCharacteristic(0, savedDelay, savedRepeat, 0);  // Notify BLE with 0 remaining
    deviceAwakeAndReady = true;                                    // Go to "ready" state for 1 minute idle
    lastActivityTime = millis();                                   // Reset activity time for idle timer
    strip.setPixelColor(0, strip.Color(255, 255, 255));            // White for ready
    strip.show();
  } else {  // Automatic completion of a single repetition
    if (scenarioRepeatCount >= scenarioRepeatMax) {
      Serial.println("DEBUG: All repetitions completed automatically.");
      waitingForReactivation = true;  // All repetitions done, waiting for user to reactivate
      scenarioRepeatCount = 0;        // Reset count for next full sequence
      EEPROM.write(15, 1);            // Save reactivation flag to EEPROM
      EEPROM.write(14, 0);            // Clear scenario count in EEPROM
      EEPROM.commit();
      updateStartStopCharacteristic(0, savedDelay, savedRepeat, 0);  // Notify BLE with 0 remaining
      deviceAwakeAndReady = true;                                    // Go to "ready" state for 1 minute idle
      lastActivityTime = millis();                                   // Reset idle timer to enter sleep after 1 min
      strip.setPixelColor(0, strip.Color(255, 255, 255));            // White for ready
      strip.show();
    } else {
      // Repetition completed, but more repetitions to go (sleep between repetitions)
      Serial.println("DEBUG: Repetition completed, more to go. Entering timed sleep.");
      waitingForNextScenario = true;  // Set flag for timed sleep
      deviceAwakeAndReady = false;    // Not in awake and ready state
      // Do NOT reset scenarioRepeatCount here, it's already incremented in startScenario()
      EEPROM.write(14, scenarioRepeatCount);  // Save current scenario count
      EEPROM.write(15, 0);                    // Ensure reactivation flag is false
      EEPROM.commit();
      // Go to deep sleep with timer + button wakeup
      enterDeepSleep(false, (unsigned long)savedDelay * 60UL * 1000UL - WAKEUP_BEFORE_NEXT_SCENARIO_MS);
      return;  // Exit function, device is going to sleep
    }
  }

  Serial.printf("DEBUG: lastActivityTime set to %lu in stopScenario()\n", lastActivityTime);  // Debug print
}

void savePostDelayAndSleep(uint16_t postDelayMinutes) {
  EEPROM.write(20, postDelayMinutes & 0xFF);
  EEPROM.write(21, (postDelayMinutes >> 8) & 0xFF);
  EEPROM.commit();
  unsigned long sleepTimeMs = (unsigned long)postDelayMinutes * 60UL * 1000UL;
  Serial.printf("💤 נכנס לשינה למשך %d דקות (%lu מילישניות)...\n", postDelayMinutes, sleepTimeMs);
  enterDeepSleep(false, sleepTimeMs);
}

void checkButtonPress() {
  bool currentButtonState = digitalRead(buttonPin);
  if (currentButtonState != lastButtonState) {
    Serial.print("שינוי מצב כפתור: ");
    Serial.println(currentButtonState == HIGH ? "HIGH" : "LOW");
    buttonPressStart = millis();
  }

  if (currentButtonState == HIGH && (millis() - buttonPressStart > debounceDelay)) {  // Use global debounceDelay
    if (!buttonHeld) {
      buttonHeld = true;
      Serial.println("כפתור נלחץ (לאחר דה-באונסינג).");
      // lastActivityTime = millis(); // Removed: Don't reset idle timer on every button press detection

      if (waitingForReactivation) {
        // First press after all scenarios completed: transition to "ready" state
        Serial.println("✅ הופעל מחדש באמצעות כפתור (מעבר למצב 'מוכן').");
        waitingForReactivation = false;
        scenarioRepeatCount = 0;
        EEPROM.write(14, 0);
        EEPROM.write(15, 0);
        EEPROM.commit();
        scenarioActive = false;
        scenarioCompleted = false;
        totalVibrations = 0;
        digitalWrite(vibPin, LOW);
        digitalWrite(ledBuiltin, LOW);
        deviceAwakeAndReady = true;                          // Now in "ready" state
        strip.setPixelColor(0, strip.Color(255, 255, 255));  // White for ready
        strip.show();
        lastActivityTime = millis();  // Reset idle timer on significant state change
      } else if (deviceAwakeAndReady) {
        // Second press after waking up or being in "ready" state: start scenario
        Serial.println("▶ כפתור נלחץ במצב 'מוכן'. מתחיל תרחיש...");
        deviceAwakeAndReady = false;  // No longer just "ready"
        waitingForNextScenario = false;
        scenarioRepeatCount = 0;  // Ensure fresh start if starting from ready
        currentSessionSource = "BUTTON";
        startScenarioWithParams(savedDelay, savedRepeat);
        lastActivityTime = millis();  // Reset idle timer on significant state change
      } else if (scenarioActive) {
        // Button pressed while scenario is active: stop scenario
        Serial.println("כפתור נלחץ. ממתין לשחרור לפני עצירת תרחיש...");
        unsigned long waitStart = millis();
        while (digitalRead(buttonPin) == HIGH && (millis() - waitStart < 3000)) {
          delay(10);
        }
        if (digitalRead(buttonPin) == HIGH) {
          Serial.println("🚫 הכפתור עדיין במצב HIGH – לא עוצר תרחיש. בדוק חומרה.");
          return;
        }
        Serial.println("כפתור שוחרר. עוצר תרחיש...");
        stopScenario(true);  // Call stopScenario with manualStop = true
      } else {
        // Initial press from idle deep sleep (not waitingForReactivation, not deviceAwakeAndReady, not scenarioActive)
        Serial.println("כפתור נלחץ. נכנס למצב 'מוכן'.");
        deviceAwakeAndReady = true;
        strip.setPixelColor(0, strip.Color(255, 255, 255));  // White for ready
        strip.show();
        lastActivityTime = millis();  // Reset idle timer on significant state change
      }
    }
  } else if (currentButtonState == LOW && buttonHeld) {
    buttonHeld = false;
    Serial.println("כפתור שוחרר.");
  }
  lastButtonState = currentButtonState;
}

// New function to initialize EEPROM with default values
void initializeEEPROMDefaults() {
  Serial.println("אתחול EEPROM עם ערכי ברירת מחדל...");
  EEPROM.write(EEPROM_MAGIC_ADDRESS, EEPROM_MAGIC_NUMBER);  // Write magic number

  // Default general settings
  EEPROM.write(1, 195);          // vibratePower
  EEPROM.write(2, 1000 >> 8);    // pulseInterval MSB
  EEPROM.write(3, 1000 & 0xFF);  // pulseInterval LSB
  EEPROM.write(4, 60 >> 8);      // pulsenumber MSB
  EEPROM.write(5, 60 & 0xFF);    // pulsenumber LSB
  EEPROM.write(6, 400 >> 8);     // vibrationDuration MSB
  EEPROM.write(7, 400 & 0xFF);   // vibrationDuration LSB

  // Default scenario parameters
  EEPROM.write(10, MIN_DELAY_MINUTES & 0xFF);         // savedDelay LSB
  EEPROM.write(11, (MIN_DELAY_MINUTES >> 8) & 0xFF);  // savedDelay MSB
  EEPROM.write(12, 4);                                // savedRepeat (default to 4 repetitions)
  EEPROM.write(13, 0);                                // Unused
  EEPROM.write(14, 0);                                // scenarioRepeatCount
  EEPROM.write(15, 0);                                // waitingForReactivation

  // MAC Address flag
  EEPROM.write(29, 0);  // MAC address not saved yet

  EEPROM.commit();
  Serial.println("EEPROM אותחל בהצלחה.");
}

void setup() {
  Serial.begin(115200);
  EEPROM.begin(512);
  initLogStorage();  // Initialize SPIFFS for log storage
  pinMode(buttonPin, INPUT);
  pinMode(vibPin, OUTPUT);
  // Changed pinMode for ledBuiltin to INPUT
  pinMode(ledBuiltin, INPUT);     // Changed from OUTPUT to INPUT to read its state
  digitalWrite(ledBuiltin, LOW);  // Still set to LOW to ensure it's off if it was an output before
                                  // pinMode(CHARGE_DETECT_PIN, INPUT_PULLUP); // REMOVED: No longer using this pin

#ifdef ESP32
  ledcAttach(vibPin, freq, resolution);
#endif
  strip.begin();
  strip.clear();
  strip.show();

  // Check EEPROM initialization
  if (EEPROM.read(EEPROM_MAGIC_ADDRESS) != EEPROM_MAGIC_NUMBER) {
    initializeEEPROMDefaults();
  }

  float currentBatteryVoltage = readBatteryVoltage();
  Serial.print("מתח סוללה: ");
  Serial.println(currentBatteryVoltage);

  wakeupReason = esp_sleep_get_wakeup_cause();
  Serial.print("סיבת התעוררות: ");
  Serial.println((int)wakeupReason);
  unsigned long wakeupTime = millis();
  Serial.print("זמן מאז ההפעלה: ");
  Serial.print(wakeupTime / 1000);
  Serial.println(" שניות");
  // בדיקת מקור התעוררות EXT1
  if (wakeupReason == ESP_SLEEP_WAKEUP_EXT1) {
    uint64_t wakeupPinMask = esp_sleep_get_ext1_wakeup_status();
    Serial.print("פינים שהפעילו EXT1 wakeup: 0x");
    Serial.println(wakeupPinMask, HEX);
  }
  loadScenarioParams();
  scenarioRepeatMax = savedRepeat;  // Ensure scenarioRepeatMax is loaded correctly
  if (wakeupReason == ESP_SLEEP_WAKEUP_EXT1) {
    Serial.println("התעורר מ-EXT1 (לחיצת כפתור).");
    bool currentButtonState = digitalRead(buttonPin);
    Serial.print("מצב כפתור לאחר התעורורות: ");
    Serial.println(currentButtonState == HIGH ? "HIGH" : "LOW");
    if (currentButtonState == HIGH) {
      Serial.println("⚠ הכפתור במצב HIGH – ממתין לשחרור.");
      unsigned long waitStart = millis();
      while (digitalRead(buttonPin) == HIGH && (millis() - waitStart < 3000)) {
        delay(10);
      }
      if (digitalRead(buttonPin) == HIGH) {
        Serial.println("🚫 הכפתור עדיין במצב HIGH – נכנס לשינה מחדש.");
        enterDeepSleep(true);
        return;
      }
    }
    delay(10);  // Small delay after button release check
    if (hasScenarioSaved()) {
      Serial.println("✅ התעוררות כפתור – נכנס למצב 'מוכן'.");
      waitingForReactivation = false;  // If it was true, we are now ready to reactivate
      scenarioRepeatCount = 0;         // Reset count for a new sequence
      EEPROM.write(14, 0);             // Clear saved count
      EEPROM.write(15, 0);             // Clear reactivation flag
      EEPROM.commit();
      deviceAwakeAndReady = true;                          // Set new flag
      lastActivityTime = millis();                         // Added this line
      strip.setPixelColor(0, strip.Color(255, 255, 255));  // White for ready
      strip.show();
      // DO NOT call startScenarioWithParams here. Wait for the second button press.
    } else {
      Serial.println("⚠ אין תסריט שמור – מציג כתום ונכנס לשינה.");
      strip.setPixelColor(0, strip.Color(255, 165, 0));  // Orange for no scenario
      strip.show();
      enterDeepSleep(true);  // Go back to sleep if nothing to run
    }
  } else if (wakeupReason == ESP_SLEEP_WAKEUP_TIMER) {
    Serial.println("התעורר מטיימר.");
    waitingForReactivation = EEPROM.read(15) == 1;  // Load flag from EEPROM
    scenarioRepeatCount = EEPROM.read(14);          // Load count from EEPROM
    if (!hasScenarioSaved()) {
      Serial.println("⛔ אין תסריט שמור – נכנס לשינה עמוקה.");
      strip.clear();
      strip.show();
      enterDeepSleep(true);
    }
    if (waitingForReactivation) {
      Serial.println("🔵 ממתין להפעלה מחדש ידנית.");
      lastActivityTime = millis();  // Ensure activity time is updated on timer wakeup to allow 1 min idle
                                    // No immediate deep sleep, let loop() handle it after IDLE_SLEEP_TIMEOUT_MS
    } else if (scenarioRepeatCount < scenarioRepeatMax) {
      Serial.printf("▶ מתחיל תסריט (%d/%d)\n", scenarioRepeatCount + 1, scenarioRepeatMax);
      scenarioCompleted = false;
      totalVibrations = 0;
      startScenario();
    } else {
      Serial.println("✅ הושג מספר החזרות המרבי. נכנס לשינה עמוקה.");
      waitingForReactivation = true;
      scenarioRepeatCount = 0;
      EEPROM.write(15, 1);
      EEPROM.commit();
      enterDeepSleep(true);
    }
  }

  // --- בלוק טיפול בכתובת MAC ושמירתה ב-EEPROM (מתוקן לשימוש ב-BLEDevice::getAddress()) ---
  uint8_t macAddressSavedFlag = EEPROM.read(29);  // בדיקה אם כתובת ה-MAC כבר נשמרה (כתובת 29 ב-EEPROM)

  if (macAddressSavedFlag != 1) {  // אם כתובת ה-MAC עדיין לא נשמרה
    // אתחול BLEDevice כדי שנוכל לקבל את הכתובת שלו
    BLEDevice::init("");                                 // אתחול זמני ללא שם, רק כדי לקבל גישה לכתובת
    BLEAddress bleMacAddress = BLEDevice::getAddress();  // קבלת כתובת ה-MAC של ה-BLE

    uint8_t macBytes[6];
    memcpy(macBytes, bleMacAddress.getNative(), 6);  // העתקת הבייטים של ה-MAC

    // שמירת בייטי כתובת ה-MAC ב-EEPROM (החל מכתובת 30)
    for (int i = 0; i < 6; i++) {
      EEPROM.write(30 + i, macBytes[i]);
    }
    EEPROM.write(29, 1);  // סימון שכתובת ה-MAC נשמרה
    EEPROM.commit();      // ביצוע השמירה ל-EEPROM
    Serial.println("כתובת MAC נשמרה ב-EEPROM בפעם הראשונה.");

    // המרת כתובת ה-MAC למחרוזת לצורך שימוש נוכחי
    sprintf(storedMacAddress, "%02X%02X%02X%02X%02X%02X", macBytes[0], macBytes[1], macBytes[2], macBytes[3], macBytes[4], macBytes[5]);
  } else {  // אם כתובת ה-MAC כבר נשמרה
    uint8_t macBytes[6];
    // טעינת בייטי כתובת ה-MAC מ-EEPROM
    for (int i = 0; i < 6; i++) {
      macBytes[i] = EEPROM.read(30 + i);
    }
    // המרת כתובת ה-MAC למחרוזת
    sprintf(storedMacAddress, "%02X%02X%02X%02X%02X%02X", macBytes[0], macBytes[1], macBytes[2], macBytes[3], macBytes[4], macBytes[5]);
    Serial.println("כתובת MAC נטענה מ-EEPROM.");
  }
  Serial.printf("כתובת MAC שמורה: %s\n", storedMacAddress);

  String deviceName = "Golden life - ";
  deviceName += storedMacAddress;  // שימוש בכתובת ה-MAC השמורה לשם מכשיר ה-BLE

  BLEDevice::init(deviceName.c_str());  // הגדרת שם מכשיר ה-BLE עם כתובת ה-MAC (אתחול סופי עם השם)

  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new MyServerCallbacks());
  BLEService *service = server->createService(SERVICE_UUID);
  vibratePowerChar = service->createCharacteristic(CHAR_UUID_VIBRATE_POWER, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  pulseIntervalChar = service->createCharacteristic(CHAR_UUID_PULSE_INTERVAL, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  pulseNumberChar = service->createCharacteristic(CHAR_UUID_PULSENUMBER, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  vibrationDurationChar = service->createCharacteristic(CHAR_UUID_VIBRATION_DURATION, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  startStopChar = service->createCharacteristic(CHAR_UUID_START, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY);
  startStopChar->addDescriptor(new BLE2902());
  vibrationStatusChar = service->createCharacteristic(CHAR_UUID_VIBRATION_STATUS, BLECharacteristic::PROPERTY_NOTIFY);
  vibrationStatusChar->addDescriptor(new BLE2902());
  vibratePowerChar->setCallbacks(new GeneralSettingsCallbacks());
  pulseIntervalChar->setCallbacks(new GeneralSettingsCallbacks());
  pulseNumberChar->setCallbacks(new GeneralSettingsCallbacks());
  vibrationDurationChar->setCallbacks(new GeneralSettingsCallbacks());
  startStopChar->setCallbacks(new StartStopCallbacks());
  service->start();

  // --- חדש: יצירת שירות OTA חדש ---
  BLEService *otaService = server->createService(OTA_SERVICE_UUID);

  // מאפיין לקבלת נתוני הקושחה (Write_No_Rsp לביצועים)
  otaDataChar = otaService->createCharacteristic(
    OTA_CHAR_DATA_UUID,
    BLECharacteristic::PROPERTY_WRITE_NR  // Write without response
  );
  otaDataChar->setCallbacks(new OtaDataCallbacks());

  // מאפיין לפקודות בקרה (Start, End, Cancel)
  otaControlChar = otaService->createCharacteristic(
    OTA_CHAR_CONTROL_UUID,
    BLECharacteristic::PROPERTY_WRITE);
  otaControlChar->setCallbacks(new OtaControlCallbacks());

  // מאפיין לסטטוס והתקדמות (Notify)
  otaStatusChar = otaService->createCharacteristic(
    OTA_CHAR_STATUS_UUID,
    BLECharacteristic::PROPERTY_NOTIFY);
  otaStatusChar->addDescriptor(new BLE2902());

  otaService->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  // --- חדש: הוסף את שירות ה-OTA לפרסום ---
  advertising->addServiceUUID(OTA_SERVICE_UUID);
  advertising->start();

  Serial.println("BLE OTA Service started and advertising.");

  loadSettings();
  if (startStopChar) {
    // Calculate initial remaining scenarios safely for setup
    uint8_t initialRemaining = 0;
    if (scenarioActive) {
      if (scenarioRepeatMax > scenarioRepeatCount) {
        initialRemaining = scenarioRepeatMax - scenarioRepeatCount;
      } else {
        initialRemaining = 0;
      }
    } else if (waitingForReactivation) {
      initialRemaining = 0;  // If waiting for reactivation, the previous sequence is considered complete
    } else if (deviceAwakeAndReady && hasScenarioSaved()) {
      // If device is awake and ready, and a scenario is saved, show the full saved repeat count.
      initialRemaining = savedRepeat;
    } else {
      initialRemaining = 0;
    }
    updateStartStopCharacteristic(0, savedDelay, savedRepeat, initialRemaining);
  }
}

void loop() {
  checkButtonPress();
  unsigned long currentMillis = millis();
  // קריאת מתח סוללה בתדירות מוגבלת
  static unsigned long lastVoltageReadTime = 0;
  const unsigned long voltageReadInterval = 5000;  // קרא מתח כל 5 שניות (5000 מילישניות)
  float batteryVoltage = previousBatteryVoltage;   // השתמש בערך האחרון שידוע
  if (currentMillis - lastVoltageReadTime >= voltageReadInterval) {
    lastVoltageReadTime = currentMillis;
    batteryVoltage = readBatteryVoltage();  // קרא מתח רק עכשיו
    // אם אתה רוצה לעדכן את ה-BLE בתדירות זו, הוסף כאן קריאה ל-updateStartStopCharacteristic
    // Calculate remaining scenarios safely for BLE update
    uint8_t currentRemaining = 0;
    if (scenarioActive) {
      if (scenarioRepeatMax > scenarioRepeatCount) {
        currentRemaining = scenarioRepeatMax - scenarioRepeatCount;
      } else {
        currentRemaining = 0;
      }
    } else if (waitingForReactivation) {
      currentRemaining = 0;  // If waiting for reactivation, the previous sequence is considered complete
    } else if (deviceAwakeAndReady && hasScenarioSaved()) {
      // If device is awake and ready, and a scenario is saved, show the full saved repeat count.
      currentRemaining = savedRepeat;
    } else {
      currentRemaining = 0;
    }
    updateStartStopCharacteristic(0, savedDelay, savedRepeat, currentRemaining);
    previousBatteryVoltage = batteryVoltage;  // שמור את הערך האחרון
  }
  static unsigned long lastDebug = 0;
  if (currentMillis - lastDebug > 60000) {
    lastDebug = currentMillis;
    Serial.printf("מתח סוללה: %.2f V | תרחיש פעיל: %s | ממתין לתרחיש הבא: %s | ממתין להפעלה מחדש: %s | מונה חזרות: %d/%d | זמן מאז הפעילות האחרונה: %lu שניות | מכשיר ער ומוכן: %s\n",
                  batteryVoltage,  // השתמש בערך המעודכן
                  scenarioActive ? "כן" : "לא",
                  waitingForNextScenario ? "כן" : "לא",
                  waitingForReactivation ? "כן" : "לא",
                  scenarioRepeatCount, scenarioRepeatMax,
                  (currentMillis - lastActivityTime) / 1000,
                  deviceAwakeAndReady ? "כן" : "לא");
  }

  // isCharging = digitalRead(CHARGE_DETECT_PIN) == HIGH; // REMOVED: No longer using this pin

  // --- LED Logic ---
  // Highest priority: Built-in LED (Pin 15) is HIGH (Green) - now handles USB power detection
  if (digitalRead(ledBuiltin) == HIGH) {             // Check if built-in LED pin is HIGH (e.g., connected to USB power)
    strip.setPixelColor(0, strip.Color(0, 255, 0));  // Green when built-in LED is HIGH (indicating USB power/charging)
    strip.show();
  }
  // Next priority: Low Battery (Red)
  else if (previousBatteryVoltage > 0 && previousBatteryVoltage < LOW_BATTERY_THRESHOLD_V) {
    strip.setPixelColor(0, strip.Color(255, 0, 0));  // Red for low battery
    strip.show();
  }
  // Next priority: Scenario Active (Cyan)
  else if (scenarioActive) {
    strip.setPixelColor(0, strip.Color(0, 255, 255));  // Cyan for active scenario
    strip.show();
  }
  // Next priority: Device Awake and Ready (White)
  else if (deviceAwakeAndReady) {
    strip.setPixelColor(0, strip.Color(255, 255, 255));  // White for ready
    strip.show();
  }
  // Next priority: Waiting for Reactivation (Blue)
  else if (waitingForReactivation) {
    strip.setPixelColor(0, strip.Color(0, 0, 255));  // Blue for waiting reactivation
    strip.show();
  }
  // Next priority: Waiting for Next Scenario (Orange)
  else if (waitingForNextScenario) {
    strip.setPixelColor(0, strip.Color(255, 165, 0));  // Orange for waiting next scenario
    strip.show();
  }
  // Default: Off
  else {
    strip.setPixelColor(0, strip.Color(0, 0, 0));  // Off by default when no special state
    strip.show();
  }

  if (scenarioActive) {
    unsigned long currentMillisVib = millis();
    unsigned long requiredInterval = (vibState == HIGH) ? vibrationDuration : (pulseInterval - vibrationDuration);
    if (currentMillisVib - previousMillisVib >= requiredInterval) {
      previousMillisVib = currentMillisVib;
      vibState = !vibState;
      if (totalVibrations < pulsenumber) {
#ifdef ESP32
        ledcWrite(vibPin, vibState == HIGH ? vibratePower : 0);
#endif
      } else {
#ifdef ESP32
        ledcWrite(vibPin, 0);
#endif
      }
      if (vibState == HIGH) {
        totalVibrations++;
        int remainingVibrations = pulsenumber - totalVibrations;
        updateVibrationStatusBLE(remainingVibrations);
        Serial.printf("רטטים נותרים: %d (סה\"כ: %d)\n", remainingVibrations, totalVibrations);
        if (totalVibrations >= pulsenumber) {
          stopScenario(false);  // Call stopScenario with manualStop = false (automatic completion)
        }
      }
    }
  }
  // בתוך פונקציית loop():
  // טיפול בשינה עמוקה כאשר המכשיר אינו פעיל (לא בתרחיש ולא ממתין לתרחיש הבא)
  // זה יופעל רק אם stopScenario לא הכניסה כבר לשינה עמוקה (כלומר, לא במצב של "שינה בין תרחישים")
  // וגם כאשר אין תהליך OTA פעיל.
  if (!scenarioActive && !waitingForNextScenario && currentOtaStatus == OTA_IDLE) {  // נוסף תנאי OTA
    unsigned long currentMillisForIdleCheck = millis();                              // Get fresh millis for idle check
    unsigned long elapsedSinceLastActivity = currentMillisForIdleCheck - lastActivityTime;
    //Serial.printf("DEBUG: Idle check - currentMillis: %lu, lastActivityTime: %lu, elapsed: %lu\n", currentMillisForIdleCheck, lastActivityTime, elapsedSinceLastActivity); // Debug print
    hasPrintedSleepMessage = false;
    if (elapsedSinceLastActivity >= IDLE_SLEEP_TIMEOUT_MS) {
      Serial.printf("אין תרחישים פעילים או המתנה לתרחיש הבא, חוסר פעילות למשך %lu שניות - נכנס לשינה עמוקה.\n",
                    elapsedSinceLastActivity / 1000UL);
      // In these states (deviceAwakeAndReady or waitingForReactivation), user wants button-only wakeup
      //Serial.println("DEBUG: Entering deep sleep (button-only wakeup).");
      enterDeepSleep(true);  // Button-only wakeup
    }
  }
}
