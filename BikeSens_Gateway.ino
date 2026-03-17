#include <Arduino.h>
#include <WiFi.h>

#include "src/Config.h"
#include "src/Logger.h"
#include "src/WiFiConfigManager.h"
#include "src/TimeSync.h"
#include "src/RecordBuffer.h"
#include "src/RecentKeys.h"
#include "src/BleScanner.h"
#include "src/FirebaseRest.h"

static WiFiConfigManager wifiCfg;
static RecordBuffer buffer;
static RecentKeys recent;
static BleScanner ble;
static FirebaseRest fb;

static uint32_t lastRxMs = 0;
static uint32_t lastReconnectAttemptMs = 0;
static bool wifiFrozen = false;
static bool timeSynced = false;

static void wifiFreeze() {
  if (WiFi.getMode() == WIFI_OFF) {
    wifiFrozen = true;
    return;
  }
  LOGI("WiFi OFF");
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(30);
  wifiFrozen = true;
}

static bool wifiThawAndConnect(uint32_t timeoutMs) {
  wifiFrozen = false;
  if (WiFi.status() == WL_CONNECTED) return true;
  if (!wifiCfg.hasSavedWiFi()) return false;
  bool ok = wifiCfg.connectSaved(timeoutMs);
  return ok;
}

static void ensureWiFiConnected() {
  // In exclusive (no-conflict) mode we only enable WiFi when sending.
  if (GATEWAY_RADIO_MODE == RADIO_MODE_EXCLUSIVE) return;

  if (WiFi.status() == WL_CONNECTED) return;

  // In runtime, don't start AP; only try periodic reconnect.
  uint32_t now = millis();
  if (now - lastReconnectAttemptMs < RECONNECT_INTERVAL_MS) return;
  lastReconnectAttemptMs = now;

  if (!wifiCfg.hasSavedWiFi()) return;
  LOGW("WiFi disconnected -> reconnect attempt");
  wifiCfg.connectSaved(WIFI_CONNECT_TIMEOUT_MS);
}

static void trySendIfNeeded() {
  if (buffer.empty()) return;

  // Exclusive mode: only enable WiFi for sending; stop BLE during send.
  if (GATEWAY_RADIO_MODE == RADIO_MODE_EXCLUSIVE && !wifiCfg.isAPRunning()) {
    // We'll decide to send first, then thaw WiFi.
  } else {
    if (WiFi.status() != WL_CONNECTED) return;
  }

  uint32_t now = millis();
  bool idle = (now - lastRxMs) >= IDLE_TIMEOUT_MS;
  bool enough = buffer.size() >= BATCH_SIZE;

  if (!idle && !enough) return;

  // Prepare batch (pop front)
  BleRecord items[BATCH_SIZE];
  uint16_t cnt = buffer.popFront(items, BATCH_SIZE);

  if (cnt == 0) return;

  // Thaw WiFi only when actually sending (exclusive mode).
  if (GATEWAY_RADIO_MODE == RADIO_MODE_EXCLUSIVE && !wifiCfg.isAPRunning()) {
    // Stop BLE to avoid radio conflict during WiFi TX.
    ble.stop();
    if (!wifiThawAndConnect(WIFI_CONNECT_TIMEOUT_MS)) {
      LOGW("WiFi thaw/connect failed -> re-queue and resume BLE");
      for (uint16_t i = 0; i < cnt; i++) buffer.push(items[i]);
      wifiFreeze();
      ble.start();
      return;
    }

    // Ensure time sync at least once when WiFi is available.
    if (!timeSynced && WiFi.status() == WL_CONNECTED) {
      TimeSync::begin();
      timeSynced = TimeSync::waitForSync(8000);
    }
  }

  uint32_t ts = TimeSync::nowUtc();
  bool ok = fb.pushBatch(GATEWAY_ID, ts, items, cnt);
  if (ok) {
    LOGI("Firebase batch OK count=%u remain=%u", cnt, buffer.size());
  } else {
    // On failure: push back? (would require shifting). Simpler: re-add to buffer end (still no loss, order might change).
    LOGE("Firebase batch FAIL; re-queue to buffer tail");
    for (uint16_t i = 0; i < cnt; i++) buffer.push(items[i]);
    // small backoff
    delay(200);
  }

  // Freeze WiFi after sending (exclusive mode) and resume BLE.
  if (GATEWAY_RADIO_MODE == RADIO_MODE_EXCLUSIVE && !wifiCfg.isAPRunning()) {
    wifiFreeze();
    ble.start();
  }
}

