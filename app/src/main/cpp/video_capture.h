#ifndef DEADSPEAK_VIDEO_CAPTURE_H
#define DEADSPEAK_VIDEO_CAPTURE_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

typedef enum {
    DS_VIDEO_FILTER_NIGHT = 0,
    DS_VIDEO_FILTER_MONO = 1,
    DS_VIDEO_FILTER_SEPIA = 2,
    DS_VIDEO_FILTER_RAW = 3,
} DSVideoFilter;

typedef struct {
    pthread_t thread;
    _Atomic bool active;
    _Atomic bool failed;
    bool thread_created;
    DSVideoFilter filter;
    char evidence_dir[512];
} DSVideoCapture;

void ds_video_start(DSVideoCapture *capture, const char *evidence_dir, DSVideoFilter filter);
void ds_video_stop(DSVideoCapture *capture);
bool ds_video_active(const DSVideoCapture *capture);
bool ds_video_failed(const DSVideoCapture *capture);

#endif
