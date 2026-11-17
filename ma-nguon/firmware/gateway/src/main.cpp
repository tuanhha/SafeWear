// Original SafeWear gateway. Third-party dependencies: THIRD_PARTY_NOTICES.md.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <RadioLib.h>
#include <SPI.h>
#include <math.h>
#include "gateway_core.h"
#include "http_transport.h"
#include "raw_capture.h"

#ifndef SAFEWEAR_WIRED_ONLY
#define SAFEWEAR_WIRED_ONLY 0
#endif
#ifndef SAFEWEAR_RAW_PPG
#define SAFEWEAR_RAW_PPG 0
#endif
#if SAFEWEAR_RAW_PPG && (!SAFEWEAR_WIRED_ONLY || SAFEWEAR_HTTP_ENABLED)
#error "Raw PPG capture requires wired transport without HTTP"
#endif

namespace {
constexpr uint8_t kRadioSck = 18;
constexpr uint8_t kRadioMiso = 19;
constexpr uint8_t kRadioMosi = 23;
constexpr uint8_t kRadioNss = 5;
constexpr uint8_t kRadioReset = 14;
constexpr uint8_t kRadioDio0 = 26;
constexpr uint8_t kBuzzer = 25;  // Active buzzer through transistor, active HIGH.
constexpr uint8_t kWiredRx = 32;
constexpr uint8_t kWiredTx = 33;
constexpr size_t kMaxLine = safewear::kMaxJsonBytes;
constexpr size_t kQueueCapacity = 8;
constexpr uint32_t kRadioRetryMs = 5000;
constexpr uint32_t kRadioRearmMs = 30000;
constexpr const char *kTransport = SAFEWEAR_WIRED_ONLY ? "wired" : "lora";

bool radio_ok = false;
#if !SAFEWEAR_WIRED_ONLY
SX1278 radio = new Module(kRadioNss, kRadioDio0, kRadioReset, RADIOLIB_NC);
volatile bool radio_irq = false;
uint32_t last_radio_attempt = 0;
uint32_t last_radio_rearm = 0;
#endif
uint32_t last_heartbeat = 0;
uint32_t rx_total = 0;
uint32_t crc_errors = 0;
uint32_t frame_errors = 0;
uint32_t duplicate_events = 0;
uint32_t queue_drops = 0;
uint32_t input_errors = 0;
safewear::AlarmBook alarms;

struct JsonLine {
  char bytes[kMaxLine + 1];
  uint16_t length;
};

template <size_t Capacity> class LineQueue {
 public:
  bool push(const JsonLine &line) {
    if (count_ == Capacity) return false;
    items_[(head_ + count_) % Capacity] = line;
    ++count_;
    return true;
  }
  bool pop(JsonLine &line) {
    if (!count_) return false;
    line = items_[head_];
    head_ = (head_ + 1) % Capacity;
    --count_;
    return true;
  }
 private:
  JsonLine items_[Capacity] = {};
  size_t head_ = 0;
  size_t count_ = 0;
};

LineQueue<kQueueCapacity> urgent_queue;
LineQueue<kQueueCapacity> normal_queue;
JsonLine sending_line = {};
size_t sending_offset = 0;

void queueJson(const JsonDocument &document, bool urgent = false) {
  JsonLine line = {};
  const size_t content_bytes = measureJson(document);
  if (document.overflowed() || content_bytes + 1 > kMaxLine) {
    ++queue_drops;
    return;
  }
  serializeJson(document, line.bytes, sizeof(line.bytes));
  line.bytes[content_bytes] = '\n';
  line.length = static_cast<uint16_t>(content_bytes + 1);
  safewear_http::capture(line.bytes, line.length, urgent);
  if (urgent) {
    if (!urgent_queue.push(line)) ++queue_drops;
  } else if (!normal_queue.push(line)) {
    // The normal queue is latest-oriented. Alarm processing never depends on it.
    JsonLine discarded = {};
    normal_queue.pop(discarded);
    ++queue_drops;
    normal_queue.push(line);
  }
}

void diagnostic(const char *message, int16_t status = 0) {
  StaticJsonDocument<256> document;
  document["kind"] = "diagnostic";
  document["message"] = message;
  document["gateway_ms"] = millis();
  document["status"] = status;
  queueJson(document);
}

void drainUsb() {
  if (sending_offset >= sending_line.length) {
    if (!urgent_queue.pop(sending_line) && !normal_queue.pop(sending_line)) return;
    sending_offset = 0;
  }
  const int available = Serial.availableForWrite();
  if (available <= 0) return;
  const size_t remaining = sending_line.length - sending_offset;
  size_t amount = remaining < static_cast<size_t>(available)
                      ? remaining : static_cast<size_t>(available);
  if (amount > 128) amount = 128;
  sending_offset += Serial.write(
      reinterpret_cast<const uint8_t *>(sending_line.bytes + sending_offset), amount);
}

void updateBuzzer() {
  // Clock-based pattern does not block the receiver. ACK silences only its ID.
  digitalWrite(kBuzzer, alarms.audible() && (millis() % 500U < 200U) ? HIGH : LOW);
}

void handleCommand(const char *line) {
  StaticJsonDocument<768> document;
  const DeserializationError error = deserializeJson(document, line,
      DeserializationOption::NestingLimit(2));

  if (error || !document.is<JsonObject>() || document.size() != 2 ||
      !document["kind"].is<const char *>() ||
      !document["event_id"].is<const char *>()) {
    ++input_errors;
    diagnostic("invalid_command");
    return;
  }
  const char *kind = document["kind"];
  const char *text_id = document["event_id"];
  safewear::EventId id = {};
  if (document["kind"].as<JsonString>().size() != strlen(kind) ||
      document["event_id"].as<JsonString>().size() != strlen(text_id) ||
      !safewear::parseEventId(text_id, id)) {
    ++input_errors;
    diagnostic("invalid_event_id");
    return;
  }
  bool accepted = false;
  if (strcmp(kind, "alarm_ack") == 0) accepted = alarms.acknowledge(id);
  else if (strcmp(kind, "alarm_resolve") == 0) accepted = alarms.resolve(id);
  else { ++input_errors; diagnostic("unknown_command"); return; }
  updateBuzzer();
  safewear_http::publishActive(alarms);
  StaticJsonDocument<256> result;
  result["kind"] = "diagnostic";
  result["message"] = accepted ? "alarm_command_accepted_local_only" : "event_not_active";
  result["event_id"] = text_id;
  result["command"] = kind;
  queueJson(result, true);
}

void forwardFrame(const uint8_t *data, size_t length, float rssi, float snr,
                  bool have_radio_metadata) {
  safewear::FrameMeta meta = {};
  const safewear::DecodeStatus status = safewear::decode(data, length, meta);
  if (status != safewear::DecodeStatus::Ok) {
    if (status == safewear::DecodeStatus::BadCrc) ++crc_errors;
    else ++frame_errors;
    return;
  }
  ++rx_total;
  const safewear::ObserveResult observed = alarms.observe(meta);
  updateBuzzer();
  safewear_http::publishActive(alarms);
  if (observed == safewear::ObserveResult::CapacityExceeded) {
    diagnostic("alarm_capacity_exceeded_buzzer_latched");
  }
  // Repeated RF EVENT transmissions are one logical event. Snapshots continue
  // upstream so the PC can recover a lost EVENT and refresh sensor freshness.
  if (meta.type == 2 && meta.event_code != 3 &&
      (observed == safewear::ObserveResult::Duplicate ||
       observed == safewear::ObserveResult::ResolvedDuplicate)) {
    ++duplicate_events;
    return;
  }
  char hex[safewear::kSnapshotBytes * 2 + 1] = {};
  constexpr char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < length; ++i) {
    hex[i * 2] = digits[data[i] >> 4];
    hex[i * 2 + 1] = digits[data[i] & 15U];
  }
  StaticJsonDocument<512> document;
  document["kind"] = "rx";
  document["payload"] = hex;
  if (have_radio_metadata && isfinite(rssi) && isfinite(snr)) {
    document["rssi"] = rssi;
    document["snr"] = snr;
  } else {
    document["rssi"] = nullptr;
    document["snr"] = nullptr;
  }
  document["gateway_ms"] = millis();
  document["transport"] = kTransport;
  queueJson(document, meta.type == 2 || meta.has_alarm);
}

