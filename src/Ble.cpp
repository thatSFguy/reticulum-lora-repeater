// src/Ble.cpp — BLE UART (Nordic UART Service) subsystem.
//
// When HAS_BLE == 1 and the CONFIG_FLAG_BT_ENABLED flag is set, this
// module starts the Adafruit Bluefruit stack with NUS so a phone can
// send the same serial console commands over Bluetooth. A small
// BlePrint adapter bridges the Print interface to the NUS TX
// characteristic, making the refactored SerialConsole dispatch work
// transparently over BLE.
//
// When HAS_BLE == 0 the entire file compiles to empty stubs.

#include "Ble.h"

#if HAS_BLE

#include <bluefruit.h>
#include "SerialConsole.h"
#include <stdio.h>

namespace rlr { namespace ble {

// ---- BLE UART service instance ------------------------------------

static BLEUart s_bleuart;

// ---- BlePrint adapter ---------------------------------------------
// Wraps BLEUart in a Print-compatible interface so dispatch_line()
// can write responses over BLE exactly as it does over USB Serial.

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

// ---- state --------------------------------------------------------

static constexpr size_t LINE_MAX = 192;
static char     s_line[LINE_MAX];
static size_t   s_len   = 0;
static bool     s_active = false;
static bool     s_connected = false;
static uint16_t s_conn_handle = BLE_CONN_HANDLE_INVALID;

// ---- BLE callbacks ------------------------------------------------

static void _on_connect(uint16_t conn_handle) {
    s_conn_handle = conn_handle;
    s_connected = false;  // not yet — wait for security
    Serial.println("BLE: connected (awaiting pairing)");
}

static void _on_disconnect(uint16_t conn_handle, uint8_t reason) {
    (void)reason;
    if (s_conn_handle == conn_handle) {
        s_conn_handle = BLE_CONN_HANDLE_INVALID;
        s_connected = false;
        Serial.println("BLE: disconnected");
    }
}

static bool s_require_mitm = false;

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
        // Level 3+ = encrypted with MITM (passkey was verified)
        // Level 2  = encrypted without MITM ("Just Works" — no PIN prompt)
        if (mode.sm >= 1 && mode.lv >= 3) {
            s_connected = true;
            Serial.println("BLE: MITM authenticated, ready");
        } else {
            Serial.println("BLE: insufficient security, disconnecting");
            Serial.println("BLE: (phone may need to 'Forget' this device first)");
            Bluefruit.disconnect(conn_handle);
        }
    } else {
        s_connected = true;
        Serial.println("BLE: connected (no PIN required)");
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

// ---- advertising setup --------------------------------------------

static void _start_advertising() {
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addTxPower();
    Bluefruit.Advertising.addService(s_bleuart);
    Bluefruit.ScanResponse.addName();
    Bluefruit.Advertising.restartOnDisconnect(true);
    Bluefruit.Advertising.setInterval(160, 240);  // 100-150 ms
    Bluefruit.Advertising.setFastTimeout(30);
    Bluefruit.Advertising.start(0);  // advertise forever
}

// ---- public API ---------------------------------------------------

bool init(const Config& cfg) {
    if (!(cfg.flags & CONFIG_FLAG_BT_ENABLED)) {
        Serial.println("BLE: disabled by config");
        s_active = false;
        return false;
    }

    Bluefruit.begin();
    bond_clear_prph();        // clear stale bonds so PIN is always prompted
    Bluefruit.setTxPower(4);  // moderate TX power for config range

    // Use display_name as BLE device name (truncated to 20 chars)
    char name[21];
    strncpy(name, cfg.display_name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    Bluefruit.setName(name);

    // Security setup — setPIN() internally sets mitm=1, lesc=0,
    // io_caps=DISPLAY_ONLY and registers the static passkey with
    // the SoftDevice. Must be called AFTER Bluefruit.begin().
    s_require_mitm = (cfg.bt_pin != 0);
    if (cfg.bt_pin != 0) {
        char pin_str[7];
        snprintf(pin_str, sizeof(pin_str), "%06lu", (unsigned long)cfg.bt_pin);
        Bluefruit.Security.setPIN(pin_str);
        Bluefruit.Security.setPairPasskeyCallback(_on_pairing_passkey);
        Bluefruit.Security.setPairCompleteCallback(_on_pairing_complete);

        // Require encrypted+MITM access to NUS characteristics
        s_bleuart.setPermission(SECMODE_ENC_WITH_MITM, SECMODE_ENC_WITH_MITM);

        Serial.print("BLE: PIN set to ");
        Serial.println(pin_str);
    }

    Bluefruit.Security.setSecuredCallback(_on_secured);

    // Connection callbacks
    Bluefruit.Periph.setConnectCallback(_on_connect);
    Bluefruit.Periph.setDisconnectCallback(_on_disconnect);

    s_bleuart.begin();

    _start_advertising();

    s_len    = 0;
    s_active = true;
    Serial.println("BLE: NUS advertising started");
    return true;
}

void tick() {
    if (!s_active) return;

    while (s_bleuart.available()) {
        int c = s_bleuart.read();
        if (c < 0) break;

        if (c == '\r' || c == '\n') {
            if (s_len > 0) {
                s_line[s_len] = '\0';
                rlr::serial_console::dispatch_line(s_line, s_ble_print);
                s_len = 0;
            }
            continue;
        }

        if (s_len < LINE_MAX - 1) {
            s_line[s_len++] = (char)c;
        } else {
            s_len = 0;
            s_ble_print.println("ERR: line too long");
        }
    }
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
