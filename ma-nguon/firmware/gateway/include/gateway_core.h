#pragma once

// Original SafeWear code. Wire contract: docs/protocol-v1.md.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace safewear {

constexpr size_t kSnapshotBytes = 70;
constexpr size_t kPpgBytes = 70;
constexpr size_t kEventBytes = 20;
constexpr uint16_t kSosActive = 1U << 2;
constexpr uint16_t kFallActive = 1U << 6;
constexpr size_t kActiveCapacity = 32;
constexpr size_t kResolvedCapacity = 64;
constexpr size_t kMaxJsonBytes = 512;

enum class LineResult : uint8_t { Incomplete, Ready, Rejected };

class LineReader {
 public:
  LineReader() : bytes_{}, length_(0), discarding_(false) {}

  LineResult feed(char value) {
    if (value == '\n') {
      const bool ready = !discarding_ && length_ != 0;
      bytes_[length_] = '\0';
      length_ = 0;
      discarding_ = false;
      return ready ? LineResult::Ready : LineResult::Incomplete;
    }
    if (discarding_) return LineResult::Incomplete;
    if (value == '\0' || length_ == kMaxJsonBytes) {
      length_ = 0;
      discarding_ = true;
      return LineResult::Rejected;
    }
    bytes_[length_++] = value;
    return LineResult::Incomplete;
  }

  const char *line() const { return bytes_; }

 private:
  char bytes_[kMaxJsonBytes + 1];
  size_t length_;
  bool discarding_;
};

struct EventId {
  uint16_t node;
  uint32_t boot;
  uint32_t sequence;
};

inline bool sameId(const EventId &a, const EventId &b) {
  return a.node == b.node && a.boot == b.boot && a.sequence == b.sequence;
}

inline uint16_t read16(const uint8_t *data) {
  return static_cast<uint16_t>(data[0]) |
         (static_cast<uint16_t>(data[1]) << 8);
}

