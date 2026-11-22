#ifndef DEADSPEAK_SENSOR_STATE_H
#define DEADSPEAK_SENSOR_STATE_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

/* Android sensor timestamps and the caller's clock use elapsed boot time.
 * Age checks invalidate readings; time never supplies a reading or a word seed. */
#define DS_SENSOR_FRESH_NS INT64_C(2000000000)
typedef enum {
    DS_SENSOR_ABSENT, DS_SENSOR_BLOCKED, DS_SENSOR_WAITING,
    DS_SENSOR_LIVE, DS_SENSOR_STALE, DS_SENSOR_INVALID, DS_SENSOR_PAUSED
} DSSensorStatus;
typedef enum { DS_SAMPLE_REJECTED, DS_SAMPLE_FIRST, DS_SAMPLE_NEXT } DSSampleResult;
typedef struct {
    DSSensorStatus status;
    bool enabled, valid;
    float xyz[3], magnitude;
    int64_t timestamp_ns, started_ns;
    uint64_t count;
    int error, accuracy;
} DSSensorReading;

static inline void ds_sensor_clear(DSSensorReading *reading, DSSensorStatus status) {
    reading->valid = false;
    reading->status = status;
    reading->xyz[0] = reading->xyz[1] = reading->xyz[2] = reading->magnitude = 0.0f;
    /* Keep the last accepted timestamp/counter to reject old queued events. */
}
static inline void ds_sensor_configure(DSSensorReading *reading, bool present, bool permitted,
                                      int enable_result, int rate_result, int64_t started_ns) {
    reading->enabled = false;
    reading->error = enable_result ? enable_result : rate_result;
    reading->started_ns = started_ns;
    ds_sensor_clear(reading, !present ? DS_SENSOR_ABSENT : DS_SENSOR_BLOCKED);
    if (!present || !permitted || enable_result != 0 || rate_result != 0 || started_ns <= 0) return;
    reading->enabled = true;
    reading->status = DS_SENSOR_WAITING;
}
static inline bool ds_sensor_live(const DSSensorReading *reading, int64_t now_ns) {
    return reading->enabled && reading->valid && reading->status == DS_SENSOR_LIVE &&
           reading->timestamp_ns > 0 && now_ns >= reading->timestamp_ns &&
           now_ns - reading->timestamp_ns <= DS_SENSOR_FRESH_NS;
}
static inline void ds_sensor_expire(DSSensorReading *reading, int64_t now_ns) {
    if (reading->valid && !ds_sensor_live(reading, now_ns)) ds_sensor_clear(reading, DS_SENSOR_STALE);
}
static inline DSSampleResult ds_sensor_accept(DSSensorReading *reading, int64_t timestamp_ns,
                                             int64_t now_ns, float x, float y, float z, int accuracy) {
    if (!reading->enabled) return DS_SAMPLE_REJECTED;
    if (timestamp_ns <= 0 || timestamp_ns < reading->started_ns || timestamp_ns <= reading->timestamp_ns ||
        timestamp_ns > now_ns || !isfinite(x) || !isfinite(y) || !isfinite(z)) {
        ds_sensor_clear(reading, DS_SENSOR_INVALID);
        return DS_SAMPLE_REJECTED;
    }
    if (now_ns - timestamp_ns > DS_SENSOR_FRESH_NS) {
        ds_sensor_clear(reading, DS_SENSOR_STALE);
        return DS_SAMPLE_REJECTED;
    }
    const float magnitude = hypotf(hypotf(x, y), z);
    if (!isfinite(magnitude) || accuracy < 0 || reading->count == UINT64_MAX) {
        ds_sensor_clear(reading, DS_SENSOR_INVALID);
        return DS_SAMPLE_REJECTED;
    }
    const DSSampleResult result = reading->valid ? DS_SAMPLE_NEXT : DS_SAMPLE_FIRST;
    reading->xyz[0] = x; reading->xyz[1] = y; reading->xyz[2] = z;
    reading->magnitude = magnitude;
    reading->timestamp_ns = timestamp_ns;
    reading->accuracy = accuracy;
    ++reading->count;
    reading->valid = true;
    reading->status = DS_SENSOR_LIVE;
    return result;
}
static inline const char *ds_sensor_status_name(DSSensorStatus status) {
    switch (status) {
        case DS_SENSOR_ABSENT: return "NO SENSOR";
        case DS_SENSOR_BLOCKED: return "BLOCKED";
        case DS_SENSOR_WAITING: return "WAITING";
        case DS_SENSOR_LIVE: return "LIVE";
        case DS_SENSOR_STALE: return "STALE";
        case DS_SENSOR_INVALID: return "INVALID";
        case DS_SENSOR_PAUSED: return "PAUSED";
    }
    return "BLOCKED";
}

#define DS_PROXIMITY_HOLD_MS UINT64_C(2500)

typedef struct {
    float baseline_cm;
    float distance_cm;
    bool baseline_ready;
    bool near_active;
    uint32_t break_count;
    uint64_t hold_until_ms;
} DSProximityState;

static inline void ds_proximity_init(DSProximityState *p) {
    if (!p) return;
    *p = (DSProximityState){0};
}

static inline bool ds_proximity_update(DSProximityState *p, float distance_cm, uint64_t now_ms) {
    if (!p || !isfinite(distance_cm) || distance_cm < 0.0f) return false;
    p->distance_cm = distance_cm;
    if (!p->baseline_ready && distance_cm > 0.0f) {
        p->baseline_cm = distance_cm;
        p->baseline_ready = true;
    } else if (p->baseline_ready && distance_cm > p->baseline_cm) {
        p->baseline_cm = distance_cm;
    }
    const bool near_now = (distance_cm < 4.0f) ||
        (p->baseline_ready && p->baseline_cm >= 3.0f && distance_cm < p->baseline_cm * 0.65f);
    const bool was_near = p->near_active;
    p->near_active = near_now;
    if (near_now) {
        p->hold_until_ms = now_ms + DS_PROXIMITY_HOLD_MS;
        if (!was_near) {
            if (p->break_count < UINT32_MAX) ++p->break_count;
            return true;
        }
    } else if (was_near) {
        p->hold_until_ms = now_ms + DS_PROXIMITY_HOLD_MS;
    }
    return false;
}

static inline float ds_proximity_surge(const DSProximityState *p, uint64_t now_ms) {
    if (!p) return 0.0f;
    if (p->near_active) return 1.0f;
    if (p->hold_until_ms > now_ms) {
        const float frac = (float)(p->hold_until_ms - now_ms) / (float)DS_PROXIMITY_HOLD_MS;
        return frac > 1.0f ? 1.0f : (frac < 0.0f ? 0.0f : frac);
    }
    return 0.0f;
}

static inline float ds_proximity_delta(const DSProximityState *p) {
    if (!p || !p->baseline_ready) return 0.0f;
    return p->distance_cm - p->baseline_cm;
}
#endif
