#include "http_transport.h"
#if SAFEWEAR_HTTP_ENABLED
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <atomic>
#include "http_core.h"
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef SAFEWEAR_WIFI_SSID
#define SAFEWEAR_WIFI_SSID ""
#endif
#ifndef SAFEWEAR_WIFI_PASSWORD
#define SAFEWEAR_WIFI_PASSWORD ""
#endif
#ifndef SAFEWEAR_GATEWAY_ID
#define SAFEWEAR_GATEWAY_ID "gw-01"
#endif
#ifndef SAFEWEAR_HTTP_URL
#define SAFEWEAR_HTTP_URL ""
#endif
#ifndef SAFEWEAR_GATEWAY_TOKEN
#define SAFEWEAR_GATEWAY_TOKEN ""
#endif

namespace safewear_http {
namespace {
PacketQueue<kQueuePackets> urgent_queue;
PacketQueue<kQueuePackets> normal_queue;
ActiveIds active = {};
portMUX_TYPE state_mux = portMUX_INITIALIZER_UNLOCKED;
QueueHandle_t command_queue = nullptr;
std::atomic<uint32_t> drop_count{0};
std::atomic<uint32_t> accepted_count{0};
std::atomic<int> status_code{0};
bool configured = false;
char request_body[contract::kRequestBytes + 1];
char response_body[contract::kResponseBytes + 1];

uint64_t monotonicMs() { return static_cast<uint64_t>(esp_timer_get_time()) / 1000U; }

bool configurationValid() {
  const char *gateway = SAFEWEAR_GATEWAY_ID;
  const size_t length = strlen(gateway);
  if (!length || length > 48 || strlen(SAFEWEAR_GATEWAY_TOKEN) < 32 ||
      strlen(SAFEWEAR_GATEWAY_TOKEN) > 256 || !strlen(SAFEWEAR_WIFI_SSID) ||
      strlen(SAFEWEAR_WIFI_PASSWORD) < 8 || strlen(SAFEWEAR_WIFI_PASSWORD) > 64) return false;
  for (size_t i = 0; i < length; ++i) {
    const char value = gateway[i];
    if (!((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
          (value >= '0' && value <= '9') || value == '_' || value == '-')) return false;
  }
  for (const char *value = SAFEWEAR_GATEWAY_TOKEN; *value; ++value) {
    if (*value < 33 || *value > 126) return false;
  }
  const char *url = SAFEWEAR_HTTP_URL;
  const size_t url_length = strlen(url), path_length = strlen(contract::kPath);
  return url_length <= 240 && url_length > path_length + 7 &&
         strncmp(url, "http://", 7) == 0 && !strchr(url, '?') && !strchr(url, '#') &&
         !strchr(url + 7, '@') && strcmp(url + url_length - path_length, contract::kPath) == 0;
}

void newBoot(char *output) {
  snprintf(output, 17, "%08lx%08lx", static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()));
}

bool popPacket(Packet &packet) {
  bool result;
  portENTER_CRITICAL(&state_mux);
  result = urgent_queue.pop(packet) || normal_queue.pop(packet);
  portEXIT_CRITICAL(&state_mux);
  return result;
}

bool urgentWaiting() {
  portENTER_CRITICAL(&state_mux);
  const bool result = urgent_queue.size() != 0;
  portEXIT_CRITICAL(&state_mux);
  return result;
}

ActiveIds advertisedNow() {
  ActiveIds copy;
  portENTER_CRITICAL(&state_mux);
  copy = active;
  portEXIT_CRITICAL(&state_mux);
  return copy;
}

class BoundedSink : public Stream {
 public:
  explicit BoundedSink(uint64_t started) : started_(started) { response_body[0] = '\0'; }
  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t *data, size_t length) override {
    if (size_ + length > contract::kResponseBytes || monotonicMs() - started_ > 6000) {
      failed_ = true;
      return 0;
    }
    memcpy(response_body + size_, data, length);
    size_ += length;
    response_body[size_] = '\0';
    return length;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  size_t size() const { return size_; }
  bool failed() const { return failed_; }
 private:
  uint64_t started_;
  size_t size_ = 0;
  bool failed_ = false;
};

Delivery sendPacket(const Packet &packet, const Identity &identity) {
  const ActiveIds advertised = advertisedNow();
  const uint64_t started = monotonicMs();
  const size_t length = buildEnvelope(request_body, sizeof(request_body),
      SAFEWEAR_GATEWAY_ID, identity, packet, advertised, started);
  if (!length) { status_code.store(-1001); return Delivery::PermanentFailure; }
  WiFiClient client;
  client.setTimeout(2);  // WiFiClient API uses seconds; HTTPClient uses ms below.
  HTTPClient http;
  http.setConnectTimeout(1500);
  http.setTimeout(2000);
  http.setReuse(false);
  http.useHTTP10(true);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  if (!http.begin(client, SAFEWEAR_HTTP_URL)) {
    status_code.store(-1002);
    return Delivery::Retry;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection", "close");
  const char *response_headers[] = {"Transfer-Encoding"};
  http.collectHeaders(response_headers, 1);
  // Credentials stay in memory/header only; never emit them on USB or logs.
  http.addHeader("Authorization", String("Bearer ") + SAFEWEAR_GATEWAY_TOKEN);
  const int status = http.POST(reinterpret_cast<uint8_t *>(request_body), length);
  status_code.store(status);
  bool valid = false;
  if (status == 200 && http.getSize() > 0 &&
      http.getSize() <= static_cast<int>(contract::kResponseBytes) &&
      http.header("Transfer-Encoding").length() == 0) {
    BoundedSink sink(started);
    const int copied = http.writeToStream(&sink);
    CommandBatch batch = {};
    if (copied >= 0 && !sink.failed() && validateResponse(response_body, sink.size(),
        SAFEWEAR_GATEWAY_ID, identity, advertised, batch)) {
      // Transport success cannot touch AlarmBook; only validated operator commands
      // travel to the main loop. A full command queue causes an idempotent retry.
      valid = batch.count == 0 || xQueueSend(command_queue, &batch, 0) == pdTRUE;
    }
  }
  http.end();
  return policy(status, valid);
}

void worker(void *) {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  uint64_t next_wifi_attempt = 0;
  Identity identity = {};
  newBoot(identity.boot);
  uint32_t next_sequence = 1;
  Packet pending = {};
  bool have_pending = false;
  uint8_t attempts = 0;
  uint64_t next_attempt = 0;
  uint64_t auth_not_before = 0;
  for (;;) {
    const uint64_t now = monotonicMs();
    if (have_pending && (expired(pending, now) ||
        normalMayBePreempted(pending, urgentWaiting()))) {
      ++drop_count;
      have_pending = false;
    }
    if (!have_pending && popPacket(pending)) {
      if (expired(pending, now)) { ++drop_count; continue; }
      // One in-flight stream, allocate identity in priority dequeue order.
      if (next_sequence == 0) { newBoot(identity.boot); next_sequence = 1; }
      identity.sequence = next_sequence++;
      have_pending = true;
      attempts = 0;
      next_attempt = now;
    }
    if (WiFi.status() != WL_CONNECTED) {
      if (now >= next_wifi_attempt) {
        WiFi.disconnect(false, false);
        WiFi.begin(SAFEWEAR_WIFI_SSID, SAFEWEAR_WIFI_PASSWORD);
        next_wifi_attempt = now + 10000;
      }
      status_code.store(-1003);
    } else if (have_pending && now >= next_attempt && now >= auth_not_before) {
      const Delivery result = sendPacket(pending, identity);
      if (result == Delivery::Accepted || result == Delivery::PermanentFailure) {
        if (result == Delivery::Accepted) ++accepted_count;
        else ++drop_count;
        have_pending = false;
      } else {
        next_attempt = monotonicMs() + backoffMs(attempts, result);
        if (result == Delivery::AuthBackoff) auth_not_before = next_attempt;
        if (attempts < 255) ++attempts;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
}  // namespace

void begin() {
  // Keep runtime validation (and the full HTTP worker link) in HTTP targets,
  // even when the ignored credential header is absent during a source build.
  volatile bool validated_configuration = configurationValid();
  configured = validated_configuration;
  if (!configured) { status_code.store(-1000); return; }
  command_queue = xQueueCreate(4, sizeof(CommandBatch));
  if (!command_queue) { configured = false; status_code.store(-1004); return; }
  // Worker network/blocking operations never run in the RF/main-loop task.
  if (xTaskCreatePinnedToCore(worker, "safewear-http", 32768, nullptr, 1, nullptr, 0) != pdPASS) {
    vQueueDelete(command_queue);
    command_queue = nullptr;
    configured = false;
    status_code.store(-1004);
  }
}

void capture(const char *line, size_t length, bool urgent) {
  if (!configured) return;
  while (length && (line[length - 1] == '\n' || line[length - 1] == '\r')) --length;
  if (!length || length > contract::kMessageBytes) { ++drop_count; return; }
  Packet packet = {};
  memcpy(packet.message, line, length);
  packet.length = static_cast<uint16_t>(length);
  packet.captured_ms = monotonicMs();
  // EVENTs survive outages; repeated active snapshots remain coalescible telemetry.
  packet.urgent = urgent && strstr(packet.message, "\"kind\":\"rx\"") &&
      strstr(packet.message, "\"payload\":\"0102");
  portENTER_CRITICAL(&state_mux);
  if (packet.urgent) {
    if (!urgent_queue.push(packet)) ++drop_count;
  } else if (!normal_queue.push(packet)) {
    Packet old = {};
    normal_queue.pop(old);
    ++drop_count;
    normal_queue.push(packet);
  }
  portEXIT_CRITICAL(&state_mux);
}

void poll(safewear::AlarmBook &book) {
  if (!configured) return;
  CommandBatch batch = {};
  // One bounded batch per iteration; repeated commands cause no diagnostics/uplink.
  if (xQueueReceive(command_queue, &batch, 0) == pdTRUE) {
    applyCommands(book, batch);
  }
  publishActive(book);
}

void publishActive(const safewear::AlarmBook &book) {
  if (!configured) return;
  safewear::EventId ids[contract::kActiveIds];
  const size_t count = book.activeIds(ids, contract::kActiveIds);
  ActiveIds next = {};
  for (size_t i = 0; i < count; ++i) {
    if (validId(ids[i])) next.ids[next.count++] = ids[i];
  }
  portENTER_CRITICAL(&state_mux);
  active = next;
  portEXIT_CRITICAL(&state_mux);
}

uint32_t dropped() { return drop_count.load(); }
uint32_t accepted() { return accepted_count.load(); }
int lastStatus() { return status_code.load(); }
bool enabled() { return configured; }
}  // namespace safewear_http
#endif
