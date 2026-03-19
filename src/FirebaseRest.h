#pragma once
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "Config.h"
#include "Logger.h"
#include "GatewayTypes.h"

class FirebaseRest {
public:
  struct UpdateControl {
    bool pending{false};
    bool force{false};
    String version;
    String url;
    String sha256;
  };

  struct DevicesControl {
    bool allowAll{true};
    uint16_t ids[MAX_SELECTED_DEVICES];
    uint16_t count{0};
  };

  struct MaintenanceControl {
    bool autoRebootEnabled{false};
    uint16_t rebootMinutes[MAX_REBOOT_SLOTS];
    uint16_t count{0};
  };

  void begin() {
    // nothing
  }

  void resetSession() {
    idToken_ = "";
    refreshToken_ = "";
    tokenExpiryMs_ = 0;
  }

  bool login() {
    // Identity Toolkit: signInWithPassword
    // POST https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=[API_KEY]
    String url = String("https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=") + FIREBASE_API_KEY;

    String payload =
      String("{\"email\":\"") + FIREBASE_EMAIL +
      "\",\"password\":\"" + FIREBASE_PASSWORD +
      "\",\"returnSecureToken\":true}";

    String resp;
    int code = httpsPostJson_(url, payload, resp, /*auth*/nullptr);
    if (code != 200) {
      LOGE("Firebase login failed: http=%d resp=%s", code, resp.c_str());
      return false;
    }

    // Parse minimal JSON without dynamic libs (simple regex-ish).
    // Expect: "idToken":"...","refreshToken":"...","expiresIn":"3600"
    idToken_ = extractJsonString_(resp, "idToken");
    refreshToken_ = extractJsonString_(resp, "refreshToken");
    String exp = extractJsonString_(resp, "expiresIn");

    if (idToken_.length() == 0 || refreshToken_.length() == 0) {
      LOGE("Firebase login parse error");
      return false;
    }

    uint32_t expiresIn = exp.length() ? (uint32_t)exp.toInt() : 3600;
    tokenExpiryMs_ = millis() + (expiresIn * 1000UL) - 30000UL; // refresh 30s early
    LOGI("Firebase login OK (expiresIn=%lu)", (unsigned long)expiresIn);
    return true;
  }

  bool ensureTokenValid() {
    if (idToken_.length() == 0 || refreshToken_.length() == 0) return login();
    if ((int32_t)(millis() - tokenExpiryMs_) < 0) return true; // still valid
    return refresh();
  }

  bool refresh() {
    // Secure Token API
    // POST https://securetoken.googleapis.com/v1/token?key=[API_KEY]
    // Content-Type: application/x-www-form-urlencoded
    String url = String("https://securetoken.googleapis.com/v1/token?key=") + FIREBASE_API_KEY;
    String body = String("grant_type=refresh_token&refresh_token=") + urlEncode_(refreshToken_);

    WiFiClientSecure client;
    if (FIREBASE_TLS_INSECURE) client.setInsecure();

    HTTPClient http;
    http.setTimeout(FIREBASE_HTTP_TIMEOUT_MS);
    if (!http.begin(client, url)) {
      LOGE("Token refresh begin failed");
      return false;
    }
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    int code = http.POST((uint8_t*)body.c_str(), body.length());
    String resp = http.getString();
    http.end();

    if (code != 200) {
      LOGE("Token refresh failed: http=%d resp=%s", code, resp.c_str());
      return false;
    }

    // Response includes: "id_token", "refresh_token", "expires_in"
    String newId = extractJsonString_(resp, "id_token");
    String newRef = extractJsonString_(resp, "refresh_token");
    String exp = extractJsonString_(resp, "expires_in");

    if (newId.length()) idToken_ = newId;
    if (newRef.length()) refreshToken_ = newRef;

    uint32_t expiresIn = exp.length() ? (uint32_t)exp.toInt() : 3600;
    tokenExpiryMs_ = millis() + (expiresIn * 1000UL) - 30000UL;

    LOGI("Firebase token refreshed (expiresIn=%lu)", (unsigned long)expiresIn);
    return true;
  }

