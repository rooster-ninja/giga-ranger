// Giga Ranger — Production Ranging Firmware
// LILYGO T3-S3 V1.3 + SX1280, SF9 BW=1625 kHz
//
// Build environments:
//   pio run -e alpha      → Alpha (permanent master, measures distance)
//   pio run -e chimp-001  → Chimp-001 (permanent slave, responds)
//
// Local monitoring: BLE NUS (Nordic UART Service)
//   Connect via nRF UART app (iOS/Android) or any BLE serial terminal
//   Advertisement name: "GR-ALPHA" or "GR-CHIMP001"
//
// No WiFi. No MQTT. No provisioning.

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Adafruit_BME280.h>
#include <NimBLEDevice.h>

// ── RF pin assignments (T3-S3 V1.3) ──────────────────────────────────────────
#define RADIO_NSS    7
#define RADIO_DIO1   9
#define RADIO_RST    8
#define RADIO_BUSY  36
#define SPI_SCK      5
#define RADIO_MISO   3
#define RADIO_MOSI   6

// ── I2C (OLED + BME280 shared bus) ───────────────────────────────────────────
#define I2C_SDA     17
#define I2C_SCL     18
#define OLED_ADDR   0x3C
#define BME_ADDR    0x76    // SDO→GND; use 0x77 if SDO→VCC

// ── RF parameters — must match calibration firmware exactly ──────────────────
#define RF_FREQ_MHZ   2450.0f
#define RF_BW_KHZ     1625.0f
#define RF_SF             9
#define RF_TX_DBM        13    // OTA — PA FEM; never exceed +5 dBm conducted

// Calibration table: SF9/BW1625 corrected 2026-07-04. CAL_TABLE[2][4] = 13089.
static const uint16_t CAL_TABLE[3][6] = {
    { 10299, 10271, 10244, 10242, 10230, 10246 },  // BW 406.25 kHz
    { 11486, 11474, 11453, 11426, 11417, 11401 },  // BW 812.50 kHz
    { 13308, 13493, 13528, 13515, 13089, 13376 },  // BW 1625.00 kHz (SF9 adjusted)
};
#define RANGING_ADDR      0xDEADBEEF
#define RANGING_INTERVAL_MS  5000   // ms between master-initiated exchanges

// ── Outlier filter ───────────────────────────────────────────────────────────
#define DELTA_GATE_M  500.0f    // reject if > ±500 m from last valid (tighten after field test)
#define MEDIAN_N      5

// ── BLE NUS UUIDs (Nordic UART Service, standard) ────────────────────────────
#define NUS_SERVICE_UUID  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID       "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_UUID       "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// ─────────────────────────────────────────────────────────────────────────────

SX1280 radio = new Module(RADIO_NSS, RADIO_DIO1, RADIO_RST, RADIO_BUSY);
U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R1, U8X8_PIN_NONE);
Adafruit_BME280 bme;

// BLE state
static NimBLECharacteristic *ble_tx = nullptr;
static bool ble_connected = false;

class BleCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *, NimBLEConnInfo &) override {
        ble_connected = true;
    }
    void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) override {
        ble_connected = false;
        NimBLEDevice::startAdvertising();
    }
};

// Outlier filter state
static float  median_buf[MEDIAN_N] = {};
static int    median_count = 0;
static float  last_valid   = NAN;

// Stats
static uint32_t ok_count  = 0;
static uint32_t rej_count = 0;

// ── Helpers ───────────────────────────────────────────────────────────────────

static volatile bool isr_fired = false;
IRAM_ATTR static void on_dio1() { isr_fired = true; }

float do_ranging(bool master) {
    isr_fired = false;
    radio.setDio1Action(on_dio1);
    int state = radio.startRanging(master, RANGING_ADDR, CAL_TABLE);
    if (state != RADIOLIB_ERR_NONE) return NAN;
    unsigned long t0 = millis();
    while (!isr_fired && millis() - t0 < 300) yield();
    return radio.getRangingResult();
}

