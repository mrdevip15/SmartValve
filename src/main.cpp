/**
 * ============================================================
 *  SISTEM KONTROL KATUP OTOMATIS  (v2.3 - Smooth RPM)
 *  Board  : Arduino Uno / Nano
 *  Sensor : KY-037 (AO), IR RPM (interrupt), Servo, LCD I2C
 * ============================================================
 */

#include <Arduino.h>
#include <LiquidCrystal_I2C.h>
#include <Servo.h>
#include <math.h>

// ==================== PIN ====================
#define SERVO_PIN 9
#define IR_SENSOR_PIN 2
#define SOUND_PIN A0
#define BUTTON_MODE1_PIN 4
#define BUTTON_MODE2_PIN 5
#define BUTTON_MODE3_PIN 6

// ==================== SERVO ====================
#define SERVO_OPEN 0
#define SERVO_CLOSE 95

// ==================== KALIBRASI dB (MAX4466) ====================
#define DB_REF 36.5f   // dB saat diam (matching user meter)
#define P2P_REF 16.0f  // baseline P2P saat diam (calibrated offline)
#define DB_SCALE 47.0f // Skala sensitivitas
#define DB_MIN 30.0f
#define DB_MAX 120.0f

// ==================== THRESHOLD ====================
#define SOUND_THRESHOLD_DB 75.0f
#define RPM_THRESHOLD 4000
#define RPM_PPR 1 // Pulses Per Revolution

// ==================== TIMING ====================
#define SAMPLE_INTERVAL 100UL   // Hitung moving average tiap 100ms
#define AVG_WINDOW      10      // Window 1 detik (10 x 100ms)
#define LCD_INTERVAL 300UL      // LCD update lebih cepat biar smooth
#define RPM_TIMEOUT_MS 10000UL
#define MODE1_CLOSE_HOLD 1000UL
#define CYCLE_DURATION 60000UL
#define DEBOUNCE_DELAY 150UL
#define RPM_DEBOUNCE_MS 10      // Heavy filter 10ms

// ==================== SOUND SAMPLING ====================
#define SOUND_WINDOW_MS 150UL
#define P2P_AVG_COUNT 4

// ==================== OBJEK ====================
LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo katupServo;

enum SystemMode : uint8_t { MODE_IDLE = 1, MODE_NORMAL = 2, MODE_BURU = 3 };

// ==================== STATE GLOBAL ====================
volatile uint16_t pulseCount = 0;
volatile unsigned long lastPulseTime = 0;
uint16_t lastPulses = 0; // Menampilkan pulsa per 100ms
unsigned long lastRPMCalcTime = 0;
uint16_t currentRPM = 0;

// Moving Average Buffer
uint16_t rpmBuffer[AVG_WINDOW] = {0};
uint8_t rpmBufIdx = 0;
uint32_t rpmRunningSum = 0;

float currentDB = 0.0f;
uint16_t lastPeakToPeak = 0;
uint16_t soundMax = 0;
uint16_t soundMin = 1023;
unsigned long soundWindowStart = 0;
uint16_t p2pBuf[P2P_AVG_COUNT] = {0};
uint8_t p2pIdx = 0;

SystemMode currentMode = MODE_IDLE;
unsigned long lastModeChange = 0;
unsigned long cycleStartTime = 0;
bool isCycleActive = false;
bool isClosingPhase = false;
unsigned long mode1CloseStart = 0;
bool mode1HoldActive = false;
unsigned long lastLCDUpdate = 0;
int servoPos = SERVO_OPEN;

// --- STRUKTUR TOMBOL ---
struct ButtonState {
  uint8_t pin;
  bool stableState;
  bool actionTaken;
  int16_t confidence;
};

ButtonState buttons[3] = {
    {BUTTON_MODE1_PIN, LOW, false, 0},
    {BUTTON_MODE2_PIN, LOW, false, 0},
    {BUTTON_MODE3_PIN, LOW, false, 0},
};