  // Push one batch as a single write (POST -> pushId).
  bool pushBatch(const char* gatewayId, uint32_t gatewayTs, const BleRecord* items, uint16_t count) {
    if (!ensureTokenValid()) return false;

    // POST https://<db>/gateways/<gw>/batches.json?auth=<idToken>
    String url = String(FIREBASE_DB_URL) + "/gateways/" + gatewayId + "/batches.json?auth=" + idToken_;

    // Build JSON (no dynamic allocations besides String).
    String json = "{";
    json += "\"gateway_id\":\"" + String(gatewayId) + "\",";
    json += "\"gateway_ts\":" + String(gatewayTs) + ",";
    json += "\"count\":" + String(count) + ",";
    json += "\"items\":[";
    for (uint16_t i = 0; i < count; i++) {
      if (i) json += ",";
      json += "{";
      json += "\"rx_ts\":" + String(items[i].rx_ts) + ",";
      json += "\"device_id\":" + String(items[i].device_id) + ",";
      json += "\"seq\":" + String(items[i].seq) + ",";
      json += "\"pulses\":" + String(items[i].pulses) + ",";
      json += "\"bat\":" + String(items[i].bat);
      json += "}";
    }
    json += "]}";

    String resp;
    int code = httpsPostJson_(url, json, resp, /*auth*/nullptr);
    if (code != 200) {
      LOGE("Firebase pushBatch failed: http=%d resp=%s", code, resp.c_str());
      return false;
    }
    return true;
  }

  bool putStatus(const char* gatewayId,
                 uint32_t lastSeen,
                 uint16_t bufferSize,
                 const char* fwVersion,
                 const char* ip,
                 int32_t wifiRssi,
                 uint16_t lastBatchCount,
                 uint32_t lastBatchTs,
                 uint32_t lastOtaTs,
                 const char* lastOtaVersion,
                 const char* lastOtaResult,
                 uint32_t lastWifiReconnectTs,
                 const char* lastWifiReconnectResult,
                 int32_t resetReason,
                 const char* lastError,
                 uint32_t uptimeS,
                 uint32_t freeHeap,
                 uint32_t statusSyncOkCount,
                 uint32_t statusSyncFailCount) {
    if (!ensureTokenValid()) return false;

    String url = String(FIREBASE_DB_URL) + "/gateways/" + gatewayId + "/status.json?auth=" + idToken_;

    String json = "{";
    json += "\"last_seen\":" + String(lastSeen) + ",";
    json += "\"buffer_size\":" + String(bufferSize) + ",";
    json += "\"fw_version\":\"" + String(fwVersion) + "\",";
    json += "\"ip\":\"" + String(ip) + "\",";
    json += "\"wifi_rssi\":" + String(wifiRssi) + ",";
    json += "\"last_batch_count\":" + String(lastBatchCount) + ",";
    json += "\"last_batch_ts\":" + String(lastBatchTs) + ",";
    json += "\"last_ota_ts\":" + String(lastOtaTs) + ",";
    json += "\"last_ota_version\":\"" + String(lastOtaVersion) + "\",";
    json += "\"last_ota_result\":\"" + String(lastOtaResult) + "\",";
    json += "\"last_wifi_reconnect_ts\":" + String(lastWifiReconnectTs) + ",";
    json += "\"last_wifi_reconnect_result\":\"" + String(lastWifiReconnectResult) + "\",";
    json += "\"reset_reason\":" + String(resetReason) + ",";
    json += "\"last_error\":\"" + String(lastError) + "\",";
    json += "\"uptime_s\":" + String(uptimeS) + ",";
    json += "\"free_heap\":" + String(freeHeap) + ",";
    json += "\"status_sync_ok_count\":" + String(statusSyncOkCount) + ",";
    json += "\"status_sync_fail_count\":" + String(statusSyncFailCount);
    json += "}";

    String resp;
    int code = httpsSendJson_(url, "PUT", json, resp);
    if (code != 200) {
      LOGE("Firebase putStatus failed: http=%d resp=%s", code, resp.c_str());
      return false;
    }
    return true;
  }

  bool clearUpdatePending(const char* gatewayId) {
    if (!ensureTokenValid()) return false;

    String url = String(FIREBASE_DB_URL) + "/gateways/" + gatewayId + "/control/update.json?auth=" + idToken_;
    String json = "{\"pending\":false}";

    String resp;
    int code = httpsSendJson_(url, "PATCH", json, resp);
    if (code != 200) {
      LOGE("Firebase clearUpdatePending failed: http=%d resp=%s", code, resp.c_str());
      return false;
    }
    return true;
  }

  bool clearReboot(const char* gatewayId) {
    if (!ensureTokenValid()) return false;

    String url = String(FIREBASE_DB_URL) + "/gateways/" + gatewayId + "/control.json?auth=" + idToken_;
    String json = "{\"reboot\":false}";

    String resp;
    int code = httpsSendJson_(url, "PATCH", json, resp);
    if (code != 200) {
      LOGE("Firebase clearReboot failed: http=%d resp=%s", code, resp.c_str());
      return false;
    }
    return true;
  }

