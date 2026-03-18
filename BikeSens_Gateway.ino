#include <Arduino.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include "src/Config.h"
#include "src/Logger.h"
#include "src/WiFiConfigManager.h"
#include "src/TimeSync.h"
#include "src/RecordBuffer.h"
#include "src/DeviceFilter.h"
#include "src/RecentKeys.h"
#include "src/BleScanner.h"
#include "src/FirebaseRest.h"

static WiFiConfigManager wifiCfg;
static RecordBuffer buffer;
static DeviceFilter deviceFilter;
static RecentKeys recent;
static BleScanner ble;
static FirebaseRest fb;

static uint32_t lastRxMs = 0;
static uint32_t lastReconnectAttemptMs = 0;
static uint32_t lastSyncMs = 0;
static bool wifiFrozen = false;
static bool timeSynced = false;
static uint16_t lastBatchCount = 0;
static uint32_t lastBatchTs = 0;
static bool lastRebootPending = false;
static bool lastUpdatePending = false;
static String lastUpdateVersion;
static bool lastUpdateAllowed = false;
static String currentUpdateVersion;
static String currentUpdateUrl;
static String currentUpdateSha256;
static bool currentUpdateForce = false;
static uint32_t lastOtaAttemptMs = 0;
static String lastOtaAttemptVersion;
static uint32_t lastOtaTs = 0;
static String lastOtaVersion;
static String lastOtaResult;

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

static bool hasHttpScheme(const String& url) {
  return url.startsWith("http://") || url.startsWith("https://");
}

static void loadDeviceFilterConfig() {
  deviceFilter.begin();

  if (WiFi.status() != WL_CONNECTED) {
    LOGW("Devices filter: WiFi not connected -> allow all");
    return;
  }

  FirebaseRest::DevicesControl cfg;
  if (!fb.getDevicesControl(wifiCfg.gatewayId(), cfg)) {
    LOGW("Devices filter: fetch failed -> allow all");
    return;
  }

  if (cfg.allowAll) {
    deviceFilter.setAllowAll(true);
    LOGI("Devices filter: mode=all");
    return;
  }

  deviceFilter.setAllowAll(false);
  deviceFilter.clearSelected();
  for (uint16_t i = 0; i < cfg.count; i++) {
    deviceFilter.addSelected(cfg.ids[i]);
  }
  deviceFilter.sortSelected();
  LOGI("Devices filter: mode=selected count=%u", deviceFilter.selectedCount());
}

static void tryRunRebootIfAllowed() {
  if (!lastRebootPending) return;

  LOGW("Remote reboot start");
  ble.stop();
  if (!fb.clearReboot(wifiCfg.gatewayId())) {
    LOGE("Remote reboot clear failed, BLE resumed");
    ble.start();
    return;
  }
  LOGW("Remote reboot restarting");
  delay(300);
  ESP.restart();
}

