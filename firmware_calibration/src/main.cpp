// SX1280 Ranging Calibration + LoRa Link Protocol — LILYGO T3-S3 V1.3
//
// Protocol phases:
//   LORA_LINK:    Chimp broadcasts CONNECT_REQ → Alpha accepts → ping/pong heartbeat
//   RANGING_INFO: User sends "start" → Alpha commands Chimp → continuous ranging loop
//
// Alpha serial commands: start / stop
// CSV header printed on RANGING_INFO entry; comment lines (#) at all times.
// BLE stubs (ble_read_cmd / ble_notify) wired later; OLED in a future commit.

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
#define CAL_FREQ_MHZ  2450.0f
#define CAL_BW_KHZ    1625.0f
#define CAL_SF            9
#define CAL_TX_DBM       13

// ── Ranging parameters ────────────────────────────────────────────────────────
#define RANGING_ADDR      0xDEADBEEF
#define CABLE_PHYS_M      1.0f
#define CABLE_VF          0.695f
#define CABLE_ELEC_M      (CABLE_PHYS_M * CABLE_VF)
#define METERS_PER_COUNT  0.1803f

// ── Run parameters ────────────────────────────────────────────────────────────
#define RANGE_TIMEOUT_MS 300

// ── Gain control ──────────────────────────────────────────────────────────────
// 0 = AGC, 1-13 = manual gain step
#define FIXED_GAIN  10

// ── Calibration table ─────────────────────────────────────────────────────────
// SF9/BW1625 = CAL_TABLE[2][4]
// *** NEEDS RE-CALIBRATION ***
// Previous auto_cal runs targeted 0.695 m (wrong: used physical × VF instead of physical / VF).
// Correct target = 1.0 / 0.695 = 1.4388 m.  Re-run auto_cal.py to derive correct values.
// Alpha master + new Chimp slave — calibrated 2026-07-20, gain=10, 40dB bench (WRONG TARGET)
static const uint16_t CAL_TABLE[3][6] = {
    { 10299, 10271, 10244, 10242, 10230, 10246 },
    { 11486, 11474, 11453, 11426, 11417, 11401 },
    { 13308, 13493, 13528, 13515, 13296, 13376 },
};

// ── Packet type bytes ─────────────────────────────────────────────────────────
#define PKT_CONNECT_REQ  0x05
#define PKT_CONNECT_ACK  0x06
#define PKT_PING         0x01
#define PKT_PONG         0x02
#define PKT_START        0x10
#define PKT_START_ACK    0x11
#define PKT_STOP         0x20
#define PKT_STOP_ACK     0x21
#define PKT_TELEM        0xAB
#define PKT_TELEM_REQ    0xAC  // Alpha → Chimp: "I'm ready to receive your TELEM"

// Control packets: { type, seq, t_ms[4] } = 6 bytes
struct __attribute__((packed)) PktCtrl {
    uint8_t  type;
    uint8_t  seq;
    uint32_t t_ms;
};

// Telemetry packet from Chimp → Alpha, 8 bytes
struct __attribute__((packed)) PktTelem {
    uint8_t type;          // PKT_TELEM
    uint8_t inst_rssi_raw; // GetInstantRSSI  → -raw/2 dBm
    uint8_t rssi_sync_raw; // GetPacketStatus byte 0 → -raw/2 dBm
    int8_t  snr_raw;       // GetPacketStatus byte 1 × 4 → raw/4 dB
    uint8_t rssi_corr_raw; // REG_RANGING_RSSI 0x0964 → -raw/2 dBm
    uint8_t gain_step;     // REG_GAIN_VALUE bits 3:0
    uint8_t freq_hi;       // freq error 0x09F6
    uint8_t freq_lo;       // freq error 0x09F7
};

// ── Hardware ──────────────────────────────────────────────────────────────────
#define BME_SDA  21
#define BME_SCL  10
#define BME_ADDR 0x76

SX1280 radio = new Module(RADIO_NSS, RADIO_DIO1, RADIO_RST, RADIO_BUSY);
Adafruit_BME280 bme;
static bool bme_ok = false;

// ── ISR ───────────────────────────────────────────────────────────────────────
volatile bool isr_fired = false;
IRAM_ATTR void onDio1() { isr_fired = true; }