void countPulse() {
  unsigned long now = millis();
  if (now - lastPulseTime > RPM_DEBOUNCE_MS) {
    pulseCount++;
    lastPulseTime = now;
  }
}

void sampleSound() {
  uint16_t s = analogRead(SOUND_PIN);
  if (s > soundMax) soundMax = s;
  if (s < soundMin) soundMin = s;

  unsigned long now = millis();
  if (now - soundWindowStart < SOUND_WINDOW_MS) return;
  soundWindowStart = now;

  uint16_t p2p = (soundMax >= soundMin) ? (soundMax - soundMin) : 0;
  soundMax = 0; soundMin = 1023;

  p2pBuf[p2pIdx] = p2p;
  p2pIdx = (p2pIdx + 1) % P2P_AVG_COUNT;

  uint32_t sum = 0;
  for (uint8_t i = 0; i < P2P_AVG_COUNT; i++) sum += p2pBuf[i];
  uint16_t p2pAvg = (uint16_t)(sum / P2P_AVG_COUNT);
  lastPeakToPeak = p2pAvg;

  float pp = (p2pAvg < 1) ? 1.0f : (float)p2pAvg;
  float db = DB_REF + DB_SCALE * log10f(pp / P2P_REF);
  if (db < DB_MIN) db = DB_MIN;
  if (db > DB_MAX) db = DB_MAX;
  currentDB = db;
}

void calculateRPM() {
  unsigned long now = millis();
  if (now - lastRPMCalcTime < SAMPLE_INTERVAL) return;

  noInterrupts();
  uint16_t pulsesThisSample = pulseCount;
  pulseCount = 0;
  interrupts();

  lastPulses = pulsesThisSample;
  lastRPMCalcTime = now;

  // Update Moving Average
  rpmRunningSum -= rpmBuffer[rpmBufIdx];
  rpmBuffer[rpmBufIdx] = pulsesThisSample;
  rpmRunningSum += rpmBuffer[rpmBufIdx];
  rpmBufIdx = (rpmBufIdx + 1) % AVG_WINDOW;

  // RPM = (Total Pulsa 1 detik * 60) / PPR
  currentRPM = (uint16_t)((rpmRunningSum * 60UL) / RPM_PPR);
}

inline void servoWrite(int pos) {
  if (pos != servoPos) {
    katupServo.write(pos);
    servoPos = pos;
  }
}
inline void servoOpen() { servoWrite(SERVO_OPEN); }
inline void servoClose() { servoWrite(SERVO_CLOSE); }

void resetCycle() {
  isCycleActive = false;
  isClosingPhase = false;
  servoOpen();
}

void processMode() {
  unsigned long now = millis();
  bool soundTrigger = (currentDB > SOUND_THRESHOLD_DB);
  bool rpmTrigger = (currentRPM > RPM_THRESHOLD);

  switch (currentMode) {
  case MODE_IDLE:
    isCycleActive = false;
    if (soundTrigger) {
      if (!mode1HoldActive) {
        servoClose();
        mode1HoldActive = true;
      }
      mode1CloseStart = now;
    } else {
      if (mode1HoldActive) {
        if (now - mode1CloseStart >= MODE1_CLOSE_HOLD) {
          servoOpen();
          mode1HoldActive = false;
        }
      } else {
        servoOpen();
      }
    }
    break;

  case MODE_NORMAL:
    if (soundTrigger && rpmTrigger) {
      if (!isCycleActive) {
        isCycleActive = true;
        isClosingPhase = true;
        cycleStartTime = now;
        servoClose();
      }
    } else {
      if (isCycleActive) resetCycle();
      else servoOpen();
    }
    break;

  case MODE_BURU:
    if (!rpmTrigger) {
      if (!isCycleActive) {
        isCycleActive = true;
        isClosingPhase = true;
        cycleStartTime = now;
        servoClose();
      }
    } else {
      if (isCycleActive) resetCycle();
      else servoOpen();
    }
    break;
  }

  if (isCycleActive && (now - cycleStartTime >= CYCLE_DURATION)) {
    isClosingPhase = !isClosingPhase;
    cycleStartTime = now;
    if (isClosingPhase) servoClose();
    else servoOpen();
  }
}

