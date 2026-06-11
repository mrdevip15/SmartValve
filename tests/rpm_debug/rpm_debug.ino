/**
 * RPM SENSOR ADVANCED DEBUG - FC-03 Speed Sensor
 * Pin  : D2 (interrupt INT0)
 * LED  : D13 (Built-in)
 * LCD  : I2C 0x27 16x2
 */

#include <Arduino.h>
#include <LiquidCrystal_I2C.h>

#define IR_SENSOR_PIN 2      // Pin sensor (Interrupt 0)
#define INDICATOR_LED 13     // LED built-in
#define RPM_PPR 1            // Ganti jika pakai piringan berlubang (misal 20)
#define CALC_INTERVAL 1000UL // Update tiap 1 detik
#define RPM_DEBOUNCE_MS 5    // Abaikan pulsa jika jarak < 2ms

LiquidCrystal_I2C lcd(0x27, 16, 2);

volatile uint32_t pulseCount = 0;
volatile unsigned long lastPulseTime = 0;

void countPulse() {
  unsigned long now = millis();
  if (now - lastPulseTime > RPM_DEBOUNCE_MS) {
    pulseCount++;
    digitalWrite(INDICATOR_LED,
                 !digitalRead(INDICATOR_LED)); // Toggle LED tiap pulsa
    lastPulseTime = now;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(IR_SENSOR_PIN, INPUT_PULLUP);
  pinMode(INDICATOR_LED, OUTPUT);

  attachInterrupt(digitalPinToInterrupt(IR_SENSOR_PIN), countPulse, FALLING);

  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("RPM Debug Tool"));
  lcd.setCursor(0, 1);
  lcd.print(F("PPR: "));
  lcd.print(RPM_PPR);

  delay(2000);
  lcd.clear();

  Serial.println(F("\n=== FC-03 RPM ADVANCED DEBUG (v2.2) ==="));
  Serial.print(F("PPR: "));
  Serial.println(RPM_PPR);
  Serial.print(F("Debounce: "));
  Serial.print(RPM_DEBOUNCE_MS);
  Serial.println(F("ms"));
  Serial.println(F("Format: [Sec] Pulses | RPM | IR_Pin"));
  Serial.println(F("---------------------------------------"));
}

unsigned long lastCalc = 0;
unsigned long seconds = 0;

void loop() {
  unsigned long now = millis();
  bool currentIR = digitalRead(IR_SENSOR_PIN);

  if (now - lastCalc >= CALC_INTERVAL) {
    noInterrupts();
    uint32_t pulses = pulseCount;
    pulseCount = 0;
    interrupts();

    unsigned long deltaMs = now - lastCalc;
    uint32_t rpm =
        (uint32_t)(((uint32_t)pulses * 60000UL) / (deltaMs * RPM_PPR));
    seconds++;
    lastCalc = now;

    // Serial
    Serial.print(F("["));
    if (seconds < 10)
      Serial.print(F("0"));
    Serial.print(seconds);
    Serial.print(F("s] "));
    Serial.print(pulses);
    Serial.print(F(" p | "));
    Serial.print(rpm);
    Serial.print(F(" RPM | IR: "));
    Serial.println(currentIR ? F("HIGH (Clear)") : F("LOW (Blocked)"));

    // LCD
    lcd.setCursor(0, 0);
    char row0[17];
    snprintf(row0, sizeof(row0), "RPM: %-5lu P:%-3lu", rpm, pulses);
    lcd.print(row0);

    lcd.setCursor(0, 1);
    char row1[17];
    snprintf(row1, sizeof(row1), "T:%-4lus IR:%s", seconds,
             currentIR ? "CLEAR" : "BLOCK");
    lcd.print(row1);
  }
}