// ── Per-exchange radio state globals ──────────────────────────────────────────
static float   g_rssi      = 0.0f;   // REG_RANGING_RSSI (inverted: more negative = stronger)
static float   g_rssi_sync = 0.0f;   // GetPacketStatus RssiSync (dead on master in ranging)
static float   g_snr       = 0.0f;   // GetPacketStatus SnrPkt   (dead on master in ranging)
static uint8_t g_gain_step = 0;      // REG_GAIN_VALUE bits 3:0
static float   g_inst_rssi   = 0.0f; // GetInstantRSSI (0x15)
static float   g_freq_err_hz = 0.0f; // Freq error reg 0x09F5-7, Hz

// ── Low-level SPI helpers ─────────────────────────────────────────────────────
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

// Switch radio back to LoRa packet type + modulation params without a full hardware reset.
// startRanging() leaves the chip in RANGING packet type; startReceive() after that
// will try to receive a ranging frame, not a LoRa frame.  Call this before rx_arm()
// when transitioning from ranging back to LoRa RX.

// GetPacketStatus (0x1D): updates g_rssi_sync and g_snr.
// Dead on master in ranging mode (returns 0/0); alive on slave.
// Call before any state transition after getRangingResult().
static void read_pkt_status() {
    SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
    digitalWrite(RADIO_NSS, LOW);
    SPI.transfer(0x1D);
    uint32_t t = millis();
    while (digitalRead(RADIO_BUSY) && millis() - t < 5) {}
    uint8_t rs = SPI.transfer(0x00);
    int8_t  sn = (int8_t)SPI.transfer(0x00);
    digitalWrite(RADIO_NSS, HIGH);
    SPI.endTransaction();
    delayMicroseconds(5);
    g_rssi_sync = (rs == 0) ? 0.0f : -(float)rs / 2.0f;
    g_snr       = (float)sn / 4.0f;
}

// GetInstantRSSI (0x15): instantaneous RSSI from last reception.
// Call before any state transition after getRangingResult().
static void read_instant_rssi() {
    SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
    digitalWrite(RADIO_NSS, LOW);
    SPI.transfer(0x15);
    uint32_t t = millis();
    while (digitalRead(RADIO_BUSY) && millis() - t < 5) {}
    uint8_t raw = SPI.transfer(0x00);
    digitalWrite(RADIO_NSS, HIGH);
    SPI.endTransaction();
    delayMicroseconds(5);
    g_inst_rssi = (raw == 0) ? 0.0f : -(float)raw / 2.0f;
}

// Reads REG_RANGING_RSSI, REG_GAIN_VALUE, and freq error (0x09F5-7) in one STANDBY_XOSC window.
static void read_radio_state() {
    radio.standby(RADIOLIB_SX128X_STANDBY_XOSC);
    uint32_t t = millis();
    while (digitalRead(RADIO_BUSY) && millis() - t < 10) {}

    uint8_t rssi_raw = spi_read_raw(0x0964);
    uint8_t gain_raw = spi_read_raw(0x089E);
    uint8_t fe_h     = spi_read_raw(0x09F5);
    uint8_t fe_m     = spi_read_raw(0x09F6);
    uint8_t fe_l     = spi_read_raw(0x09F7);

    t = millis();
    while (digitalRead(RADIO_BUSY) && millis() - t < 10) {}
    radio.standby();

    g_rssi      = (rssi_raw == 0) ? 0.0f : -(float)rssi_raw / 2.0f;
    g_gain_step = gain_raw & 0x0F;

    int32_t fe_raw = ((int32_t)(fe_h & 0x0F) << 16) | ((int32_t)fe_m << 8) | fe_l;
    if (fe_raw & 0x80000) fe_raw |= (int32_t)0xFFF00000;
    g_freq_err_hz = (float)fe_raw * (1625000.0f / 8388608.0f);
}

