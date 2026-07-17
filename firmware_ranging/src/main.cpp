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
#include <WiFi.h>
#include <RadioLib.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Adafruit_BME280.h>
#include <NimBLEDevice.h>
#include <SD.h>

// ── RF pin assignments (T3-S3 V1.3) ──────────────────────────────────────────
#define RADIO_NSS    7
#define RADIO_DIO1   9
#define RADIO_RST    8
#define RADIO_BUSY  36
#define SPI_SCK      5
#define RADIO_MISO   3
#define RADIO_MOSI   6

// ── I2C buses ────────────────────────────────────────────────────────────────
// Wire  (I2C0): OLED — GPIO17/18 hardwired on T3-S3 V1.3, not on any header
// Wire1 (I2C1): BME280 — connector 1 (GND·3V3·IO10·IO21)
#define OLED_SDA    18
#define OLED_SCL    17
#define OLED_ADDR   0x3C
#define BME_SDA     21   // connector 1 pin IO21
#define BME_SCL     10   // connector 1 pin IO10
#define BME_ADDR    0x76 // SDO→GND; use 0x77 if SDO→VCC

// ── SD/TF card SPI (T3-S3 V1.3) — verify against schematic before first flash ─
#define SD_CS    13
#define SD_MOSI  11
#define SD_SCK   14
#define SD_MISO   2
#ifdef ROLE_ALPHA
#define SD_LOG_FILE "/ALPHA.CSV"
#else
#define SD_LOG_FILE "/CHIMP.CSV"
#endif

// ── RF parameters — must match calibration firmware exactly ──────────────────
#define RF_FREQ_MHZ   2450.0f
#define RF_BW_KHZ     1625.0f
#define RF_SF             9
#define RF_TX_DBM        13    // OTA — PA FEM; never exceed +5 dBm conducted
#define FW_REV           "CMP-001"  // comparison firmware — CAL vs PROD ranging

// Calibration table: SF9/BW1625 auto_cal 2026-07-16, RadioLib 7.7.1. CAL_TABLE[2][4] = 13343.
// Alpha as master, n=500, mean=0.6849m, sigma=0.9813m, die=38.6°C, amb=27.1°C.
static const uint16_t CAL_TABLE[3][6] = {
    { 10299, 10271, 10244, 10242, 10230, 10246 },  // BW 406.25 kHz
    { 11486, 11474, 11453, 11426, 11417, 11401 },  // BW 812.50 kHz
    { 13308, 13493, 13528, 13515, 13343, 13376 },  // BW 1625.00 kHz (SF9 [2][4] = 13343)
};
#define RANGING_ADDR      0xDEADBEEF
#define RANGING_INTERVAL_MS  5000   // ms between master-initiated exchanges
#define LINK_TIMEOUT_MS     30000   // ms without a detected exchange before link_ok = false

// ── Outlier filter ───────────────────────────────────────────────────────────
#define DELTA_GATE_M  500.0f    // reject if > ±500 m from last valid (tighten after field test)
#define MEDIAN_N      5

// ── Temperature correction ────────────────────────────────────────────────────
// Icebox sweep 2026-07-16 (Assets/master_20260716_214604.csv, 141 min, CAL=13229, RadioLib 7.7.1):
//   Die temp coefficient: -0.063 m/°C (higher die → lower reading).
//   Phase endpoints: cold die=16.6°C → raw=4.64m; hot die=48.6°C → raw=2.64m; Δdie=32°C, Δraw=-2.01m.
// NOTE: TEMP_COEFF below was derived from BME ambient in Icebox 2026-07-15 (+0.0665 m/°C amb).
//   The two coefficients have opposite signs because BME ambient reflects cable thermal effects
//   (warmer cable → longer electrical path → higher reading) while die temp reflects chip clock drift
//   (warmer die → shorter ToF result). Net ambient coefficient may still be positive — leave as-is
//   until a dedicated ambient regression is done on the 2026-07-16 sweep.
// corrected = median - TEMP_COEFF * (bme_amb - CAL_AMB_C)
#define TEMP_COEFF   0.0665f   // m/°C — BME ambient, Operation Icebox 2026-07-15
#define CAL_AMB_C    27.1f     // °C   — BME ambient at auto_cal convergence 2026-07-16

