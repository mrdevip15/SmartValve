/**
 * ============================================================
 *  SISTEM KONTROL KATUP OTOMATIS
 *  Board  : Arduino Uno / Nano
 *  Sensor : KY-037 (AO), IR RPM (interrupt), Servo, LCD I2C
 * ============================================================
 *
 *  KALIBRASI dB
 *  ─────────────
 *  KY-037 tidak memiliki kalibrasi factory. Formula yang digunakan:
 *
 *    dB = DB_REF + 20 * log10(voltage / V_REF)
 *
 *  DB_REF  : nilai dB yang kamu ukur dengan sound meter HP/alat
 *            saat sensor membaca tegangan V_REF
 *  V_REF   : tegangan AO pada kondisi DB_REF (dalam volt)
 *
 *  Cara kalibrasi cepat:
 *    1. Buka app "Sound Meter" di HP
 *    2. Letakkan HP dan KY-037 berdampingan
 *    3. Buat suara konstan (misal putar tone 1kHz dari speaker)
 *    4. Catat nilai dB dari HP → isi ke DB_REF
 *    5. Catat nilai ADC dari Serial Monitor → hitung volt = (adc/1023.0)*5.0
 *       → isi ke V_REF
 * ============================================================
 */

#include <Arduino.h>
#include <LiquidCrystal_I2C.h>
#include <Servo.h>
#include <math.h>

// ==================== PIN ====================
#define SERVO_PIN 9
#define IR_SENSOR_PIN 2 // harus pin interrupt (Uno: 2 atau 3)
#define SOUND_PIN A0
#define BUTTON_MODE1_PIN 4
#define BUTTON_MODE2_PIN 5
#define BUTTON_MODE3_PIN 6

// ==================== SERVO ====================
#define SERVO_OPEN 0
#define SERVO_CLOSE 90

// ==================== KALIBRASI dB ====================
// Ubah dua konstanta ini setelah melakukan kalibrasi lapangan.
// Default awal: asumsi sensor pada ADC=512 ≈ 60 dB (tipikal KY-037 medium gain)
#define DB_REF 75.0f // dB referensi hasil ukur sound meter
#define V_REF 2.5f   // tegangan AO (volt) saat DB_REF terukur
#define VCC 5.0f     // tegangan supply Arduino
#define ADC_MAX 1023.0f

// Batas clamp output dB (hindari -inf / nilai aneh saat sunyi total)
#define DB_MIN 30.0f
#define DB_MAX 120.0f

// ==================== THRESHOLD ====================
#define SOUND_THRESHOLD_DB                                                     \
  75.0f // dB — setara ±percakapan keras / kebisingan kendaraan
#define RPM_THRESHOLD 4000

// ==================== TIMING ====================
#define RPM_CALC_INTERVAL 1000UL // ms — hitung RPM tiap 1 detik
#define RPM_TIMEOUT_MS 10000UL   // ms — reset ke IDLE jika RPM=0 selama ini
#define LCD_INTERVAL 500UL       // ms — refresh LCD
#define MODE1_CLOSE_HOLD                                                       \
  1000UL                       // ms — jeda sebelum servo buka kembali di Mode 1
#define CYCLE_DURATION 60000UL // ms — periode siklus buka/tutup Mode 2 & 3
#define DEBOUNCE_DELAY 50UL    // ms

// ==================== SOUND SAMPLING ====================
// Peak-to-peak amplitude: butuh cukup sampel untuk capture 1 siklus penuh
// analogRead ~100µs/sample → 500 sampel ≈ 50ms (cukup untuk 20Hz)
#define SOUND_SAMPLES 500
#define P2P_AVG_COUNT 8  // rata-rata N kali P2P untuk stabilisasi

// ==================== OBJEK ====================
LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo katupServo;

// ==================== ENUM MODE ====================
enum SystemMode : uint8_t { MODE_IDLE = 1, MODE_NORMAL = 2, MODE_BURU = 3 };

// ==================== STATE GLOBAL ====================
// RPM
volatile uint16_t pulseCount = 0;
unsigned long lastRPMCalcTime = 0;
uint16_t currentRPM = 0;
unsigned long lastRPMUpdate = 0;

// Sound
float currentDB = 0.0f;
uint16_t lastPeakToPeak = 0; // untuk kalibrasi LCD

// Mode
SystemMode currentMode = MODE_IDLE;

// Siklus (Mode 2 & 3)
unsigned long cycleStartTime = 0;
bool isCycleActive = false;
bool isClosingPhase = false;

// Mode 1 close hold
unsigned long mode1CloseStart = 0;
bool mode1HoldActive = false;

// RPM timeout
unsigned long rpmZeroStart = 0;

// LCD
unsigned long lastLCDUpdate = 0;