void checkButtons() {
  for (uint8_t i = 0; i < 3; i++) {
    bool reading = digitalRead(buttons[i].pin);
    if (reading == HIGH) {
      if (buttons[i].confidence < 8) buttons[i].confidence++;
    } else {
      if (buttons[i].confidence > 0) buttons[i].confidence--;
    }

    bool currentState = buttons[i].stableState;
    if (buttons[i].confidence > 5) currentState = HIGH;
    else if (buttons[i].confidence < 2) currentState = LOW;

    if (currentState != buttons[i].stableState) {
      buttons[i].stableState = currentState;
      if (currentState == HIGH && !buttons[i].actionTaken && (millis() - lastModeChange) > 800UL) {
        buttons[i].actionTaken = true;
        lastModeChange = millis();
        currentMode = (SystemMode)(i + 1);
        mode1HoldActive = false;
        resetCycle();
        for (uint8_t j = 0; j < 3; j++) if (j != i) buttons[j].confidence = 0;
        Serial.print(F("Mode: ")); Serial.println(currentMode);
      }
    }
    if (currentState == LOW) buttons[i].actionTaken = false;
  }
}

unsigned long lastSerialDebug = 0;
void printDebug() {
  unsigned long now = millis();
  if (now - lastSerialDebug < 500) return;
  lastSerialDebug = now;

  const char *modeNames[] = {"IDLE", "NORM", "BURU"};
  bool isClosed = (servoPos == SERVO_CLOSE);
  bool irState = digitalRead(IR_SENSOR_PIN);

  Serial.print(F("M:")); Serial.print(modeNames[currentMode - 1]);
  Serial.print(F(" | RPM:")); Serial.print(currentRPM);
  Serial.print(F(" | IR:")); Serial.print(irState ? F("H") : F("L"));
  Serial.print(F(" | dB:")); Serial.print(currentDB, 1);
  Serial.print(F(" | S:")); Serial.println(isClosed ? F("CLOSE") : F("OPEN"));
}

void updateLCD() {
  unsigned long now = millis();
  if (now - lastLCDUpdate < LCD_INTERVAL) return;
  lastLCDUpdate = now;

  lcd.setCursor(0, 0);
  const char *modeNames[] = {"IDLE", "NORM", "BURU"};
  char row0[17];
  snprintf(row0, sizeof(row0), "%-4s R:%-4u P:%-2u", modeNames[currentMode - 1], currentRPM, lastPulses);
  lcd.print(row0);

  lcd.setCursor(0, 1);
  char dbBuf[6];
  dtostrf(currentDB, 4, 1, dbBuf);
  bool isClosed = (servoPos == SERVO_CLOSE);
  bool irState = digitalRead(IR_SENSOR_PIN);
  char row1[17];
  snprintf(row1, sizeof(row1), "%sdB %s IR:%c", dbBuf, isClosed ? "[C]" : "[O]", irState ? 'H' : 'L');
  lcd.print(row1);
}

void setup() {
  Serial.begin(115200);
  katupServo.attach(SERVO_PIN);
  servoPos = -1;
  servoOpen();

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("SmartValve v2.3");
  lcd.setCursor(0, 1);
  lcd.print("Smooth RPM Mode");
  delay(1500);
  lcd.clear();

  analogReference(DEFAULT);
  pinMode(BUTTON_MODE1_PIN, INPUT);
  pinMode(BUTTON_MODE2_PIN, INPUT);
  pinMode(BUTTON_MODE3_PIN, INPUT);
  pinMode(IR_SENSOR_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(IR_SENSOR_PIN), countPulse, FALLING);

  unsigned long t = millis();
  lastRPMCalcTime = t;
  lastLCDUpdate = t;
  soundWindowStart = t;
}

void loop() {
  sampleSound();
  checkButtons();
  calculateRPM();
  processMode();
  updateLCD();
  printDebug();
}
