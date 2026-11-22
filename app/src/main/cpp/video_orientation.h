#ifndef DEADSPEAK_VIDEO_ORIENTATION_H
#define DEADSPEAK_VIDEO_ORIENTATION_H

#include <stdbool.h>

/* Output clockwise rotation: (sensor - display*90) for rear, (sensor + display*90) for front. */
static inline int ds_video_orientation_facing(int sensor_orientation, int display_rotation, bool front) {
    if (sensor_orientation < 0 || sensor_orientation >= 360 || sensor_orientation % 90 != 0 ||
        display_rotation < 0 || display_rotation > 3) return -1;
    if (front) return (sensor_orientation + display_rotation * 90) % 360;
    return (sensor_orientation - display_rotation * 90 + 360) % 360;
}

static inline int ds_video_orientation_hint(int sensor_orientation, int display_rotation) {
    return ds_video_orientation_facing(sensor_orientation, display_rotation, false);
}

#endif