#if SAFEWEAR_WIRED_ONLY
void handleWired(const char *line) {
  StaticJsonDocument<768> document;
  const DeserializationError error = deserializeJson(document, line,
      DeserializationOption::NestingLimit(2));
#if SAFEWEAR_RAW_PPG
  if (!error && document["kind"].is<const char *>() &&
      strcmp(document["kind"], "ppg_raw") == 0) {
    if (!safewear::validRawCapture(document, strlen(line))) { ++input_errors; return; }
    StaticJsonDocument<192> raw;
    raw["kind"] = "ppg_raw";
    raw["sample_ms"] = document["sample_ms"].as<uint32_t>();
    raw["red"] = document["red"].as<uint32_t>();
    raw["ir"] = document["ir"].as<uint32_t>();
    raw["motion_ok"] = document["motion_ok"].as<bool>();
    queueJson(raw);  // Bounded normal queue; EVENT uses urgent queue.
    return;
  }
#endif
  if (error || !document.is<JsonObject>() ||
      !document["kind"].is<const char *>() ||
      strcmp(document["kind"], "rx") != 0 ||
      !document["payload"].is<const char *>()) {
    ++input_errors;
    return;
  }
  uint8_t data[safewear::kSnapshotBytes] = {};
  size_t length = 0;
  const char *payload = document["payload"];
  if (document["payload"].as<JsonString>().size() != strlen(payload) ||
      !safewear::decodeHex(payload, data, length)) {
    ++frame_errors;
    return;
  }
  forwardFrame(data, length, 0, 0, false);
}
#endif

safewear::LineReader usb_input;
#if SAFEWEAR_WIRED_ONLY
safewear::LineReader wired_input;
#endif