// ── BLE NUS UUIDs (Nordic UART Service, standard) ────────────────────────────
#define NUS_SERVICE_UUID  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID       "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_UUID       "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// ─────────────────────────────────────────────────────────────────────────────

SX1280 radio = new Module(RADIO_NSS, RADIO_DIO1, RADIO_RST, RADIO_BUSY);
U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R3, U8X8_PIN_NONE);
Adafruit_BME280 bme;
static bool bme_ok = false;

// BLE state
static NimBLECharacteristic *ble_tx = nullptr;
static bool ble_connected = false;

// SD + epoch state
static SPIClass  sd_spi(HSPI);
static bool      sd_ok      = false;
static uint32_t  boot_epoch = 0;  // unix seconds at boot; 0 = not set; set via BLE EPOCH=<n>

class BleRxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override {
        std::string val = c->getValue();
        if (val.rfind("EPOCH=", 0) == 0) {
            uint32_t epoch = (uint32_t)strtoul(val.c_str() + 6, nullptr, 10);
            if (epoch > 1700000000UL) {  // sanity: after 2023
                boot_epoch = epoch - millis() / 1000;
                Serial.printf("[BLE] epoch set: boot=%lu\n", boot_epoch);
                if (sd_ok) {
                    File f = SD.open(SD_LOG_FILE, FILE_APPEND);
                    if (f) {
                        char buf[64];
                        snprintf(buf, sizeof(buf), "BOOT,epoch=%lu,t=%lu\n",
                            boot_epoch, millis() / 1000);
                        f.print(buf);
                        f.close();
                    }
                }
            }
        }
    }
};

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
static bool     link_ok   = false;   // Chimp: RSSI-based link state; Alpha: DIO1-based

// ── Helpers ───────────────────────────────────────────────────────────────────

static volatile bool isr_fired = false;
IRAM_ATTR static void on_dio1() { isr_fired = true; }

static float g_rssi        = 0;
static float g_snr         = 0;
static bool  g_exchange_ok = false;

// Debug captures — raw hardware values every cycle, unfiltered
static bool          dbg_isr     = false;
static unsigned long dbg_elapsed = 0;
static float         dbg_rssi    = 0;
static float         dbg_snr     = 0;
static float         dbg_range   = NAN;

// Read the RSSI of the ranging exchange on the master side via REG_RANGING_RSSI (0x0964).
// GetPacketStatus is not populated for the ranging master; 0x0964 is the ranging engine's
// own RSSI measurement of the slave response. Must be in STANDBY_XOSC to access it.
// getRangingResult() already enables the ranging clock (reg 0x097F bit 1).
// SX128x ReadRegister stream frame: [0x19][addrMSB][addrLSB][NOP][NOP]
//   MISO data is the 5th byte (cmdLen=3, statusWidth=1 → data at index 4).
//   BUSY wait is OUTSIDE the NSS window (RadioLib Module::SPItransferStream protocol).
#ifdef ROLE_ALPHA
static float get_ranging_rssi() {
    radio.standby(RADIOLIB_SX128X_STANDBY_XOSC);

    uint32_t t = millis();
    while (digitalRead(RADIO_BUSY) && millis() - t < 10) {}

    SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
    digitalWrite(RADIO_NSS, LOW);
    SPI.transfer(0x19);   // CMD_READ_REGISTER
    SPI.transfer(0x09);   // addr MSB
    SPI.transfer(0x64);   // addr LSB  (0x0964 = REG_RANGING_RSSI)
    SPI.transfer(0x00);   // NOP — status slot
    uint8_t rssi_raw = SPI.transfer(0x00);  // NOP → data
    digitalWrite(RADIO_NSS, HIGH);
    SPI.endTransaction();

    delayMicroseconds(1);
    t = millis();
    while (digitalRead(RADIO_BUSY) && millis() - t < 10) {}

    radio.standby();  // back to STANDBY_RC

    if (rssi_raw == 0) return 0.0f;
    return -(float)rssi_raw / 2.0f;
}