  bool getRebootControl(const char* gatewayId, bool& reboot) {
    if (!ensureTokenValid()) return false;

    String url = String(FIREBASE_DB_URL) + "/gateways/" + gatewayId + "/control/reboot.json?auth=" + idToken_;

    String resp;
    int code = httpsGet_(url, resp);
    if (code != 200) {
      LOGE("Firebase getRebootControl failed: http=%d resp=%s", code, resp.c_str());
      return false;
    }

    reboot = resp.startsWith("true");
    return true;
  }

  bool getUpdateControl(const char* gatewayId, UpdateControl& out) {
    if (!ensureTokenValid()) return false;

    String url = String(FIREBASE_DB_URL) + "/gateways/" + gatewayId + "/control/update.json?auth=" + idToken_;

    String resp;
    int code = httpsGet_(url, resp);
    if (code != 200) {
      LOGE("Firebase getUpdateControl failed: http=%d resp=%s", code, resp.c_str());
      return false;
    }

    out.pending = extractJsonBool_(resp, "pending");
    out.force = extractJsonBool_(resp, "force");
    out.version = extractJsonString_(resp, "version");
    out.url = extractJsonString_(resp, "url");
    out.sha256 = extractJsonString_(resp, "sha256");
    return true;
  }

  bool getDevicesControl(const char* gatewayId, DevicesControl& out) {
    if (!ensureTokenValid()) return false;

    String url = String(FIREBASE_DB_URL) + "/gateways/" + gatewayId + "/devices.json?auth=" + idToken_;

    String resp;
    int code = httpsGet_(url, resp);
    if (code != 200) {
      LOGE("Firebase getDevicesControl failed: http=%d resp=%s", code, resp.c_str());
      return false;
    }

    String mode = extractJsonString_(resp, "mode");
    out.allowAll = (mode != "selected");
    out.count = 0;

    int allowedPos = resp.indexOf("\"allowed\":");
    if (allowedPos < 0) return true;

    int braceStart = resp.indexOf('{', allowedPos);
    if (braceStart < 0) return true;

    int braceEnd = findMatchingBrace_(resp, braceStart);
    if (braceEnd < 0) return true;

    int pos = braceStart + 1;
    while (pos < braceEnd && out.count < MAX_SELECTED_DEVICES) {
      int keyStart = resp.indexOf('"', pos);
      if (keyStart < 0 || keyStart >= braceEnd) break;
      int keyEnd = resp.indexOf('"', keyStart + 1);
      if (keyEnd < 0 || keyEnd >= braceEnd) break;

      String key = resp.substring(keyStart + 1, keyEnd);
      uint32_t v = (uint32_t)key.toInt();
      if (v <= 0xFFFFUL) {
        out.ids[out.count++] = (uint16_t)v;
      }
      pos = keyEnd + 1;
    }

    // insertion sort once at load time
    for (uint16_t i = 1; i < out.count; i++) {
      uint16_t val = out.ids[i];
      int16_t j = (int16_t)i - 1;
      while (j >= 0 && out.ids[j] > val) {
        out.ids[j + 1] = out.ids[j];
        j--;
      }
      out.ids[j + 1] = val;
    }
    return true;
  }

