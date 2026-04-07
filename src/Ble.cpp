// src/Ble.cpp — BLE subsystem with custom GATT service for config
// and NUS for read-only log stream.
//
// Custom RLR Service:
//   CONFIG characteristic (read/write, 160 bytes) — pipe-delimited config
//   COMMIT characteristic (write, 1 byte)         — write 0x01 to persist + reboot
//   COMMAND characteristic (write, 64 bytes)       — text commands (ANNOUNCE, STATUS, etc.)
//
// NUS (Nordic UART Service):
//   TX only — firmware log stream (alive markers, command responses)
//
// When HAS_BLE == 0 the entire file compiles to empty stubs.

#include "Ble.h"

#if HAS_BLE

#include <bluefruit.h>
#include "SerialConsole.h"
#include <stdio.h>
#include <string.h>

namespace rlr { namespace ble {

// ---- Custom RLR Service UUIDs ------------------------------------
// Base: rlr00000-7272-4c52-a5a5-000000000000
// Service:    rlr00001-...
// CONFIG chr: rlr00002-...
// COMMIT chr: rlr00003-...
// COMMAND chr: rlr00004-...

static const uint8_t RLR_UUID_SERVICE[] = {
    0x00, 0x00, 0x00, 0x00, 0xa5, 0xa5, 0x52, 0x4c,
    0x72, 0x72, 0x00, 0x00, 0x01, 0x00, 0x72, 0x6c
};
static const uint8_t RLR_UUID_CONFIG[] = {
    0x00, 0x00, 0x00, 0x00, 0xa5, 0xa5, 0x52, 0x4c,
    0x72, 0x72, 0x00, 0x00, 0x02, 0x00, 0x72, 0x6c
};
static const uint8_t RLR_UUID_COMMIT[] = {
    0x00, 0x00, 0x00, 0x00, 0xa5, 0xa5, 0x52, 0x4c,
    0x72, 0x72, 0x00, 0x00, 0x03, 0x00, 0x72, 0x6c
};
static const uint8_t RLR_UUID_COMMAND[] = {
    0x00, 0x00, 0x00, 0x00, 0xa5, 0xa5, 0x52, 0x4c,
    0x72, 0x72, 0x00, 0x00, 0x04, 0x00, 0x72, 0x6c
};

// ---- Service + characteristic instances --------------------------

static BLEService        s_rlr_service(RLR_UUID_SERVICE);
static BLECharacteristic s_config_chr(RLR_UUID_CONFIG);
static BLECharacteristic s_commit_chr(RLR_UUID_COMMIT);
static BLECharacteristic s_command_chr(RLR_UUID_COMMAND);

// NUS for log stream (TX only)
static BLEUart s_bleuart;

// ---- BlePrint adapter (log stream via NUS TX) --------------------

class BlePrint : public Print {
public:
    size_t write(uint8_t c) override {
        return s_bleuart.write(c);
    }
    size_t write(const uint8_t* buf, size_t len) override {
        return s_bleuart.write(buf, len);
    }
};

static BlePrint  s_ble_print;

// ---- state -------------------------------------------------------

static bool     s_active = false;
static bool     s_connected = false;
static uint16_t s_conn_handle = BLE_CONN_HANDLE_INVALID;
static bool     s_require_mitm = false;
static Config*  s_cfg_ptr = nullptr;  // pointer to live config

// ---- Config pipe buffer ------------------------------------------

static constexpr size_t PIPE_BUF_SIZE = 160;

// Build the pipe string into a static buffer, return length.
static size_t _build_pipe(char* buf, size_t bufsize) {
    // Use a small Print adapter that writes to the buffer
    struct BufPrint : public Print {
        char* p;
        size_t remaining;
        size_t total;
        BufPrint(char* b, size_t s) : p(b), remaining(s), total(0) {}
        size_t write(uint8_t c) override {
            if (remaining > 0) { *p++ = (char)c; remaining--; }
            total++;
            return 1;
        }
        size_t write(const uint8_t* b, size_t len) override {
            for (size_t i = 0; i < len; i++) write(b[i]);
            return len;
        }
    };

    BufPrint bp(buf, bufsize - 1);  // leave room for NUL
    config::print_fields_pipe(rlr::serial_console::staging(), bp);
    size_t n = bp.total < bufsize ? bp.total : bufsize - 1;
    buf[n] = '\0';
    return n;
}

// ---- GATT callbacks ----------------------------------------------

static void _on_config_write(uint16_t conn_hdl, BLECharacteristic* chr, uint8_t* data, uint16_t len) {
    (void)conn_hdl;
    (void)chr;

    // Parse pipe-delimited config string and set each field.
    // Field order must match PIPE_FIELDS in console.js and print_fields_pipe.
    static const char* keys[] = {
        "display_name", "freq_hz", "bw_hz", "sf", "cr", "txp_dbm",
        "batt_mult", "tele_interval_ms", "lxmf_interval_ms",
        "telemetry", "lxmf", "heartbeat", "bt_enabled", "bt_pin",
        "latitude", "longitude", "altitude"
    };
    static constexpr size_t NUM_KEYS = sizeof(keys) / sizeof(keys[0]);

    // Copy to mutable NUL-terminated buffer
    char buf[PIPE_BUF_SIZE];
    size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, data, n);
    buf[n] = '\0';

