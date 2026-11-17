// SafeWear chest node: nRF52840 Pro Micro + Ra-02 (SX1278) + MPU6050 + SOS
// button (P1.11 to GND) + buzzer (P1.06 via C1815) + LED (P0.15).
// Ported from firmware/node/src/main.c and transport.c (STM32): same 5 s
// snapshot window, 50 Hz IMU features, fall/SOS logic, journal record, event
// priority (<=3 transmissions), latest-only snapshot and 99x airtime off-time.
// The core logic is firmware/node/src/node_core.c compiled unchanged.
// HR/SpO2 come from the wrist board over BLE advertising (wrist_ble.h).
// Battery voltage is not measured (no verified divider): always 65535.
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <bluefruit.h>
#include <nrf_gpio.h>
#include <RadioLib.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
#include <math.h>
#include <string.h>
extern "C" {
#include "node_core.h"
}
#include "wrist_ble.h"

using namespace Adafruit_LittleFS_Namespace;

static const uint32_t LORA_SCK = NRF_GPIO_PIN_MAP(0, 8);
static const uint32_t LORA_MISO = NRF_GPIO_PIN_MAP(0, 17);
static const uint32_t LORA_MOSI = NRF_GPIO_PIN_MAP(0, 6);
static const uint32_t LORA_NSS = NRF_GPIO_PIN_MAP(0, 20);
static const uint32_t LORA_RST = NRF_GPIO_PIN_MAP(0, 24);
static const uint32_t LORA_DIO0 = NRF_GPIO_PIN_MAP(1, 0);
static const uint32_t I2C_SDA = NRF_GPIO_PIN_MAP(0, 11);
static const uint32_t I2C_SCL = NRF_GPIO_PIN_MAP(0, 22);
static const uint32_t SOS_PIN = NRF_GPIO_PIN_MAP(1, 11);
static const uint32_t VCC_CTRL = NRF_GPIO_PIN_MAP(0, 13);
static const uint32_t BUZZER = NRF_GPIO_PIN_MAP(1, 6);
static const uint32_t LED = NRF_GPIO_PIN_MAP(0, 15);
static const uint8_t MPU_ADDR = 0x68;

// ---------------------------------------------------------------- RadioLib HAL
class NrfBitBangHal : public RadioLibHal {
 public:
  NrfBitBangHal() : RadioLibHal(0, 1, 0, 1, 2, 3) {}
  // Module::init() only calls hal->init(); configure SPI pins here (otherwise
  // the first begin() reads garbage and returns -2).
  void init() override { spiBegin(); }
  void pinMode(uint32_t pin, uint32_t mode) override {
    if (pin == RADIOLIB_NC) return;
    if (mode == 1) nrf_gpio_cfg_output(pin); else nrf_gpio_cfg_input(pin, NRF_GPIO_PIN_NOPULL);
  }
  void digitalWrite(uint32_t pin, uint32_t value) override {
    if (pin == RADIOLIB_NC) return;
    value ? nrf_gpio_pin_set(pin) : nrf_gpio_pin_clear(pin);
  }
  uint32_t digitalRead(uint32_t pin) override { return pin == RADIOLIB_NC ? 0 : nrf_gpio_pin_read(pin); }
  void attachInterrupt(uint32_t, void (*)(void), uint32_t) override {}
  void detachInterrupt(uint32_t) override {}
  void delay(RadioLibTime_t ms) override { ::delay(ms); }
  void delayMicroseconds(RadioLibTime_t us) override { ::delayMicroseconds(us); }
  RadioLibTime_t millis() override { return ::millis(); }
  RadioLibTime_t micros() override { return ::micros(); }
  long pulseIn(uint32_t, uint32_t, RadioLibTime_t) override { return 0; }
  void spiBegin() override {
    nrf_gpio_cfg_output(LORA_SCK); nrf_gpio_pin_clear(LORA_SCK);
    nrf_gpio_cfg_output(LORA_MOSI); nrf_gpio_pin_clear(LORA_MOSI);
    nrf_gpio_cfg_input(LORA_MISO, NRF_GPIO_PIN_NOPULL);
  }
  void spiBeginTransaction() override {}
  void spiTransfer(uint8_t *out, size_t len, uint8_t *in) override {
    for (size_t n = 0; n < len; n++) {
      uint8_t o = out ? out[n] : 0, r = 0;
      for (int i = 7; i >= 0; i--) {
        (o >> i) & 1 ? nrf_gpio_pin_set(LORA_MOSI) : nrf_gpio_pin_clear(LORA_MOSI);
        ::delayMicroseconds(1);
        nrf_gpio_pin_set(LORA_SCK);
        r = (r << 1) | (nrf_gpio_pin_read(LORA_MISO) ? 1 : 0);
        ::delayMicroseconds(1);
        nrf_gpio_pin_clear(LORA_SCK);
      }
      if (in) in[n] = r;
    }
  }
  void spiEndTransaction() override {}
  void spiEnd() override {}
};
static NrfBitBangHal hal;
static SX1278 radio(new Module(&hal, LORA_NSS, LORA_DIO0, LORA_RST, RADIOLIB_NC));

