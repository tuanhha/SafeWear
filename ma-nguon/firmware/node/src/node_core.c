#include "node_core.h"
#include <math.h>
#include <string.h>
static void put16(uint8_t *p, uint16_t x) { p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8); }
static void put32(uint8_t *p, uint32_t x) { put16(p, (uint16_t)x); put16(p + 2, (uint16_t)(x >> 16)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | ((uint32_t)get16(p + 2) << 16); }
uint16_t sw_crc16(const uint8_t *data, uint16_t len) {
    uint16_t crc = 0xFFFFU;
    for (uint16_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
    }
    return crc;
}
static void encode_header(uint8_t *out, const sw_header *h, uint8_t type) {
    out[0] = 1; out[1] = type; put16(out + 2, h->node_id);
    put32(out + 4, h->boot_id); put32(out + 8, h->sequence); put32(out + 12, h->uptime_ms);
}
void sw_encode_event(uint8_t out[20], const sw_header *h, uint8_t code, uint8_t reason) {
    encode_header(out, h, 2); out[16] = code; out[17] = reason;
    put16(out + 18, sw_crc16(out, 18));
}
void sw_encode_snapshot(uint8_t out[70], const sw_snapshot *s) {
    memset(out, 0, 70); encode_header(out, &s->header, 1);
    put16(out + 16, s->flags); put16(out + 18, s->hr); put16(out + 20, s->spo2);
    put16(out + 22, s->battery_mv); put16(out + 24, s->hr_age); put16(out + 26, s->spo2_age);
    put16(out + 28, 5000); put16(out + 30, s->imu_count); put16(out + 32, s->lost_imu);
    put16(out + 34, s->lost_ppg); put32(out + 36, s->active_boot); put32(out + 40, s->active_sequence);
    out[44] = 1; out[45] = 10;
    for (uint8_t i = 0; i < 10; ++i) put16(out + 46 + 2 * i, (uint16_t)s->features[i]);
    put16(out + 68, sw_crc16(out, 68));
}
int16_t sw_quantize(float v) {
    if (!isfinite(v)) return 0;
    if (v >= 32767.0f) return 32767;
    if (v <= -32768.0f) return -32768;
    return (int16_t)lroundf(v);
}
void sw_features_begin(sw_features *f, uint32_t start_ms) {
    memset(f, 0, sizeof(*f)); f->start_ms = start_ms;
    f->min_norm = 1e9f; f->min_tilt = 1e9f;
}
void sw_features_lost(sw_features *f, uint32_t count) {
    uint32_t total = (uint32_t)f->lost + count;
    f->lost = total > 65535U ? 65535U : (uint16_t)total;
    f->gap = true;
}
void sw_features_add(sw_features *f, float x, float y, float z, uint32_t now) {
    if (!isfinite(x) || !isfinite(y) || !isfinite(z) || now - f->start_ms >= 5000U) {
        sw_features_lost(f, 1); return;
    }
    float norm = sqrtf(x*x + y*y + z*z), tilt = 0;
    /* Algebraically acos(z/norm), but stable in float32 near vertical. */
    if (norm > 0) tilt = atan2f(sqrtf(x*x + y*y), z) * 57.2957795131f;
    if (f->have_previous) {
        uint32_t dt = now - f->last_ms;
        if (dt == 0 || dt > 60U) f->gap = true;
        if (dt) f->max_jerk = fmaxf(f->max_jerk, fabsf(norm - f->previous_norm) * 1000.0f / dt);
    } else if (now - f->start_ms > 60U) f->gap = true;
    f->have_previous = true; f->previous_norm = norm; f->last_ms = now;
    ++f->count; f->sum_x += x; f->sum_y += y; f->sum_z += z;
    float delta = norm - f->mean_norm; f->mean_norm += delta / f->count;
    f->m2_norm += delta * (norm - f->mean_norm); f->sum_sq_norm += norm * norm;
    f->min_norm = fminf(f->min_norm, norm); f->max_norm = fmaxf(f->max_norm, norm);
    f->min_tilt = fminf(f->min_tilt, tilt); f->max_tilt = fmaxf(f->max_tilt, tilt);
    if (now - f->start_ms >= 4000U) {
        ++f->tail_count; delta = norm - f->tail_mean; f->tail_mean += delta / f->tail_count;
        f->tail_m2 += delta * (norm - f->tail_mean);
    }
}
bool sw_features_finish(const sw_features *f, int16_t out[10]) {
    memset(out, 0, 20);
    if (!f->count) return false;
    out[0] = sw_quantize(1000 * f->sum_x / f->count);
    out[1] = sw_quantize(1000 * f->sum_y / f->count);
    out[2] = sw_quantize(1000 * f->sum_z / f->count);
    out[3] = sw_quantize(1000 * sqrtf(fmaxf(0, f->m2_norm / f->count)));
    out[4] = sw_quantize(1000 * f->min_norm); out[5] = sw_quantize(1000 * f->max_norm);
    out[6] = sw_quantize(1000 * sqrtf(fmaxf(0, f->sum_sq_norm / f->count)));
    out[7] = sw_quantize(10 * f->max_jerk); out[8] = sw_quantize(100 * (f->max_tilt - f->min_tilt));
    if (f->tail_count) out[9] = sw_quantize(1000 * sqrtf(fmaxf(0, f->tail_m2 / f->tail_count)));
    return f->count >= 225U && f->count <= 275U && f->tail_count > 0U && !f->gap &&
           f->last_ms - f->start_ms >= 4940U;
}
void sw_journal_encode(uint8_t out[32], const sw_persistent *s) {
    memset(out, 0, 32); put16(out, 0x534AU); put16(out + 2, 1);
    put32(out + 4, s->generation); put32(out + 8, s->boot_counter);
    put32(out + 12, s->active_boot); put32(out + 16, s->active_sequence);
    out[20] = s->kind; out[21] = s->reason;
    put16(out + 28, sw_crc16(out, 28)); put16(out + 30, 0xA55AU);
}
bool sw_journal_decode(const uint8_t r[32], sw_persistent *s) {
    if (get16(r) != 0x534AU || get16(r + 2) != 1 || get16(r + 30) != 0xA55AU ||
        get16(r + 28) != sw_crc16(r, 28) || r[20] > SW_FALL_SUSPECT) return false;
    s->generation = get32(r + 4); s->boot_counter = get32(r + 8);
    s->active_boot = get32(r + 12); s->active_sequence = get32(r + 16);
    s->kind = r[20]; s->reason = r[21];
    if (s->kind && (!s->active_boot || !s->active_sequence)) return false;
    return true;
}
uint8_t sw_button_update(sw_button *b, bool pressed, uint8_t active_kind, uint32_t now) {
    if (pressed != b->raw) { b->raw = pressed; b->raw_since = now; }
    if (now - b->raw_since < 30U) return 0;
    if (b->stable != b->raw) {
        b->stable = b->raw; b->action_fired = false;
        if (b->stable) b->pressed_since = now;
    }
    if (!b->stable) { if (active_kind) b->clear_armed = true; return 0; }
    if (b->action_fired) return 0;
    uint32_t held = now - b->pressed_since;
    if ((active_kind == 0 || active_kind == SW_FALL_SUSPECT) && held >= 1000U) {
        b->action_fired = true; b->clear_armed = false; return SW_MANUAL_SOS;
    }
    if (active_kind && b->clear_armed && held >= 5000U) {
        b->action_fired = true; b->clear_armed = false; return SW_LOCAL_CLEAR;
    }
    return 0;
}
bool sw_fall_update(sw_fall *f, float x, float y, float z, uint32_t now) {
    float n = sqrtf(x*x + y*y + z*z);
    if (!isfinite(n)) { f->phase = 0; f->have_previous = false; return false; }
    uint32_t dt = now - f->last_ms;
    if (f->have_previous && (dt == 0 || dt > 60U)) f->phase = 0;
    float change = f->have_previous ? fabsf(n - f->previous_norm) : 0;
    f->previous_norm = n; f->last_ms = now; f->have_previous = true;
    if (f->phase == 0) {
        if (n < 0.55f) { f->phase = 1; f->low_since = now; }
    } else if (f->phase == 1) {
        if (now - f->low_since > 800U) f->phase = 0;
        else if (now - f->low_since >= 60U && n > 2.2f) {
            f->phase = 2; f->impact_since = now; f->stable_since = 0;
        }
    } else {
        if (now - f->impact_since > 3500U) { f->phase = 0; return false; }
        float tilt = n > 0 ? atan2f(sqrtf(x*x + y*y), z) * 57.2957795131f : 0;
        bool stable = n >= 0.7f && n <= 1.3f && tilt >= 60.0f && change < 0.15f;
        if (!stable) f->stable_since = 0;
        else if (!f->stable_since) f->stable_since = now;
        else if (now - f->stable_since >= 1500U) { f->phase = 0; return true; }
    }
    return false;
}
