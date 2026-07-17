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

// Calibration table: AN1200.89 factory baseline. SF9/BW1625 = [2][4] = 13430.
// Compensates for SX1280 internal processing latency. Adjust [2][4] iteratively
// until mean ≈ CABLE_ELEC_M (0.695 m). Formula: new_cal = old_cal + CalVal × 8.06
static const uint16_t CAL_TABLE[3][6] = {
    { 10299, 10271, 10244, 10242, 10230, 10246 },
    { 11486, 11474, 11453, 11426, 11417, 11401 },
    { 13308, 13493, 13528, 13515, 13316, 13376 },  // SF9 [2][4] = 13316: Alpha as master — calibrated 2026-07-17, RadioLib 7.7.1, amb 25.5°C die ~36°C
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

IRAM_ATTR void onDio1() {
    isr_fired = true;
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

#ifdef CAL_MASTER
    // ── MASTER — free-run, no sample limit (temperature calibration) ───────────
    Serial.println("Role: MASTER (Alpha)");
    Serial.printf("Cable: %.4f m electrical  CAL=%d\n", CABLE_ELEC_M, CAL_TABLE[2][4]);
    Serial.println("t_ms,raw_m,die_c,amb_c");

    double sum = 0.0, sum_sq = 0.0;
    int ok = 0, err = 0, outlier = 0;

    while (true) {
        float m   = do_ranging(true);
        float die = (float)temperatureRead();
        float amb = bme_ok ? bme.readTemperature() : NAN;
        unsigned long t = millis();

        if (!isnan(m)) {
            if (m < -100.0f || m > 2000.0f) {
                Serial.printf("# outlier %.4f die=%.1f\n", m, die);
                outlier++;
            } else {
                if (isnan(amb))
                    Serial.printf("%lu,%.4f,%.1f,NA\n", t, m, die);
                else
                    Serial.printf("%lu,%.4f,%.1f,%.2f\n", t, m, die, amb);
                sum    += m;
                sum_sq += (double)m * m;
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
            Serial.printf("# [n=%d] mean=%.4f m  sigma=%.4f m  die=%.1f C  CalVal=%.0f\n",
                ok, mean_m, sigma_m, die, cal_val);
        }

        delay(EXCHANGE_GAP_MS);
        { volatile uint32_t x = 0; uint32_t t0 = millis(); while (millis() - t0 < CPU_BURN_MS) x++; }
    }

#else
    // ── SLAVE ─────────────────────────────────────────────────────────────────
    Serial.println("Role: SLAVE (responder)");
    Serial.println("t_ms,die_c,amb_c");
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
        Serial.printf("%lu,%.1f,NA\n", millis(), die);
    else
        Serial.printf("%lu,%.1f,%.2f\n", millis(), die, amb);
#endif
}
