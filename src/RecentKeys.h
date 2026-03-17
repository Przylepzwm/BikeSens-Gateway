#pragma once
#include <Arduino.h>
#include "Config.h"

// Key = (device_id << 16) | seq
// Fixed-size open addressing hash-set (no dynamic allocation).

#if (RECENT_KEYS_MAX <= 256)
  #define RK_TABLE_SIZE 512
#else
  #define RK_TABLE_SIZE 1024
#endif

class RecentKeys {
public:
  void begin() { clear(); }

  void clear() {
    for (uint16_t i = 0; i < RK_TABLE_SIZE; i++) {
      used_[i] = 0;
      keys_[i] = 0;
      ts_[i] = 0;
    }
    count_ = 0;
  }

  // Returns true if key already present recently; otherwise inserts/refreshes and returns false.
  bool seenOrInsert(uint32_t key) {
    const uint16_t n = RK_TABLE_SIZE;
    uint16_t idx = (uint16_t)(hash_(key) % n);
    uint32_t now = millis();

    for (uint16_t probe = 0; probe < n; probe++) {
      uint16_t i = (uint16_t)((idx + probe) % n);

      if (!used_[i]) {
        used_[i] = 1;
        keys_[i] = key;
        ts_[i] = now;
        if (count_ < n) count_++;

        // If too full -> clear to keep O(1) (duplicates may pass after reset)
        if (count_ > (n * 3) / 4) {
          clear();
          insertAfterClear_(key, now);
        }
        return false;
      }

      if (keys_[i] == key) {
        if ((uint32_t)(now - ts_[i]) < RECENT_KEYS_TTL_MS) return true;
        ts_[i] = now;
        return false;
      }
    }

    // Table full: clear and insert
    clear();
    insertAfterClear_(key, now);
    return false;
  }

private:
  static constexpr uint32_t RECENT_KEYS_TTL_MS = 60000UL;

  static uint32_t hash_(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dUL;
    x ^= x >> 15;
    x *= 0x846ca68bUL;
    x ^= x >> 16;
    return x;
  }

  void insertAfterClear_(uint32_t key, uint32_t now) {
    const uint16_t n = RK_TABLE_SIZE;
    uint16_t idx = (uint16_t)(hash_(key) % n);

    for (uint16_t probe = 0; probe < n; probe++) {
      uint16_t i = (uint16_t)((idx + probe) % n);
      if (!used_[i]) {
        used_[i] = 1;
        keys_[i] = key;
        ts_[i] = now;
        count_ = 1;
        return;
      }
    }
  }

  uint32_t keys_[RK_TABLE_SIZE];
  uint32_t ts_[RK_TABLE_SIZE];
  uint8_t  used_[RK_TABLE_SIZE];
  uint16_t count_{0};
};

#undef RK_TABLE_SIZE
