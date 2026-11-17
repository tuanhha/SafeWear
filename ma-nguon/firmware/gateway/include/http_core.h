#pragma once
#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>
#include "gateway_core.h"
#include "http_contract.h"

namespace safewear_http {

constexpr size_t kQueuePackets = 8;
constexpr uint64_t kNormalLifetimeMs = 15000;

struct Packet {
  char message[contract::kMessageBytes + 1];
  uint16_t length;
  uint64_t captured_ms;
  bool urgent;
};

template <size_t Capacity> class PacketQueue {
 public:
  bool push(const Packet &packet) {
    if (count_ == Capacity) return false;
    packets_[(head_ + count_) % Capacity] = packet;
    ++count_;
    return true;
  }
  bool pop(Packet &packet) {
    if (!count_) return false;
    packet = packets_[head_];
    head_ = (head_ + 1) % Capacity;
    --count_;
    return true;
  }
  size_t size() const { return count_; }
 private:
  Packet packets_[Capacity] = {};
  size_t head_ = 0;
  size_t count_ = 0;
};

struct ActiveIds {
  safewear::EventId ids[contract::kActiveIds];
  size_t count;
};
struct Command {
  safewear::EventId id;
  bool resolve;
};
struct CommandBatch {
  Command commands[contract::kCommands];
  size_t count;
};
struct Identity {
  char boot[17];
  uint32_t sequence;
};

inline bool validIdentity(const char *gateway, const Identity &identity) {
  if (!gateway || !identity.sequence || !gateway[0] || strlen(gateway) > 48 ||
      identity.boot[16] != '\0') return false;
  for (size_t i = 0; gateway[i]; ++i) {
    const char value = gateway[i];
    if (!((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
          (value >= '0' && value <= '9') || value == '_' || value == '-')) return false;
  }
  for (size_t i = 0; i < 16; ++i) {
    const char value = identity.boot[i];
    if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'))) return false;
  }
  return true;
}

inline bool validId(const safewear::EventId &id) {
  return id.node != 0 && (id.boot != 0 || id.sequence != 0);
}
inline uint32_t ageMs(uint64_t now, uint64_t captured) {
  const uint64_t age = now > captured ? now - captured : 0;
  return age > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(age);
}
inline bool expired(const Packet &packet, uint64_t now) {
  return !packet.urgent && now >= packet.captured_ms &&
         now - packet.captured_ms > kNormalLifetimeMs;
}
inline bool normalMayBePreempted(const Packet &packet, bool urgent_waiting) {
  return !packet.urgent && urgent_waiting;
}
enum class Delivery : uint8_t { Retry, AuthBackoff, PermanentFailure, Accepted };
inline Delivery policy(int status, bool response_valid) {
  if (status == 200 && response_valid) return Delivery::Accepted;
  if (status == 401 || status == 403) return Delivery::AuthBackoff;
  if ((status >= 400 && status < 500 && status != 408 && status != 429) ||
      (status >= 300 && status < 400)) return Delivery::PermanentFailure;
  return Delivery::Retry;
}
inline uint32_t backoffMs(uint8_t attempt, Delivery delivery) {
  if (delivery == Delivery::AuthBackoff) return 60000;
  const uint8_t shift = attempt > 5 ? 5 : attempt;
  const uint32_t delay = 1000U << shift;
  return delay > 30000 ? 30000 : delay;
}

inline bool contains(const ActiveIds &active, const safewear::EventId &id) {
  for (size_t i = 0; i < active.count; ++i) {
    if (safewear::sameId(active.ids[i], id)) return true;
  }
  return false;
}

class BufferWriter {
 public:
  BufferWriter(char *output, size_t capacity) : output_(output), capacity_(capacity) {
    if (capacity_) output_[0] = '\0';
  }
  bool append(const char *text, size_t count) {
    if (!ok_ || size_ + count >= capacity_) { ok_ = false; return false; }
    memcpy(output_ + size_, text, count);
    size_ += count;
    output_[size_] = '\0';
    return true;
  }
  bool append(const char *text) { return append(text, strlen(text)); }
  size_t size() const { return ok_ ? size_ : 0; }
 private:
  char *output_;
  size_t capacity_;
  size_t size_ = 0;
  bool ok_ = true;
};

inline size_t buildEnvelope(char *output, size_t capacity, const char *gateway,
                            const Identity &identity, const Packet &packet,
                            const ActiveIds &active, uint64_t now) {
  // Gateway/boot and IDs are validated ASCII tokens; message is captured JSON.
  if (!validIdentity(gateway, identity) || !packet.length ||
      packet.length > contract::kMessageBytes) return 0;
  BufferWriter writer(output, capacity);
  char prefix[220];
  snprintf(prefix, sizeof(prefix),
           "{\"version\":%u,\"gateway_id\":\"%s\",\"boot_id\":\"%s\","
           "\"sequence\":%lu,\"age_ms\":%lu,\"active_event_ids\":[",
           contract::kVersion, gateway, identity.boot,
           static_cast<unsigned long>(identity.sequence),
           static_cast<unsigned long>(ageMs(now, packet.captured_ms)));
  writer.append(prefix);
  if (active.count > contract::kActiveIds) return 0;
  for (size_t i = 0; i < active.count; ++i) {
    if (!validId(active.ids[i])) return 0;
    for (size_t j = 0; j < i; ++j) {
      if (safewear::sameId(active.ids[j], active.ids[i])) return 0;
    }
    char token[32];
    snprintf(token, sizeof(token), "%s\"%u:%lu:%lu\"", i ? "," : "",
             active.ids[i].node, static_cast<unsigned long>(active.ids[i].boot),
             static_cast<unsigned long>(active.ids[i].sequence));
    writer.append(token);
  }
  writer.append("],\"message\":");
  writer.append(packet.message, packet.length);
  writer.append("}");
  return writer.size();
}

inline int responseKey(const char *start, size_t length) {
  static const char *keys[] = {"version", "accepted", "duplicate", "gateway_id",
      "boot_id", "sequence", "commands", "kind", "event_id"};
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
    if (strlen(keys[i]) == length && memcmp(keys[i], start, length) == 0) return i;
  }
  return -1;
}

inline bool noDuplicateKeys(const char *json, size_t length) {
  // JSON syntax is separately checked by ArduinoJson. Restrict response keys to
  // canonical unescaped names, and track duplicate fields per object scope.
  uint16_t seen[4] = {};
  size_t depth = 0;
  bool started = false, finished = false;
  for (size_t i = 0; i < length; ++i) {
    const bool whitespace = json[i] == ' ' || json[i] == '\r' ||
                            json[i] == '\n' || json[i] == '\t';
    if (finished) { if (!whitespace) return false; else continue; }
    if (!started && !whitespace && json[i] != '{') return false;
    if (json[i] == '{') {
      started = true;
      if (depth == 4) return false;
      seen[depth++] = 0;
    } else if (json[i] == '}') {
      if (!depth) return false;
      --depth;
      if (depth == 0) finished = true;
    } else if (json[i] == '"') {
      const size_t start = ++i;
      bool escaped = false;
      while (i < length && json[i] != '"') {
        if (json[i] == '\\') { escaped = true; ++i; }
        ++i;
      }
      if (i == length) return false;
      const size_t end = i;
      size_t next = i + 1;
      while (next < length && (json[next] == ' ' || json[next] == '\r' ||
             json[next] == '\n' || json[next] == '\t')) ++next;
      if (next < length && json[next] == ':') {
        if (!depth || escaped) return false;
        const int key = responseKey(json + start, end - start);
        if (key < 0 || (seen[depth - 1] & (1U << key))) return false;
        seen[depth - 1] |= 1U << key;
      }
    }
  }
  return finished && depth == 0;
}

inline bool exactString(JsonVariantConst value, const char *expected) {
  if (!value.is<const char *>()) return false;
  const JsonString string = value.as<JsonString>();
  return string.size() == strlen(expected) &&
         memcmp(string.c_str(), expected, string.size()) == 0;
}

inline bool validateResponse(const char *body, size_t length, const char *gateway,
                             const Identity &identity, const ActiveIds &advertised,
                             CommandBatch &batch) {
  batch = {};
  if (!validIdentity(gateway, identity) || !length || length > contract::kResponseBytes ||
      memchr(body, '\0', length) || !noDuplicateKeys(body, length)) return false;
  StaticJsonDocument<6144> document;
  const DeserializationError error = deserializeJson(document, body, length,
      DeserializationOption::NestingLimit(4));
  if (error || !document.is<JsonObject>() || document.size() != 7 ||
      !document["version"].is<unsigned>() || document["version"].as<unsigned>() != contract::kVersion ||
      !document["accepted"].is<bool>() || !document["accepted"].as<bool>() ||
      !document["duplicate"].is<bool>() ||
      !exactString(document["gateway_id"], gateway) ||
      !exactString(document["boot_id"], identity.boot) ||
      !document["sequence"].is<uint32_t>() || identity.sequence == 0 ||
      document["sequence"].as<uint32_t>() != identity.sequence ||
      !document["commands"].is<JsonArray>() ||
      document["commands"].size() > contract::kCommands) return false;
  for (JsonObjectConst command : document["commands"].as<JsonArrayConst>()) {
    if (command.size() != 2 || !command["event_id"].is<const char *>()) return false;
    const bool resolve = exactString(command["kind"], "alarm_resolve");
    if (!resolve && !exactString(command["kind"], "alarm_ack")) return false;
    const JsonString text = command["event_id"].as<JsonString>();
    safewear::EventId id = {};
    if (text.size() != strlen(text.c_str()) || !safewear::parseEventId(text.c_str(), id) ||
        !validId(id) || !contains(advertised, id)) return false;
    batch.commands[batch.count++] = {id, resolve};
  }
  return true;
}

inline void applyCommands(safewear::AlarmBook &book, const CommandBatch &batch) {
  for (size_t i = 0; i < batch.count && i < contract::kCommands; ++i) {
    if (batch.commands[i].resolve) book.resolve(batch.commands[i].id);
    else book.acknowledge(batch.commands[i].id);
  }
}

}  // namespace safewear_http