// ---------------------------------------------------------------- bit-banged I2C (MPU6050)
static void odCfg(uint32_t pin) {
  nrf_gpio_cfg(pin, NRF_GPIO_PIN_DIR_OUTPUT, NRF_GPIO_PIN_INPUT_CONNECT, NRF_GPIO_PIN_NOPULL,
               NRF_GPIO_PIN_S0D1, NRF_GPIO_PIN_NOSENSE);
  nrf_gpio_pin_set(pin);
}
static inline void i2cDelay() { delayMicroseconds(3); }
static inline void sda(bool v) { v ? nrf_gpio_pin_set(I2C_SDA) : nrf_gpio_pin_clear(I2C_SDA); }
static inline bool sclRelease() {
  nrf_gpio_pin_set(I2C_SCL);
  for (int i = 0; i < 1000; i++) {
    if (nrf_gpio_pin_read(I2C_SCL)) return true;
    delayMicroseconds(1);
  }
  return false;
}
static inline void sclLow() { nrf_gpio_pin_clear(I2C_SCL); }
static void i2cStart() { sda(1); sclRelease(); i2cDelay(); sda(0); i2cDelay(); sclLow(); i2cDelay(); }
static void i2cStop() { sda(0); i2cDelay(); sclRelease(); i2cDelay(); sda(1); i2cDelay(); }
static bool i2cWrite(uint8_t b) {
  for (int i = 7; i >= 0; i--) {
    sda((b >> i) & 1);
    i2cDelay();
    if (!sclRelease()) return false;
    i2cDelay();
    sclLow();
  }
  sda(1);
  i2cDelay();
  if (!sclRelease()) return false;
  i2cDelay();
  bool ack = !nrf_gpio_pin_read(I2C_SDA);
  sclLow();
  i2cDelay();
  return ack;
}
static uint8_t i2cRead(bool ack) {
  uint8_t b = 0;
  sda(1);
  for (int i = 0; i < 8; i++) {
    i2cDelay(); sclRelease(); i2cDelay();
    b = (b << 1) | (nrf_gpio_pin_read(I2C_SDA) ? 1 : 0);
    sclLow();
  }
  sda(!ack); i2cDelay(); sclRelease(); i2cDelay(); sclLow(); sda(1); i2cDelay();
  return b;
}
static bool i2cWriteReg(uint8_t reg, uint8_t v) {
  i2cStart();
  bool ok = i2cWrite(MPU_ADDR << 1) && i2cWrite(reg) && i2cWrite(v);
  i2cStop();
  return ok;
}
static bool i2cReadRegs(uint8_t reg, uint8_t *d, int n) {
  i2cStart();
  if (!i2cWrite(MPU_ADDR << 1) || !i2cWrite(reg)) { i2cStop(); return false; }
  i2cStart();
  if (!i2cWrite((MPU_ADDR << 1) | 1)) { i2cStop(); return false; }
  for (int i = 0; i < n; i++) d[i] = i2cRead(i < n - 1);
  i2cStop();
  return true;
}
static void i2cRecover() {  // clock out a stuck slave, then STOP
  sda(1);
  for (int i = 0; i < 9; i++) { sclLow(); i2cDelay(); sclRelease(); i2cDelay(); }
  i2cStop();
}

