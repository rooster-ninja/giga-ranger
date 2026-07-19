// SX1280 Ranging Calibration — LILYGO T3-S3 V1.3
//
// Procedure:
//   1. Connect boards: [Master]──atten──coax──atten──[Slave]
//   2. Flash master board:  pio run -e master -t upload
//   3. Flash slave board:   pio run -e slave  -t upload
//   4. Open serial monitor on master (115200)
//   5. Record CalibrationValue printed at end of run
//   6. Swap roles (re-flash with -e slave / -e master), repeat, average the two values
//   7. Write averaged CalibrationValue to production firmware via radio.setRangingCalibration()

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <RadioLib.h>
#include <Adafruit_BME280.h>

// ── Pin assignments ───────────────────────────────────────────────────────────
#define RADIO_NSS    7
#define RADIO_DIO1   9
#define RADIO_RST    8
#define RADIO_BUSY  36
#define SPI_SCK      5
#define RADIO_MISO   3
#define RADIO_MOSI   6

// ── RF parameters ─────────────────────────────────────────────────────────────
// Must match production firmware exactly — CalibrationValue is SF-specific
#define CAL_FREQ_MHZ  2450.0f
#define CAL_BW_KHZ    1625.0f
#define CAL_SF            9
#define CAL_TX_DBM       13     // Must match RF_TX_DBM in production firmware exactly.
                                // (13 dBm - 40 dB atten = -27 dBm at RX — within linear range)
                                // WARNING: board has PA FEM — never exceed +5 dBm conducted

// Ranging address — must match on both boards
#define RANGING_ADDR  0xDEADBEEF

// ── Cable parameters ──────────────────────────────────────────────────────────
// DigiKey J10302-ND / 415-0031-M1.0
// Jacket marking: JAN M17/113-RG316 MIL-DTL-17, Amphenol CIT
// RG-316 VF = 0.695 (MIL-DTL-17 Type RG-316/U specification)
#define CABLE_PHYS_M   1.0f      // physical length in metres
#define CABLE_VF       0.695f    // RG-316 velocity factor (MIL-DTL-17 spec, confirmed from jacket)
#define CABLE_ELEC_M   (CABLE_PHYS_M * CABLE_VF)

// Empirical conversion: 0.1803 m per raw SX1280 ranging count
// Source: StuartsProjects 40 km field test; cross-check after initial radiated test
#define METERS_PER_COUNT  0.1803f  // SF9, BW=1625 kHz: c/(2×1625000×2^9)

// ── Run parameters ────────────────────────────────────────────────────────────
#define N_SAMPLES       500
#define EXCHANGE_GAP_MS  20      // delay between exchanges (ms)
#define CPU_BURN_MS     400      // busy-loop after each exchange to match production thermal load
#define TIMEOUT_MS     2000      // per-exchange timeout

// ── AGC / Gain control ────────────────────────────────────────────────────────
// FIXED_GAIN: 0 = let AGC run freely (default for 40 dB bench calibration).
//             1-13 = lock to this gain step per SX1280 Table 4-2 (13=max sensitivity, 1=min).
//             Use 0 for initial characterisation; set a fixed value once the correct step
//             per-attenuation level is known to eliminate discrete gain-state transitions.
#define FIXED_GAIN  0

// Calibration table: AN1200.89 factory baseline. SF9/BW1625 = [2][4] = 13430.
// Compensates for SX1280 internal processing latency. Adjust [2][4] iteratively
// until mean ≈ CABLE_ELEC_M (0.695 m). Formula: new_cal = old_cal + CalVal × 8.06
static const uint16_t CAL_TABLE[3][6] = {
    { 10299, 10271, 10244, 10242, 10230, 10246 },
    { 11486, 11474, 11453, 11426, 11417, 11401 },
    { 13308, 13493, 13528, 13515, 13382, 13376 },  // SF9 [2][4] = 13382: Alpha as master — calibrated 2026-07-19, RadioLib 7.7.1, amb 29.6°C die 40.6°C, 40dB bench, with RSSI logging
};

// ─────────────────────────────────────────────────────────────────────────────

#define BME_SDA  21
#define BME_SCL  10
#define BME_ADDR 0x76

