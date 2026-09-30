#undef NDEBUG
#include "../app/src/main/cpp/motion_filter.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    DSMotionFilter filter;
    ds_motion_filter_init(&filter);

    const int64_t start = INT64_C(1000000000);
    assert(!ds_motion_is_moving(&filter, start));

    /* Stationary test (gravity = 9.8 m/s^2 on Z, 0 rad/s gyro) */
    const float accel_still[3] = {0.0f, 0.0f, 9.80665f};
    const float gyro_still[3] = {0.0f, 0.0f, 0.0f};

    for (int i = 0; i < 20; ++i) {
        ds_motion_filter_update(&filter, accel_still, gyro_still, start + i * INT64_C(50000000));
    }
    assert(!ds_motion_is_moving(&filter, start + INT64_C(1000000000)));
    assert(filter.motion_level < 0.05f);

    /* Rotating test (1.5 rad/s rotation) */
    const float gyro_rot[3] = {1.2f, 0.8f, 0.2f};
    for (int i = 0; i < 10; ++i) {
        ds_motion_filter_update(&filter, accel_still, gyro_rot, start + INT64_C(2000000000) + i * INT64_C(50000000));
    }
    assert(ds_motion_is_moving(&filter, start + INT64_C(2500000000)));
    assert(filter.motion_level > DS_MOTION_THRESHOLD);

    /* Settling test: rotation stops, hold time persists */
    for (int i = 0; i < 10; ++i) {
        ds_motion_filter_update(&filter, accel_still, gyro_still, start + INT64_C(3000000000) + i * INT64_C(50000000));
    }
    /* Still within 1.2s settling window */
    assert(ds_motion_is_moving(&filter, start + INT64_C(3500000000)));

    /* After 2.0s settling window */
    assert(!ds_motion_is_moving(&filter, start + INT64_C(5000000000)));

    puts("PASS: motion filter stationary stability, rotation detection, and settling hold");
    return 0;
}