inline uint32_t read32(const uint8_t *data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

inline uint16_t crc16(const uint8_t *data, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < length; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000U) ? static_cast<uint16_t>((crc << 1) ^ 0x1021U)
                            : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

enum class DecodeStatus : uint8_t {
  Ok, BadLength, BadCrc, BadVersion, BadType, BadContent
};

struct FrameMeta {
  uint8_t type;
  EventId frame_id;
  uint32_t source_uptime_ms;
  uint16_t flags;
  uint8_t event_code;
  bool has_alarm;
  EventId alarm_id;
  uint8_t alarm_code;
};

inline DecodeStatus decode(const uint8_t *data, size_t length, FrameMeta &meta) {
  meta = {};
  if (length != kSnapshotBytes && length != kEventBytes) {
    return DecodeStatus::BadLength;
  }
  if (crc16(data, length - 2) != read16(data + length - 2)) {
    return DecodeStatus::BadCrc;
  }
  if (data[0] != 1) return DecodeStatus::BadVersion;
  if (data[1] != 1 && data[1] != 2 && data[1] != 3) return DecodeStatus::BadType;
  if ((data[1] == 1 && length != kSnapshotBytes) ||
      (data[1] == 2 && length != kEventBytes) ||
      (data[1] == 3 && length != kPpgBytes)) {
    return DecodeStatus::BadLength;
  }
  meta.type = data[1];
  meta.frame_id = {read16(data + 2), read32(data + 4), read32(data + 8)};
  meta.source_uptime_ms = read32(data + 12);
  if (meta.frame_id.node == 0) return DecodeStatus::BadContent;
  if (meta.type == 2) {
    meta.event_code = data[16];
    if (meta.event_code < 1 || meta.event_code > 3) {
      return DecodeStatus::BadContent;
    }
    // LOCAL_CLEAR only reports the wearable state. It never clears this alarm.
    if (meta.event_code != 3) {
      meta.has_alarm = true;
      meta.alarm_id = meta.frame_id;
      meta.alarm_code = meta.event_code;
    }
  } else if (meta.type == 3) {
    meta.flags = read16(data + 16);
    const uint16_t samples = read16(data + 20);
    const uint16_t lost = read16(data + 22);
    if ((meta.flags & ~0x0103U) != 0 || read16(data + 18) != 10000 ||
        samples > 250 || data[24] != 1 || data[25] != 12 || read16(data + 50) > 2000) {
      return DecodeStatus::BadContent;
    }
    if ((meta.flags & 1U) && (samples < 225 || lost != 0 || (meta.flags & 2U))) {
      return DecodeStatus::BadContent;
    }
    for (size_t i = 52; i < 68; ++i) {
      if (data[i] != 0) return DecodeStatus::BadContent;
    }
    // PPG quality flags never identify an alarm or change the alarm book.
  } else {
    if (data[44] != 1 || data[45] != 10 || read16(data + 28) != 5000 ||
        read16(data + 30) > 250 || read16(data + 66) != 0) {
      return DecodeStatus::BadContent;
    }
    meta.flags = read16(data + 16);
    if ((meta.flags & ~0x01FFU) != 0) return DecodeStatus::BadContent;
    if ((meta.flags & (kSosActive | kFallActive)) != 0) {
      meta.alarm_id = {meta.frame_id.node, read32(data + 36), read32(data + 40)};
      if (meta.alarm_id.boot == 0 && meta.alarm_id.sequence == 0) {
        return DecodeStatus::BadContent;
      }
      meta.has_alarm = true;
      meta.alarm_code = (meta.flags & kSosActive) ? 1 : 2;
    }
  }
  return DecodeStatus::Ok;
}

inline int8_t hexDigit(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

inline bool decodeHex(const char *text, uint8_t *data, size_t &length) {
  const size_t chars = strlen(text);
  if (chars != kSnapshotBytes * 2 && chars != kEventBytes * 2) return false;
  length = chars / 2;
  for (size_t i = 0; i < length; ++i) {
    const int8_t high = hexDigit(text[i * 2]);
    const int8_t low = hexDigit(text[i * 2 + 1]);
    if (high < 0 || low < 0) return false;
    data[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

inline bool parseNumber(const char *&cursor, uint32_t limit, uint32_t &value) {
  if (*cursor < '0' || *cursor > '9') return false;
  value = 0;
  while (*cursor >= '0' && *cursor <= '9') {
    const uint32_t digit = static_cast<uint32_t>(*cursor - '0');
    if (value > (limit - digit) / 10U) return false;
    value = value * 10U + digit;
    ++cursor;
  }
  return true;
}

inline bool parseEventId(const char *text, EventId &id) {
  if (!text || strlen(text) > 27) return false;
  uint32_t node = 0;
  const char *cursor = text;
  if (!parseNumber(cursor, UINT16_MAX, node) || *cursor++ != ':') return false;
  if (!parseNumber(cursor, UINT32_MAX, id.boot) || *cursor++ != ':') return false;
  if (!parseNumber(cursor, UINT32_MAX, id.sequence) || *cursor != '\0') return false;
  if (node == 0) return false;
  id.node = static_cast<uint16_t>(node);
  return true;
}

struct AlarmSlot {
  EventId id;
  uint8_t code;
  bool used;
  bool acknowledged;
};

struct ResolvedSlot {
  EventId id;
  bool used;
};

enum class ObserveResult : uint8_t {
  NoAlarm, NewAlarm, Duplicate, ResolvedDuplicate, CapacityExceeded
};

class AlarmBook {
 public:
  AlarmBook() : active_{}, resolved_{}, next_resolved_(0), overflow_(false) {}

  ObserveResult observe(const FrameMeta &meta) {
    if (!meta.has_alarm) return ObserveResult::NoAlarm;
    if (wasResolved(meta.alarm_id)) return ObserveResult::ResolvedDuplicate;
    for (size_t i = 0; i < kActiveCapacity; ++i) {
      if (active_[i].used && sameId(active_[i].id, meta.alarm_id)) {
        return ObserveResult::Duplicate;
      }
    }
    for (size_t i = 0; i < kActiveCapacity; ++i) {
      if (!active_[i].used) {
        active_[i] = {meta.alarm_id, meta.alarm_code, true, false};
        return ObserveResult::NewAlarm;
      }
    }
    // Capacity faults stay audible until reboot; never overwrite an open event.
    overflow_ = true;
    return ObserveResult::CapacityExceeded;
  }

  bool acknowledge(const EventId &id) {
    for (size_t i = 0; i < kActiveCapacity; ++i) {
      if (active_[i].used && sameId(active_[i].id, id)) {
        active_[i].acknowledged = true;
        return true;
      }
    }
    return wasResolved(id);  // Repeating a command is idempotent.
  }

  bool resolve(const EventId &id) {
    if (wasResolved(id)) return true;
    for (size_t i = 0; i < kActiveCapacity; ++i) {
      if (active_[i].used && sameId(active_[i].id, id)) {
        active_[i] = {};
        resolved_[next_resolved_] = {id, true};
        next_resolved_ = (next_resolved_ + 1) % kResolvedCapacity;
        return true;
      }
    }
    return false;  // Unknown commands cannot pre-resolve a future event.
  }

  bool wasResolved(const EventId &id) const {
    for (size_t i = 0; i < kResolvedCapacity; ++i) {
      if (resolved_[i].used && sameId(resolved_[i].id, id)) return true;
    }
    return false;
  }

  bool audible() const {
    if (overflow_) return true;
    for (size_t i = 0; i < kActiveCapacity; ++i) {
      if (active_[i].used && !active_[i].acknowledged) return true;
    }
    return false;
  }

  size_t activeCount() const {
    size_t count = 0;
    for (size_t i = 0; i < kActiveCapacity; ++i) count += active_[i].used ? 1 : 0;
    return count;
  }

  bool overflowed() const { return overflow_; }

  size_t activeIds(EventId *output, size_t capacity) const {
    size_t count = 0;
    for (size_t i = 0; i < kActiveCapacity && count < capacity; ++i) {
      if (active_[i].used) output[count++] = active_[i].id;
    }
    return count;
  }

 private:
  AlarmSlot active_[kActiveCapacity];
  ResolvedSlot resolved_[kResolvedCapacity];
  size_t next_resolved_;
  bool overflow_;
};

}  // namespace safewear
