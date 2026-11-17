#pragma once
#include <ArduinoJson.h>
#include <stdint.h>
#include <string.h>

namespace safewear {
// Strict capture-only boundary; never forward arbitrary node JSON to the host.
inline bool validRawCapture(const JsonDocument &document, size_t bytes) {
  if (bytes > 128 || !document.is<JsonObjectConst>() || document.size() != 5 ||
      !document["kind"].is<const char *>() ||
      document["kind"].as<JsonString>().size() != 7 ||
      strcmp(document["kind"], "ppg_raw") != 0) return false;
  return document["motion_ok"].is<bool>() && document["sample_ms"].is<uint32_t>() &&
      document["red"].is<uint32_t>() && document["ir"].is<uint32_t>() &&
      document["red"].as<uint32_t>() <= 262143U &&
      document["ir"].as<uint32_t>() <= 262143U;
}
}