// ---------------------------------------------------------------- IMU (same config as node)
static bool imuOk = false;
static uint32_t imuRetry = 0;
static bool imuConfigure() {
  uint8_t id = 0;
  if (!i2cReadRegs(0x75, &id, 1) || (id & 0x7E) != 0x68) return false;
  return i2cWriteReg(0x6B, 0x01) &&  // PLL, wake
         i2cWriteReg(0x1A, 0x04) &&  // DLPF ~21 Hz
         i2cWriteReg(0x19, 19) &&    // 1 kHz / 20 = 50 Hz
         i2cWriteReg(0x1C, 0x10);    // +-8 g, 4096 LSB/g
}
static bool imuRead(float *x, float *y, float *z, uint32_t now) {
  if (!imuOk) {
    if (now - imuRetry < 1000) return false;
    imuRetry = now;
    i2cRecover();
    imuOk = imuConfigure();
    if (!imuOk) return false;
  }
  uint8_t raw[6];
  if (!i2cReadRegs(0x3B, raw, 6)) { imuOk = false; imuRetry = now; return false; }
  *x = (int16_t)((uint16_t)raw[0] << 8 | raw[1]) / 4096.0f;
  *y = (int16_t)((uint16_t)raw[2] << 8 | raw[3]) / 4096.0f;
  *z = (int16_t)((uint16_t)raw[4] << 8 | raw[5]) / 4096.0f;
  return true;
}

// ---------------------------------------------------------------- journal (LittleFS)
static const char *JOURNAL_PATH = "/sw_journal.bin";
static uint32_t journalGeneration = 0;
static void journalLoad(sw_persistent *s) {
  memset(s, 0, sizeof(*s));
  File f(InternalFS);
  uint8_t rec[32];
  if (f.open(JOURNAL_PATH, FILE_O_READ)) {
    if (f.read(rec, 32) == 32) {
      sw_persistent c;
      if (sw_journal_decode(rec, &c)) { *s = c; journalGeneration = c.generation; }
    }
    f.close();
  }
}
static bool journalStore(sw_persistent *s) {
  sw_persistent c = *s;
  c.generation = journalGeneration + 1;
  uint8_t rec[32], back[32];
  sw_journal_encode(rec, &c);
  File f(InternalFS);
  if (!f.open(JOURNAL_PATH, FILE_O_WRITE)) return false;
  f.seek(0);
  bool ok = f.write(rec, 32) == 32;
  f.close();
  if (!ok || !f.open(JOURNAL_PATH, FILE_O_READ)) return false;
  ok = f.read(back, 32) == 32 && memcmp(rec, back, 32) == 0;
  f.close();
  if (ok) { *s = c; journalGeneration = c.generation; }
  return ok;
}

