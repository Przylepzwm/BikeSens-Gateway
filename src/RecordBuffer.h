#pragma once
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "GatewayTypes.h"
#include "Config.h"
#include "Logger.h"

class RecordBuffer {
public:
  void begin() { clear(); }

  void clear() {
    portENTER_CRITICAL(&mux_);
    count_ = 0;
    portEXIT_CRITICAL(&mux_);
  }

  uint16_t size() const {
    portENTER_CRITICAL(&mux_);
    uint16_t v = count_;
    portEXIT_CRITICAL(&mux_);
    return v;
  }
  uint16_t capacity() const { return BUFFER_MAX; }
  bool empty() const {
    portENTER_CRITICAL(&mux_);
    bool v = (count_ == 0);
    portEXIT_CRITICAL(&mux_);
    return v;
  }
  bool full() const {
    portENTER_CRITICAL(&mux_);
    bool v = (count_ >= BUFFER_MAX);
    portEXIT_CRITICAL(&mux_);
    return v;
  }

  const BleRecord& at(uint16_t i) const { return recs_[i]; }
  BleRecord& at(uint16_t i) { return recs_[i]; }

  // Add record. If full, performs overflow cleanup first (removes BATCH_SIZE smallest pulses).
  void push(const BleRecord& r) {
    bool dropNewest = false;
    portENTER_CRITICAL(&mux_);
    if (count_ >= BUFFER_MAX) {
      overflowCleanup_(BATCH_SIZE);
    }
    if (count_ >= BUFFER_MAX) {
      // still full (BATCH_SIZE could be 0)
      // drop newest (this record)
      dropNewest = true;
    } else {
      recs_[count_++] = r;
    }
    portEXIT_CRITICAL(&mux_);
    if (dropNewest) {
      LOGW("Buffer still full after cleanup; dropping newest");
    }
  }

  // Pop a batch of up to n records (copies them out, removes from buffer).
  // This implementation removes the first n records (stable order).
  uint16_t popFront(BleRecord* out, uint16_t n) {
    portENTER_CRITICAL(&mux_);
    if (n == 0 || count_ == 0) {
      portEXIT_CRITICAL(&mux_);
      return 0;
    }
    uint16_t k = (n > count_) ? count_ : n;

    for (uint16_t i = 0; i < k; i++) out[i] = recs_[i];

    // compact
    for (uint16_t i = k; i < count_; i++) recs_[i - k] = recs_[i];
    count_ -= k;
    portEXIT_CRITICAL(&mux_);
    return k;
  }

  // Overflow strategy: remove `k` entries with the smallest pulses.
  // "First encountered" order is acceptable; no need for oldest.
  void overflowCleanup_(uint16_t k) {
    if (k == 0 || count_ == 0) return;
    uint16_t toRemove = (k > count_) ? count_ : k;

    // Multi-pass over pulses value: start from min observed pulses and remove first-matching.
    uint16_t minP = 0xFFFF, maxP = 0;
    for (uint16_t i = 0; i < count_; i++) {
      uint16_t p = recs_[i].pulses;
      if (p < minP) minP = p;
      if (p > maxP) maxP = p;
    }

    uint16_t removed = 0;
    for (uint16_t p = minP; p <= maxP && removed < toRemove; p++) {
      uint16_t i = 0;
      while (i < count_ && removed < toRemove) {
        if (recs_[i].pulses == p) {
          removeAt_(i); // compacts by moving tail left
          removed++;
        } else {
          i++;
        }
      }
      if (p == 0xFFFF) break; // safety
    }

    // If still not removed enough (e.g., maxP loop weird), remove from front.
    while (removed < toRemove && count_ > 0) {
      removeAt_(0);
      removed++;
    }

    LOGW("Buffer overflow cleanup: removed=%u, remain=%u", removed, count_);
  }

private:
  void removeAt_(uint16_t idx) {
    if (idx >= count_) return;
    for (uint16_t i = idx + 1; i < count_; i++) recs_[i - 1] = recs_[i];
    count_--;
  }

  BleRecord recs_[BUFFER_MAX];
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  uint16_t count_{0};
};