static void dailyNtpResyncIfNeeded() {
  static uint32_t lastSyncDay = 0;

  uint32_t nowUtc = TimeSync::nowUtc();
  if (nowUtc == 0) return;

  uint32_t day = nowUtc / 86400UL;
  uint32_t secOfDay = nowUtc % 86400UL;

  // run once, in the first 60s after midnight UTC
  if (day == lastSyncDay || secOfDay >= 60UL) return;

  LOGI("NTP resync (midnight UTC)");

  if (GATEWAY_RADIO_MODE == RADIO_MODE_EXCLUSIVE && !wifiCfg.isAPRunning()) {
    // pause BLE, thaw WiFi only for NTP
    ble.stop();
    if (wifiThawAndConnect(WIFI_CONNECT_TIMEOUT_MS)) {
      TimeSync::begin();
      timeSynced = TimeSync::waitForSync(8000);
    } else {
      LOGW("NTP resync skipped: WiFi thaw/connect failed");
    }
    wifiFreeze();
    ble.start();
  } else {
    // BLE+WiFi mode
    if (WiFi.status() == WL_CONNECTED) {
      TimeSync::begin();
      timeSynced = TimeSync::waitForSync(8000);
    } else {
      LOGW("NTP resync skipped: WiFi not connected");
    }
  }

  lastSyncDay = day;
}

void setup() {
  Logger::begin();
  LOGI("BikeSens Gateway boot (%s)", GATEWAY_ID);
  LOGI("Reset reason: %d", esp_reset_reason());

  wifiCfg.begin();
  buffer.begin();
  recent.begin();
  fb.begin();

  bool wifiOk = false;
  if (wifiCfg.hasSavedWiFi()) {
    wifiOk = wifiCfg.connectSaved(WIFI_CONNECT_TIMEOUT_MS);
  }

  if (!wifiOk) {
    // Start AP config mode (still scan BLE + buffer)
    wifiCfg.startAP();
  }

  if (WiFi.status() == WL_CONNECTED) {
    TimeSync::begin();
    timeSynced = TimeSync::waitForSync(10000);
  }

#if FIREBASE_LOGIN_BEFORE_BLE
  if (WiFi.status() == WL_CONNECTED) {
    fb.login(); // if fails, we'll retry on first send
  }
#endif

  // In exclusive mode, freeze WiFi after boot sync/login (unless AP config is running).
  if (GATEWAY_RADIO_MODE == RADIO_MODE_EXCLUSIVE && WiFi.status() == WL_CONNECTED && !wifiCfg.isAPRunning()) {
    wifiFreeze();
  }

  ble.begin(&buffer, &recent);
  ble.start();

  lastRxMs = millis();
}

void loop() {
  // AP webserver (if running)
  wifiCfg.handleClient();

  // >>> FIX: when AP config is running, do nothing else (no BLE scan, no WiFi ops)
  // This stabilizes WPA2 handshake on macOS and avoids reboots/radio conflicts.
  if (wifiCfg.isAPRunning()) {
    ble.stop();
    delay(10);
    return;
  }
  // <<< FIX end

  // If WiFi connected now, ensure time once (best effort)
  if (!timeSynced && WiFi.status() == WL_CONNECTED) {
    TimeSync::begin();
    timeSynced = TimeSync::waitForSync(8000);
  }

  // BLE runs in callbacks; we only manage wifi + sending here.
  ensureWiFiConnected();
  // Time sync during night
  dailyNtpResyncIfNeeded();

  // Update lastRxMs when buffer grows (means BLE callback pushed a record)
  static uint16_t lastBufSize = 0;
  uint16_t curSize = buffer.size();
  if (curSize > lastBufSize) {
    lastRxMs = millis();
  }
  lastBufSize = curSize;

  trySendIfNeeded();

  // Idle CPU a bit
  delay(5);
}
