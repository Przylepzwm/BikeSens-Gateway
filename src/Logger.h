#pragma once
#include <Arduino.h>
#include "Config.h"

#if LOG_ENABLED
  #define LOGI(fmt, ...) Logger::logf("I", fmt, ##__VA_ARGS__)
  #define LOGW(fmt, ...) Logger::logf("W", fmt, ##__VA_ARGS__)
  #define LOGE(fmt, ...) Logger::logf("E", fmt, ##__VA_ARGS__)
#else
  #define LOGI(...) do{}while(0)
  #define LOGW(...) do{}while(0)
  #define LOGE(...) do{}while(0)
#endif

class Logger {
public:
  static void begin() {
  #if LOG_ENABLED
    Serial.begin(LOG_BAUD);
    delay(50);
  #endif
  }

  static void logf(const char* lvl, const char* fmt, ...) {
  #if LOG_ENABLED
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Serial.printf("[%s] %s\n", lvl, buf);
  #endif
  }
};