// Re-apply manual gain after any LoRa mode switch (setPacketType may reset registers).
static void apply_gain() {
#if FIXED_GAIN > 0
    radio.standby(RADIOLIB_SX128X_STANDBY_XOSC);
    uint32_t t = millis();
    while (digitalRead(RADIO_BUSY) && millis() - t < 10) {}
    spi_write_raw(0x089F, spi_read_raw(0x089F) | 0x80);
    spi_write_raw(0x0895, spi_read_raw(0x0895) | 0x01);
    spi_write_raw(0x089E, (spi_read_raw(0x089E) & 0xF0) | (FIXED_GAIN & 0x0F));
    t = millis();
    while (digitalRead(RADIO_BUSY) && millis() - t < 10) {}
    radio.standby();
#endif
}

// One-time gain setup at boot; prints diagnostic.
static void setup_gain() {
#if FIXED_GAIN > 0
    apply_gain();
    uint8_t actual = spi_read_raw(0x089E) & 0x0F;
    Serial.printf("# gain: MANUAL step=%d (0x089E[3:0]=%d)\n", FIXED_GAIN, actual);
#else
    Serial.println("# gain: AGC auto");
#endif
}

// Ranging exchange with timeout. Returns distance (m) or NAN on failure.
// Updates g_rssi_sync, g_snr, g_inst_rssi, g_rssi, g_gain_step, g_freq_err_hz.
static float do_ranging(bool master) {
    // Radio may be in LoRa RX (after rx_wait STOP check) — standby first so
    // startRanging's SetPacketType(RANGING) is accepted cleanly.
    radio.standby();
    isr_fired = false;
    radio.setDio1Action(onDio1);
    int state = radio.startRanging(master, RANGING_ADDR, CAL_TABLE);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("# startRanging err %d\n", state);
        return NAN;
    }
    unsigned long t0 = millis();
    while (!isr_fired && millis() - t0 < RANGE_TIMEOUT_MS) yield();

    float result = radio.getRangingResult();
    read_pkt_status();
    read_instant_rssi();
    read_radio_state();
    if (!isr_fired)  return NAN;    // timed out — exchange never completed
    if (!master)     return 0.0f;   // slave: exchange completed, no range measurement
    if (result == 0.0f) return NAN; // master: unexpected zero
    return result;
}

// ── Non-blocking serial command reader ────────────────────────────────────────
static String _serial_buf;
static String poll_cmd() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            String cmd = _serial_buf;
            _serial_buf = "";
            cmd.trim();
            cmd.toLowerCase();
            if (cmd.length() > 0) return cmd;
        } else {
            _serial_buf += c;
        }
    }
    return "";
}

// ── BLE stubs (wire NimBLE characteristics here later) ────────────────────────
static String ble_read_cmd()           { return ""; }
static void   ble_notify(const char*)  {}

// ── Helper: send a 6-byte control packet ──────────────────────────────────────
static void send_ctrl(uint8_t type, uint8_t seq) {
    PktCtrl p = { type, seq, (uint32_t)millis() };
    radio.transmit((uint8_t*)&p, sizeof(p));
}

// ── Helper: startReceive with fresh ISR flag ──────────────────────────────────
static void rx_arm() {
    isr_fired = false;
    radio.setDio1Action(onDio1);
    radio.startReceive();
}

// ── Helper: wait for DIO1 with timeout, return true if fired ─────────────────
static bool rx_wait(uint32_t timeout_ms) {
    uint32_t t0 = millis();
    while (!isr_fired && millis() - t0 < timeout_ms) yield();
    return isr_fired;
}

// ─────────────────────────────────────────────────────────────────────────────

#ifdef CAL_MASTER
// ═════════════════════════════════════════════════════════════════════════════
//  ALPHA (MASTER)
// ═════════════════════════════════════════════════════════════════════════════

enum AlphaState { A_LISTENING, A_LORA_LINK, A_RANGING_INFO };
static AlphaState a_state = A_LISTENING;
static int     a_miss     = 0;
static uint8_t a_ping_seq = 0;
static uint32_t a_last_ping = 0;

// Decoded Chimp telemetry (updated each exchange when PKT_TELEM received)
static struct {
    float   inst_rssi, rssi_sync, snr, rssi_corr, freq_err;
    uint8_t gain;
    bool    ok;
} g_chimp = {};

static float g_lora_rssi = 0.0f;
static float g_lora_snr  = 0.0f;