// ---------------------------------------------------------------- wrist data via BLE
struct WristState {
  uint32_t rxMs;
  uint16_t seq, hr, spo2, hrAge, spo2Age, lost;
  uint8_t flags;
  int8_t rssi;
};
static WristState wrist = {};
static uint32_t wristPackets = 0;
static uint16_t wristLostAtLastSnapshot = 0;
static uint16_t get16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static void scanCallback(ble_gap_evt_adv_report_t *report) {
  uint8_t m[32];
  uint8_t len = Bluefruit.Scanner.parseReportByType(report, BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA, m, sizeof(m));
  if (len >= WRIST_ADV_LEN && m[0] == 0xFF && m[1] == 0xFF && m[2] == 'H' && m[3] == 1) {
    WristState w;
    w.rxMs = millis();
    w.seq = get16(m + 4);
    w.hr = get16(m + 6);
    w.spo2 = get16(m + 8);
    w.hrAge = get16(m + 10);
    w.spo2Age = get16(m + 12);
    w.flags = m[14];
    w.lost = get16(m + 15);
    w.rssi = report->rssi;
    taskENTER_CRITICAL();
    wrist = w;
    wristPackets++;
    taskEXIT_CRITICAL();
  }
  Bluefruit.Scanner.resume();
}
static WristState wristSnapshot() {
  taskENTER_CRITICAL();
  WristState w = wrist;
  taskEXIT_CRITICAL();
  return w;
}

// ---------------------------------------------------------------- feedback
static uint32_t buzzerUntil = 0;
static void beepFor(uint32_t ms, uint32_t now) {
  buzzerUntil = now + ms;
  if (!buzzerUntil) buzzerUntil = 1;
  nrf_gpio_pin_set(BUZZER);
}
static void feedback(bool active, bool fault, uint32_t now) {
  if (buzzerUntil && (int32_t)(now - buzzerUntil) >= 0) { buzzerUntil = 0; nrf_gpio_pin_clear(BUZZER); }
  bool led;
  if (active) led = (now / 250) % 2;          // alarm: 2 Hz blink
  else if (fault) led = (now % 2000) < 100;   // fault: short blip every 2 s
  else led = (now % 5000) < 30;               // OK: tiny blip every 5 s
  led ? nrf_gpio_pin_set(LED) : nrf_gpio_pin_clear(LED);
}

// ---------------------------------------------------------------- transport (port of node transport.c)
static uint8_t snapshotFrame[70];
static bool snapshotPending = false;
struct PendingEvent { uint8_t bytes[20]; uint8_t attempts; };
static PendingEvent events[2];
static uint8_t eventCount = 0;
static bool healthy = false, transmitting = false, sendingEvent = false;
static uint32_t eligibleMs = 0, txStart = 0, retryMs = 0;
static uint16_t currentLength = 0;
static uint32_t sequence = 0;
static uint32_t txOk = 0, txFail = 0;
static int16_t lastRadioStatus = 0;