// Debounce
struct ButtonState {
  uint8_t pin;
  bool lastReading;
  bool stableState;
  unsigned long lastDebounce;
  bool actionTaken; // cegah double-trigger selama tombol held
};

ButtonState buttons[3] = {
    {BUTTON_MODE1_PIN, HIGH, HIGH, 0, false},
    {BUTTON_MODE2_PIN, HIGH, HIGH, 0, false},
    {BUTTON_MODE3_PIN, HIGH, HIGH, 0, false},
};

// ============================================================
//  ISR — Hitung pulsa RPM
// ============================================================
void countPulse() { pulseCount++; }

// ============================================================
//  Baca suara → desibel
//  Menggunakan rata-rata N sampel untuk reduksi noise
// ============================================================
float readSoundDB() {
  uint32_t p2pSum = 0;
  for (uint8_t r = 0; r < P2P_AVG_COUNT; r++) {
    uint16_t maxVal = 0, minVal = 1023;
    for (uint16_t i = 0; i < SOUND_SAMPLES; i++) {
      uint16_t sample = analogRead(SOUND_PIN);
      if (sample > maxVal) maxVal = sample;
      if (sample < minVal) minVal = sample;
    }
    p2pSum += (maxVal - minVal);
  }

  uint16_t peakToPeak = (uint16_t)(p2pSum / P2P_AVG_COUNT);
  lastPeakToPeak = peakToPeak;

  // Clamp agar tidak log(0)
  if (peakToPeak < 1) peakToPeak = 1;

  float voltage = (peakToPeak / ADC_MAX) * VCC;

  // SPL formula: dB = DB_REF + 20*log10(V_pp / V_REF)
  // V_REF di sini = tegangan peak-to-peak saat kondisi DB_REF
  float db = DB_REF + 20.0f * log10f(voltage / V_REF);

  // Clamp ke rentang masuk akal
  if (db < DB_MIN)
    db = DB_MIN;
  if (db > DB_MAX)
    db = DB_MAX;

  return db;
}

// ============================================================
//  Hitung RPM (non-blocking, interval 1 detik)
// ============================================================
void calculateRPM() {
  unsigned long now = millis();
  if (now - lastRPMCalcTime < RPM_CALC_INTERVAL)
    return;

  noInterrupts();
  uint16_t pulses = pulseCount;
  pulseCount = 0;
  interrupts();

  unsigned long delta = now - lastRPMCalcTime;
  currentRPM = (pulses > 0) ? (uint16_t)((pulses * 60000UL) / delta) : 0;

  lastRPMCalcTime = now;
  lastRPMUpdate = now;
}

// ============================================================
//  Servo helper
// ============================================================
inline void servoOpen() { katupServo.write(SERVO_OPEN); }
inline void servoClose() { katupServo.write(SERVO_CLOSE); }

// ============================================================
//  Reset siklus
// ============================================================
void resetCycle() {
  isCycleActive = false;
  isClosingPhase = false;
  servoOpen();
}

