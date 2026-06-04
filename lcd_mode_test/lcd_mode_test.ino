/**
 * LCD MODE SWITCH TEST
 * =====================
 * Board  : Arduino Uno / Nano
 * LCD    : I2C 0x27, 16x2
 * Button : Pin 4 = Mode1, Pin 5 = Mode2, Pin 6 = Mode3
 *          (Active HIGH - kabel ke 3.3V, tanpa pull-down)
 *
 * Tekan tombol -> LCD tampil "Mode 1 / 2 / 3"
 */

#include <LiquidCrystal_I2C.h>

// ---- PIN ----
#define BTN1 4
#define BTN2 5
#define BTN3 6

// ---- DEBOUNCE ----
#define DEBOUNCE_MS   150
#define CONFIRM_COUNT 5    // baca HIGH berturut-turut sebelum konfirmasi

LiquidCrystal_I2C lcd(0x27, 16, 2);

uint8_t currentMode  = 0;
uint8_t conf[3]      = {0, 0, 0};
unsigned long lastTrigger = 0;

// ---- tampil mode di LCD ----
void showMode(uint8_t m) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("  MODE SWITCH   ");
  lcd.setCursor(0, 1);
  switch (m) {
    case 1: lcd.print("   >> MODE 1 << "); break;
    case 2: lcd.print("   >> MODE 2 << "); break;
    case 3: lcd.print("   >> MODE 3 << "); break;
    default: lcd.print("  tekan 1 / 2 / 3"); break;
  }
}

void setup() {
  pinMode(BTN1, INPUT);
  pinMode(BTN2, INPUT);
  pinMode(BTN3, INPUT);

  lcd.init();
  lcd.backlight();
  showMode(0);
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
        showMode(currentMode);
      }
      lastTrigger = now;
      for (uint8_t j = 0; j < 3; j++) if (j != i) conf[j] = 0;
    }
  }

  delay(10);
}