static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void set16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static uint16_t ageAdd(const uint8_t *p, uint32_t delayMs) {
  uint32_t old = get16(p);
  return old == 65535 || old + delayMs >= 65535 ? 65535 : (uint16_t)(old + delayMs);
}
static bool radioInit() {
  int16_t st = radio.begin(433.92F, 500.0F, 7, 5, 0x12, 7, 8, 0);
  if (st == RADIOLIB_ERR_NONE) st = radio.explicitHeader();
  if (st == RADIOLIB_ERR_NONE) st = radio.setCRC(true);
  lastRadioStatus = st;
  return st == RADIOLIB_ERR_NONE;
}
static void transportInit(uint32_t now) {
  healthy = radioInit();
  retryMs = now;
  eligibleMs = now + 3200;  // protocol boot cooldown
}
static void transportSnapshot(const uint8_t f[70]) {
  memcpy(snapshotFrame, f, 70);
  snapshotPending = true;
}
static bool transportEvent(const uint8_t f[20]) {
  if (eventCount == 2) return false;
  memcpy(events[eventCount].bytes, f, 20);
  events[eventCount].attempts = 0;
  ++eventCount;
  return true;
}
static bool startSend(const uint8_t *data, uint16_t length, uint32_t now) {
  currentLength = length;
  txStart = now;
  int16_t st = radio.startTransmit(data, length);
  lastRadioStatus = st;
  return st == RADIOLIB_ERR_NONE;
}
static void sentOrTimedOut(uint32_t now, bool success) {
  radio.finishTransmit();  // standby + clear IRQ before the off-time
  uint32_t elapsed = now - txStart;
  uint32_t minimumOff = currentLength == 20 ? 1401 : 3175;
  uint32_t off = elapsed * 99;
  eligibleMs = now + (off > minimumOff ? off : minimumOff);
  transmitting = false;
  if (success) txOk++; else txFail++;
  if (sendingEvent && eventCount) {
    if (++events[0].attempts >= 3) {
      if (eventCount == 2) events[0] = events[1];
      --eventCount;
    }
  }
  if (!success) { healthy = false; retryMs = now; }
}
static void transportService(uint32_t now, bool durable) {
  if (transmitting) {
    bool complete = nrf_gpio_pin_read(LORA_DIO0) ||
                    (radio.getIRQFlags() & RADIOLIB_SX127X_CLEAR_IRQ_FLAG_TX_DONE);
    if (complete) sentOrTimedOut(now, true);
    else if (now - txStart >= 100) sentOrTimedOut(now, false);
    return;
  }
  if (!healthy && now - retryMs >= 1000) { retryMs = now; healthy = radioInit(); }
  if (!healthy || !durable || (int32_t)(now - eligibleMs) < 0) return;
  if (eventCount) {
    sendingEvent = true;
    transmitting = startSend(events[0].bytes, 20, now);
  } else {
    if (snapshotPending && now - get32(snapshotFrame + 12) > 2000) snapshotPending = false;
    if (!snapshotPending) return;
    uint32_t age = now - get32(snapshotFrame + 12);
    uint8_t f[70];
    memcpy(f, snapshotFrame, 70);
    uint32_t seq = ++sequence;
    for (int i = 0; i < 4; i++) f[8 + i] = (seq >> (8 * i)) & 0xFF;
    set16(f + 24, ageAdd(snapshotFrame + 24, age));
    set16(f + 26, ageAdd(snapshotFrame + 26, age));
    set16(f + 68, sw_crc16(f, 68));
    sendingEvent = false;
    transmitting = startSend(f, 70, now);
    if (transmitting) snapshotPending = false;
  }
  if (!transmitting) {
    radio.standby();
    eligibleMs = now + 9900;
    healthy = false;
    retryMs = now;
  }
}

// ---------------------------------------------------------------- node logic (port of node main.c)
static sw_persistent persistent;
static sw_features features;
static sw_button button;
static sw_fall fall;
static uint32_t journalRetry = 0;
static bool identityDurable = false, journalDirty = false, eventQueueFault = false;

