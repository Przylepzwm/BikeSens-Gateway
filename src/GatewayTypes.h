#pragma once
#include <Arduino.h>

struct BleRecord {
  uint32_t rx_ts;     // epoch seconds (UTC)
  uint16_t device_id; // from telegram
  uint16_t seq;       // from telegram
  uint16_t pulses;    // from telegram
  uint16_t bat;       // from telegram (raw; unit decided by sensor)
  int8_t   rssi;      // helpful for debug (optional)
};