static void print_master_row(unsigned long t, float m, float die, float amb) {
    char s_amb[12];
    if (isnan(amb)) strcpy(s_amb, "NA");
    else snprintf(s_amb, sizeof(s_amb), "%.2f", amb);

    // First 10 always-present fields
    Serial.printf("%lu,%.4f,%.1f,%s,%.1f,%d,%.1f,%.1f,%.1f,%.0f",
        t, m, die, s_amb,
        g_rssi, g_gain_step, g_snr, g_rssi_sync,
        g_inst_rssi, g_freq_err_hz);

    // Last 8 conditional fields (empty when PKT_TELEM missed)
    if (g_chimp.ok) {
        Serial.printf(",%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%u,%.0f",
            g_lora_rssi, g_lora_snr,
            g_chimp.inst_rssi, g_chimp.rssi_sync, g_chimp.snr,
            g_chimp.rssi_corr, (unsigned)g_chimp.gain, g_chimp.freq_err);
    } else {
        Serial.print(",,,,,,,,");
    }
    Serial.println();
}

static void alpha_enter_ranging_info() {
    a_state = A_RANGING_INFO;
    a_miss  = 0;
    Serial.println("# MODE: RANGING_INFO");
    ble_notify("MODE: RANGING_INFO");
    Serial.println("t_ms,raw_m,die_c,amb_c,rssi_dbm,gain_step,snr_db,rssi_sync,"
                   "inst_rssi_dbm,freq_err_hz,lora_rssi_dbm,lora_snr_db,"
                   "chimp_inst_rssi_dbm,chimp_rssi_sync_dbm,chimp_snr_db,"
                   "chimp_rssi_corr_dbm,chimp_gain_step,chimp_freq_err_hz");
    apply_gain();
}

static void alpha_send_start() {
    for (int r = 0; r < 3; r++) {
        send_ctrl(PKT_START, (uint8_t)r);
        rx_arm();
        if (rx_wait(1000)) {
            uint8_t rx[16] = {};
            if (radio.readData(rx, 16) == RADIOLIB_ERR_NONE && rx[0] == PKT_START_ACK) {
                alpha_enter_ranging_info();
                return;
            }
        }
        Serial.printf("# START retry %d\n", r + 1);
    }
    Serial.println("# START failed — no ACK from Chimp");
    rx_arm();
}

static void alpha_link_lost() {
    Serial.println("# LINK LOST");
    ble_notify("LINK LOST");
    a_state = A_LISTENING;
    a_miss  = 0;
    rx_arm();
}

#endif  // CAL_MASTER

// ─────────────────────────────────────────────────────────────────────────────

#ifndef CAL_MASTER
// ═════════════════════════════════════════════════════════════════════════════
//  CHIMP (SLAVE)
// ═════════════════════════════════════════════════════════════════════════════

enum ChimpState { C_SEEKING, C_LORA_LINK, C_RANGING_INFO };
static ChimpState c_state    = C_SEEKING;
static int        c_miss     = 0;
static uint8_t    c_req_seq  = 0;
static uint32_t   c_last_req = 0;
static uint32_t   c_last_rx  = 0;   // timestamp of last received packet in LORA_LINK

static void chimp_link_lost() {
    Serial.println("# LINK LOST — returning to SEEK");
    c_state    = C_SEEKING;
    c_miss     = 0;
    c_last_req = 0;
}

#endif  // !CAL_MASTER

// ─────────────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(3000);

    Wire1.begin(BME_SDA, BME_SCL);
    bme_ok = bme.begin(BME_ADDR, &Wire1);
    if (!bme_ok) bme_ok = bme.begin(0x77, &Wire1);
    Serial.printf("# BME280: %s\n", bme_ok ? "OK" : "NA");

    SPI.begin(SPI_SCK, RADIO_MISO, RADIO_MOSI, RADIO_NSS);

    Serial.println("\n=== SX1280 Ranging Calibration / LoRa Link ===");
    Serial.printf("# RF: %.0f MHz  BW=%.0f kHz  SF%d  %d dBm\n",
        CAL_FREQ_MHZ, CAL_BW_KHZ, CAL_SF, CAL_TX_DBM);
    Serial.printf("# CAL[2][4]: %u\n", CAL_TABLE[2][4]);

    int state = radio.begin(CAL_FREQ_MHZ, CAL_BW_KHZ, CAL_SF, 5, 0x12, CAL_TX_DBM);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[FATAL] radio.begin() failed: %d\n", state);
        while (true) delay(1000);
    }
    Serial.println("# Radio OK");
    radio.setDio1Action(onDio1);
    setup_gain();

