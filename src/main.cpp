/**
 * ============================================================
 *  SISTEM KONTROL KATUP OTOMATIS  (v2.1 - revisi)
 *  Board  : Arduino Uno / Nano
 *  Sensor : KY-037 (AO), IR RPM (interrupt), Servo, LCD I2C
 * ============================================================
 *
 *  PERUBAHAN v2.1
 *  ──────────────
 *  1. Pembacaan suara TIDAK lagi blocking. Dulu readSoundDB()
 *     menahan loop ~400ms (500 sampel x 8) sehingga tombol sering
 *     telat / terlewat dan servo lambat. Sekarang sampling
 *     streaming: 1 analogRead per loop, peak-to-peak dihitung per
 *     window 50ms lalu di-moving-average 8 window (~400ms, sama
 *     seperti sebelumnya). Loop kembali cepat -> tombol responsif.
 *  2. Timeout RPM (auto ke IDLE) tidak lagi mengganggu logika suara
 *     saat mode SUDAH di IDLE (dulu katup bisa "kedip" tiap 10 detik
 *     saat suara menahan katup tertutup sementara RPM = 0).
 *  3. Prosedur kalibrasi diperbaiki: V_REF = tegangan PEAK-TO-PEAK
 *     pada level DB_REF (bukan tegangan diam ADC=512).
 *  4. Servo hanya ditulis saat posisi berubah (kurangi jitter),
 *     variabel mati dibuang, buffer string dB diperlebar.
 *
 *  KALIBRASI dB
 *  ─────────────
 *  KY-037 tidak punya kalibrasi pabrik. Formula:
 *
 *    dB = DB_REF + 20 * log10(V_pp / V_REF)
 *
 *  V_pp    : tegangan peak-to-peak hasil sampling (otomatis)
 *  DB_REF  : nilai dB yang Anda ukur dgn sound meter saat
 *            sensor membaca V_pp = V_REF
 *  V_REF   : tegangan peak-to-peak (volt) pada kondisi DB_REF
 *
 *  Cara kalibrasi:
 *    1. Buka app "Sound Meter" di HP, letakkan berdampingan dgn KY-037.
 *    2. Bunyikan suara konstan (mis. tone 1kHz dari speaker).
 *    3. Catat nilai dB dari HP -> isi ke DB_REF.
 *    4. Lihat nilai "P2P:" di LCD pada kondisi yang sama.
 *       Hitung: V_REF = (P2P / 1023.0) * 5.0  -> isi ke V_REF.
 *    (Atur potensiometer KY-037 agar P2P tidak mentok 0 / 1023.)
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
// Ubah dua konstanta ini setelah kalibrasi lapangan (lihat header).
#define DB_REF 75.0f // dB referensi hasil ukur sound meter
#define V_REF 2.5f   // tegangan PEAK-TO-PEAK (volt) saat DB_REF terukur
#define VCC 5.0f     // tegangan supply Arduino
#define ADC_MAX 1023.0f

// Batas clamp output dB (hindari -inf saat sunyi total)
#define DB_MIN 30.0f
#define DB_MAX 120.0f

// ==================== THRESHOLD ====================
#define SOUND_THRESHOLD_DB 75.0f // dB — setara percakapan keras/kebisingan
#define RPM_THRESHOLD 4000

// ==================== TIMING ====================
#define RPM_CALC_INTERVAL 1000UL // ms — hitung RPM tiap 1 detik
#define RPM_TIMEOUT_MS 10000UL   // ms — reset ke IDLE jika RPM=0 selama ini
#define LCD_INTERVAL 500UL       // ms — refresh LCD
#define MODE1_CLOSE_HOLD                                                       \
  1000UL                       // ms — jeda sebelum servo buka kembali di Mode 1
#define CYCLE_DURATION 60000UL // ms — periode siklus buka/tutup Mode 2 & 3
#define DEBOUNCE_DELAY 100UL   // ms — filter floating pin

// ==================== SOUND SAMPLING (streaming, non-blocking)
// ====================
#define SOUND_WINDOW_MS 50UL // panjang window deteksi peak-to-peak
#define P2P_AVG_COUNT 8 // jumlah window untuk moving-average (8 x 50ms = 400ms)

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

// Sound (streaming sampler)
float currentDB = 0.0f;
uint16_t lastPeakToPeak = 0;
uint16_t soundMax = 0;
uint16_t soundMin = 1023;
unsigned long soundWindowStart = 0;
uint16_t p2pBuf[P2P_AVG_COUNT] = {0};
uint8_t p2pIdx = 0;

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

// Posisi servo (cache, agar tidak menulis berulang)
int servoPos = SERVO_OPEN;

// Debounce
struct ButtonState {
  uint8_t pin;
  bool lastReading;
  bool stableState;
  unsigned long lastDebounce;
  bool actionTaken; // cegah double-trigger selama tombol held
};

ButtonState buttons[3] = {
    {BUTTON_MODE1_PIN, LOW, LOW, 0, false},
    {BUTTON_MODE2_PIN, LOW, LOW, 0, false},
    {BUTTON_MODE3_PIN, LOW, LOW, 0, false},
};

// ============================================================
//  ISR — Hitung pulsa RPM
// ============================================================
void countPulse() { pulseCount++; }

// ============================================================
//  Sampling suara — NON-BLOCKING (streaming peak detector)
//  Dipanggil tiap loop. Satu analogRead per panggilan; min/max
//  diakumulasi sepanjang window SOUND_WINDOW_MS. Saat window penuh:
//  hitung peak-to-peak -> moving average -> konversi ke dB.
// ============================================================
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

  // Moving average peak-to-peak (ring buffer)
  p2pBuf[p2pIdx] = p2p;
  p2pIdx = (p2pIdx + 1) % P2P_AVG_COUNT;

  uint32_t sum = 0;
  for (uint8_t i = 0; i < P2P_AVG_COUNT; i++)
    sum += p2pBuf[i];
  uint16_t p2pAvg = (uint16_t)(sum / P2P_AVG_COUNT);
  lastPeakToPeak = p2pAvg;

  float pp = (p2pAvg < 1) ? 1.0f : (float)p2pAvg; // hindari log(0)
  float voltage = (pp / ADC_MAX) * VCC;
  float db = DB_REF + 20.0f * log10f(voltage / V_REF);

  if (db < DB_MIN)
    db = DB_MIN;
  if (db > DB_MAX)
    db = DB_MAX;
  currentDB = db;
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
}

// ============================================================
//  Servo helper — hanya tulis saat posisi berubah
// ============================================================
inline void servoWrite(int pos) {
  if (pos != servoPos) {
    katupServo.write(pos);
    servoPos = pos;
  }
}
inline void servoOpen() { servoWrite(SERVO_OPEN); }
inline void servoClose() { servoWrite(SERVO_CLOSE); }

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
  //     Hanya berlaku saat BUKAN di IDLE, agar logika suara di
  //     MODE_IDLE tidak terganggu (fix: katup tidak lagi "kedip").
  if (currentMode != MODE_IDLE) {
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
  }

  bool soundTrigger = (currentDB > SOUND_THRESHOLD_DB);
  bool rpmTrigger = (currentRPM > RPM_THRESHOLD);

  switch (currentMode) {

  // ── MODE 1: IDLE ──────────────────────────────────────────
  case MODE_IDLE:
    isCycleActive = false;

    if (soundTrigger) {
      if (!mode1HoldActive) {
        servoClose();
        mode1HoldActive = true;
      }
      mode1CloseStart = now; // perpanjang hold selama suara masih keras
    } else {
      if (mode1HoldActive) {
        // Tunggu hold habis setelah suara reda
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

  // Baris 0: Mode + status tombol (debug)
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
  char dbBuf[7];
  dtostrf(currentDB, 4, 1, dbBuf);
  char lcdLine1[17];
  snprintf(lcdLine1, sizeof(lcdLine1), "P2P:%-4u %sdB", lastPeakToPeak, dbBuf);
  lcd.print(lcdLine1);
}

// ============================================================
//  Baca tombol — debounce non-blocking, aksi pada tepi naik
//  TERVERIFIKASI (LOW → HIGH). active HIGH: tombol ke VCC,
//  butuh pull-down eksternal. Flag actionTaken cegah aksi berulang
//  selama tombol ditahan.
// ============================================================
void checkButtons() {
  unsigned long now = millis();

  for (uint8_t i = 0; i < 3; i++) {
    bool reading = digitalRead(buttons[i].pin);

    // Reset timer debounce setiap ada perubahan sinyal mentah
    if (reading != buttons[i].lastReading) {
      buttons[i].lastDebounce = now;
    }
    buttons[i].lastReading = reading;

    // Belum melewati window debounce → skip
    if (now - buttons[i].lastDebounce <= DEBOUNCE_DELAY)
      continue;

    // Sinyal stabil — update stableState dulu, lalu cek tepi
    bool prevStable = buttons[i].stableState;
    buttons[i].stableState = reading;

    if (prevStable == LOW && reading == HIGH && !buttons[i].actionTaken) {
      buttons[i].actionTaken = true;

      currentMode = (SystemMode)(i + 1); // index 0→MODE1, 1→MODE2, 2→MODE3
      mode1HoldActive = false;
      rpmZeroStart = 0;
      resetCycle();

      Serial.print(F("[BTN] Mode -> "));
      Serial.println(currentMode);
    }

    // Reset flag saat tombol dilepas
    if (reading == LOW) {
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
  servoPos = -1; // paksa write pertama kali
  servoOpen();

  // LCD
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Sistem Katup");
  lcd.setCursor(0, 1);
  lcd.print("Otomatis v2.1");
  delay(1500);
  lcd.clear();

  // Tombol — INPUT: butuh pull-down eksternal (HIGH saat ditekan ke VCC)
  pinMode(BUTTON_MODE1_PIN, INPUT);
  pinMode(BUTTON_MODE2_PIN, INPUT);
  pinMode(BUTTON_MODE3_PIN, INPUT);

  // IR Sensor RPM
  pinMode(IR_SENSOR_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(IR_SENSOR_PIN), countPulse, FALLING);

  // Init timestamp
  unsigned long t = millis();
  lastRPMCalcTime = t;
  lastLCDUpdate = t;
  soundWindowStart = t;

  Serial.println(F("=== Sistem Katup Otomatis v2.1 ==="));
  Serial.println(F("Format: Mode | RPM | dB | Servo"));
}

// ============================================================
//  LOOP UTAMA
// ============================================================
void loop() {
  sampleSound(); // non-blocking (1 sampel/loop)
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
    Serial.println(servoPos);
  }
}
