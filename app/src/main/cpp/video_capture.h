#ifndef DEADSPEAK_VIDEO_CAPTURE_H
#define DEADSPEAK_VIDEO_CAPTURE_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

typedef struct {
    pthread_t thread;
    _Atomic bool active;
    _Atomic bool failed;
    bool thread_created;
    char evidence_dir[512];
} DSVideoCapture;

void ds_video_start(DSVideoCapture *capture, const char *evidence_dir);
void ds_video_stop(DSVideoCapture *capture);
bool ds_video_active(const DSVideoCapture *capture);
bool ds_video_failed(const DSVideoCapture *capture);

#endif