void readLines(Stream &stream, safewear::LineReader &input,
               void (*handler)(const char *)) {
  // A finite byte budget prevents an input flood from starving the RF/alarm loop.
  for (size_t budget = 0; budget < 128 && stream.available() > 0; ++budget) {
    const int value = stream.read();
    if (value < 0) break;
    const safewear::LineResult result = input.feed(static_cast<char>(value));
    if (result == safewear::LineResult::Ready) handler(input.line());
    else if (result == safewear::LineResult::Rejected) ++input_errors;
  }
}

#if !SAFEWEAR_WIRED_ONLY
void IRAM_ATTR onRadioReceive() { radio_irq = true; }

void startRadio() {
  last_radio_attempt = millis();
  radio_ok = false;
  radio_irq = false;
  int16_t status = radio.begin(433.92F, 500.0F, 7, 5, 0x12, 7, 8, 0);
  if (status == RADIOLIB_ERR_NONE) status = radio.explicitHeader();
  if (status == RADIOLIB_ERR_NONE) status = radio.setCRC(true);
  if (status == RADIOLIB_ERR_NONE) {
    radio.setPacketReceivedAction(onRadioReceive);
    status = radio.startReceive();
  }
  radio_ok = status == RADIOLIB_ERR_NONE;
  last_radio_rearm = millis();
  diagnostic(radio_ok ? "radio_ready" : "radio_initialization_failed_retrying", status);
}

void receiveRadio() {
  if (!radio_ok) {
    if (millis() - last_radio_attempt >= kRadioRetryMs) startRadio();
    return;
  }
  bool pending = false;
  noInterrupts();
  pending = radio_irq;
  radio_irq = false;
  interrupts();
  if (pending) {
    const size_t length = radio.getPacketLength();
    uint8_t data[safewear::kSnapshotBytes] = {};
    int16_t status = RADIOLIB_ERR_NONE;
    if (length == safewear::kSnapshotBytes || length == safewear::kEventBytes) {
      status = radio.readData(data, length);
      const float rssi = radio.getRSSI();
      const float snr = radio.getSNR();
      if (status == RADIOLIB_ERR_NONE) forwardFrame(data, length, rssi, snr, true);
      else if (status == RADIOLIB_ERR_CRC_MISMATCH) ++crc_errors;
      else ++frame_errors;
    } else {
      ++frame_errors;
    }
    status = radio.startReceive();
    if (status != RADIOLIB_ERR_NONE) {
      radio_ok = false;
      last_radio_attempt = millis();
      diagnostic("radio_receive_restart_failed", status);
    }
    last_radio_rearm = millis();
  } else if (millis() - last_radio_rearm >= kRadioRearmMs) {
    // Re-arm a quiet/stuck receiver periodically. This is not proof of coverage.
    const int16_t status = radio.startReceive();
    radio_ok = status == RADIOLIB_ERR_NONE;
    last_radio_rearm = millis();
    if (!radio_ok) {
      last_radio_attempt = millis();
      diagnostic("radio_receive_health_failed", status);
    }
  }
}
#endif

void heartbeat() {
  const uint32_t now = millis();
  if (now - last_heartbeat < 1000U) return;
  last_heartbeat = now;
  StaticJsonDocument<512> document;
  document["kind"] = "heartbeat";
  document["gateway_ms"] = now;
  document["radio_ok"] = radio_ok;
  document["transport"] = kTransport;
  document["rx_total"] = rx_total;
  document["crc_errors"] = crc_errors;
  document["frame_errors"] = frame_errors;
  document["queue_drops"] = queue_drops;
  document["duplicate_events"] = duplicate_events;
  document["input_errors"] = input_errors;
  document["active_alarms"] = alarms.activeCount();
  document["alarm_on"] = alarms.audible();
  document["alarm_overflow"] = alarms.overflowed();
#if SAFEWEAR_HTTP_ENABLED
  document["http_enabled"] = safewear_http::enabled();
  document["http_accepted"] = safewear_http::accepted();
  document["http_drops"] = safewear_http::dropped();
  document["http_status"] = safewear_http::lastStatus();
#endif
  queueJson(document);
}
}  // namespace

void setup() {
  pinMode(kBuzzer, OUTPUT);
  digitalWrite(kBuzzer, LOW);
  Serial.begin(115200);
  // Never wait for a host or a radio before servicing the alarm/USB loop.
#if SAFEWEAR_WIRED_ONLY
  Serial2.begin(115200, SERIAL_8N1, kWiredRx, kWiredTx);
  diagnostic("wired_validation_mode_no_radio");
#else
  SPI.begin(kRadioSck, kRadioMiso, kRadioMosi, kRadioNss);
  startRadio();
#endif
  safewear_http::begin();
}

void loop() {
#if SAFEWEAR_WIRED_ONLY
  readLines(Serial2, wired_input, handleWired);
#else
  receiveRadio();
#endif
  readLines(Serial, usb_input, handleCommand);
  safewear_http::poll(alarms);
  updateBuzzer();
  heartbeat();
  drainUsb();
  delay(1);  // Yield one RTOS tick; no polling wait or unbounded retry loop.
}
