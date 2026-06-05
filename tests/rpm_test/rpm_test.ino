/**
 * RPM SENSOR TEST - IR / Hall sensor
 * Pin  : D2 (interrupt INT0)
 * LCD  : I2C 0x27 16x2
 *
 * Method: pulse count via interrupt, calc every 1s (same as main.cpp)
 *
 * Row 0: "RPM: 04500      "
 * Row 1: "P:00123 T:1000ms"   pulse count + calc interval
 *
 * Serial @ 9600 → "pulses\tRPM\tdelta_ms"
 */

#include <LiquidCrystal_I2C.h>

#define IR_SENSOR_PIN   2       // INT0
#define RPM_CALC_INTERVAL 1000UL
#define LCD_INTERVAL    500UL
#define RPM_THRESHOLD   4000

LiquidCrystal_I2C lcd(0x27, 16, 2);

volatile uint16_t pulseCount = 0;
uint16_t currentRPM = 0;
uint16_t lastPulses = 0;
unsigned long lastRPMCalcTime = 0;
unsigned long lastDelta = 0;

void countPulse() {
    pulseCount++;
}

void calculateRPM() {
    unsigned long now = millis();
    unsigned long delta = now - lastRPMCalcTime;
    if (delta < RPM_CALC_INTERVAL) return;

    noInterrupts();
    uint16_t pulses = pulseCount;
    pulseCount = 0;
    interrupts();

    currentRPM = (pulses > 0) ? (uint16_t)((pulses * 60000UL) / delta) : 0;
    lastPulses = pulses;
    lastDelta = delta;
    lastRPMCalcTime = now;

    // Serial log
    Serial.print(pulses);
    Serial.print(F("\t"));
    Serial.print(currentRPM);
    Serial.print(F("\t"));
    Serial.println(delta);
}

void setup() {
    Serial.begin(9600);
    Serial.println(F("=== RPM SENSOR TEST ==="));
    Serial.println(F("pulses\tRPM\tdelta_ms"));

    pinMode(IR_SENSOR_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(IR_SENSOR_PIN), countPulse, FALLING);

    lastRPMCalcTime = millis();

    lcd.init();
    lcd.backlight();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(F("RPM Test...     "));
    delay(1000);
    lcd.clear();
}

void loop() {
    calculateRPM();

    static unsigned long lastUpdate = 0;
    unsigned long now = millis();
    if (now - lastUpdate >= LCD_INTERVAL) {
        lastUpdate = now;

        // Row 0: "RPM: 04500 HIGH" or "RPM: 04500     "
        lcd.setCursor(0, 0);
        char row0[17];
        bool over = currentRPM >= RPM_THRESHOLD;
        snprintf(row0, sizeof(row0), "RPM: %05u %s", currentRPM, over ? "HIGH" : "    ");
        lcd.print(row0);

        // Row 1: "P:00123 T:1000 "
        lcd.setCursor(0, 1);
        char row1[17];
        snprintf(row1, sizeof(row1), "P:%05u T:%04lu ", lastPulses, lastDelta);
        lcd.print(row1);
    }
}
