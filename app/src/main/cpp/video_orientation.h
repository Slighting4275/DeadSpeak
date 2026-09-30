#ifndef DEADSPEAK_VIDEO_ORIENTATION_H
#define DEADSPEAK_VIDEO_ORIENTATION_H

/* Rear-camera output: clockwise sensor orientation minus display rotation. */
static inline int ds_video_orientation_hint(int sensor_orientation, int display_rotation) {
    if (sensor_orientation < 0 || sensor_orientation >= 360 || sensor_orientation % 90 != 0 ||
        display_rotation < 0 || display_rotation > 3) return -1;
    return (sensor_orientation - display_rotation * 90 + 360) % 360;
}

#endif
