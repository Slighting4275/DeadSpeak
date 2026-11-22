#ifndef DEADSPEAK_VIDEO_CAPTURE_H
#define DEADSPEAK_VIDEO_CAPTURE_H

#include "camera_ui.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    DS_VIDEO_FILTER_NIGHT = 0,
    DS_VIDEO_FILTER_THERMAL = 1,
    DS_VIDEO_FILTER_EDGE = 2,
    DS_VIDEO_FILTER_MONO = 3,
    DS_VIDEO_FILTER_SEPIA = 4,
    DS_VIDEO_FILTER_RAW = 5,
} DSVideoFilter;

typedef struct {
    bool burn_in;
    float emf_ut;
    float emf_delta;
    float signal;
    bool anomaly;
    char word[48];
    char filter_name[16];
} DSVideoHudState;

typedef struct {
    pthread_t thread;
    _Atomic bool active;
    _Atomic bool failed;
    bool thread_created;
    _Atomic int filter;
    DSVideoAspect aspect;
    int display_rotation;
    char evidence_dir[512];
    /* Private worker state. Zero-initialize the entire capture before first use.
     * Lifecycle/start/stop/copy calls are serialized by the application thread. */
    void *implementation;
    _Atomic bool worker_accepting;
    _Atomic int worker_users;
    _Atomic bool preview_running;
    _Atomic bool preview_ready;
    _Atomic int preview_orientation;
} DSVideoCapture;

/* Start is asynchronous. Unsupported filters/permission/device failures set failed.
 * A changed rotation restarts the preview; filter changes update in real-time. */
void ds_video_preview_start(DSVideoCapture *capture, DSVideoFilter filter, int display_rotation);
void ds_video_set_filter(DSVideoCapture *capture, DSVideoFilter filter);
void ds_video_set_hud(DSVideoCapture *capture, const DSVideoHudState *hud);
bool ds_video_preview_ready(const DSVideoCapture *capture);
/* Copies only an unseen actual frame no older than 500 ms. Unrotated RGBA8888,
 * tightly packed, <=640x480; caller should allocate 640*480*4 bytes.
 * On insufficient capacity reports dimensions but does not consume the frame.
 * Timestamp is the camera's sensor timestamp, not a wall-clock timestamp. */
bool ds_video_copy_frame(DSVideoCapture *capture, uint8_t *rgba, size_t capacity,
                         int *width, int *height, int64_t *timestamp_ns);
int ds_video_preview_orientation(const DSVideoCapture *capture);
/* Stops BOTH recording and preview and joins all worker cleanup. */
void ds_video_preview_stop(DSVideoCapture *capture);

/* Starts filtered + optional HUD + AAC audio muxed MP4 recording using requested aspect.
 * Pending recording startup counts as active. */
void ds_video_start(DSVideoCapture *capture, const char *evidence_dir, DSVideoFilter filter,
                    DSVideoAspect aspect, int display_rotation);
/* Pushes live microphone mono PCM16 frames for AAC encoding & muxing while recording. */
void ds_video_push_audio(DSVideoCapture *capture, const int16_t *pcm, size_t samples, uint32_t sample_rate);
/* Synchronously finalizes recording ONLY; preview worker remains running. */
void ds_video_stop(DSVideoCapture *capture);
bool ds_video_active(const DSVideoCapture *capture);
bool ds_video_failed(const DSVideoCapture *capture);

#endif