// Continuous burn on core 0 to maximise die temp
static void burn_core0(void *) {
    volatile uint32_t x = 0;
    while (true) {
        for (int i = 0; i < 50000; i++) x++;
        vTaskDelay(1);  // 1ms sleep — lets idle task reset watchdog
    }
}

SX1280 radio = new Module(RADIO_NSS, RADIO_DIO1, RADIO_RST, RADIO_BUSY);
Adafruit_BME280 bme;
static bool bme_ok = false;

volatile bool isr_fired = false;
static float   g_rssi      = 0.0f;   // REG_RANGING_RSSI 0x0964 — correlation peak amplitude (inverted: more negative = stronger)
static float   g_rssi_sync = 0.0f;   // GetPacketStatus RssiSync — RSSI at sync-word detection
static float   g_snr       = 0.0f;   // GetPacketStatus SnrPkt — per-exchange SNR in dB
static uint8_t g_gain_step = 0;      // REG_GAIN_VALUE 0x089E bits 3:0 — AGC/manual gain step 1-13

IRAM_ATTR void onDio1() {
    isr_fired = true;
}

// ── Low-level SPI helpers (chip must be in STANDBY_XOSC when called) ──────────

static uint8_t spi_read_raw(uint16_t addr) {
    SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
    digitalWrite(RADIO_NSS, LOW);
    SPI.transfer(0x19);
    SPI.transfer(addr >> 8);
    SPI.transfer(addr & 0xFF);
    SPI.transfer(0x00);
    uint8_t v = SPI.transfer(0x00);
    digitalWrite(RADIO_NSS, HIGH);
    SPI.endTransaction();
    delayMicroseconds(5);
    return v;
}

static void spi_write_raw(uint16_t addr, uint8_t val) {
    SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
    digitalWrite(RADIO_NSS, LOW);
    SPI.transfer(0x18);
    SPI.transfer(addr >> 8);
    SPI.transfer(addr & 0xFF);
    SPI.transfer(val);
    digitalWrite(RADIO_NSS, HIGH);
    SPI.endTransaction();
    delayMicroseconds(5);
}

// GetPacketStatus (0x1D): reads RssiSync (byte 0) and SnrPkt (byte 1, signed) from the last
// ranging exchange. Must be called immediately after getRangingResult(), before any state
// transition, while the packet status registers are still valid.
// Updates g_rssi_sync (dBm, standard convention: more negative = weaker) and g_snr (dB).
static void read_pkt_status() {
    SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
    digitalWrite(RADIO_NSS, LOW);
    SPI.transfer(0x1D);                          // GetPacketStatus opcode
    uint32_t t = millis();
    while (digitalRead(RADIO_BUSY) && millis() - t < 5) {}
    uint8_t rs = SPI.transfer(0x00);             // RssiSync (unsigned)
    int8_t  sn = (int8_t)SPI.transfer(0x00);     // SnrPkt (signed, dB × 4)
    digitalWrite(RADIO_NSS, HIGH);
    SPI.endTransaction();
    delayMicroseconds(5);
    g_rssi_sync = (rs == 0) ? 0.0f : -(float)rs / 2.0f;
    g_snr       = (float)sn / 4.0f;
}

// Reads REG_RANGING_RSSI (0x0964) and REG_GAIN_VALUE (0x089E) in one STANDBY_XOSC window.
// Must be called immediately after getRangingResult(); getRangingResult() enables the
// ranging clock (reg 0x097F bit 1) which gates the RSSI register read.
// Updates g_rssi (dBm, inverted convention: more negative = stronger) and g_gain_step (1-13).
static void read_radio_state() {
    radio.standby(RADIOLIB_SX128X_STANDBY_XOSC);
    uint32_t t = millis();
    while (digitalRead(RADIO_BUSY) && millis() - t < 10) {}

    uint8_t rssi_raw = spi_read_raw(0x0964);  // REG_RANGING_RSSI
    uint8_t gain_raw = spi_read_raw(0x089E);  // REG_GAIN_VALUE (bits 3:0 = step 1-13)

    t = millis();
    while (digitalRead(RADIO_BUSY) && millis() - t < 10) {}
    radio.standby();

    g_rssi      = (rssi_raw == 0) ? 0.0f : -(float)rssi_raw / 2.0f;
    g_gain_step = gain_raw & 0x0F;
}

