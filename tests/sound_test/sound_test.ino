/**
 * SOUND SENSOR DEBUG - MAX4466 VERSION (CALIBRATED OFFLINE)
 * Pin  : A0
 * Power: 3.3V recommended
 * 
 * Note: Kalibrasi ini dilakukan saat tidak terhubung ke Laptop
 *       untuk menghindari noise grounding dari charger.
 */

#include <LiquidCrystal_I2C.h>
#include <math.h>

#define SOUND_PIN       A0

// Kalibrasi Akhir MAX4466 (Data Offline)
#define DB_REF          45.0f   // dB saat diam (matching user meter)
#define P2P_REF         24.0f   // baseline P2P saat diam (diperoleh dari Mi:806 Ma:830)
#define DB_SCALE        55.0f   // Skala sensitivitas (adj: (114-45)/log10(290/24) ≈ 64, kita pakai 55-60)
#define DB_MIN          30.0f
#define DB_MAX          120.0f
#define SOUND_THRESHOLD_DB 75.0f

#define SOUND_WINDOW_MS 150UL   
#define P2P_AVG_COUNT   4      
#define LCD_INTERVAL    250UL

LiquidCrystal_I2C lcd(0x27, 16, 2);

uint16_t soundMax = 0;
uint16_t soundMin = 1023;
uint16_t dispMax = 0;
uint16_t dispMin = 0;
unsigned long soundWindowStart = 0;
uint16_t p2pBuf[P2P_AVG_COUNT] = {0};
uint8_t p2pIdx = 0;
uint16_t lastP2P = 0;
float currentDB = 0.0f;

void sampleSound() {
    uint16_t raw = analogRead(SOUND_PIN);
    if (raw > soundMax) soundMax = raw;
    if (raw < soundMin) soundMin = raw;

    unsigned long now = millis();
    if (now - soundWindowStart < SOUND_WINDOW_MS) return;
    
    dispMax = soundMax;
    dispMin = soundMin;
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
    // Rumus dB logaritmik
    float db = DB_REF + DB_SCALE * log10f(pp / P2P_REF);
    if (db < DB_MIN) db = DB_MIN;
    if (db > DB_MAX) db = DB_MAX;
    currentDB = db;
}

void setup() {
    Serial.begin(9600);
    analogReference(DEFAULT); 
    pinMode(SOUND_PIN, INPUT);

    lcd.init();
    lcd.backlight();
    lcd.clear();
    lcd.print(F("MAX4466 Calib"));
    delay(1000);
    lcd.clear();
}

void loop() {
    sampleSound();

    static unsigned long lastUpdate = 0;
    if (millis() - lastUpdate >= LCD_INTERVAL) {
        lastUpdate = millis();

        // Tampilkan Min/Max Raw
        lcd.setCursor(0, 0);
        char row0[17];
        snprintf(row0, sizeof(row0), "Mi:%04u Ma:%04u", dispMin, dispMax);
        lcd.print(row0);

        // Tampilkan P2P & dB
        lcd.setCursor(0, 1);
        char dbBuf[6];
        dtostrf(currentDB, 4, 1, dbBuf);
        char row1[17];
        snprintf(row1, sizeof(row1), "P2P:%04u dB:%s", lastP2P, dbBuf);
        lcd.print(row1);

        Serial.print(dispMin); Serial.print("\t");
        Serial.print(dispMax); Serial.print("\t");
        Serial.print(lastP2P); Serial.print("\t");
        Serial.println(currentDB, 1);
    }
}
