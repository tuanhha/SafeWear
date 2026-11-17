// SafeWear wrist (ESP32-C3 SuperMini + MAX30102).
// PPG acquisition and quality gate adapted from firmware/node/src/sensors.c
// (itself adapted from rsecss/helmet aefdb2b, Apache-2.0): MAX30102 100 Hz,
// average 4 -> 25 Hz, 4 s window, Maxim algorithm, optical quality gate, values
// expire after 2 s. Difference from node: there is no IMU on the wrist, so the
// node's motion gate is NOT applied here (motion artefacts can pass).
#include <Arduino.h>
#include <Wire.h>
#include <BLEDevice.h>
#include <BLEAdvertising.h>
#include <math.h>
#include <string.h>
#include "wrist_ble.h"

extern "C" void maxim_heart_rate_and_oxygen_saturation(uint32_t *ir, int32_t n, uint32_t *red, int32_t *spo2,
                                                       int8_t *spo2_valid, int32_t *hr, int8_t *hr_valid);

static const int SDA_PIN = 0, SCL_PIN = 1, LED_PIN = 8;  // SuperMini blue LED, active low
static const uint8_t MAX_ADDR = 0x57;
static const uint16_t UNKNOWN = 65535;

static bool maxOk = false, ppgGap = true;
static uint32_t maxRetry = 0, lastPpgMs = 0, calcMs = 0, maxActivityMs = 0;
static uint16_t ppgLost = 0, filled = 0, newSamples = 0;
static uint32_t red[100], infrared[100];
static uint16_t hr = UNKNOWN, spo2 = UNKNOWN;
static uint32_t hrMs = 0, spo2Ms = 0;
static bool hrValid = false, spo2Valid = false;
static uint32_t lastIr = 0;

static bool writeReg(uint8_t reg, uint8_t v) {
  Wire.beginTransmission(MAX_ADDR);
  Wire.write(reg);
  Wire.write(v);
  return Wire.endTransmission() == 0;
}
static bool readRegs(uint8_t reg, uint8_t *d, size_t n) {
  Wire.beginTransmission(MAX_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)MAX_ADDR, (int)n) != (int)n) return false;
  for (size_t i = 0; i < n; i++) d[i] = Wire.read();
  return true;
}
static void ppgInvalidate() { hrValid = spo2Valid = false; hr = spo2 = UNKNOWN; }
static void ppgReset() { filled = newSamples = 0; lastPpgMs = 0; ppgGap = true; ppgInvalidate(); }
static void ppgLoss(uint16_t n) {
  uint32_t t = (uint32_t)ppgLost + n;
  ppgLost = t > 65535 ? 65535 : t;
  ppgReset();
}
static bool fifoReset() { return writeReg(0x04, 0) && writeReg(0x05, 0) && writeReg(0x06, 0); }
static bool maxConfigure() {
  uint8_t id = 0;
  if (!readRegs(0xFF, &id, 1) || id != 0x15) return false;
  bool ok = writeReg(0x09, 0x80) && writeReg(0x02, 0) && writeReg(0x03, 0) && fifoReset() &&
            writeReg(0x08, 0x40) &&                       // sample average 4, no rollover
            writeReg(0x0A, 0x27) &&                       // 4096 nA, 100 Hz, 411 us 18 bit
            writeReg(0x0C, 0x24) && writeReg(0x0D, 0x24) &&  // LED currents ~7 mA
            writeReg(0x09, 0x03);                          // SpO2 mode
  uint8_t discard[2];
  if (ok) ok = readRegs(0x00, discard, 2);
  ppgReset();
  maxActivityMs = millis();
  return ok;
}
static void ppgPush(uint32_t r, uint32_t ir) {
  if (filled < 100) {
    red[filled] = r;
    infrared[filled] = ir;
    ++filled;
  } else {
    memmove(red, red + 1, 99 * sizeof(uint32_t));
    memmove(infrared, infrared + 1, 99 * sizeof(uint32_t));
    red[99] = r;
    infrared[99] = ir;
  }
  ++newSamples;
  lastIr = ir;
  if (r < 1000 || ir < 1000 || r >= 250000 || ir >= 250000) ppgInvalidate();  // no finger / clipped
}
static void ppgPoll(uint32_t now) {
  uint8_t ptr[3];
  uint8_t count;
  if (!maxOk) {
    if (now - maxRetry < 1000) return;
    maxRetry = now;
    maxOk = maxConfigure();
    Serial.printf("MAX30102 %s\n", maxOk ? "online" : "not found");
    return;
  }
  if (!readRegs(0x04, ptr, 3)) goto fault;
  if (ptr[1] & 0x1F) {
    ppgLoss(ptr[1] & 0x1F);
    if (!fifoReset()) goto fault;
    return;
  }
  count = (ptr[0] - ptr[2]) & 0x1F;
  if (!count) {
    if (now - maxActivityMs > 200) goto fault;
    return;
  }
  if (lastPpgMs && now - lastPpgMs > 200) ppgLoss(1);
  lastPpgMs = maxActivityMs = now;
  while (count) {
    uint8_t n = count > 5 ? 5 : count, d[30];
    if (!readRegs(0x07, d, n * 6)) goto fault;
    for (uint8_t i = 0; i < n; i++) {
      uint8_t *p = d + i * 6;
      ppgPush(((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]) & 0x3FFFF,
              ((uint32_t)p[3] << 16 | (uint32_t)p[4] << 8 | p[5]) & 0x3FFFF);
    }
    count -= n;
  }
  return;
fault:
  maxOk = false;
  maxRetry = now;
  ppgLoss(1);
  Serial.println("MAX30102 bus fault");
}
static bool opticalQuality() {
  double sr = 0, si = 0, rr = 0, ii = 0, ri = 0;
  for (int i = 0; i < 100; i++) {
    if (red[i] < 1000 || infrared[i] < 1000 || red[i] >= 250000 || infrared[i] >= 250000) return false;
    sr += red[i];
    si += infrared[i];
  }
  double mr = sr / 100, mi = si / 100;
  for (int i = 0; i < 100; i++) {
    double r = red[i] - mr, x = infrared[i] - mi;
    rr += r * r;
    ii += x * x;
    ri += r * x;
  }
  if (rr <= 0 || ii <= 0) return false;
  double ar = sqrt(rr / 100) / mr, ai = sqrt(ii / 100) / mi;
  return ar >= 0.001 && ai >= 0.001 && ar <= 0.2 && ai <= 0.2 && ri > 0 && ri * ri >= 0.64 * rr * ii;
}
static void ppgCalculate(uint32_t now) {
  if (!maxOk || filled < 100 || now - lastPpgMs > 160) {
    ppgInvalidate();
    return;
  }
  if (newSamples < 25 || now - calcMs < 1000) return;
  newSamples = 0;
  calcMs = now;
  ppgInvalidate();
  if (!opticalQuality()) return;
  int32_t chr = -999, cspo2 = -999;
  int8_t hv = 0, sv = 0;
  maxim_heart_rate_and_oxygen_saturation(infrared, 100, red, &cspo2, &sv, &chr, &hv);
  if (hv && chr > 0 && chr <= 6553) {
    hr = chr * 10;
    hrMs = now;
    hrValid = true;
  }
  if (sv && cspo2 >= 0 && cspo2 <= 100) {
    spo2 = cspo2 * 100;
    spo2Ms = now;
    spo2Valid = true;
  }
  ppgGap = false;
}

