#ifndef SAFEWEAR_NODE_CORE_H
#define SAFEWEAR_NODE_CORE_H
#include <stdbool.h>
#include <stdint.h>
#define SW_SNAPSHOT_SIZE 70U
#define SW_EVENT_SIZE 20U
#define SW_UNKNOWN 65535U
#define SW_HR_VALID (1U << 0)
#define SW_SPO2_VALID (1U << 1)
#define SW_SOS_ACTIVE (1U << 2)
#define SW_IMU_VALID (1U << 3)
#define SW_PPG_FAULT (1U << 4)
#define SW_FALL_ACTIVE (1U << 6)
#define SW_SENSOR_GAP (1U << 7)
enum { SW_MANUAL_SOS = 1, SW_FALL_SUSPECT = 2, SW_LOCAL_CLEAR = 3 };
typedef struct {
    uint16_t node_id; uint32_t boot_id, sequence, uptime_ms;
} sw_header;
typedef struct {
    sw_header header;
    uint16_t flags, hr, spo2, battery_mv, hr_age, spo2_age;
    uint16_t imu_count, lost_imu, lost_ppg;
    uint32_t active_boot, active_sequence;
    int16_t features[10];
} sw_snapshot;
typedef struct {
    uint32_t start_ms, last_ms;
    uint16_t count, tail_count, lost;
    bool gap, have_previous;
    float sum_x, sum_y, sum_z, mean_norm, m2_norm, sum_sq_norm;
    float tail_mean, tail_m2, min_norm, max_norm, min_tilt, max_tilt;
    float previous_norm, max_jerk;
} sw_features;
typedef struct {
    uint32_t generation, boot_counter, active_boot, active_sequence;
    uint8_t kind, reason;
} sw_persistent;
typedef struct {
    bool raw, stable, action_fired, clear_armed;
    uint32_t raw_since, pressed_since;
} sw_button;
typedef struct {
    uint8_t phase;
    uint32_t low_since, impact_since, stable_since, last_ms;
    float previous_norm;
    bool have_previous;
} sw_fall;
uint16_t sw_crc16(const uint8_t *data, uint16_t len);
void sw_encode_snapshot(uint8_t out[70], const sw_snapshot *snapshot);
void sw_encode_event(uint8_t out[20], const sw_header *header, uint8_t code, uint8_t reason);
void sw_features_begin(sw_features *features, uint32_t start_ms);
void sw_features_add(sw_features *features, float x, float y, float z, uint32_t now);
void sw_features_lost(sw_features *features, uint32_t count);
bool sw_features_finish(const sw_features *features, int16_t out[10]);
int16_t sw_quantize(float value);
bool sw_journal_decode(const uint8_t record[32], sw_persistent *state);
void sw_journal_encode(uint8_t record[32], const sw_persistent *state);
uint8_t sw_button_update(sw_button *button, bool pressed, uint8_t active_kind, uint32_t now);
bool sw_fall_update(sw_fall *fall, float x, float y, float z, uint32_t now);
#endif