#ifdef CAL_MASTER
    Serial.println("# Role: ALPHA (master)");
    Serial.println("# Listening for Chimp CONNECT_REQ…");
    Serial.println("# Commands: start / stop");
    rx_arm();
#else
    Serial.println("# Role: CHIMP (slave)");
    Serial.println("# Seeking Alpha…");
    c_last_req = 0;
    c_last_rx  = millis();
#endif
}

// ─────────────────────────────────────────────────────────────────────────────

void loop() {
#ifdef CAL_MASTER
    // ── ALPHA STATE MACHINE ───────────────────────────────────────────────────

    switch (a_state) {

    case A_LISTENING: {
        // Waiting for Chimp to broadcast PKT_CONNECT_REQ
        if (!isr_fired) return;
        isr_fired = false;
        uint8_t rx[16] = {};
        if (radio.readData(rx, 16) == RADIOLIB_ERR_NONE && rx[0] == PKT_CONNECT_REQ) {
            float rssi = radio.getRSSI();
            float snr  = radio.getSNR();
            send_ctrl(PKT_CONNECT_ACK, rx[1]);
            a_state = A_LORA_LINK;
            a_miss  = 0;
            a_last_ping = 0;
            char msg[64];
            snprintf(msg, sizeof(msg), "# LINK ESTABLISHED rssi=%.1f snr=%.1f", rssi, snr);
            Serial.println(msg);
            ble_notify(msg);
            apply_gain();
        }
        rx_arm();
        break;
    }

    case A_LORA_LINK: {
        // Heartbeat: ping Chimp every 500 ms, log RF metrics
        if (millis() - a_last_ping >= 500) {
            a_last_ping = millis();

            send_ctrl(PKT_PING, a_ping_seq++);
            rx_arm();
            bool got_pong = rx_wait(400);

            if (got_pong) {
                uint8_t rx[16] = {};
                if (radio.readData(rx, 16) == RADIOLIB_ERR_NONE && rx[0] == PKT_PONG) {
                    float rssi = radio.getRSSI();
                    float snr  = radio.getSNR();
                    uint32_t rtt = millis() - a_last_ping;
                    char msg[80];
                    snprintf(msg, sizeof(msg), "# LINK seq=%d rtt=%lums rssi=%.1f snr=%.1f",
                             (int)a_ping_seq - 1, rtt, rssi, snr);
                    Serial.println(msg);
                    ble_notify(msg);
                    a_miss = 0;
                } else {
                    a_miss++;
                }
            } else {
                a_miss++;
            }

            if (a_miss >= 5) { alpha_link_lost(); return; }

            // Re-arm RX between pings
            rx_arm();
        }

        // Check for user command
        String cmd = poll_cmd();
        if (cmd.length() == 0) cmd = ble_read_cmd();
        if (cmd == "start") {
            alpha_send_start();
        }
        break;
    }

    case A_RANGING_INFO: {
        // Continuous ranging exchange — one CSV row per exchange
        float m   = do_ranging(true);
        float die = (float)temperatureRead();
        float amb = bme_ok ? bme.readTemperature() : NAN;
        unsigned long t = millis();

        if (isnan(m)) {
            a_miss++;
            if (a_miss >= 5) { alpha_link_lost(); return; }
            delay(20);
            return;
        }
        a_miss = 0;

        // radio.begin() resets to LoRa mode in ~4ms on T3-S3 without hardware reset.
        radio.begin(CAL_FREQ_MHZ, CAL_BW_KHZ, CAL_SF, 5, 0x12, CAL_TX_DBM);
        apply_gain();

        // Handshake: signal Chimp we're ready to receive its TELEM, then listen.
        // Chimp waits in LoRa RX for this trigger, sends TELEM immediately after.
        {
            PktCtrl p = { PKT_TELEM_REQ, 0, (uint32_t)millis() };
            radio.transmit((uint8_t*)&p, sizeof(p));
        }
        apply_gain();

        g_chimp = {};
        rx_arm();
        if (rx_wait(500)) {
            PktTelem pl{};
            int rderr = radio.readData((uint8_t*)&pl, sizeof(pl));
            if (rderr == RADIOLIB_ERR_NONE && pl.type == PKT_TELEM) {
                g_lora_rssi = radio.getRSSI();
                g_lora_snr  = radio.getSNR();
                auto u2f = [](uint8_t r) -> float { return r ? -(float)r / 2.0f : 0.0f; };
                g_chimp.inst_rssi  = u2f(pl.inst_rssi_raw);
                g_chimp.rssi_sync  = u2f(pl.rssi_sync_raw);
                g_chimp.snr        = (float)(int8_t)pl.snr_raw / 4.0f;
                g_chimp.rssi_corr  = u2f(pl.rssi_corr_raw);
                g_chimp.gain       = pl.gain_step;
                g_chimp.freq_err   = (float)(int16_t)((pl.freq_hi << 8) | pl.freq_lo)
                                     * (1625000.0f / 65536.0f);
                g_chimp.ok = true;
            } else {
                Serial.printf("# TELEM rx: err=%d type=0x%02X\n", rderr, (unsigned)pl.type);
            }
        } else {
            Serial.println("# TELEM rx: timeout");
        }
        apply_gain();

        // Log row — all fields, immediately
        if (m < -100.0f || m > 2000.0f)
            Serial.printf("# outlier %.4f\n", m);
        print_master_row(t, m, die, amb);

        // Check for stop command
        String cmd = poll_cmd();
        if (cmd.length() == 0) cmd = ble_read_cmd();
        if (cmd == "stop") {
            send_ctrl(PKT_STOP, 0);
            rx_arm();
            rx_wait(500);   // wait briefly for STOP_ACK (consume or ignore)
            if (isr_fired) { uint8_t rx[16]={}; radio.readData(rx, 16); }
            a_state = A_LORA_LINK;
            a_miss  = 0;
            a_last_ping = 0;
            apply_gain();
            Serial.println("# MODE: LORA_LINK");
            ble_notify("MODE: LORA_LINK");
            rx_arm();
        }

        break;
    }

    }  // switch a_state

#else
    // ── CHIMP STATE MACHINE ───────────────────────────────────────────────────

    switch (c_state) {

    case C_SEEKING: {
        // Broadcast CONNECT_REQ every 500 ms; listen for CONNECT_ACK
        if (millis() - c_last_req >= 500) {
            c_last_req = millis();
            send_ctrl(PKT_CONNECT_REQ, c_req_seq++);
            Serial.printf("# SEEK seq=%d\n", (int)c_req_seq - 1);

            rx_arm();
            if (rx_wait(400)) {
                uint8_t rx[16] = {};
                if (radio.readData(rx, 16) == RADIOLIB_ERR_NONE
                        && rx[0] == PKT_CONNECT_ACK) {
                    c_state   = C_LORA_LINK;
                    c_miss    = 0;
                    c_last_rx = millis();
                    Serial.println("# LINKED to Alpha");
                    apply_gain();
                    rx_arm();   // listen for first PING
                    return;
                }
            }
            // No ACK — stay in C_SEEKING, loop will retry after 500 ms
        }
        break;
    }

    case C_LORA_LINK: {
        // Connection-loss timeout: if no packet received for 3 s, return to seeking
        if (millis() - c_last_rx > 3000) {
            chimp_link_lost();
            return;
        }

        if (!isr_fired) return;   // stay in RX, nothing to do yet
        isr_fired = false;

        uint8_t rx[16] = {};
        if (radio.readData(rx, 16) != RADIOLIB_ERR_NONE) {
            rx_arm(); return;
        }
        float rssi = radio.getRSSI();
        float snr  = radio.getSNR();
        c_last_rx  = millis();
        c_miss     = 0;

        if (rx[0] == PKT_PING) {
            send_ctrl(PKT_PONG, rx[1]);
            Serial.printf("# LINK rssi=%.1f snr=%.1f\n", rssi, snr);
            rx_arm();

        } else if (rx[0] == PKT_START) {
            send_ctrl(PKT_START_ACK, rx[1]);
            c_state = C_RANGING_INFO;
            c_miss  = 0;
            Serial.println("# MODE: RANGING_INFO");
            Serial.println("t_ms,die_c,amb_c,rssi_dbm,gain_step,snr_db,rssi_sync,"
                           "inst_rssi_dbm,freq_err_hz");
            apply_gain();
            // Do NOT re-arm startReceive — next loop() enters do_ranging(false)

        } else {
            rx_arm();   // unknown packet type, re-arm
        }
        break;
    }

    case C_RANGING_INFO: {
        // Continuous ranging + telemetry transmit to Alpha
        float result = do_ranging(false);
        bool exchange_ok = !isnan(result);  // 0.0f = slave exchange completed

        if (!exchange_ok) {
            c_miss++;
            if (c_miss >= 5) { chimp_link_lost(); return; }
            break;  // skip TELEM/log on timeout; retry ranging next loop()
        }
        c_miss = 0;

        // radio.begin() resets to LoRa mode in ~4ms on T3-S3 without hardware reset.
        radio.begin(CAL_FREQ_MHZ, CAL_BW_KHZ, CAL_SF, 5, 0x12, CAL_TX_DBM);
        apply_gain();

        // Handshake: wait for Alpha's PKT_TELEM_REQ before sending TELEM.
        // This eliminates the fixed-delay timing race — we only transmit when
        // Alpha has confirmed it is ready to receive.
        rx_arm();
        if (rx_wait(500)) {
            uint8_t req[16] = {};
            if (radio.readData(req, 16) == RADIOLIB_ERR_NONE && req[0] == PKT_TELEM_REQ) {
                // Alpha is now entering RX — give it a moment to arm
                apply_gain();
                delay(30);
                int16_t fe16 = (int16_t)(g_freq_err_hz * 65536.0f / 1625000.0f);
                PktTelem pl{};
                pl.type          = PKT_TELEM;
                pl.inst_rssi_raw = (g_inst_rssi  < 0.f) ? (uint8_t)(-g_inst_rssi  * 2.f) : 0;
                pl.rssi_sync_raw = (g_rssi_sync  < 0.f) ? (uint8_t)(-g_rssi_sync  * 2.f) : 0;
                pl.snr_raw       = (int8_t)(g_snr * 4.f);
                pl.rssi_corr_raw = (g_rssi       < 0.f) ? (uint8_t)(-g_rssi       * 2.f) : 0;
                pl.gain_step     = g_gain_step;
                pl.freq_hi       = (uint8_t)(((uint16_t)fe16 >> 8) & 0xFF);
                pl.freq_lo       = (uint8_t)((uint16_t)fe16 & 0xFF);
                radio.transmit((uint8_t*)&pl, sizeof(pl));
            }
        }
        apply_gain();

        // Log slave CSV row immediately
        float die = (float)temperatureRead();
        float amb = bme_ok ? bme.readTemperature() : NAN;
        if (isnan(amb))
            Serial.printf("%lu,%.1f,NA,%.1f,%d,%.1f,%.1f,%.1f,%.0f\n",
                millis(), die, g_rssi, g_gain_step, g_snr, g_rssi_sync,
                g_inst_rssi, g_freq_err_hz);
        else
            Serial.printf("%lu,%.1f,%.2f,%.1f,%d,%.1f,%.1f,%.1f,%.0f\n",
                millis(), die, amb, g_rssi, g_gain_step, g_snr, g_rssi_sync,
                g_inst_rssi, g_freq_err_hz);

        // Brief RX window to catch PKT_STOP from Alpha
        rx_arm();
        if (rx_wait(30)) {
            uint8_t rx[16] = {};
            if (radio.readData(rx, 16) == RADIOLIB_ERR_NONE && rx[0] == PKT_STOP) {
                send_ctrl(PKT_STOP_ACK, rx[1]);
                c_state   = C_LORA_LINK;
                c_miss    = 0;
                c_last_rx = millis();
                Serial.println("# MODE: LORA_LINK");
                apply_gain();
                rx_arm();
                return;
            }
        }
        // No STOP — go straight back into do_ranging(false) next loop()
        break;
    }

    }  // switch c_state

#endif  // !CAL_MASTER
}
