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
#define SERVO_CLOSE 90

// ==================== KALIBRASI dB ====================
#define DB_REF 75.0f 
#define V_REF 2.5f   
#define VCC 5.0f     
#define ADC_MAX 1023.0f
#define DB_MIN 30.0f
#define DB_MAX 120.0f

// ==================== THRESHOLD ====================
#define SOUND_THRESHOLD_DB 75.0f 
#define RPM_THRESHOLD 4000

// ==================== TIMING ====================
#define RPM_CALC_INTERVAL 1000UL 
#define RPM_TIMEOUT_MS 10000UL   
#define LCD_INTERVAL 500UL       
#define MODE1_CLOSE_HOLD 1000UL  
#define CYCLE_DURATION 60000UL 
#define DEBOUNCE_DELAY 150UL   // Dinaikkan untuk meredam noise

// ==================== SOUND SAMPLING ====================
#define SOUND_WINDOW_MS 50UL 
#define P2P_AVG_COUNT 8 

// ==================== OBJEK ====================
LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo katupServo;

enum SystemMode : uint8_t { MODE_IDLE = 1, MODE_NORMAL = 2, MODE_BURU = 3 };

// ==================== STATE GLOBAL ====================
volatile uint16_t pulseCount = 0;
unsigned long lastRPMCalcTime = 0;
uint16_t currentRPM = 0;

float currentDB = 0.0f;
uint16_t lastPeakToPeak = 0;
uint16_t soundMax = 0;
uint16_t soundMin = 1023;
unsigned long soundWindowStart = 0;
uint16_t p2pBuf[P2P_AVG_COUNT] = {0};
uint8_t p2pIdx = 0;

SystemMode currentMode = MODE_IDLE;
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
  float voltage = (pp / ADC_MAX) * VCC;
  float db = DB_REF + 20.0f * log10f(voltage / V_REF);

  if (db < DB_MIN) db = DB_MIN;
  if (db > DB_MAX) db = DB_MAX;
  currentDB = db;
}

void calculateRPM() {
  unsigned long now = millis();
  if (now - lastRPMCalcTime < RPM_CALC_INTERVAL) return;
  noInterrupts();
  uint16_t pulses = pulseCount;
  pulseCount = 0;
  interrupts();
  unsigned long delta = now - lastRPMCalcTime;
  currentRPM = (pulses > 0) ? (uint16_t)((pulses * 60000UL) / delta) : 0;
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
  if (currentMode != MODE_IDLE) {
    if (currentRPM == 0) {
      if (rpmZeroStart == 0) rpmZeroStart = now;
      else if (now - rpmZeroStart > RPM_TIMEOUT_MS) {
        currentMode = MODE_IDLE;
        mode1HoldActive = false;
        rpmZeroStart = 0;
        resetCycle();
        return;
      }
    } else { rpmZeroStart = 0; }
  }

  bool soundTrigger = (currentDB > SOUND_THRESHOLD_DB);
  bool rpmTrigger = (currentRPM > RPM_THRESHOLD);

  switch (currentMode) {
    case MODE_IDLE:
      isCycleActive = false;
      if (soundTrigger) {
        if (!mode1HoldActive) { servoClose(); mode1HoldActive = true; }
        mode1CloseStart = now;
      } else {
        if (mode1HoldActive) {
          if (now - mode1CloseStart >= MODE1_CLOSE_HOLD) { servoOpen(); mode1HoldActive = false; }
        } else { servoOpen(); }
      }
      break;

    case MODE_NORMAL:
      if (soundTrigger && rpmTrigger) {
        if (!isCycleActive) { isCycleActive = true; isClosingPhase = true; cycleStartTime = now; servoClose(); }
      } else { if (isCycleActive) resetCycle(); else servoOpen(); }
      break;

    case MODE_BURU:
      if (!rpmTrigger) {
        if (!isCycleActive) { isCycleActive = true; isClosingPhase = true; cycleStartTime = now; servoClose(); }
      } else { if (isCycleActive) resetCycle(); else servoOpen(); }
      break;
  }

  if (isCycleActive && (now - cycleStartTime >= CYCLE_DURATION)) {
    isClosingPhase = !isClosingPhase;
    cycleStartTime = now;
    if (isClosingPhase) servoClose(); else servoOpen();
  }
}

// ============================================================
//  Fungsi Filter Tombol Noisy (Floating Fix)
// ============================================================
void checkButtons() {
  for (uint8_t i = 0; i < 3; i++) {
    bool reading = digitalRead(buttons[i].pin);

    // Confidence Integrator: 
    // Jika bacaan HIGH, naikkan score. Jika LOW, turunkan score.
    // Ini akan membuang noise "percikan" yang hanya muncul sekejap.
    if (reading == HIGH) {
      if (buttons[i].confidence < 150) buttons[i].confidence++;
    } else {
      if (buttons[i].confidence > 0) buttons[i].confidence--;
    }

    // Tentukan status stabil berdasarkan score (Hysteresis)
    bool currentState = buttons[i].stableState;
    if (buttons[i].confidence > 120) currentState = HIGH;
    else if (buttons[i].confidence < 30) currentState = LOW;

    if (currentState != buttons[i].stableState) {
      buttons[i].stableState = currentState;
      
      // Aksi saat transisi ke HIGH (Ditekan)
      if (currentState == HIGH && !buttons[i].actionTaken) {
        buttons[i].actionTaken = true;
        currentMode = (SystemMode)(i + 1);
        mode1HoldActive = false;
        rpmZeroStart = 0;
        resetCycle();
        Serial.print(F("Mode changed to: ")); Serial.println(currentMode);
      }
    }

    if (currentState == LOW) {
      buttons[i].actionTaken = false;
    }
  }
}

void updateLCD() {
  unsigned long now = millis();
  if (now - lastLCDUpdate < LCD_INTERVAL) return;
  lastLCDUpdate = now;

  lcd.setCursor(0, 0);
  lcd.print("M:"); lcd.print(currentMode);
  lcd.print(" B:");
  // Debug tampilan B tetap menunjukkan status stabil hasil filter
  lcd.print(buttons[0].stableState ? "H" : "L");
  lcd.print(buttons[1].stableState ? "H" : "L");
  lcd.print(buttons[2].stableState ? "H" : "L");
  lcd.print("      ");

  lcd.setCursor(0, 1);
  char dbBuf[7];
  dtostrf(currentDB, 4, 1, dbBuf);
  char lcdLine1[17];
  snprintf(lcdLine1, sizeof(lcdLine1), "P2P:%-4u %sdB", lastPeakToPeak, dbBuf);
  lcd.print(lcdLine1);
}

void setup() {
  Serial.begin(115200);
  katupServo.attach(SERVO_PIN);
  servoPos = -1;
  servoOpen();

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Sistem Katup");
  lcd.setCursor(0, 1);
  lcd.print("v2.1 FixNoise");
  delay(1500);
  lcd.clear();

  // Tetap INPUT biasa karena hardware terhubung ke 3.3V
  pinMode(BUTTON_MODE1_PIN, INPUT);
  pinMode(BUTTON_MODE2_PIN, INPUT);
  pinMode(BUTTON_MODE3_PIN, INPUT);

  pinMode(IR_SENSOR_PIN, INPUT);
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
}
