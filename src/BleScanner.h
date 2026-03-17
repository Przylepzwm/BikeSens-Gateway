#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>
#include "Config.h"
#include "Logger.h"
#include "GatewayTypes.h"
#include "RecordBuffer.h"
#include "RecentKeys.h"
#include "TimeSync.h"

class BleScanner : public NimBLEScanCallbacks {
public:
  void begin(RecordBuffer* buf, RecentKeys* recent) {
    buf_ = buf;
    recent_ = recent;

    NimBLEDevice::init("");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9); // max RX sensitivity/power

    scan_ = NimBLEDevice::getScan();
    scan_->setScanCallbacks(this, false);

    scan_->setActiveScan(true);
    scan_->setInterval(160);
    scan_->setWindow(160);

    scan_->setDuplicateFilter(false);
  }

  void onScanEnd(const NimBLEScanResults& results, int reason) override {
    LOGW("SCAN END results=%d reason=%d", results.getCount(), reason);
  }

  void start() {
    if (!scan_) return;
    if (running_) return;

    uint32_t now = millis();
    if (last_off_ms_ != 0) {
      LOGI("BLE ON  (slept %lu ms)", (unsigned long)(now - last_off_ms_));
    } else {
      LOGI("BLE ON");
    }

    last_on_ms_ = now;
    running_ = true;
    scan_->start(0, false, false);
  }

  void stop() {
    if (!scan_) return;
    if (!running_) return;

    uint32_t now = millis();
    LOGI("BLE OFF (ran %lu ms)", (unsigned long)(now - last_on_ms_));

    last_off_ms_ = now;
    running_ = false;
    scan_->stop();
  }

  bool isRunning() const { return running_; }

  void onDiscovered(const NimBLEAdvertisedDevice* dev) override { handle_(dev); }
  void onResult(const NimBLEAdvertisedDevice* dev) override { handle_(dev); }

private:
  void handle_(const NimBLEAdvertisedDevice* dev) {
    if (!dev->haveManufacturerData()) return;

    std::string md = dev->getManufacturerData();
    const uint8_t* d = (const uint8_t*)md.data();
    const size_t   n = md.size();

    if (n < 11) return;

    uint16_t company = (uint16_t)d[0] | ((uint16_t)d[1] << 8);
    if (company != BLE_COMPANY_ID) return;
    if (d[2] != BLE_PREFIX0 || d[3] != BLE_PREFIX1) return;

    uint16_t device_id = (uint16_t)d[4] | ((uint16_t)d[5] << 8);
    uint16_t seq       = (uint16_t)d[6] | ((uint16_t)d[7] << 8);
    uint16_t pulses    = (uint16_t)d[8] | ((uint16_t)d[9] << 8);
    uint16_t bat       = (uint16_t)d[10];

    uint32_t key = ((uint32_t)device_id << 16) | (uint32_t)seq;
    if (recent_ && recent_->seenOrInsert(key)) return;

    BleRecord r;
    r.rx_ts = TimeSync::nowUtc();
    r.device_id = device_id;
    r.seq = seq;
    r.pulses = pulses;
    r.bat = bat;
    r.rssi = dev->getRSSI();

    buf_->push(r);

    LOGI("BLE rx dev=%u seq=%u pulses=%u bat=%u ts=%lu rssi=%d buf=%u/%u",
         r.device_id, r.seq, r.pulses, r.bat,
         (unsigned long)r.rx_ts, (int)r.rssi,
         buf_->size(), buf_->capacity());
  }

  NimBLEScan* scan_{nullptr};
  bool running_{false};

  uint32_t last_on_ms_{0};
  uint32_t last_off_ms_{0};

  RecordBuffer* buf_{nullptr};
  RecentKeys* recent_{nullptr};
};