// CAL-style ranging: exact reproduction of calibration firmware's do_ranging(true).
// Differences from prod: re-attaches setDio1Action before every exchange, no get_ranging_rssi,
// runs EXCHANGE_GAP_MS(20ms) + CPU_BURN_MS(400ms) after the result read.
static float do_ranging_cal() {
    isr_fired = false;
    radio.setDio1Action(on_dio1);  // calibration firmware re-attaches every call
    int state = radio.startRanging(true, RANGING_ADDR, CAL_TABLE);
    if (state != RADIOLIB_ERR_NONE) return NAN;
    unsigned long t0 = millis();
    while (!isr_fired && millis() - t0 < 300) yield();
    float result = radio.getRangingResult();  // calibration firmware: only this call
    delay(20);
    { volatile uint32_t x = 0; uint32_t tb = millis(); while (millis() - tb < 400) x++; }
    return result;
}

// PROD-style ranging: current production firmware's do_ranging(true) logic.
// Differences from cal: no setDio1Action in loop, calls get_ranging_rssi after result.
static float do_ranging_prod(float *rssi_out) {
    isr_fired = false;
    // setDio1Action NOT called here — production calls it only once in setup()
    int state = radio.startRanging(true, RANGING_ADDR, CAL_TABLE);
    if (state != RADIOLIB_ERR_NONE) { *rssi_out = 0.0f; return NAN; }
    unsigned long t0 = millis();
    while (!isr_fired && millis() - t0 < 300) yield();
    float result = radio.getRangingResult();
    *rssi_out = get_ranging_rssi();  // production: extra call with standby(XOSC) inside
    return result;
}
#endif

// SX1280 ClearIrqStatus (0x97) — clears all IRQ flags, bringing DIO1 LOW.
// radio.getRangingResult() does NOT clear slave IRQs empirically; this does.
// Must be called after every slave do_ranging() cycle so the next exchange
// produces a fresh RISING edge on DIO1 for the ISR to detect.
static void clear_sx1280_irq() {
    uint32_t t = millis();
    while (digitalRead(RADIO_BUSY) && millis() - t < 10) {}
    SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
    digitalWrite(RADIO_NSS, LOW);
    SPI.transfer(0x97);  // ClearIrqStatus
    SPI.transfer(0xFF);  // clear all IRQ bits 15-8
    SPI.transfer(0xFF);  // clear all IRQ bits 7-0
    digitalWrite(RADIO_NSS, HIGH);
    SPI.endTransaction();
    t = millis();
    while (digitalRead(RADIO_BUSY) && millis() - t < 10) {}
}

