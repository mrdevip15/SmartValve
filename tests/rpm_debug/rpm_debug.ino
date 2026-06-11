/**
 * RPM DEBUG - MOVING AVERAGE (REAL-TIME FEEL)
 * Pin  : D2 (interrupt INT0)
 * 
 * Update: Update display setiap 100ms menggunakan Moving Average.
 * Hasil: RPM terasa lebih halus dan responsif.
 */

#include <Arduino.h>
#include <LiquidCrystal_I2C.h>

#define IR_SENSOR_PIN   2
#define INDICATOR_LED   13
#define RPM_PPR         1
#define SAMPLE_INTERVAL 100UL   // Ambil sampel setiap 100ms (10x lebih cepat)
#define AVG_WINDOW      10      // Gunakan 10 sampel terakhir (10 x 100ms = 1 detik total)

// Filter Noise (10ms)
#define RPM_DEBOUNCE_MICROS 10000 

LiquidCrystal_I2C lcd(0x27, 16, 2);

volatile uint32_t pulseCount = 0;
volatile unsigned long lastPulseMicros = 0;

// Buffer untuk Moving Average
uint16_t sampleBuffer[AVG_WINDOW];
uint8_t bufferIdx = 0;
uint32_t runningSum = 0;

void countPulse() {
    unsigned long now = micros();
    if (now - lastPulseMicros > RPM_DEBOUNCE_MICROS) {
        pulseCount++;
        digitalWrite(INDICATOR_LED, !digitalRead(INDICATOR_LED));
        lastPulseMicros = now;
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
    lcd.print(F("Moving Average"));
    
    // Inisialisasi buffer dengan 0
    for(int i=0; i<AVG_WINDOW; i++) sampleBuffer[i] = 0;

    Serial.println(F("\n=== RPM REAL-TIME (MOVING AVG) ==="));
    Serial.println(F("Format: [RPM] | Instant_P | Avg_Sum"));
    Serial.println(F("----------------------------------"));
}

unsigned long lastSampleTime = 0;
unsigned long seconds = 0;
bool lastIRState = false;

void loop() {
    unsigned long now = millis();
    bool currentIR = digitalRead(IR_SENSOR_PIN);

    // Cek State Fisik (Instan)
    if (currentIR != lastIRState) {
        lastIRState = currentIR;
        lcd.setCursor(0, 1);
        lcd.print(F("IR: "));
        lcd.print(currentIR ? F("CLEAR (H)") : F("BLOCK (L)"));
    }

    // Hitung Moving Average setiap 100ms
    if (now - lastSampleTime >= SAMPLE_INTERVAL) {
        noInterrupts();
        uint16_t pulsesThisSample = pulseCount;
        pulseCount = 0;
        interrupts();

        // Update Moving Average
        runningSum -= sampleBuffer[bufferIdx];      // Kurangi sampel tertua
        sampleBuffer[bufferIdx] = pulsesThisSample;  // Masukkan sampel baru
        runningSum += sampleBuffer[bufferIdx];      // Tambahkan ke total
        bufferIdx = (bufferIdx + 1) % AVG_WINDOW;   // Geser index

        // Rumus RPM: (Total Pulsa dalam 1 detik) * 60 / PPR
        // Karena window kita totalnya 1000ms (10 sampel x 100ms), 
        // runningSum sudah mewakili "pulsa per detik".
        uint32_t rpm = (runningSum * 60UL) / RPM_PPR;
        
        lastSampleTime = now;

        // Log ke Serial setiap 500ms agar tidak memenuhi layar
        static unsigned long lastSerial = 0;
        if (now - lastSerial >= 500) {
            lastSerial = now;
            Serial.print(F("RPM: ")); Serial.print(rpm);
            Serial.print(F(" | Sample_P: ")); Serial.print(pulsesThisSample);
            Serial.print(F(" | Sum_1s: ")); Serial.println(runningSum);
        }

        // Update LCD (Baris Atas)
        lcd.setCursor(0, 0);
        char row0[17];
        snprintf(row0, sizeof(row0), "RPM:%-5lu P:%-3u", rpm, pulsesThisSample);
        lcd.print(row0);
    }
}
