#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include "Config.h"
#include "Logger.h"

class WiFiConfigManager {
public:
  void begin() {
    prefs_.begin("bsgw", false);
    //prefs_.clear();   //  usuń zapisane WiFi
    server_ = new WebServer(80);
  }

  bool hasSavedWiFi() {
    String ssid = prefs_.getString("ssid", "");
    return ssid.length() > 0;
  }

  void loadSavedWiFi(String& ssid, String& pass) {
    ssid = prefs_.getString("ssid", "");
    pass = prefs_.getString("pass", "");
  }

  void saveWiFi(const String& ssid, const String& pass) {
    prefs_.putString("ssid", ssid);
    prefs_.putString("pass", pass);
  }

  bool connectSaved(uint32_t timeoutMs) {
    String ssid, pass;
    loadSavedWiFi(ssid, pass);
    if (ssid.length() == 0) return false;

    if (WiFi.getMode() == WIFI_OFF) {
      LOGI("WiFi ON");
    }

    LOGI("WiFi connect: ssid=%s", ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());

    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - t0) < timeoutMs) {
      delay(200);
    }
    if (WiFi.status() == WL_CONNECTED) {
      LOGI("WiFi connected: ip=%s", WiFi.localIP().toString().c_str());
      return true;
    }
    LOGW("WiFi connect failed");
    return false;
  }

  void startAP() {
    if (apRunning_) return;

    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    delay(100);

    IPAddress ip = WiFi.softAPIP();
    LOGI("AP started: ssid=%s pass=%s ip=%s", AP_SSID, AP_PASS, ip.toString().c_str());

    server_->on("/", HTTP_GET, [&](){
      String html =
        "<html><body>"
        "<h2>BikeSens Gateway Wi-Fi setup</h2>"
        "<form method='POST' action='/save'>"
        "SSID:<br><input name='ssid' length=32><br>"
        "Password:<br><input name='pass' length=64 type='password'><br><br>"
        "<input type='submit' value='Save & Restart'>"
        "</form>"
        "</body></html>";
      server_->send(200, "text/html", html);
    });

    server_->on("/save", HTTP_POST, [&](){
      String ssid = server_->arg("ssid");
      String pass = server_->arg("pass");
      ssid.trim();
      pass.trim();
      if (ssid.length() == 0) {
        server_->send(400, "text/plain", "SSID required");
        return;
      }
      saveWiFi(ssid, pass);
      server_->send(200, "text/plain", "Saved. Restarting...");
      delay(300);
      ESP.restart();
    });

    server_->begin();
    apRunning_ = true;
  }

  void handleClient() {
    if (apRunning_ && server_) server_->handleClient();
  }

  bool isAPRunning() const { return apRunning_; }

private:
  Preferences prefs_;
  WebServer* server_{nullptr};
  bool apRunning_{false};
};