  bool getMaintenanceControl(const char* gatewayId, MaintenanceControl& out) {
    if (!ensureTokenValid()) return false;

    String url = String(FIREBASE_DB_URL) + "/gateways/" + gatewayId + "/control/maintenance.json?auth=" + idToken_;

    String resp;
    int code = httpsGet_(url, resp);
    if (code != 200) {
      LOGE("Firebase getMaintenanceControl failed: http=%d resp=%s", code, resp.c_str());
      return false;
    }

    out.autoRebootEnabled = extractJsonBool_(resp, "auto_reboot_enabled");
    out.count = 0;

    int timesPos = resp.indexOf("\"times\":");
    if (timesPos < 0) return true;

    int braceStart = resp.indexOf('{', timesPos);
    if (braceStart < 0) return true;

    int braceEnd = findMatchingBrace_(resp, braceStart);
    if (braceEnd < 0) return true;

    int pos = braceStart + 1;
    while (pos < braceEnd && out.count < MAX_REBOOT_SLOTS) {
      int valueQuote1 = resp.indexOf('"', pos);
      if (valueQuote1 < 0 || valueQuote1 >= braceEnd) break;
      int valueQuote2 = resp.indexOf('"', valueQuote1 + 1);
      if (valueQuote2 < 0 || valueQuote2 >= braceEnd) break;

      String timeStr = resp.substring(valueQuote1 + 1, valueQuote2);
      if (timeStr.length() == 5 && timeStr[2] == ':') {
        int hour = timeStr.substring(0, 2).toInt();
        int minute = timeStr.substring(3, 5).toInt();
        if (hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59) {
          out.rebootMinutes[out.count++] = (uint16_t)(hour * 60 + minute);
        }
      }
      pos = valueQuote2 + 1;
    }

    for (uint16_t i = 1; i < out.count; i++) {
      uint16_t val = out.rebootMinutes[i];
      int16_t j = (int16_t)i - 1;
      while (j >= 0 && out.rebootMinutes[j] > val) {
        out.rebootMinutes[j + 1] = out.rebootMinutes[j];
        j--;
      }
      out.rebootMinutes[j + 1] = val;
    }
    return true;
  }

private:
  int httpsPostJson_(const String& url, const String& payload, String& outResp, const char* /*unused*/) {
    return httpsSendJson_(url, "POST", payload, outResp);
  }

  int httpsGet_(const String& url, String& outResp) {
    WiFiClientSecure client;
    if (FIREBASE_TLS_INSECURE) client.setInsecure();

    HTTPClient http;
    http.setTimeout(FIREBASE_HTTP_TIMEOUT_MS);
    if (!http.begin(client, url)) {
      LOGE("HTTP begin failed");
      outResp = "";
      return -1;
    }
    int code = http.GET();
    outResp = http.getString();
    http.end();
    return code;
  }

  // Send JSON helper (https)
  int httpsSendJson_(const String& url, const char* method, const String& payload, String& outResp) {
    WiFiClientSecure client;
    if (FIREBASE_TLS_INSECURE) client.setInsecure();

    HTTPClient http;
    http.setTimeout(FIREBASE_HTTP_TIMEOUT_MS);
    if (!http.begin(client, url)) {
      LOGE("HTTP begin failed");
      outResp = "";
      return -1;
    }
    http.addHeader("Content-Type", "application/json");
    int code = http.sendRequest(method, (uint8_t*)payload.c_str(), payload.length());
    outResp = http.getString();
    http.end();
    return code;
  }

  bool extractJsonBool_(const String& json, const char* key) {
    String needle = String("\"") + key + "\":";
    int pos = json.indexOf(needle);
    if (pos < 0) return false;
    pos += needle.length();
    while (pos < (int)json.length() && (json[pos] == ' ')) pos++;
    if (pos >= (int)json.length()) return false;
    return json.startsWith("true", pos);
  }

  int findMatchingBrace_(const String& json, int openPos) {
    int depth = 0;
    for (int i = openPos; i < (int)json.length(); i++) {
      if (json[i] == '{') depth++;
      else if (json[i] == '}') {
        depth--;
        if (depth == 0) return i;
      }
    }
    return -1;
  }

  // Very small JSON string extractor: finds "key":"value"
  String extractJsonString_(const String& json, const char* key) {
    String needle = String("\"") + key + "\":";
    int pos = json.indexOf(needle);
    if (pos < 0) return "";
    pos += needle.length();
    // skip spaces
    while (pos < (int)json.length() && (json[pos] == ' ')) pos++;
    if (pos >= (int)json.length()) return "";

    // if quoted
    if (json[pos] == '"') {
      pos++;
      int end = json.indexOf('"', pos);
      if (end < 0) return "";
      return json.substring(pos, end);
    }

    // unquoted value until comma/brace
    int end = pos;
    while (end < (int)json.length() && json[end] != ',' && json[end] != '}' && json[end] != ' ') end++;
    return json.substring(pos, end);
  }

  String urlEncode_(const String& s) {
    String o;
    const char* hex = "0123456789ABCDEF";
    for (size_t i = 0; i < s.length(); i++) {
      char c = s[i];
      if (isalnum((unsigned char)c) || c=='-' || c=='_' || c=='.' || c=='~') {
        o += c;
      } else {
        o += '%';
        o += hex[(c >> 4) & 0xF];
        o += hex[c & 0xF];
      }
    }
    return o;
  }

  String idToken_;
  String refreshToken_;
  uint32_t tokenExpiryMs_{0};
};