float do_ranging(bool master) {
    isr_fired = false;
    int state = radio.startRanging(master, RANGING_ADDR, CAL_TABLE);
    if (state != RADIOLIB_ERR_NONE) { g_exchange_ok = false; return NAN; }
    unsigned long t0 = millis();
    while (!isr_fired && millis() - t0 < 300) yield();

    dbg_isr     = isr_fired;
    dbg_elapsed = millis() - t0;
#ifdef ROLE_ALPHA
    dbg_range = radio.getRangingResult();
    dbg_rssi  = get_ranging_rssi();
#else
    radio.getRangingResult();
    dbg_range = NAN;
    if (isr_fired) {
        dbg_rssi = radio.getRSSI();
        dbg_snr  = radio.getSNR();
        clear_sx1280_irq();
    }
#endif

    g_rssi = dbg_rssi;
    g_snr  = dbg_snr;
    g_exchange_ok = (dbg_rssi < -5.0f && dbg_rssi > -115.0f);
    return dbg_range;
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

static void sd_log(const char *line) {
    if (!sd_ok) return;
    File f = SD.open(SD_LOG_FILE, FILE_APPEND);
    if (!f) return;
    f.print(line);
    f.close();
}

static void epoch_field(char *buf, size_t len) {
    if (boot_epoch == 0) {
        strncpy(buf, "NA", len);
    } else {
        snprintf(buf, len, "%lu", boot_epoch + millis() / 1000);
    }
}

// ── Display ───────────────────────────────────────────────────────────────────

// U8G2_R3 portrait: 64 px wide × 128 px tall.
// Font u8g2_font_6x10_tr: 6 px/char wide → 10 chars max per row.
// Row spacing 10 px; baselines at y = 10, 20, 30 … 120.

#ifdef ROLE_ALPHA
static float display_range = NAN;
#endif
static float display_rssi = 0;
static float display_snr  = 0;

void update_display() {
    char buf[24];
    display.clearBuffer();
    display.setFont(u8g2_font_6x10_tr);

    // ── Role + BLE + SD ──
#ifdef ROLE_ALPHA
    display.drawStr(0, 10, "ALPHA");
#else
    display.drawStr(0, 10, "CHIMP-001");
#endif
    display.drawStr(0, 20, ble_connected ? "BLE:CONN" : "BLE:WAIT");
    display.drawStr(0, 30, sd_ok ? "SD:LOG" : "SD:NA");
    display.drawHLine(0, 34, 64);

    // ── RF section ──
#ifdef ROLE_ALPHA
    if (isnan(display_range)) {
        display.drawStr(0, 44, "---");
    } else if (display_range >= 1000.0f) {
        snprintf(buf, sizeof(buf), "%.3f km", display_range / 1000.0f);
        display.drawStr(0, 44, buf);
    } else {
        snprintf(buf, sizeof(buf), "%.1f m", display_range);
        display.drawStr(0, 44, buf);
    }
    snprintf(buf, sizeof(buf), "RSSI:%.0f", display_rssi);
    display.drawStr(0, 54, buf);
    display.drawHLine(0, 62, 64);
    display.drawStr(0, 78, link_ok ? "Link: OK" : "Link: --");
    snprintf(buf, sizeof(buf), "DIE:%.1fC", (float)temperatureRead());
    display.drawStr(0, 88, buf);
    display.drawHLine(0, 92, 64);
    if (bme_ok) {
        snprintf(buf, sizeof(buf), "T:%.1fC", bme.readTemperature());
        display.drawStr(0, 102, buf);
        snprintf(buf, sizeof(buf), "H:%.1f%%", bme.readHumidity());
        display.drawStr(0, 112, buf);
        snprintf(buf, sizeof(buf), "P:%.0fhPa", bme.readPressure() / 100.0f);
        display.drawStr(0, 122, buf);
    } else {
        display.drawStr(0, 102, "T:NA");
        display.drawStr(0, 112, "H:NA");
        display.drawStr(0, 122, "P:NA");
    }
#else
    snprintf(buf, sizeof(buf), "RSSI:%.0f", display_rssi);
    display.drawStr(0, 44, buf);
    snprintf(buf, sizeof(buf), "SNR:%.1f", display_snr);
    display.drawStr(0, 54, buf);
    display.drawHLine(0, 58, 64);
    display.drawStr(0, 68, link_ok ? "Link: OK" : "Link: --");
    snprintf(buf, sizeof(buf), "DIE:%.1fC", (float)temperatureRead());
    display.drawStr(0, 78, buf);
    display.drawHLine(0, 82, 64);
    if (bme_ok) {
        snprintf(buf, sizeof(buf), "T:%.1fC", bme.readTemperature());
        display.drawStr(0, 92, buf);
        snprintf(buf, sizeof(buf), "H:%.1f%%", bme.readHumidity());
        display.drawStr(0, 102, buf);
        snprintf(buf, sizeof(buf), "P:%.0fhPa", bme.readPressure() / 100.0f);
        display.drawStr(0, 112, buf);
    } else {
        display.drawStr(0, 92,  "T:NA");
        display.drawStr(0, 102, "H:NA");
        display.drawStr(0, 112, "P:NA");
    }
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
    rx->setCallbacks(new BleRxCallbacks());

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(NUS_SERVICE_UUID);
    adv->start();
}

// ── setup ─────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    WiFi.mode(WIFI_OFF);
    delay(2000);

    Wire1.begin(BME_SDA, BME_SCL);
    // OLED and SD disabled for CMP firmware — serial only
    // Wire.begin(OLED_SDA, OLED_SCL);
    // display.begin(); display.clearBuffer(); display.sendBuffer();

    // BME280 — Wire1, connector 1 (IO21=SDA, IO10=SCL)
    bme_ok = bme.begin(BME_ADDR, &Wire1);
    if (!bme_ok) {
        Serial.println("[WARN] BME280 not found at 0x76 — trying 0x77");
        bme_ok = bme.begin(0x77, &Wire1);
        if (!bme_ok) Serial.println("[WARN] BME280 not found — reporting NA");
    }

    // SX1280
    SPI.begin(SPI_SCK, RADIO_MISO, RADIO_MOSI, RADIO_NSS);
    int state = radio.begin(RF_FREQ_MHZ, RF_BW_KHZ, RF_SF, 5, 0x12, RF_TX_DBM);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[FATAL] radio.begin() failed: %d\n", state);
        while (true) delay(1000);
    }
    radio.setDio1Action(on_dio1);

    Serial.printf("[OK] Radio: %.0f MHz  BW=%.0f kHz  SF%d  %d dBm\n",
        RF_FREQ_MHZ, RF_BW_KHZ, RF_SF, RF_TX_DBM);

    // BLE
