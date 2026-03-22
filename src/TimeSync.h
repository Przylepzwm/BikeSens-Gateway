#pragma once
#include <Arduino.h>
#include <time.h>
#include "Config.h"
#include "Logger.h"

class TimeSync {
public:
  static void begin() {
    configTzTime(TIMEZONE_TZ, NTP_SERVER);
  }

  static bool waitForSync(uint32_t timeoutMs=10000) {
    uint32_t t0 = millis();
    time_t now;
    while ((millis() - t0) < timeoutMs) {
      time(&now);
      if (now > 1700000000) { // sanity (2023+)
        LOGI("NTP time OK: %lu", (unsigned long)now);
        return true;
      }
      delay(200);
    }
    LOGW("NTP sync timeout");
    return false;
  }

  static uint32_t nowUtc() {
    time_t now;
    time(&now);
    return (uint32_t)now;
  }
};
