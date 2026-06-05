/**
 * SOUND SENSOR TEST - KY-037 AO
 * Pin  : A0 (analog output)
 * LCD  : I2C 0x27 16x2
 *
 * Method: peak-to-peak per window (same as main.cpp)
 *
 * Row 0: "P2P:1023 R:0512"   peak-to-peak + raw ADC
 * Row 1: "dB: 075.3 THR:75"  estimated dB + threshold marker
 *
 * Serial @ 9600 → "RAW\tP2P\tdB"
 */

#include <LiquidCrystal_I2C.h>
#include <math.h>

#define SOUND_PIN       A0

// Calibration — INTERNAL ref (1.1V), two-point empirical:
//   P2P_REF (silence)  → DB_REF
//   hard blow          → ~114 dB
//
// Dengan INTERNAL ref, P2P values ~4.5x lebih besar dari 5V ref
// karena resolusi ADC lebih halus (1.1V/1023 = 1.07mV per step)
// Sinyal >1.1V akan clip di 1023 — normal, blow keras tetap terdeteksi
//
// !! SETELAH UPLOAD: lihat Serial, catat P2P saat diam → update P2P_REF
//                    lalu tiup → catat P2P → hitung DB_SCALE baru
// DB_SCALE = (114 - DB_REF) / log10(P2P_blow / P2P_REF)
#define DB_REF          35.0f   // dB at silence
#define P2P_REF         43.0f   // baseline P2P saat diam (measured on hardware)
#define DB_SCALE        47.0f   // estimasi untuk 1.1V ref — UPDATE setelah cal
#define DB_MIN          30.0f
#define DB_MAX          120.0f
#define SOUND_THRESHOLD_DB 75.0f

#define SOUND_WINDOW_MS 150UL   // naik dari 50 → tangkap lebih banyak amplitudo
#define P2P_AVG_COUNT   16      // naik dari 8 → smoothing lebih baik
#define LCD_INTERVAL    200UL

LiquidCrystal_I2C lcd(0x27, 16, 2);

uint16_t soundMax = 0;
uint16_t soundMin = 1023;
unsigned long soundWindowStart = 0;
uint16_t p2pBuf[P2P_AVG_COUNT] = {0};
uint8_t p2pIdx = 0;
uint16_t lastP2P = 0;
float currentDB = 0.0f;
uint16_t lastRaw = 0;

void sampleSound() {
    lastRaw = analogRead(SOUND_PIN);
    if (lastRaw > soundMax) soundMax = lastRaw;
    if (lastRaw < soundMin) soundMin = lastRaw;

    unsigned long now = millis();
    if (now - soundWindowStart < SOUND_WINDOW_MS) return;
    soundWindowStart = now;

    uint16_t p2p = (soundMax >= soundMin) ? (soundMax - soundMin) : 0;
    soundMax = 0;
    soundMin = 1023;

    p2pBuf[p2pIdx] = p2p;
    p2pIdx = (p2pIdx + 1) % P2P_AVG_COUNT;

    uint32_t sum = 0;
    for (uint8_t i = 0; i < P2P_AVG_COUNT; i++) sum += p2pBuf[i];
    lastP2P = (uint16_t)(sum / P2P_AVG_COUNT);

    float pp = (lastP2P < 1) ? 1.0f : (float)lastP2P;
    float db = DB_REF + DB_SCALE * log10f(pp / P2P_REF);
    if (db < DB_MIN) db = DB_MIN;
    if (db > DB_MAX) db = DB_MAX;
    currentDB = db;
}

void setup() {
    Serial.begin(9600);
    Serial.println(F("=== SOUND SENSOR TEST ==="));
    Serial.println(F("RAW\tP2P\tdB"));

    analogReference(INTERNAL);  // 1.1V ref — resolusi 5x lebih halus
    pinMode(SOUND_PIN, INPUT);
    delay(10);  // beri waktu ADC settle setelah ganti referensi

    lcd.init();
    lcd.backlight();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(F("Sound Test...   "));
    delay(1000);
    lcd.clear();
}

void loop() {
    sampleSound();

    static unsigned long lastUpdate = 0;
    unsigned long now = millis();
    if (now - lastUpdate >= LCD_INTERVAL) {
        lastUpdate = now;

        // Row 0: "P2P:1023 R:0512"
        lcd.setCursor(0, 0);
        char row0[17];
        snprintf(row0, sizeof(row0), "P2P:%04u R:%04u", lastP2P, lastRaw);
        lcd.print(row0);

        // Row 1: "dB:075.3 [LOUD]" or "dB:075.3 [    ]"
        lcd.setCursor(0, 1);
        uint8_t dbInt = (uint8_t)currentDB;
        uint8_t dbDec = (uint8_t)((currentDB - dbInt) * 10);
        char row1[17];
        bool loud = currentDB >= SOUND_THRESHOLD_DB;
        snprintf(row1, sizeof(row1), "dB:%03u.%u %s", dbInt, dbDec, loud ? "[LOUD]" : "[    ]");
        lcd.print(row1);

        // Serial log
        Serial.print(lastRaw);
        Serial.print(F("\t"));
        Serial.print(lastP2P);
        Serial.print(F("\t"));
        Serial.println(currentDB, 1);
    }
}
