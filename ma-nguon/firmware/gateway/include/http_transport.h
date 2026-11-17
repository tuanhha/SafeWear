#pragma once
#include "gateway_core.h"
#ifndef SAFEWEAR_HTTP_ENABLED
#define SAFEWEAR_HTTP_ENABLED 0
#endif
namespace safewear_http {
#if SAFEWEAR_HTTP_ENABLED
void begin();
void capture(const char *line, size_t length, bool urgent);
void poll(safewear::AlarmBook &book);
void publishActive(const safewear::AlarmBook &book);
uint32_t dropped();
uint32_t accepted();
int lastStatus();
bool enabled();
#else
inline void begin() {}
inline void capture(const char *, size_t, bool) {}
inline void poll(safewear::AlarmBook &) {}
inline void publishActive(const safewear::AlarmBook &) {}
inline uint32_t dropped() { return 0; }
inline uint32_t accepted() { return 0; }
inline int lastStatus() { return 0; }
inline bool enabled() { return false; }
#endif
}