// ============================================================
//  Proses logika mode (state machine)
// ============================================================
void processMode() {
  unsigned long now = millis();

  // --- Auto-reset ke IDLE jika RPM = 0 terlalu lama ---
  if (currentRPM == 0) {
    if (rpmZeroStart == 0) {
      rpmZeroStart = now;
    } else if (now - rpmZeroStart > RPM_TIMEOUT_MS) {
      currentMode = MODE_IDLE;
      mode1HoldActive = false;
      rpmZeroStart = 0;
      resetCycle();
      return;
    }
  } else {
    rpmZeroStart = 0;
  }

  bool soundTrigger = (currentDB > SOUND_THRESHOLD_DB);
  bool rpmTrigger = (currentRPM > RPM_THRESHOLD);

  switch (currentMode) {

  // ── MODE 1: IDLE ──────────────────────────────────────────
  case MODE_IDLE:
    isCycleActive = false;

    if (soundTrigger) {
      // Aktifkan close hold jika belum aktif
      if (!mode1HoldActive) {
        servoClose();
        mode1HoldActive = true;
        mode1CloseStart = now;
      }
      // Selama suara masih keras → perpanjang hold
      else {
        mode1CloseStart = now;
      }
    } else {
      if (mode1HoldActive) {
        // Tunggu hold 1 detik habis setelah suara reda
        if (now - mode1CloseStart >= MODE1_CLOSE_HOLD) {
          servoOpen();
          mode1HoldActive = false;
        }
        // else: servo tetap close, tunggu
      } else {
        servoOpen();
      }
    }
    break;

  // ── MODE 2: NORMAL ────────────────────────────────────────
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

  // ── MODE 3: BURU ──────────────────────────────────────────
  case MODE_BURU:
    if (!rpmTrigger) { // RPM di bawah threshold → aktifkan
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

  // --- Proses siklus 1 menit (Mode 2 & 3) ---
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
//  Update LCD (non-blocking)
// ============================================================
void updateLCD() {
  unsigned long now = millis();
  if (now - lastLCDUpdate < LCD_INTERVAL)
    return;
  lastLCDUpdate = now;

  bool closed = (katupServo.read() >= SERVO_CLOSE - 5);

  // Baris 0: Mode + status tombol (DEBUG)
  lcd.setCursor(0, 0);
  lcd.print("M:");
  lcd.print(currentMode);
  lcd.print(" B:");
  lcd.print(digitalRead(BUTTON_MODE1_PIN) ? "H" : "L");
  lcd.print(digitalRead(BUTTON_MODE2_PIN) ? "H" : "L");
  lcd.print(digitalRead(BUTTON_MODE3_PIN) ? "H" : "L");
  lcd.print("      ");

  // Baris 1: P2P (kalibrasi) + dB
  lcd.setCursor(0, 1);
  char dbBuf[6];
  dtostrf(currentDB, 4, 1, dbBuf);
  char lcdLine1[17];
  snprintf(lcdLine1, sizeof(lcdLine1), "P2P:%-4u %sdB", lastPeakToPeak, dbBuf);
  lcd.print(lcdLine1);
}

// ============================================================
//  Baca tombol — debounce proper (non-blocking, tanpa while)
//  Aksi hanya pada tepi naik TERVERIFIKASI (LOW → HIGH)
//
//  Fix bug double-trigger:
//    stableState diupdate DULU, baru cek perubahan tepi.
//    Flag actionTaken mencegah aksi dipanggil lebih dari sekali
//    selama tombol masih ditekan.
// ============================================================
void checkButtons() {
  unsigned long now = millis();

  for (uint8_t i = 0; i < 3; i++) {
    bool reading = digitalRead(buttons[i].pin);

    // Reset timer debounce setiap ada perubahan sinyal mentah
    if (reading != buttons[i].lastReading) {
      buttons[i].lastDebounce = now;
    }
    buttons[i].lastReading = reading; // simpan di akhir setiap iterasi

    // Belum melewati window debounce → skip
    if (now - buttons[i].lastDebounce <= DEBOUNCE_DELAY)
      continue;

    // Sinyal sudah stabil — update stableState DULU
    bool prevStable = buttons[i].stableState;
    buttons[i].stableState = reading;

    // Tepi turun terverifikasi: stable berubah HIGH → LOW (active LOW)
    // actionTaken mencegah aksi berulang selama tombol held
    if (prevStable == HIGH && reading == LOW && !buttons[i].actionTaken) {
      buttons[i].actionTaken = true;

      SystemMode newMode =
          (SystemMode)(i + 1); // index 0→MODE1, 1→MODE2, 2→MODE3
      currentMode = newMode;   // set langsung, termasuk mode yang sama
      mode1HoldActive = false;
      rpmZeroStart = 0;
      resetCycle();

      Serial.print(F("[BTN] Mode -> "));
      Serial.println(currentMode);
    }

    // Reset flag saat tombol dilepas (pin kembali HIGH)
    if (reading == HIGH) {
      buttons[i].actionTaken = false;
    }
  }
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);

  // Servo
  katupServo.attach(SERVO_PIN);
  servoOpen();

  // LCD
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Sistem Katup");
  lcd.setCursor(0, 1);
  lcd.print("Otomatis v2.0");
  delay(1500);
  lcd.clear();

  // Tombol — INPUT_PULLUP: pin HIGH saat lepas, LOW saat ditekan (active LOW)
  pinMode(BUTTON_MODE1_PIN, INPUT_PULLUP);
  pinMode(BUTTON_MODE2_PIN, INPUT_PULLUP);
  pinMode(BUTTON_MODE3_PIN, INPUT_PULLUP);

  // IR Sensor RPM
  pinMode(IR_SENSOR_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(IR_SENSOR_PIN), countPulse, FALLING);

  // Init timestamp
  lastRPMCalcTime = millis();
  lastLCDUpdate = millis();

  Serial.println(F("=== Sistem Katup Otomatis v2.0 ==="));
  Serial.println(F("Format: Mode | RPM | dB | Servo"));
}

// ============================================================
//  LOOP UTAMA
// ============================================================
void loop() {
  currentDB = readSoundDB();

  checkButtons();
  calculateRPM();
  processMode();
  updateLCD();

  // Debug Serial (interval sama dengan LCD agar tidak flood)
  static unsigned long lastSerial = 0;
  if (millis() - lastSerial >= LCD_INTERVAL) {
    lastSerial = millis();
    Serial.print(F("Mode:"));
    Serial.print(currentMode);
    Serial.print(F(" RPM:"));
    Serial.print(currentRPM);
    Serial.print(F(" dB:"));
    Serial.print(currentDB, 1);
    Serial.print(F(" Servo:"));
    Serial.println(katupServo.read());
  }
}