// BLE disabled for ranging accuracy test — re-enable after bench validation
// #ifdef ROLE_ALPHA
//     ble_init("GR-ALPHA");
//     Serial.println("[OK] BLE: GR-ALPHA advertising");
// #else
//     ble_init("GRCHIMP1");
//     Serial.println("[OK] BLE: GRCHIMP1 advertising");
// #endif

    Serial.println("[OK] Ready — " FW_REV);
}

// ── loop ──────────────────────────────────────────────────────────────────────

#ifdef ROLE_ALPHA

// Alpha comparison loop: each iteration fires one CAL-style exchange then one PROD-style
// exchange, printing results with FW_REV tag so version is unambiguous in the log.
void loop() {
    static uint32_t n = 0;
    n++;

    // --- CAL STYLE: exact calibration firmware master behaviour ---
    float cal_result = do_ranging_cal();
    bool  cal_isr    = isr_fired;
    Serial.printf("CAL," FW_REV ",n=%lu,isr=%d,range=%.4f,t=%lu\n",
        n, (int)cal_isr, cal_result, millis() / 1000);

    delay(2000);  // gap — Chimp re-arms during this window before PROD exchange

    // --- PROD STYLE: current production firmware master behaviour ---
    float prod_rssi;
    float prod_result = do_ranging_prod(&prod_rssi);
    bool  prod_isr    = isr_fired;
    Serial.printf("PROD," FW_REV ",n=%lu,isr=%d,range=%.4f,rssi=%.1f,t=%lu\n",
        n, (int)prod_isr, prod_result, prod_rssi, millis() / 1000);

    delay(5000);
}

#else  // Chimp-001 slave

void loop() {
    do_ranging(false);
    bool exchange = dbg_isr;
    if (exchange) ok_count++;
    Serial.printf("DBG," FW_REV ",t=%lu,exch=%d,ok=%lu,rssi=%.1f,snr=%.1f\n",
        millis() / 1000, (int)exchange, (unsigned long)ok_count, dbg_rssi, dbg_snr);
}

#endif
