#ifndef DEADSPEAK_FIELD_BASELINE_H
#define DEADSPEAK_FIELD_BASELINE_H

#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

#define DS_BASELINE_SAMPLES 20U

typedef enum {
    DS_SENSITIVITY_HIGH = 0,   /* 4.0 uT - high sensitivity for quiet locations */
    DS_SENSITIVITY_MEDIUM = 1, /* 8.0 uT - balanced default */
    DS_SENSITIVITY_LOW = 2     /* 15.0 uT - high EMI / noisy locations */
} DSSensitivity;

typedef struct {
    /* 3-axis and magnitude baseline */
    double sum_x;
    double sum_y;
    double sum_z;
    double sum_mag;
    unsigned count;
    float base_x;
    float base_y;
    float base_z;
    float field;               /* Baseline magnitude in uT */
    bool ready;

    /* Sensitivity threshold */
    DSSensitivity sensitivity;

    /* Event-driven transient trigger state */
    bool armed;                /* True when field has settled and ready for a new spike */
    bool accumulating;         /* True during an ongoing transient spike above threshold */
    float peak_delta;          /* Highest delta observed during current spike */
    float peak_x;              /* Vector coordinates at the peak moment */
    float peak_y;
    float peak_z;
    unsigned peak_hold_count;  /* Consecutive samples where delta plateaued or started decaying */
    unsigned spike_samples;    /* Number of consecutive samples above threshold */
    int64_t last_word_ns;      /* Timestamp of the last triggered word */

    /* Latched vector values from the triggered peak event */
    float triggered_x;
    float triggered_y;
    float triggered_z;
    float triggered_delta;
} DSFieldBaseline;

static inline void ds_baseline_reset(DSFieldBaseline *baseline) {
    if (!baseline) return;
    const DSSensitivity sens = baseline->sensitivity;
    *baseline = (DSFieldBaseline){0};
    baseline->sensitivity = sens;
}

static inline int64_t ds_baseline_cooldown_ns(DSSensitivity sensitivity) {
    switch (sensitivity) {
        case DS_SENSITIVITY_HIGH:   return INT64_C(1500000000); /* 1.5s - high sensitivity */
        case DS_SENSITIVITY_LOW:    return INT64_C(4000000000); /* 4.0s - noisy locations */
        case DS_SENSITIVITY_MEDIUM:
        default:                    return INT64_C(2500000000); /* 2.5s - balanced default */
    }
}

static inline float ds_baseline_threshold(DSSensitivity sensitivity) {
    switch (sensitivity) {
        case DS_SENSITIVITY_HIGH:   return 4.0f;
        case DS_SENSITIVITY_LOW:    return 15.0f;
        case DS_SENSITIVITY_MEDIUM:
        default:                    return 8.0f;
    }
}

static inline void ds_baseline_sample_3d(DSFieldBaseline *b, float x, float y, float z, bool is_moving, int64_t now_ns) {
    if (!b || !isfinite(x) || !isfinite(y) || !isfinite(z)) return;
    const float mag = sqrtf(x * x + y * y + z * z);
    if (mag < 0.0f || mag > 1000000.0f) return;

    if (!b->ready) {
        b->sum_x += x;
        b->sum_y += y;
        b->sum_z += z;
        b->sum_mag += mag;
        if (++b->count >= DS_BASELINE_SAMPLES) {
            b->base_x = (float)(b->sum_x / DS_BASELINE_SAMPLES);
            b->base_y = (float)(b->sum_y / DS_BASELINE_SAMPLES);
            b->base_z = (float)(b->sum_z / DS_BASELINE_SAMPLES);
            b->field = sqrtf(b->base_x * b->base_x + b->base_y * b->base_y + b->base_z * b->base_z);
            b->ready = true;
            b->armed = true;
        }
        return;
    }

    /* Baseline is ready: calculate delta from current ambient baseline */
    const float dx = x - b->base_x;
    const float dy = y - b->base_y;
    const float dz = z - b->base_z;
    const float delta = sqrtf(dx * dx + dy * dy + dz * dz);
    const float threshold = ds_baseline_threshold(b->sensitivity);
    const float reset_threshold = threshold * 0.5f;

    /* Device in motion suppresses any pending transient anomaly accumulation */
    if (is_moving) {
        b->accumulating = false;
        b->peak_delta = 0.0f;
        b->peak_hold_count = 0;
    }

    /* Adaptive baseline tracking: when device is stationary and environmental field is calm,
     * slowly adapt baseline via EWMA (alpha = 0.015, ~7 sec time constant at 10 Hz) to absorb slow room drift. */
    if (!is_moving && delta < threshold) {
        b->base_x += 0.015f * (x - b->base_x);
        b->base_y += 0.015f * (y - b->base_y);
        b->base_z += 0.015f * (z - b->base_z);
        b->field = sqrtf(b->base_x * b->base_x + b->base_y * b->base_y + b->base_z * b->base_z);
    }

    /* Re-arm condition: field must calm down below reset threshold AND cooldown window must pass */
    if (!b->armed && delta < reset_threshold) {
        const int64_t cooldown = ds_baseline_cooldown_ns(b->sensitivity);
        if (!b->last_word_ns || now_ns <= 0 || (now_ns > b->last_word_ns && now_ns - b->last_word_ns >= cooldown)) {
            b->armed = true;
            b->accumulating = false;
            b->peak_delta = 0.0f;
            b->peak_hold_count = 0;
            b->spike_samples = 0;
        }
    }
}

static inline bool ds_baseline_word_trigger_3d(DSFieldBaseline *b, float x, float y, float z, bool is_moving, int64_t now_ns) {
    if (!b || !b->ready || !isfinite(x) || !isfinite(y) || !isfinite(z) || now_ns <= 0) return false;
    if (is_moving) {
        b->accumulating = false;
        b->peak_delta = 0.0f;
        b->peak_hold_count = 0;
        b->spike_samples = 0;
        return false;
    }
    if (!b->armed) return false;

    const float dx = x - b->base_x;
    const float dy = y - b->base_y;
    const float dz = z - b->base_z;
    const float delta = sqrtf(dx * dx + dy * dy + dz * dz);
    const float threshold = ds_baseline_threshold(b->sensitivity);

    if (delta >= threshold) {
        b->accumulating = true;
        b->spike_samples++;
        if (delta > b->peak_delta) {
            b->peak_delta = delta;
            b->peak_x = x;
            b->peak_y = y;
            b->peak_z = z;
            b->peak_hold_count = 0;
        } else {
            b->peak_hold_count++;
        }

        /* Anomaly event resolved: sustained for at least 2 samples (>= 200 ms) and peaked */
        if (b->spike_samples >= 2U &&
            (b->peak_hold_count >= 1U || delta <= b->peak_delta * 0.85f || b->spike_samples >= 5U)) {
            b->triggered_x = b->peak_x;
            b->triggered_y = b->peak_y;
            b->triggered_z = b->peak_z;
            b->triggered_delta = b->peak_delta;
            b->last_word_ns = now_ns;

            b->armed = false; /* Disarm: prevents continuous word repeats during sustained magnetic interference */
            b->accumulating = false;
            b->peak_delta = 0.0f;
            b->spike_samples = 0;
            b->peak_hold_count = 0;
            return true;
        }
    } else {
        if (b->accumulating) {
            /* Sub-threshold drop before peak resolution */
            b->accumulating = false;
            b->peak_delta = 0.0f;
            b->spike_samples = 0;
            b->peak_hold_count = 0;
        }
    }
    return false;
}

#endif
