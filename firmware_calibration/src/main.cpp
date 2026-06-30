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
//
// Radio pins confirmed for T3-S3 V1.2; verify schematic for V1.3 if results look wrong.

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>

// ── Pin assignments ───────────────────────────────────────────────────────────
// Source: T3_S3_V1.3_schematic.pdf — all confirmed. See Assets/T3S3_V1.3_SX1280_pins.md
#define RADIO_NSS    7
#define RADIO_DIO1  33
#define RADIO_RST    8
#define RADIO_BUSY  34
#define SPI_SCK      5
#define RADIO_MISO   3
#define RADIO_MOSI   6

// ── RF parameters ─────────────────────────────────────────────────────────────
// Must match production firmware exactly — CalibrationValue is SF-specific
#define CAL_FREQ_MHZ  2450.0f
#define CAL_BW_KHZ    1625.0f
#define CAL_SF            9
#define CAL_TX_DBM       13     // 13 dBm = production power; 80 dB atten keeps RX safe

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
#define METERS_PER_COUNT  0.1803f

// ── Run parameters ────────────────────────────────────────────────────────────
#define N_SAMPLES       500      // Wolf et al. used >1000; 500 is fast with good σ
#define EXCHANGE_GAP_MS  20      // delay between exchanges (ms)
#define TIMEOUT_MS     2000      // per-exchange timeout

// ─────────────────────────────────────────────────────────────────────────────

SX1280 radio = new Module(RADIO_NSS, RADIO_DIO1, RADIO_RST, RADIO_BUSY);

volatile bool isr_fired = false;

IRAM_ATTR void onDio1() {
    isr_fired = true;
}

// Blocking ranging exchange with timeout. Returns measured distance (m) or NAN on failure.
float do_ranging(bool master) {
    isr_fired = false;
    int state = radio.startRanging(master, RANGING_ADDR);
    if (state != RADIOLIB_ERR_NONE) return NAN;

    unsigned long t0 = millis();
    while (!isr_fired) {
        if (millis() - t0 > TIMEOUT_MS) return NAN;
    }

    return radio.getRangingResult();
}

void setup() {
    Serial.begin(115200);
    delay(2000);   // wait for USB CDC enumeration

    SPI.begin(SPI_SCK, RADIO_MISO, RADIO_MOSI, RADIO_NSS);

    Serial.println("\n=== SX1280 Ranging Calibration ===");
    Serial.printf("RF:    %.0f MHz  BW=%.0f kHz  SF%d  +%d dBm\n",
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

    // RxTxDelay register resets to 0x0000 on power-up — no zeroing needed.
    // Do not call setRangingCalibration() before this measurement run.
    radio.setDio1Action(onDio1);

#ifdef CAL_MASTER
    // ── MASTER ────────────────────────────────────────────────────────────────
    Serial.println("Role: MASTER (initiator)");
    Serial.printf("Collecting %d samples...\n\n", N_SAMPLES);
    Serial.println("raw_m");   // CSV header for capture

    double sum = 0.0, sum_sq = 0.0;
    int ok = 0, err = 0;

    for (int i = 0; i < N_SAMPLES; i++) {
        float m = do_ranging(true);
        if (!isnan(m) && m > 0.0f) {
            Serial.println(m, 4);
            sum    += m;
            sum_sq += (double)m * m;
            ok++;
        } else {
            Serial.println("# timeout");
            err++;
        }
        delay(EXCHANGE_GAP_MS);
    }

    if (ok == 0) {
        Serial.println("\n[ERROR] Zero successful exchanges.");
        Serial.println("Check: slave is running, cable connected, attenuators in place.");
        return;
    }

    double mean_m  = sum / ok;
    double var     = (sum_sq / ok) - (mean_m * mean_m);
    double sigma_m = sqrt(var < 0.0 ? 0.0 : var);
    float  cal_val = (float)((mean_m - CABLE_ELEC_M) / METERS_PER_COUNT);

    Serial.println("\n=== RESULTS ===");
    Serial.printf("Exchanges:    %d ok  /  %d failed\n", ok, err);
    Serial.printf("Mean:         %.4f m\n", mean_m);
    Serial.printf("Std dev:      %.4f m  (%.0f mm)\n", sigma_m, sigma_m * 1000.0);
    Serial.printf("Cable (elec): %.4f m\n", (double)CABLE_ELEC_M);

    Serial.println();
    Serial.printf(">>> CalibrationValue = %.0f  (SF%d) <<<\n", cal_val, CAL_SF);
    Serial.println();
    Serial.println("--- Next steps ---");
    Serial.println("1. Record CalibrationValue above.");
    Serial.println("2. Swap board roles (re-flash -e slave on this board, -e master on other).");
    Serial.println("3. Re-run and record second CalibrationValue.");
    Serial.println("4. Final value = (run_A + run_B) / 2  (cancels TX/RX path asymmetry).");
    Serial.println("5. In production firmware:");
    Serial.printf("     radio.setRangingCalibration(YOUR_AVERAGED_VALUE);\n");

#else
    // ── SLAVE ─────────────────────────────────────────────────────────────────
    Serial.println("Role: SLAVE (responder)");
    Serial.println("Listening for master... (no output during exchanges)");
    for (;;) {
        do_ranging(false);   // respond and re-enter immediately
    }
#endif
}

void loop() {}