bool outlier_filter(float m, float *out) {
    if (!isnan(last_valid) && fabsf(m - last_valid) > DELTA_GATE_M) return false;
    last_valid = m;
    median_buf[median_count % MEDIAN_N] = m;
    median_count++;
    int n = median_count < MEDIAN_N ? median_count : MEDIAN_N;
    float tmp[MEDIAN_N];
    memcpy(tmp, median_buf, n * sizeof(float));
    for (int i = 1; i < n; i++) {
        float k = tmp[i]; int j = i - 1;
        while (j >= 0 && tmp[j] > k) { tmp[j + 1] = tmp[j]; j--; }
        tmp[j + 1] = k;
    }
    *out = tmp[n / 2];
    return true;
}

void ble_send(const char *line) {
    if (ble_tx && ble_connected) {
        ble_tx->setValue((uint8_t *)line, strlen(line));
        ble_tx->notify();
    }
}

// ── Display ───────────────────────────────────────────────────────────────────

// Rotated 90° (U8G2_R1): logical canvas is 64 px wide × 128 px tall.
// Font u8g2_font_6x10_tr: 6 px wide, 10 px tall — ~10 chars/row, 12 rows.
// Row baselines (y): 10, 22, 34, 46, 58, 70, 82, 94, 106, 118, 128 (clipped).

#ifdef ROLE_ALPHA
static float display_range = NAN;
#endif
static float display_rssi = 0;
static float display_snr  = 0;

void update_display() {
    char buf[24];
    display.clearBuffer();
    display.setFont(u8g2_font_6x10_tr);

#ifdef ROLE_ALPHA
    display.drawStr(0, 10, "ALPHA");
#else
    display.drawStr(0, 10, "CHIMP-001");
#endif

    display.drawStr(0, 22, ble_connected ? "BLE:CONN" : "BLE:WAIT");

    display.drawHLine(0, 27, 64);

#ifdef ROLE_ALPHA
    if (isnan(display_range)) {
        display.drawStr(0, 40, "---");
    } else if (display_range >= 1000.0f) {
        snprintf(buf, sizeof(buf), "%.3f km", display_range / 1000.0f);
        display.drawStr(0, 40, buf);
    } else {
        snprintf(buf, sizeof(buf), "%.1f m", display_range);
        display.drawStr(0, 40, buf);
    }
#endif

#ifdef ROLE_ALPHA
    snprintf(buf, sizeof(buf), "RSSI:%.0fdBm", display_rssi);
    display.drawStr(0, 52, buf);
    snprintf(buf, sizeof(buf), "SNR: %.1fdB", display_snr);
    display.drawStr(0, 64, buf);
    display.drawHLine(0, 69, 64);
    snprintf(buf, sizeof(buf), "OK:%6lu", (unsigned long)ok_count);
    display.drawStr(0, 82, buf);
    snprintf(buf, sizeof(buf), "DIE:%.1fC", (float)temperatureRead());
    display.drawStr(0, 94, buf);
#else
    snprintf(buf, sizeof(buf), "RSSI:%.0fdBm", display_rssi);
    display.drawStr(0, 40, buf);
    snprintf(buf, sizeof(buf), "SNR: %.1fdB", display_snr);
    display.drawStr(0, 52, buf);
    display.drawHLine(0, 57, 64);
    snprintf(buf, sizeof(buf), "OK:%6lu", (unsigned long)ok_count);
    display.drawStr(0, 70, buf);
    snprintf(buf, sizeof(buf), "DIE:%.1fC", (float)temperatureRead());
    display.drawStr(0, 82, buf);
#endif

    display.sendBuffer();
}

// ── BLE init ──────────────────────────────────────────────────────────────────

