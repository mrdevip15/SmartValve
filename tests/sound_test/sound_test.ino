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

// Mirror main.cpp calibration constants exactly
#define DB_REF          75.0f
#define V_REF           2.5f
#define VCC             5.0f
#define ADC_MAX         1023.0f
#define DB_MIN          30.0f
#define DB_MAX          120.0f
#define SOUND_THRESHOLD_DB 75.0f

#define SOUND_WINDOW_MS 50UL
#define P2P_AVG_COUNT   8
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
    float voltage = (pp / ADC_MAX) * VCC;
    float db = DB_REF + 20.0f * log10f(voltage / V_REF);
    if (db < DB_MIN) db = DB_MIN;
    if (db > DB_MAX) db = DB_MAX;
    currentDB = db;
}

void setup() {
    Serial.begin(9600);
    Serial.println(F("=== SOUND SENSOR TEST ==="));
    Serial.println(F("RAW\tP2P\tdB"));

    analogReference(DEFAULT);
    pinMode(SOUND_PIN, INPUT);

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
