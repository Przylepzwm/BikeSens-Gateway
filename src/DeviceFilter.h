#pragma once
#include <Arduino.h>
#include "Config.h"

class DeviceFilter {
public:
  void begin() {
    allowAll_ = true;
    count_ = 0;
  }

  void setAllowAll(bool allowAll) {
    allowAll_ = allowAll;
  }

  bool allowAll() const { return allowAll_; }

  void clearSelected() {
    count_ = 0;
  }

  bool addSelected(uint16_t deviceId) {
    if (count_ >= MAX_SELECTED_DEVICES) return false;
    ids_[count_++] = deviceId;
    return true;
  }

  void sortSelected() {
    for (uint16_t i = 1; i < count_; i++) {
      uint16_t v = ids_[i];
      int16_t j = (int16_t)i - 1;
      while (j >= 0 && ids_[j] > v) {
        ids_[j + 1] = ids_[j];
        j--;
      }
      ids_[j + 1] = v;
    }
  }

  uint16_t selectedCount() const { return count_; }

  bool accepts(uint16_t deviceId) const {
    if (allowAll_) return true;
    int16_t lo = 0;
    int16_t hi = (int16_t)count_ - 1;
    while (lo <= hi) {
      int16_t mid = lo + ((hi - lo) / 2);
      uint16_t v = ids_[mid];
      if (v == deviceId) return true;
      if (v < deviceId) {
        lo = mid + 1;
      } else {
        hi = mid - 1;
      }
    }
    return false;
  }

private:
  bool allowAll_{true};
  uint16_t count_{0};
  uint16_t ids_[MAX_SELECTED_DEVICES];
};