// If FIXED_GAIN > 0, locks the SX1280 to a specific gain step (SX1280 Table 4-1).
// Must be called after radio.begin() and before first ranging exchange.
static void setup_gain() {
#if FIXED_GAIN > 0
    radio.standby(RADIOLIB_SX128X_STANDBY_XOSC);
    uint32_t t = millis();
    while (digitalRead(RADIO_BUSY) && millis() - t < 10) {}
    spi_write_raw(0x089F, spi_read_raw(0x089F) | 0x80);           // enable manual gain (bit 7)
    spi_write_raw(0x0895, spi_read_raw(0x0895) | 0x01);           // enable manual gain (bit 0)
    spi_write_raw(0x089E, (spi_read_raw(0x089E) & 0xF0) | (FIXED_GAIN & 0x0F));  // set step
    uint8_t actual = spi_read_raw(0x089E) & 0x0F;
    t = millis();
    while (digitalRead(RADIO_BUSY) && millis() - t < 10) {}
    radio.standby();
    Serial.printf("# gain: MANUAL step=%d (verified 0x089E[3:0]=%d)\n", FIXED_GAIN, actual);
#else
    Serial.println("# gain: AGC auto");
#endif
}

// Blocking ranging exchange with timeout. Returns measured distance (m) or NAN on failure.
float do_ranging(bool master) {
    isr_fired = false;
    radio.setDio1Action(onDio1);
    int state = radio.startRanging(master, RANGING_ADDR, CAL_TABLE);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("# startRanging err %d\n", state);
        return NAN;
    }

    // Wait for ISR or fixed ceiling — DIO1 may not be mapped for ranging events
    unsigned long t0 = millis();
    while (!isr_fired && millis() - t0 < 300) yield();

    float result = radio.getRangingResult();
    read_pkt_status();               // updates g_rssi_sync and g_snr (before state transition)
    read_radio_state();              // updates g_rssi and g_gain_step
    if (result == 0.0f) return NAN;  // discard uninitialized register (startup artifact)
    return result;
}

void setup() {
    Serial.begin(115200);
    delay(3000);

    xTaskCreatePinnedToCore(burn_core0, "burn0", 1024, nullptr, 1, nullptr, 0);
    Wire1.begin(BME_SDA, BME_SCL);
    bme_ok = bme.begin(BME_ADDR, &Wire1);
    if (!bme_ok) bme_ok = bme.begin(0x77, &Wire1);
    Serial.printf("BME280: %s\n", bme_ok ? "OK" : "NA");

    SPI.begin(SPI_SCK, RADIO_MISO, RADIO_MOSI, RADIO_NSS);

    Serial.println("\n=== SX1280 Ranging Calibration ===");
    Serial.printf("RF:    %.0f MHz  BW=%.0f kHz  SF%d  %d dBm\n",
        CAL_FREQ_MHZ, CAL_BW_KHZ, CAL_SF, CAL_TX_DBM);
    Serial.printf("Cable: %.3f m physical  VF=%.3f  → %.4f m electrical\n\n",
        CABLE_PHYS_M, CABLE_VF, CABLE_ELEC_M);

    // 0x12 = standard LoRa private-network sync word (RadioLib default for SX128x)
    int state = radio.begin(CAL_FREQ_MHZ, CAL_BW_KHZ, CAL_SF, 5, 0x12, CAL_TX_DBM);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[FATAL] radio.begin() failed: %d\n", state);
        Serial.println("Check: SPI wiring, NSS/BUSY/RST pins, power.");
        while (true) delay(1000);
    }
    Serial.println("Radio OK");
    radio.setDio1Action(onDio1);

    setup_gain();  // AGC or manual, per FIXED_GAIN

