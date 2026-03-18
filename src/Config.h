#pragma once

// =======================
// BikeSens Gateway Config
// =======================

// -------- Identity --------
static const char* DEFAULT_GATEWAY_ID = "gw01";
static const char* FW_VERSION = "1.0.3";

// -------- Firebase --------
#include "LoginData.h" //ignore git

#define FIREBASE_DB_URL firebase_url
#define FIREBASE_API_KEY firebase_api_key
#define FIREBASE_EMAIL firebase_email
#define FIREBASE_PASSWORD firebase_password

// Insecure TLS (development only). If 1, certificate validation is disabled.
#define FIREBASE_TLS_INSECURE 1
#define FIREBASE_HTTP_TIMEOUT_MS 15000

// If 1, perform Firebase login before starting BLE scan (can reduce first-send delays).
#define FIREBASE_LOGIN_BEFORE_BLE 1

// -------- BLE filter --------
// Manufacturer data header: [company_id LE(2)][prefix0][prefix1]
#define BLE_COMPANY_ID 0xBAFF
#define BLE_PREFIX0 0xB1
#define BLE_PREFIX1 0x6B
// 0: passive scan, 1: active scan
#define BLE_ACTIVE_SCAN 0

// -------- Buffer & batching --------
static const uint16_t BUFFER_MAX = 400;
static const uint16_t BATCH_SIZE = 3; // target: 30

// Send when no new data for this time (ms)
static const uint32_t IDLE_TIMEOUT_MS = 5UL * 60UL * 1000UL;

// -------- Wi-Fi / reconnect / AP --------
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 30UL * 1000UL;
static const uint32_t RECONNECT_INTERVAL_MS = 60UL * 1000UL;
static const uint32_t STATUS_INTERVAL_MS = 5UL * 60UL * 1000UL;
static const uint32_t OTA_RETRY_INTERVAL_MS = 5UL * 60UL * 1000UL;

static const char* AP_SSID = "BikeSense-Gateway";
static const char* AP_PASS = "bikesense123";

// -------- Dedup cache --------
static constexpr uint16_t RECENT_KEYS_MAX = 512; // must be power-of-two? (no, but keep >= 256)

// -------- Logging --------
#define LOG_ENABLED 1
#define BLE_DEBUG_RAW 0
#define BLE_SCAN_DEBUG 1
#define LOG_BAUD 115200

// -------- Radio coexistence mode --------
// 0: BLE + WiFi can run together (default)
// 1: BLE OR WiFi (no-conflict) -> BLE is stopped during WiFi send; WiFi is enabled only for sending
#define RADIO_MODE_BLE_WIFI 0
#define RADIO_MODE_EXCLUSIVE 1
// Choose mode:
#define GATEWAY_RADIO_MODE RADIO_MODE_BLE_WIFI

// -------- Time --------
static const long GMT_OFFSET_SEC = 0;
static const int  DST_OFFSET_SEC = 0;
static const char* NTP_SERVER = "pool.ntp.org";