static bool performOtaFromUrl(const String& url) {
  HTTPClient http;
  http.setTimeout(FIREBASE_HTTP_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int code = -1;
  int contentLength = -1;

  if (url.startsWith("https://")) {
    WiFiClientSecure client;
    if (FIREBASE_TLS_INSECURE) client.setInsecure();
    if (!http.begin(client, url)) {
      LOGE("OTA HTTP begin failed");
      return false;
    }
    code = http.GET();
    contentLength = http.getSize();
    if (code != 200) {
      LOGE("OTA HTTP GET failed: code=%d", code);
      http.end();
      return false;
    }
    if (!Update.begin(contentLength > 0 ? (size_t)contentLength : UPDATE_SIZE_UNKNOWN)) {
      LOGE("OTA begin failed: err=%u", Update.getError());
      http.end();
      return false;
    }
    WiFiClient* stream = http.getStreamPtr();
    size_t written = Update.writeStream(*stream);
    bool finished = Update.end();
    http.end();
    if (!finished || !Update.isFinished()) {
      LOGE("OTA end failed: err=%u", Update.getError());
      return false;
    }
    if (contentLength > 0 && written != (size_t)contentLength) {
      LOGE("OTA size mismatch: written=%u expected=%d", (unsigned)written, contentLength);
      return false;
    }
    return true;
  }

  if (url.startsWith("http://")) {
    WiFiClient client;
    if (!http.begin(client, url)) {
      LOGE("OTA HTTP begin failed");
      return false;
    }
    code = http.GET();
    contentLength = http.getSize();
    if (code != 200) {
      LOGE("OTA HTTP GET failed: code=%d", code);
      http.end();
      return false;
    }
    if (!Update.begin(contentLength > 0 ? (size_t)contentLength : UPDATE_SIZE_UNKNOWN)) {
      LOGE("OTA begin failed: err=%u", Update.getError());
      http.end();
      return false;
    }
    WiFiClient* stream = http.getStreamPtr();
    size_t written = Update.writeStream(*stream);
    bool finished = Update.end();
    http.end();
    if (!finished || !Update.isFinished()) {
      LOGE("OTA end failed: err=%u", Update.getError());
      return false;
    }
    if (contentLength > 0 && written != (size_t)contentLength) {
      LOGE("OTA size mismatch: written=%u expected=%d", (unsigned)written, contentLength);
      return false;
    }
    return true;
  }

  LOGE("OTA URL must start with http:// or https://");
  return false;
}

static void tryRunOtaIfAllowed() {
  if (!lastUpdatePending || !lastUpdateAllowed) return;
  if (currentUpdateVersion.length() == 0) {
    LOGE("OTA skipped: empty version");
    return;
  }
  if (currentUpdateVersion == FW_VERSION) return;
  if (!hasHttpScheme(currentUpdateUrl)) {
    LOGE("OTA skipped: invalid url=%s", currentUpdateUrl.c_str());
    return;
  }

  uint32_t nowMs = millis();
  if (lastOtaAttemptVersion == currentUpdateVersion &&
      (nowMs - lastOtaAttemptMs) < OTA_RETRY_INTERVAL_MS) {
    return;
  }

  lastOtaAttemptMs = nowMs;
  lastOtaAttemptVersion = currentUpdateVersion;

  LOGW("OTA start version=%s", currentUpdateVersion.c_str());
  ble.stop();

  bool ok = performOtaFromUrl(currentUpdateUrl);
  if (ok) {
    lastOtaTs = TimeSync::nowUtc();
    lastOtaVersion = currentUpdateVersion;
    lastOtaResult = "success";
    fb.putStatus(
      wifiCfg.gatewayId(),
      lastOtaTs,
      buffer.size(),
      FW_VERSION,
      WiFi.localIP().toString().c_str(),
      WiFi.RSSI(),
      lastBatchCount,
      lastBatchTs,
      lastOtaTs,
      lastOtaVersion.c_str(),
      lastOtaResult.c_str()
    );
    fb.clearUpdatePending(wifiCfg.gatewayId());
    LOGW("OTA success, restarting");
    delay(300);
    ESP.restart();
    return;
  }

  lastOtaTs = TimeSync::nowUtc();
  lastOtaVersion = currentUpdateVersion;
  lastOtaResult = "failed";
  fb.putStatus(
    wifiCfg.gatewayId(),
    lastOtaTs,
    buffer.size(),
    FW_VERSION,
    WiFi.localIP().toString().c_str(),
    WiFi.RSSI(),
    lastBatchCount,
    lastBatchTs,
    lastOtaTs,
    lastOtaVersion.c_str(),
    lastOtaResult.c_str()
  );
  LOGE("OTA failed, BLE resumed");
  ble.start();
}

static bool pollUpdateControl() {
  FirebaseRest::UpdateControl ctrl;
  bool ok = fb.getUpdateControl(wifiCfg.gatewayId(), ctrl);
  if (!ok) return false;

  if (ctrl.pending) {
    bool allowed = (buffer.size() == 0);
    bool changed = (!lastUpdatePending) || (ctrl.version != lastUpdateVersion);
    if (changed) {
      LOGW("OTA pending version=%s force=%d url=%s",
           ctrl.version.c_str(),
           ctrl.force ? 1 : 0,
           ctrl.url.c_str());
    }
    if (allowed && !lastUpdateAllowed) {
      LOGW("OTA allowed version=%s", ctrl.version.c_str());
    }
    if (!allowed && (!lastUpdatePending || lastUpdateAllowed || changed)) {
      LOGI("OTA waiting buffer=%u", buffer.size());
    }
    lastUpdateAllowed = allowed;
  } else if (lastUpdatePending) {
    LOGI("OTA cleared");
    lastUpdateAllowed = false;
  }

  lastUpdatePending = ctrl.pending;
  lastUpdateVersion = ctrl.version;
  currentUpdateVersion = ctrl.version;
  currentUpdateUrl = ctrl.url;
  currentUpdateSha256 = ctrl.sha256;
  currentUpdateForce = ctrl.force;
  return true;
}

static bool pollRebootControl() {
  bool reboot = false;
  bool ok = fb.getRebootControl(wifiCfg.gatewayId(), reboot);
  if (!ok) return false;

  if (reboot) {
    if (!lastRebootPending) {
      LOGW("Remote reboot pending");
    }
    LOGW("Remote reboot allowed");
  } else if (lastRebootPending) {
    LOGI("Remote reboot cleared");
  }

  lastRebootPending = reboot;
  return true;
}

static bool syncFirebaseMaintenance(bool force) {
  if (wifiCfg.isAPRunning()) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  uint32_t nowMs = millis();
  if (!force && (nowMs - lastSyncMs) < STATUS_INTERVAL_MS) return false;

  String ip = WiFi.localIP().toString();
  uint32_t lastSeen = TimeSync::nowUtc();
  bool statusOk = fb.putStatus(
    wifiCfg.gatewayId(),
    lastSeen,
    buffer.size(),
    FW_VERSION,
    ip.c_str(),
    WiFi.RSSI(),
    lastBatchCount,
    lastBatchTs,
    lastOtaTs,
    lastOtaVersion.c_str(),
    lastOtaResult.c_str()
  );
  if (statusOk) {
    LOGI("Firebase status OK buf=%u batch_count=%u", buffer.size(), lastBatchCount);
  }
  bool rebootOk = pollRebootControl();
  bool controlOk = pollUpdateControl();
  if (statusOk && rebootOk && controlOk) {
    lastSyncMs = nowMs;
    tryRunRebootIfAllowed();
    tryRunOtaIfAllowed();
    return true;
  }
  return false;
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
  bool ok = fb.pushBatch(wifiCfg.gatewayId(), ts, items, cnt);
  if (ok) {
    LOGI("Firebase batch OK count=%u remain=%u", cnt, buffer.size());
    lastBatchCount = cnt;
    lastBatchTs = ts;
    syncFirebaseMaintenance(true);
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
  wifiCfg.begin();
  LOGI("BikeSens Gateway boot (%s)", wifiCfg.gatewayId());
  LOGI("Reset reason: %d", esp_reset_reason());
  buffer.begin();
  deviceFilter.begin();
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
    loadDeviceFilterConfig();
    syncFirebaseMaintenance(true);
  }
#endif

  // In exclusive mode, freeze WiFi after boot sync/login (unless AP config is running).
  if (GATEWAY_RADIO_MODE == RADIO_MODE_EXCLUSIVE && WiFi.status() == WL_CONNECTED && !wifiCfg.isAPRunning()) {
    wifiFreeze();
  }

  ble.begin(&buffer, &recent, &deviceFilter);
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
  syncFirebaseMaintenance(false);

  // Idle CPU a bit
  delay(5);
}