#ifdef CAL_MASTER
    // ── MASTER — free-run, no sample limit (temperature calibration) ───────────
    Serial.println("Role: MASTER (Alpha)");
    Serial.printf("Cable: %.4f m electrical  CAL=%d\n", CABLE_ELEC_M, CAL_TABLE[2][4]);
    Serial.println("t_ms,raw_m,die_c,amb_c,rssi_dbm,gain_step,snr_db,rssi_sync");

    double sum = 0.0, sum_sq = 0.0, sum_rssi = 0.0;
    uint16_t gain_hist[16] = {};   // histogram for gain step mode
    int ok = 0, err = 0, outlier = 0;

    while (true) {
        float m   = do_ranging(true);
        float die = (float)temperatureRead();
        float amb = bme_ok ? bme.readTemperature() : NAN;
        unsigned long t = millis();

        if (!isnan(m)) {
            if (m < -100.0f || m > 2000.0f) {
                Serial.printf("# outlier %.4f die=%.1f rssi=%.1f gain=%d\n",
                    m, die, g_rssi, g_gain_step);
                outlier++;
            } else {
                if (isnan(amb))
                    Serial.printf("%lu,%.4f,%.1f,NA,%.1f,%d,%.1f,%.1f\n",
                        t, m, die, g_rssi, g_gain_step, g_snr, g_rssi_sync);
                else
                    Serial.printf("%lu,%.4f,%.1f,%.2f,%.1f,%d,%.1f,%.1f\n",
                        t, m, die, amb, g_rssi, g_gain_step, g_snr, g_rssi_sync);
                sum      += m;
                sum_sq   += (double)m * m;
                sum_rssi += g_rssi;
                if (g_gain_step < 16) gain_hist[g_gain_step]++;
                ok++;
            }
        } else {
            Serial.printf("# timeout t=%lu\n", t);
            err++;
        }

        if (ok > 0 && ok % 500 == 0) {
            double mean_m  = sum / ok;
            double var     = (sum_sq / ok) - (mean_m * mean_m);
            double sigma_m = sqrt(var < 0.0 ? 0.0 : var);
            float  cal_val = (float)((mean_m - CABLE_ELEC_M) / METERS_PER_COUNT);
            float  mean_rssi = (float)(sum_rssi / ok);
            // Mode gain step
            uint8_t mode_gain = 0;
            uint16_t mode_cnt = 0;
            for (int i = 1; i < 16; i++) {
                if (gain_hist[i] > mode_cnt) { mode_cnt = gain_hist[i]; mode_gain = i; }
            }
            Serial.printf("# [n=%d] mean=%.4f m  sigma=%.4f m  rssi=%.1f dBm  snr=%.1f dB  rssi_sync=%.1f dBm  gain=%d  die=%.1f C  CalVal=%.0f\n",
                ok, mean_m, sigma_m, mean_rssi, g_snr, g_rssi_sync, mode_gain, die, cal_val);
            // Reset accumulators for next batch
            sum = 0.0; sum_sq = 0.0; sum_rssi = 0.0;
            memset(gain_hist, 0, sizeof(gain_hist));
        }

        delay(EXCHANGE_GAP_MS);
        { volatile uint32_t x = 0; uint32_t t0 = millis(); while (millis() - t0 < CPU_BURN_MS) x++; }
    }

#else
    // ── SLAVE ─────────────────────────────────────────────────────────────────
    Serial.println("Role: SLAVE (responder)");
    Serial.println("t_ms,die_c,amb_c,rssi_dbm,gain_step,snr_db,rssi_sync");
#endif
}

void loop() {
#ifndef CAL_MASTER
    do_ranging(false);
    // No CPU burn on slave — slave must cycle at ~350 ms so it is always in RX
    // when the master fires its 300 ms ranging window.  Master and slave at equal
    // burn times (~720 ms each) phase-lock and produce long runs of stale reads.
    float die = (float)temperatureRead();
    float amb = bme_ok ? bme.readTemperature() : NAN;
    if (isnan(amb))
        Serial.printf("%lu,%.1f,NA,%.1f,%d,%.1f,%.1f\n",
            millis(), die, g_rssi, g_gain_step, g_snr, g_rssi_sync);
    else
        Serial.printf("%lu,%.1f,%.2f,%.1f,%d,%.1f,%.1f\n",
            millis(), die, amb, g_rssi, g_gain_step, g_snr, g_rssi_sync);
#endif
}
