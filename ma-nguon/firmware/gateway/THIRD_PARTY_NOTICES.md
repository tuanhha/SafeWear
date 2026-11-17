# Dependency provenance

Gateway application and `gateway_core.h` were written for SafeWear in this workspace. No source was copied from the surveyed smart-helmet projects (`sentinel`, `helmet`, `SafeMove`, or Guardian). Repository ownership/license decisions remain at the project root.

| Dependency | Pinned version | Upstream / license |
| --- | --- | --- |
| RadioLib | 7.8.1 | https://github.com/jgromes/RadioLib/tree/7.8.1 — MIT, exact upstream text retained in `licenses/RadioLib-MIT.txt` |
| ArduinoJson | 6.21.5 | https://github.com/bblanchon/ArduinoJson/tree/v6.21.5 — MIT, exact upstream text retained in `licenses/ArduinoJson-MIT.txt` |
| PlatformIO Espressif32 platform | 6.10.0 | https://github.com/platformio/platform-espressif32/tree/v6.10.0 — Apache-2.0 platform tooling |
| Arduino-ESP32 / ESP-IDF bundled by platform | Arduino 2.0.17 | https://github.com/espressif/arduino-esp32/tree/2.0.17 — Arduino core LGPL-2.1; bundled ESP-IDF and components have their own notices/licenses |

PlatformIO downloads libraries into `F:/SafeWear/.cache/libdeps/gateway` and packages into `F:/SafeWear/.cache/platformio`; those sources and their original notices are not vendored into the deliverable. A source bundle that redistributes dependencies or firmware binaries should include the relevant upstream licenses and any obligations of linked components. The package source and downloaded license texts, rather than this summary, are authoritative. Host-only test tooling Zig 0.13.0 (MIT, https://github.com/ziglang/zig/tree/0.13.0) is stored under `.cache/tools/zig` and is not linked into ESP32 firmware.

Official RadioLib SX1278 API checked against the pinned tag: `src/modules/SX127x/SX1278.h` and `SX127x.h`. The application uses its receive APIs and explicitly sets header/CRC/profile; it contains no transmit path.

HTTP targets additionally use built-in `WiFi`, `WiFiClient`, `HTTPClient`, ESP-IDF timer/RNG and FreeRTOS APIs from the pinned Arduino-ESP32 2.0.17 package. No additional cloud/network library is vendored. The gateway HTTP worker, finite queues, contract validator and tests are original project code; upstream Arduino/ESP-IDF component licenses above still apply. API behavior (notably WiFiClient timeout seconds versus HTTPClient milliseconds) was checked against this installed source package.
