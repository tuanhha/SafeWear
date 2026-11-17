// BLE advertising payload wrist -> chest (manufacturer specific data).
// Test-only company ID 0xFFFF. Little endian. Not authenticated: any nearby
// device could imitate it (documented limitation).
#pragma once
#include <stdint.h>
enum : uint8_t {
  WRIST_HR_VALID = 1u << 0,
  WRIST_SPO2_VALID = 1u << 1,
  WRIST_PPG_FAULT = 1u << 2,  // MAX30102 not responding
  WRIST_SENSOR_GAP = 1u << 3, // FIFO overflow / stale samples / no finger
};
// [0..1] FF FF, [2] 'H', [3] version 1, [4..5] adv sequence, [6..7] hr_bpm_x10,
// [8..9] spo2_x100, [10..11] hr_age_ms, [12..13] spo2_age_ms, [14] flags,
// [15..16] lost PPG samples since boot (saturating), [17..18] IR DC level/16.
static const uint8_t WRIST_ADV_LEN = 19;
