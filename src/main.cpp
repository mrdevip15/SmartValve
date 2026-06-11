/**
 * ============================================================
 *  SISTEM KONTROL KATUP OTOMATIS  (v2.1 - revisi)
 *  Board  : Arduino Uno / Nano
 *  Sensor : KY-037 (AO), IR RPM (interrupt), Servo, LCD I2C
 * ============================================================
 *
 *  LOGIKA TOMBOL (HANYA SOFTWARE FIX):
 *  Karena kabel disolder ke 3.3V (Active HIGH) dan tidak ada resistor
 *  pull-down eksternal, pin menjadi "FLOATING" (ngaco).
 *  Kodingan ini menggunakan "Confidence Counter" untuk memfilter
 *  noise listrik agar mode tidak pindah-pindah sendiri.
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
#define RPM_PPR 1 // Pulses Per Revolution (Ganti jika pakai piringan berlubang banyak)

// ==================== TIMING ====================
#define RPM_CALC_INTERVAL 1000UL
#define RPM_TIMEOUT_MS 10000UL
#define LCD_INTERVAL 500UL
#define MODE1_CLOSE_HOLD 1000UL
#define CYCLE_DURATION 60000UL
#define DEBOUNCE_DELAY 150UL

// ==================== SOUND SAMPLING ====================
#define SOUND_WINDOW_MS 150UL
#define P2P_AVG_COUNT 4

// ==================== OBJEK ====================
LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo katupServo;

enum SystemMode : uint8_t { MODE_IDLE = 1, MODE_NORMAL = 2, MODE_BURU = 3 };
// ==================== STATE GLOBAL ====================
volatile uint16_t pulseCount = 0;
volatile unsigned long lastPulseTime = 0; // Untuk debouncing
#define RPM_DEBOUNCE_MS 2 // Abaikan pulsa jika jarak < 2ms

uint16_t lastPulses = 0; // Simpan jumlah pulsa terakhir untuk debug
unsigned long lastRPMCalcTime = 0;
uint16_t currentRPM = 0;

float currentDB = 0.0f;
// ... (rest of state)
uint16_t lastPeakToPeak = 0;
uint16_t soundMax = 0;
uint16_t soundMin = 1023;
unsigned long soundWindowStart = 0;
uint16_t p2pBuf[P2P_AVG_COUNT] = {0};
uint8_t p2pIdx = 0;

SystemMode currentMode = MODE_IDLE;
// ... (rest of countPulse)
void countPulse() {
  unsigned long now = millis();
  if (now - lastPulseTime > RPM_DEBOUNCE_MS) {
    pulseCount++;
    lastPulseTime = now;
  }
}

unsigned long cycleStartTime = 0;
bool isCycleActive = false;
bool isClosingPhase = false;
unsigned long mode1CloseStart = 0;
bool mode1HoldActive = false;
unsigned long rpmZeroStart = 0;
unsigned long lastLCDUpdate = 0;
int servoPos = SERVO_OPEN;

// --- STRUKTUR TOMBOL BARU DENGAN FILTER KUAT ---
struct ButtonState {
  uint8_t pin;
  bool lastReading;
  bool stableState;
  unsigned long lastDebounce;
  bool actionTaken;
  int16_t confidence; // Filter noise
};

ButtonState buttons[3] = {
    {BUTTON_MODE1_PIN, LOW, LOW, 0, false, 0},
    {BUTTON_MODE2_PIN, LOW, LOW, 0, false, 0},
    {BUTTON_MODE3_PIN, LOW, LOW, 0, false, 0},
};

void countPulse() { pulseCount++; }

void sampleSound() {
  uint16_t s = analogRead(SOUND_PIN);
  if (s > soundMax)
    soundMax = s;
  if (s < soundMin)
    soundMin = s;

  unsigned long now = millis();
  if (now - soundWindowStart < SOUND_WINDOW_MS)
    return;
  soundWindowStart = now;

  uint16_t p2p = (soundMax >= soundMin) ? (soundMax - soundMin) : 0;
  soundMax = 0;
  soundMin = 1023;

  p2pBuf[p2pIdx] = p2p;
  p2pIdx = (p2pIdx + 1) % P2P_AVG_COUNT;

  uint32_t sum = 0;
  for (uint8_t i = 0; i < P2P_AVG_COUNT; i++)
    sum += p2pBuf[i];
  uint16_t p2pAvg = (uint16_t)(sum / P2P_AVG_COUNT);
  lastPeakToPeak = p2pAvg;

  float pp = (p2pAvg < 1) ? 1.0f : (float)p2pAvg;
  float db = DB_REF + DB_SCALE * log10f(pp / P2P_REF);

  if (db < DB_MIN)
    db = DB_MIN;
  if (db > DB_MAX)
    db = DB_MAX;
  currentDB = db;
}

void calculateRPM() {
  unsigned long now = millis();
  if (now - lastRPMCalcTime < RPM_CALC_INTERVAL)
    return;
  noInterrupts();
  uint16_t pulses = pulseCount;
  pulseCount = 0;
  interrupts();
  lastPulses = pulses;
  unsigned long delta = now - lastRPMCalcTime;
  // RPM = (pulses / PPR) * (60000 / delta_ms)
  currentRPM = (pulses > 0) ? (uint16_t)(((uint32_t)pulses * 60000UL) / (delta * RPM_PPR)) : 0;
  lastRPMCalcTime = now;
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
  rpmZeroStart = 0;

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
      if (isCycleActive)
        resetCycle();
      else
        servoOpen();
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
      if (isCycleActive)
        resetCycle();
      else
        servoOpen();
    }
    break;
  }

  if (isCycleActive && (now - cycleStartTime >= CYCLE_DURATION)) {
    isClosingPhase = !isClosingPhase;
    cycleStartTime = now;
    if (isClosingPhase)
      servoClose();
    else
      servoOpen();
  }
}

// ============================================================
//  Fungsi Filter Tombol Noisy (Floating Fix)
// ============================================================
void checkButtons() {
  for (uint8_t i = 0; i < 3; i++) {
    bool reading = digitalRead(buttons[i].pin);

    if (reading == HIGH) {
      if (buttons[i].confidence < 8)
        buttons[i].confidence++;
    } else {
      if (buttons[i].confidence > 0)
        buttons[i].confidence--;
    }

    bool currentState = buttons[i].stableState;
    if (buttons[i].confidence > 5)
      currentState = HIGH;
    else if (buttons[i].confidence < 2)
      currentState = LOW;

    if (currentState != buttons[i].stableState) {
      buttons[i].stableState = currentState;

      if (currentState == HIGH && !buttons[i].actionTaken &&
          (millis() - lastModeChange) > 800UL) {
        buttons[i].actionTaken = true;
        lastModeChange = millis();
        currentMode = (SystemMode)(i + 1);
        mode1HoldActive = false;
        rpmZeroStart = 0;
        resetCycle();
        for (uint8_t j = 0; j < 3; j++)
          if (j != i)
            buttons[j].confidence = 0;
        Serial.print(F("Mode changed to: "));
        Serial.println(currentMode);
      }
    }

    if (currentState == LOW) {
      buttons[i].actionTaken = false;
    }
  }
}

unsigned long lastSerialDebug = 0;
#define SERIAL_DEBUG_INTERVAL 500UL

void printDebug() {
  unsigned long now = millis();
  if (now - lastSerialDebug < SERIAL_DEBUG_INTERVAL)
    return;
  lastSerialDebug = now;

  const char *modeNames[] = {"IDLE", "NORM", "BURU"};
  bool isClosed = (servoPos == SERVO_CLOSE);
  bool irState = digitalRead(IR_SENSOR_PIN);

  Serial.print(F("MODE:"));
  Serial.print(modeNames[currentMode - 1]);
  Serial.print(F(" | RPM:"));
  Serial.print(currentRPM);
  Serial.print(F(" ("));
  Serial.print(lastPulses);
  Serial.print(F("p)"));
  Serial.print(F(" | IR:"));
  Serial.print(irState ? F("HIGH") : F("LOW "));
  Serial.print(F(" | dB:"));
  Serial.print(currentDB, 1);
  Serial.print(F(" | P2P:"));
  Serial.print(lastPeakToPeak);
  Serial.print(F(" | SERVO:"));
  Serial.print(isClosed ? F("CLOSE") : F("OPEN "));
  Serial.print(F(" | CYCLE:"));
  Serial.print(isCycleActive ? F("ON ") : F("OFF"));
  Serial.print(F(" | BTN_CONF:"));
  Serial.print(buttons[0].confidence);
  Serial.print(F("/"));
  Serial.print(buttons[1].confidence);
  Serial.print(F("/"));
  Serial.println(buttons[2].confidence);
}

void updateLCD() {
  unsigned long now = millis();
  if (now - lastLCDUpdate < LCD_INTERVAL)
    return;
  lastLCDUpdate = now;

  // Baris 0: "IDLE R:4500 P:75" (16 char)
  lcd.setCursor(0, 0);
  const char *modeNames[] = {"IDLE", "NORM", "BURU"};
  char row0[17];
  snprintf(row0, sizeof(row0), "%-4s R:%-4u P:%-2u", 
           modeNames[currentMode - 1], currentRPM, lastPulses);
  lcd.print(row0);

  // Baris 1: "75.2dB [C] IR:H" (16 char)
  lcd.setCursor(0, 1);
  char dbBuf[6];
  dtostrf(currentDB, 4, 1, dbBuf);
  bool isClosed = (servoPos == SERVO_CLOSE);
  bool irState = digitalRead(IR_SENSOR_PIN);
  char row1[17];
  snprintf(row1, sizeof(row1), "%sdB %s IR:%c", 
           dbBuf, isClosed ? "[C]" : "[O]", irState ? 'H' : 'L');
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
  lcd.print("SmartValve v2.2");
  lcd.setCursor(0, 1);
  lcd.print("Calib MAX4466");
  delay(1500);
  lcd.clear();

  analogReference(DEFAULT); // MAX4466 butuh 5V ref agar tidak saturasi
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
