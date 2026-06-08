/**
 * BUTTON TEST - DEBUG MODE
 * Pins: Button1=4, Button2=5, Button3=6 (Active HIGH, 3.3V soldered)
 * LCD  : I2C 0x27 16x2
 *
 * Row 0: raw pin state B1 B2 B3 (0/1)
 * Row 1: mode yg aktif
 */

#include <LiquidCrystal_I2C.h>

#define BTN1 4
#define BTN2 5
#define BTN3 6

#define DEBOUNCE_MS   150UL
#define CONFIRM_COUNT 5

LiquidCrystal_I2C lcd(0x27, 16, 2);

uint8_t currentMode = 0;
uint8_t conf[3] = {0, 0, 0};
unsigned long lastTrigger = 0;

void showDebug(bool b1, bool b2, bool b3) {
    lcd.setCursor(0, 0);
    lcd.print("B1:");
    lcd.print(b1 ? "1" : "0");
    lcd.print(" B2:");
    lcd.print(b2 ? "1" : "0");
    lcd.print(" B3:");
    lcd.print(b3 ? "1" : "0");
    lcd.print("  ");

    lcd.setCursor(0, 1);
    lcd.print("Mode: ");
    if (currentMode == 0) {
        lcd.print("--      ");
    } else {
        lcd.print(currentMode);
        lcd.print("       ");
    }
}

void setup() {
    Serial.begin(9600);
    pinMode(BTN1, INPUT);
    pinMode(BTN2, INPUT);
    pinMode(BTN3, INPUT);

    lcd.init();
    lcd.backlight();
    lcd.clear();
}

void loop() {
    bool btn[3] = {
        digitalRead(BTN1) == HIGH,
        digitalRead(BTN2) == HIGH,
        digitalRead(BTN3) == HIGH
    };

    unsigned long now = millis();

    for (uint8_t i = 0; i < 3; i++) {
        if (btn[i]) {
            if (conf[i] < CONFIRM_COUNT) conf[i]++;
        } else {
            conf[i] = 0;
        }

        if (conf[i] >= CONFIRM_COUNT && (now - lastTrigger) > DEBOUNCE_MS) {
            uint8_t newMode = i + 1;
            if (currentMode != newMode) {
                currentMode = newMode;
                Serial.print("Mode -> ");
                Serial.println(currentMode);
            }
            lastTrigger = now;
            for (uint8_t j = 0; j < 3; j++) if (j != i) conf[j] = 0;
        }
    }

    static unsigned long lastUpdate = 0;
    if (millis() - lastUpdate >= 100) {
        lastUpdate = millis();
        showDebug(btn[0], btn[1], btn[2]);
    }

    delay(10);
}
