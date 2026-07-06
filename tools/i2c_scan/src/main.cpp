#include <Arduino.h>
#include <Wire.h>

#define SDA_PIN 17
#define SCL_PIN 18

void scan(int sda, int scl) {
    Wire.end();
    Wire.begin(sda, scl);
    Serial.printf("\nScanning SDA=%d SCL=%d ...\n", sda, scl);
    int found = 0;
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("  Found: 0x%02X\n", addr);
            found++;
        }
    }
    if (!found) Serial.println("  Nothing found.");
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println("=== I2C Scanner ===");
    scan(18, 17);   // T3-S3 V1.3: SDA=18, SCL=17
    scan(17, 18);   // swapped fallback
    scan(21, 22);   // other common variant
    Serial.println("\nDone. Press reset to rescan.");
}

void loop() {}