    // Split on '|' and apply each field
    Config& staging = rlr::serial_console::staging();
    char* p = buf;
    for (size_t i = 0; i < NUM_KEYS; i++) {
        char* next = strchr(p, '|');
        if (next) *next = '\0';

        const char* err = config::set_field(staging, keys[i], p);
        if (err) {
            Serial.print("BLE CONFIG write: ");
            Serial.print(keys[i]);
            Serial.print(" error: ");
            Serial.println(err);
        }

        if (next) p = next + 1;
        else break;
    }
    Serial.println("BLE: config staged via GATT write");
}

static void _on_commit_write(uint16_t conn_hdl, BLECharacteristic* chr, uint8_t* data, uint16_t len) {
    (void)conn_hdl;
    (void)chr;
    if (len < 1 || data[0] != 0x01) return;

    Config& staging = rlr::serial_console::staging();
    if (!config::validate(staging)) {
        Serial.println("BLE COMMIT: validation failed");
        return;
    }
    if (!config::save(staging)) {
        Serial.println("BLE COMMIT: save failed");
        return;
    }
    Serial.println("BLE COMMIT: saved, rebooting...");
    Serial.flush();
    delay(50);
    NVIC_SystemReset();
}

static void _on_command_write(uint16_t conn_hdl, BLECharacteristic* chr, uint8_t* data, uint16_t len) {
    (void)conn_hdl;
    (void)chr;

    char cmd[64];
    size_t n = len < sizeof(cmd) - 1 ? len : sizeof(cmd) - 1;
    memcpy(cmd, data, n);
    cmd[n] = '\0';

    // Strip trailing whitespace/newlines
    while (n > 0 && (cmd[n-1] == '\r' || cmd[n-1] == '\n' || cmd[n-1] == ' ')) {
        cmd[--n] = '\0';
    }

    Serial.print("BLE CMD: ");
    Serial.println(cmd);

    // Dispatch through the serial console command parser.
    // Responses go to NUS TX (log stream).
    rlr::serial_console::dispatch_line(cmd, s_ble_print);
}

// ---- Connection callbacks ----------------------------------------

static void _on_connect(uint16_t conn_handle) {
    s_conn_handle = conn_handle;
    s_connected = false;
    Serial.println("BLE: connected (awaiting pairing)");
}

static void _on_disconnect(uint16_t conn_handle, uint8_t reason) {
    if (s_conn_handle == conn_handle) {
        s_conn_handle = BLE_CONN_HANDLE_INVALID;
        s_connected = false;
        Serial.print("BLE: disconnected, reason=0x");
        Serial.println(reason, HEX);
    }
}

static void _on_secured(uint16_t conn_handle) {
    if (s_conn_handle != conn_handle) return;

    BLEConnection* conn = Bluefruit.Connection(conn_handle);
    if (!conn) return;

    ble_gap_conn_sec_mode_t mode = conn->getSecureMode();
    Serial.print("BLE: secured sm=");
    Serial.print(mode.sm);
    Serial.print(" lv=");
    Serial.println(mode.lv);

    if (s_require_mitm) {
        if (mode.sm >= 1 && mode.lv >= 3) {
            s_connected = true;
            Serial.println("BLE: MITM authenticated, ready");
        } else {
            Serial.println("BLE: insufficient security, disconnecting");
            Bluefruit.disconnect(conn_handle);
        }
    } else {
        s_connected = true;
        Serial.println("BLE: connected (no PIN required)");
    }

    // Request relaxed connection parameters so LoRa TX (which blocks
    // the MCU for tens of ms) doesn't trigger a supervision timeout.
    // Units: interval = 1.25ms, timeout = 10ms.
    if (s_connected) {
        ble_gap_conn_params_t params;
        params.min_conn_interval = 12;   // 15ms
        params.max_conn_interval = 24;   // 30ms
        params.slave_latency     = 4;    // skip up to 4 events
        params.conn_sup_timeout  = 400;  // 4000ms (4s timeout)
        sd_ble_gap_conn_param_update(conn_handle, &params);
    }

    // Update the CONFIG characteristic value so a read returns current config
    if (s_connected) {
        char pipe[PIPE_BUF_SIZE];
        size_t n = _build_pipe(pipe, sizeof(pipe));
        s_config_chr.write(pipe, n);
    }
}

static bool _on_pairing_passkey(uint16_t conn_handle, uint8_t const passkey[6], bool match_request) {
    (void)conn_handle;
    (void)match_request;
    Serial.print("BLE: passkey displayed: ");
    Serial.write(passkey, 6);
    Serial.println();
    return true;
}

static void _on_pairing_complete(uint16_t conn_handle, uint8_t auth_status) {
    Serial.print("BLE: pairing complete, status=0x");
    Serial.println(auth_status, HEX);
    if (s_conn_handle == conn_handle && auth_status != BLE_GAP_SEC_STATUS_SUCCESS) {
        Serial.println("BLE: pairing failed, disconnecting");
        Bluefruit.disconnect(conn_handle);
    }
}

// ---- advertising setup -------------------------------------------

static void _start_advertising() {
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addTxPower();
    Bluefruit.Advertising.addService(s_rlr_service);
    Bluefruit.ScanResponse.addName();
    Bluefruit.Advertising.restartOnDisconnect(true);
    Bluefruit.Advertising.setInterval(160, 240);
    Bluefruit.Advertising.setFastTimeout(30);
    Bluefruit.Advertising.start(0);
}

// ---- public API --------------------------------------------------

bool init(const Config& cfg) {
    if (!(cfg.flags & CONFIG_FLAG_BT_ENABLED)) {
        Serial.println("BLE: disabled by config");
        s_active = false;
        return false;
    }

    s_cfg_ptr = const_cast<Config*>(&cfg);

    Bluefruit.begin();
    Bluefruit.setTxPower(4);

    // Set PPCP so the phone knows our preferred connection parameters.
    // Relaxed timing prevents disconnects during LoRa TX blocking.
    {
        ble_gap_conn_params_t ppcp;
        ppcp.min_conn_interval = 12;   // 15ms
        ppcp.max_conn_interval = 24;   // 30ms
        ppcp.slave_latency     = 4;    // skip up to 4 connection events
        ppcp.conn_sup_timeout  = 400;  // 4000ms supervision timeout
        sd_ble_gap_ppcp_set(&ppcp);
    }

    // Device name
    char name[21];
    strncpy(name, cfg.display_name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    Bluefruit.setName(name);

    // Security
    s_require_mitm = (cfg.bt_pin != 0);
    SecureMode_t sec_read  = SECMODE_OPEN;
    SecureMode_t sec_write = SECMODE_OPEN;
    if (cfg.bt_pin != 0) {
        char pin_str[7];
        snprintf(pin_str, sizeof(pin_str), "%06lu", (unsigned long)cfg.bt_pin);
        Bluefruit.Security.setPIN(pin_str);
        Bluefruit.Security.setPairPasskeyCallback(_on_pairing_passkey);
        Bluefruit.Security.setPairCompleteCallback(_on_pairing_complete);
        sec_read  = SECMODE_ENC_WITH_MITM;
        sec_write = SECMODE_ENC_WITH_MITM;
        Serial.print("BLE: PIN set to ");
        Serial.println(pin_str);
    }

    Bluefruit.Security.setSecuredCallback(_on_secured);
    Bluefruit.Periph.setConnectCallback(_on_connect);
    Bluefruit.Periph.setDisconnectCallback(_on_disconnect);

    // ---- Custom RLR Service ----
    s_rlr_service.begin();

    // CONFIG characteristic: read + write, up to 160 bytes
    s_config_chr.setProperties(CHR_PROPS_READ | CHR_PROPS_WRITE);
    s_config_chr.setPermission(sec_read, sec_write);
    s_config_chr.setMaxLen(PIPE_BUF_SIZE);
    s_config_chr.setWriteCallback(_on_config_write);
    s_config_chr.setUserDescriptor("Config");
    s_config_chr.begin();

    // Populate initial config value
    {
        char pipe[PIPE_BUF_SIZE];
        size_t n = _build_pipe(pipe, sizeof(pipe));
        s_config_chr.write(pipe, n);
    }

    // COMMIT characteristic: write-only, 1 byte (0x01 = commit + reboot)
    s_commit_chr.setProperties(CHR_PROPS_WRITE);
    s_commit_chr.setPermission(SECMODE_NO_ACCESS, sec_write);
    s_commit_chr.setFixedLen(1);
    s_commit_chr.setWriteCallback(_on_commit_write);
    s_commit_chr.setUserDescriptor("Commit");
    s_commit_chr.begin();

    // COMMAND characteristic: write-only, up to 64 bytes
    s_command_chr.setProperties(CHR_PROPS_WRITE);
    s_command_chr.setPermission(SECMODE_NO_ACCESS, sec_write);
    s_command_chr.setMaxLen(64);
    s_command_chr.setWriteCallback(_on_command_write);
    s_command_chr.setUserDescriptor("Command");
    s_command_chr.begin();

    // ---- NUS for log stream (TX only) ----
    if (cfg.bt_pin != 0) {
        s_bleuart.setPermission(sec_read, SECMODE_NO_ACCESS);
    }
    s_bleuart.begin();

    _start_advertising();

    s_active = true;
    Serial.println("BLE: GATT service + NUS log stream started");
    return true;
}

void tick() {
    // No polling needed — all config operations are GATT callback-driven.
    // NUS TX is write-only from our side (log stream from dispatch_line).
}

bool active() {
    return s_active;
}

}} // namespace rlr::ble

#else // !HAS_BLE

namespace rlr { namespace ble {
    bool init(const Config&) { return false; }
    void tick() {}
    bool active() { return false; }
}} // namespace rlr::ble

#endif // HAS_BLE