static BLEAdvertising *adv;
static uint16_t advSeq = 0;
static void put16(uint8_t *p, uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = v >> 8;
}

static void advertise(uint32_t now) {
  bool fresh = maxOk && lastPpgMs && now - lastPpgMs <= 160;
  bool hv = hrValid && fresh && now - hrMs <= 2000;
  bool sv = spo2Valid && fresh && now - spo2Ms <= 2000;
  uint8_t m[WRIST_ADV_LEN] = {0xFF, 0xFF, 'H', 1};
  put16(m + 4, ++advSeq);
  put16(m + 6, hv ? hr : UNKNOWN);
  put16(m + 8, sv ? spo2 : UNKNOWN);
  put16(m + 10, hv ? (uint16_t)(now - hrMs) : UNKNOWN);
  put16(m + 12, sv ? (uint16_t)(now - spo2Ms) : UNKNOWN);
  m[14] = (hv ? WRIST_HR_VALID : 0) | (sv ? WRIST_SPO2_VALID : 0) | (!maxOk ? WRIST_PPG_FAULT : 0) |
          ((ppgGap || !fresh) ? WRIST_SENSOR_GAP : 0);
  put16(m + 15, ppgLost);
  uint32_t dc = lastIr / 16;
  put16(m + 17, dc > 65535 ? 65535 : dc);
  BLEAdvertisementData d;
  d.setFlags(0x06);
  d.setManufacturerData(std::string((const char *)m, sizeof(m)));
  adv->stop();
  adv->setAdvertisementData(d);
  adv->start();
  Serial.printf("ADV seq=%u hr=%s%u spo2=%s%u ir=%lu flags=0x%02X lost=%u\n", advSeq, hv ? "" : "~",
                hv ? hr / 10 : 0, sv ? "" : "~", sv ? spo2 / 100 : 0, (unsigned long)lastIr, m[14], ppgLost);
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);
  Serial.begin(115200);
  delay(1500);
  Serial.println("SafeWear wrist (ESP32-C3 + MAX30102)");
  Wire.begin(SDA_PIN, SCL_PIN, 100000);
  Wire.setTimeOut(5);
  maxOk = maxConfigure();
  Serial.printf("MAX30102 %s\n", maxOk ? "online" : "not found");
  maxRetry = millis();
  BLEDevice::init("SW-WRIST");
  adv = BLEDevice::getAdvertising();
  adv->setMinInterval(0xA0);  // 100 ms
  adv->setMaxInterval(0xA0);
}

void loop() {
  static uint32_t nextPoll = 0, nextAdv = 0;
  uint32_t now = millis();
  if ((int32_t)(now - nextPoll) >= 0) {
    nextPoll = now + 20;
    ppgPoll(now);
    ppgCalculate(millis());
  }
  if ((int32_t)(now - nextAdv) >= 0) {
    nextAdv = now + 1000;
    advertise(now);
    digitalWrite(LED_PIN, LOW);
    delay(5);
    digitalWrite(LED_PIN, HIGH);
  }
  delay(2);
}