void ble_init(const char *name) {
    NimBLEDevice::init(name);
    NimBLEServer *server = NimBLEDevice::createServer();
    server->setCallbacks(new BleCallbacks());

    NimBLEService *svc = server->createService(NUS_SERVICE_UUID);

    ble_tx = svc->createCharacteristic(NUS_TX_UUID,
                NIMBLE_PROPERTY::NOTIFY);

    NimBLECharacteristic *rx = svc->createCharacteristic(NUS_RX_UUID,
                NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    (void)rx; // RX commands not implemented in V1

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(NUS_SERVICE_UUID);
    adv->start();
}

// ── setup ─────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(2000);

    Wire.begin(I2C_SDA, I2C_SCL);

    // OLED
    display.setI2CAddress(OLED_ADDR << 1);
    display.begin();
    display.clearBuffer();
    display.setFont(u8g2_font_6x10_tr);
#ifdef ROLE_ALPHA
    display.drawStr(0, 10, "ALPHA");
#else
    display.drawStr(0, 10, "CHIMP-001");
#endif
    display.drawStr(0, 22, "Init...");
    display.sendBuffer();

    // BME280
    if (!bme.begin(BME_ADDR, &Wire)) {
        Serial.println("[WARN] BME280 not found at 0x76 — trying 0x77");
        if (!bme.begin(0x77, &Wire)) {
            Serial.println("[WARN] BME280 not found — atmospheric data unavailable");
        }
    }

    // SX1280
    SPI.begin(SPI_SCK, RADIO_MISO, RADIO_MOSI, RADIO_NSS);
    int state = radio.begin(RF_FREQ_MHZ, RF_BW_KHZ, RF_SF, 5, 0x12, RF_TX_DBM);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[FATAL] radio.begin() failed: %d\n", state);
        char errbuf[16];
        snprintf(errbuf, sizeof(errbuf), "err %d", state);
        display.clearBuffer();
        display.drawStr(0, 10, "RADIO FAIL");
        display.drawStr(0, 22, errbuf);
        display.sendBuffer();
        while (true) delay(1000);
    }
    radio.setDio1Action(on_dio1);

    Serial.printf("[OK] Radio: %.0f MHz  BW=%.0f kHz  SF%d  %d dBm\n",
        RF_FREQ_MHZ, RF_BW_KHZ, RF_SF, RF_TX_DBM);

    // BLE
#ifdef ROLE_ALPHA
    ble_init("GR-ALPHA");
    Serial.println("[OK] BLE: GR-ALPHA advertising");
#else
    ble_init("GR-CHIMP001");
    Serial.println("[OK] BLE: GR-CHIMP001 advertising");
#endif

    update_display();
    Serial.println("[OK] Ready");
}

// ── loop ──────────────────────────────────────────────────────────────────────

#ifdef ROLE_ALPHA

void loop() {
    float raw = do_ranging(true);
    char line[80];

    if (isnan(raw)) {
        Serial.println("# ranging timeout");
        snprintf(line, sizeof(line), "ALPHA,t=%lu,err=timeout\n", millis() / 1000);
        ble_send(line);
    } else {
        display_rssi = radio.getRSSI();
        display_snr  = radio.getSNR();

        float median;
        if (outlier_filter(raw, &median)) {
            ok_count++;
            display_range = median;

            Serial.printf("range=%.1f m  rssi=%.0f  snr=%.1f  ok=%lu\n",
                median, display_rssi, display_snr, (unsigned long)ok_count);

            snprintf(line, sizeof(line),
                "ALPHA,t=%lu,r=%.1f,rssi=%.0f,snr=%.1f,ok=%lu,rej=%lu\n",
                millis() / 1000, median, display_rssi, display_snr,
                (unsigned long)ok_count, (unsigned long)rej_count);
            ble_send(line);
        } else {
            rej_count++;
            Serial.printf("# outlier rejected: %.1f m  (last_valid=%.1f)\n", raw, last_valid);
            snprintf(line, sizeof(line),
                "ALPHA,t=%lu,outlier=%.1f,rej=%lu\n",
                millis() / 1000, raw, (unsigned long)rej_count);
            ble_send(line);
        }

        update_display();
    }

    delay(RANGING_INTERVAL_MS);
}

#else  // Chimp-001

void loop() {
    do_ranging(false);

    display_rssi = radio.getRSSI();
    display_snr  = radio.getSNR();
    ok_count++;

    char line[64];
    snprintf(line, sizeof(line),
        "CHIMP,t=%lu,rssi=%.0f,snr=%.1f,ok=%lu\n",
        millis() / 1000, display_rssi, display_snr, (unsigned long)ok_count);
    Serial.print(line);
    ble_send(line);

    update_display();
    // no delay — immediately re-arms for next ranging request from Alpha
}

#endif