static void persistTransition(uint32_t now) {
  journalDirty = !journalStore(&persistent);
  if (!journalDirty) identityDurable = true;
  journalRetry = now;
}
static void eventTransition(uint8_t code, uint8_t reason, uint32_t now) {
  sw_header header = {SAFEWEAR_NODE_ID, persistent.boot_counter, 0, now};
  if (code == SW_LOCAL_CLEAR) {
    if (!persistent.kind) return;
    header.boot_id = persistent.active_boot;
    header.sequence = persistent.active_sequence;
    persistent.kind = 0;
    persistent.reason = 0;
    persistent.active_boot = persistent.active_sequence = 0;
  } else {
    if (persistent.kind == SW_MANUAL_SOS || persistent.kind == code) return;
    header.sequence = ++sequence;
    persistent.kind = code;
    persistent.reason = reason;
    persistent.active_boot = header.boot_id;
    persistent.active_sequence = header.sequence;
  }
  beepFor(persistent.kind ? 600 : 120, now);  // local feedback before the flash write
  persistTransition(now);
  uint8_t encoded[20];
  sw_encode_event(encoded, &header, code, reason);
  if (!transportEvent(encoded)) eventQueueFault = true;
  Serial.printf("EVENT %s seq=%lu journal=%s\n",
                code == SW_LOCAL_CLEAR ? "LOCAL_CLEAR" : (code == SW_MANUAL_SOS ? "MANUAL_SOS" : "FALL_SUSPECT"),
                (unsigned long)header.sequence, journalDirty ? "FAILED" : "ok");
}
static void makeSnapshot(uint32_t now) {
  sw_snapshot s;
  memset(&s, 0, sizeof(s));
  s.header = (sw_header){SAFEWEAR_NODE_ID, persistent.boot_counter, 0, now};
  WristState w = wristSnapshot();
  bool seen = w.rxMs && now - w.rxMs <= 5000;
  uint32_t since = now - w.rxMs;
  uint32_t hrAge = (uint32_t)w.hrAge + since, spo2Age = (uint32_t)w.spo2Age + since;
  bool hrOk = seen && (w.flags & WRIST_HR_VALID) && w.hrAge != 65535 && hrAge <= 5000;
  bool spo2Ok = seen && (w.flags & WRIST_SPO2_VALID) && w.spo2Age != 65535 && spo2Age <= 5000;
  s.hr = hrOk ? w.hr : SW_UNKNOWN;
  s.spo2 = spo2Ok ? w.spo2 : SW_UNKNOWN;
  s.hr_age = hrOk ? (uint16_t)hrAge : SW_UNKNOWN;
  s.spo2_age = spo2Ok ? (uint16_t)spo2Age : SW_UNKNOWN;
  s.battery_mv = SW_UNKNOWN;  // not measured: no verified divider
  uint16_t lostDelta = 0;
  if (seen) {
    lostDelta = (uint16_t)(w.lost - wristLostAtLastSnapshot);
    wristLostAtLastSnapshot = w.lost;
  }
  s.imu_count = features.count;
  s.lost_imu = features.lost;
  s.lost_ppg = lostDelta;
  if (sw_features_finish(&features, s.features)) s.flags |= SW_IMU_VALID;
  if (hrOk) s.flags |= SW_HR_VALID;
  if (spo2Ok) s.flags |= SW_SPO2_VALID;
  if (!seen || (w.flags & WRIST_PPG_FAULT)) s.flags |= SW_PPG_FAULT;
  if (!seen || (w.flags & WRIST_SENSOR_GAP) || lostDelta || features.gap || features.lost) s.flags |= SW_SENSOR_GAP;
  if (persistent.kind) {
    s.flags |= SW_SOS_ACTIVE;
    if (persistent.kind == SW_FALL_SUSPECT) s.flags |= SW_FALL_ACTIVE;
    s.active_boot = persistent.active_boot;
    s.active_sequence = persistent.active_sequence;
  }
  uint8_t encoded[70];
  sw_encode_snapshot(encoded, &s);
  transportSnapshot(encoded);
  char hrText[12] = "-", spo2Text[8] = "-";
  if (hrOk) snprintf(hrText, sizeof(hrText), "%u.%u", s.hr / 10, s.hr % 10);
  if (spo2Ok) snprintf(spo2Text, sizeof(spo2Text), "%u", s.spo2 / 100);
  Serial.printf("SNAP flags=0x%03X hr=%s spo2=%s imu=%u lost_imu=%u wrist=%s rssi=%d pkts=%lu | radio %s ok=%lu fail=%lu st=%d\n",
                s.flags, hrText, spo2Text, s.imu_count, s.lost_imu, seen ? "seen" : "MISSING", seen ? w.rssi : 0,
                (unsigned long)wristPackets, healthy ? "ok" : "DOWN", (unsigned long)txOk, (unsigned long)txFail,
                lastRadioStatus);
}

static uint32_t nextImu, nextWindow;

