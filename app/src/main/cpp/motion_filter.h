#ifndef DEADSPEAK_MOTION_FILTER_H
#define DEADSPEAK_MOTION_FILTER_H

#include <stdbool.h>
#include <stdint.h>
#include <math.h>

typedef struct {
    float accel_smoothed[3];
    float gyro_smoothed[3];
    float motion_level;      /* 0.0 (still) to 1.0 (rapid motion) */
    bool initialized;
    int64_t last_motion_ns;
} DSMotionFilter;

#define DS_MOTION_THRESHOLD 0.28f
#define DS_GRAVITY_EARTH 9.80665f

static inline void ds_motion_filter_init(DSMotionFilter *f) {
    if (f) *f = (DSMotionFilter){0};
}

static inline void ds_motion_filter_update(DSMotionFilter *f,
                                          const float accel[3],
                                          const float gyro[3],
                                          int64_t timestamp_ns) {
    if (!f || !accel || !gyro || timestamp_ns <= 0) return;
    if (!isfinite(accel[0]) || !isfinite(accel[1]) || !isfinite(accel[2]) ||
        !isfinite(gyro[0]) || !isfinite(gyro[1]) || !isfinite(gyro[2])) return;

    if (!f->initialized) {
        for (int i = 0; i < 3; ++i) {
            f->accel_smoothed[i] = accel[i];
            f->gyro_smoothed[i] = gyro[i];
        }
        f->initialized = true;
        f->motion_level = 0.0f;
        return;
    }

    const float alpha = 0.25f;
    for (int i = 0; i < 3; ++i) {
        f->accel_smoothed[i] += alpha * (accel[i] - f->accel_smoothed[i]);
        f->gyro_smoothed[i] += alpha * (gyro[i] - f->gyro_smoothed[i]);
    }

    /* Angular velocity magnitude in rad/s */
    const float gyro_mag = sqrtf(gyro[0] * gyro[0] + gyro[1] * gyro[1] + gyro[2] * gyro[2]);

    /* Total acceleration magnitude relative to 1g */
    const float accel_mag = sqrtf(accel[0] * accel[0] + accel[1] * accel[1] + accel[2] * accel[2]);
    const float accel_dev = fabsf(accel_mag - DS_GRAVITY_EARTH);

    /* Instantaneous motion metric */
    const float instant_motion = (gyro_mag * 0.4f) + (accel_dev * 0.15f);
    f->motion_level += 0.35f * (instant_motion - f->motion_level);
    if (f->motion_level > 1.0f) f->motion_level = 1.0f;
    if (f->motion_level < 0.0f) f->motion_level = 0.0f;

    if (f->motion_level > DS_MOTION_THRESHOLD) {
        f->last_motion_ns = timestamp_ns;
    }
}

static inline bool ds_motion_is_moving(const DSMotionFilter *f, int64_t now_ns) {
    if (!f || !f->initialized) return false;
    if (f->motion_level > DS_MOTION_THRESHOLD) return true;
    /* Hold motion flag for 1.2 seconds after movement stops to let magnetic field settle */
    if (f->last_motion_ns > 0 && now_ns >= f->last_motion_ns &&
        (now_ns - f->last_motion_ns) < INT64_C(1200000000)) {
        return true;
    }
    return false;
}

#endif /* DEADSPEAK_MOTION_FILTER_H */
