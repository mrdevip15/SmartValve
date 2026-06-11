/**
 * RPM SENSOR ADVANCED DEBUG - HIGH NOISE FILTER
 * Pin  : D2 (interrupt INT0)
 * 
 * Update: Menggunakan micros() dan debounce lebih tinggi (10ms)
 * untuk menghilangkan pulsa hantu akibat noise kabel busi.
 */

#include <Arduino.h>
#include <LiquidCrystal_I2C.h>

#define IR_SENSOR_PIN   2
#define INDICATOR_LED   13
#define RPM_PPR         1
#define CALC_INTERVAL   1000UL

// FILTER: Abaikan sinyal liar di bawah 10 milidetik (10000 micros)
// 10ms = max 6000 RPM (untuk PPR 1). Jika mesin lebih cepat, kecilkan angka ini.
#define RPM_DEBOUNCE_MICROS 10000 

LiquidCrystal_I2C lcd(0x27, 16, 2);

volatile uint32_t pulseCount = 0;
volatile unsigned long lastPulseMicros = 0;
bool lastIRState = false;

void countPulse() {
    unsigned long now = micros();
    // Filter Noise: Sinyal harus berjarak minimal 10ms
    if (now - lastPulseMicros > RPM_DEBOUNCE_MICROS) {
        pulseCount++;
        digitalWrite(INDICATOR_LED, !digitalRead(INDICATOR_LED));
        lastPulseMicros = now;
    }
}

void setup() {
    Serial.begin(115200);
    // Gunakan INPUT_PULLUP internal untuk stabilitas ekstra
    pinMode(IR_SENSOR_PIN, INPUT_PULLUP);
    pinMode(INDICATOR_LED, OUTPUT);
    
    attachInterrupt(digitalPinToInterrupt(IR_SENSOR_PIN), countPulse, FALLING);
    
    lcd.init();
    lcd.backlight();
    lcd.clear();
    lcd.print(F("Heavy Filtering"));
    
    Serial.println(F("\n=== RPM DEBUG (HEAVY FILTER) ==="));
    Serial.print(F("Debounce Time: ")); Serial.print(RPM_DEBOUNCE_MICROS / 1000); Serial.println(F("ms"));
    Serial.println(F("Jika P masih muncul saat diam, pasang kapasitor 104!"));
    Serial.println(F("-------------------------------------------------"));
}

unsigned long lastCalc = 0;
unsigned long seconds = 0;

void loop() {
    unsigned long now = millis();
    bool currentIR = digitalRead(IR_SENSOR_PIN);

    if (currentIR != lastIRState) {
        lastIRState = currentIR;
        lcd.setCursor(0, 1);
        lcd.print(F("IR: "));
        lcd.print(currentIR ? F("CLEAR (H)") : F("BLOCK (L)"));
    }

    if (now - lastCalc >= CALC_INTERVAL) {
        noInterrupts();
        uint32_t pulses = pulseCount;
        pulseCount = 0;
        interrupts();
        
        unsigned long deltaMs = now - lastCalc;
        uint32_t rpm = (uint32_t)(((uint32_t)pulses * 60000UL) / (deltaMs * RPM_PPR));
        lastCalc = now;
        seconds++;
        
        Serial.print(F("[")); if (seconds < 10) Serial.print(F("0")); Serial.print(seconds);
        Serial.print(F("s] P:")); Serial.print(pulses);
        Serial.print(F(" | RPM:")); Serial.print(rpm);
        Serial.print(F(" | STATE:")); Serial.println(currentIR ? F("H") : F("L"));
        
        lcd.setCursor(0, 0);
        char row0[17];
        snprintf(row0, sizeof(row0), "RPM:%-5lu P:%-3lu", rpm, pulses);
        lcd.print(row0);
    }
}