void setup() {
  nrf_gpio_cfg_output(VCC_CTRL);
  nrf_gpio_pin_set(VCC_CTRL);  // 3.3 V rail for Ra-02/MPU/buzzer
  nrf_gpio_cfg_output(BUZZER);
  nrf_gpio_pin_clear(BUZZER);
  nrf_gpio_cfg_output(LED);
  nrf_gpio_pin_clear(LED);
  nrf_gpio_cfg_input(SOS_PIN, NRF_GPIO_PIN_PULLUP);
  nrf_gpio_cfg_input(LORA_DIO0, NRF_GPIO_PIN_NOPULL);
  odCfg(I2C_SDA);
  odCfg(I2C_SCL);
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) delay(10);
  Serial.println("SafeWear chest node (nRF52840)");

  InternalFS.begin();
  journalLoad(&persistent);
  ++persistent.boot_counter;
  if (!persistent.boot_counter) ++persistent.boot_counter;
  delay(100);  // rail settle
  uint32_t now = millis();
  persistTransition(now);  // boot identity must be durable before any frame
  if (persistent.kind) beepFor(600, now);  // recovered active alarm
  Serial.printf("boot_id=%lu journal %s, recovered alarm kind=%u\n", (unsigned long)persistent.boot_counter,
                journalDirty ? "WRITE FAILED" : "ok", persistent.kind);

  Bluefruit.begin(0, 1);
  Bluefruit.setName("SW-CHEST");
  Bluefruit.Scanner.setRxCallback(scanCallback);
  Bluefruit.Scanner.setInterval(160, 80);
  Bluefruit.Scanner.useActiveScan(false);
  Bluefruit.Scanner.start(0);

  imuOk = imuConfigure();
  imuRetry = millis();
  transportInit(millis());
  Serial.printf("MPU6050 %s, LoRa %s (status %d)\n", imuOk ? "ok" : "NOT FOUND", healthy ? "ok" : "FAILED",
                lastRadioStatus);
  if (!persistent.kind) beepFor(healthy && imuOk ? 80 : 400, millis());

  now = millis();
  nextImu = now;
  nextWindow = now + 5000;
  sw_features_begin(&features, now);
}

void loop() {
  uint32_t now = millis();
  bool sosPressed = nrf_gpio_pin_read(SOS_PIN) == 0;
  static bool sosLogged = false;
  static uint32_t sosSince = 0;
  if (sosPressed != sosLogged) {  // raw pin log: helps check the button wiring
    Serial.printf("SOS pin %s (previous state lasted %lu ms)\n", sosPressed ? "PRESSED" : "released",
                  (unsigned long)(now - sosSince));
    sosLogged = sosPressed;
    sosSince = now;
  }
  uint8_t action = sw_button_update(&button, sosPressed, persistent.kind, now);
  if (action) eventTransition(action, 0x02, now);
  if (journalDirty && now - journalRetry >= 1000) persistTransition(now);

  // Close the half-open window BEFORE adding a sample at its end.
  if ((int32_t)(now - nextWindow) >= 0) {
    makeSnapshot(now);
    do { nextWindow += 5000; } while ((int32_t)(now - nextWindow) >= 0);
    sw_features_begin(&features, nextWindow - 5000);
  }
  if ((int32_t)(now - nextImu) >= 0) {
    uint32_t missed = (now - nextImu) / 20;
    if (missed) sw_features_lost(&features, missed);
    nextImu += (missed + 1) * 20;
    float x, y, z;
    if (imuRead(&x, &y, &z, now)) {
      sw_features_add(&features, x, y, z, now);
      if (sw_fall_update(&fall, x, y, z, now) && !persistent.kind) eventTransition(SW_FALL_SUSPECT, 0x01, now);
    } else {
      sw_features_lost(&features, 1);
      fall.phase = 0;
      fall.have_previous = false;
    }
  }
  now = millis();
  transportService(now, identityDurable);
  WristState w = wristSnapshot();
  bool wristMissing = !w.rxMs || now - w.rxMs > 5000;
  feedback(persistent.kind != 0, !imuOk || !healthy || journalDirty || eventQueueFault || wristMissing, now);
  delay(1);
}
