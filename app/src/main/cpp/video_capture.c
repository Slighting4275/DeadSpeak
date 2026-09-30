#define _POSIX_C_SOURCE 200809L
#include "video_capture.h"

#include <android/log.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadataTags.h>
#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCaptureRequest.h>
#include <media/NdkImage.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaMuxer.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DS_VIDEO_LOG "DeadSpeakVideo"
#define DS_VIDEO_WIDTH 1920
#define DS_VIDEO_HEIGHT 1080
#define DS_VIDEO_FPS 30
#define DS_VIDEO_BITRATE 8000000
#define DS_COLOR_FORMAT_SURFACE 0x7F000789

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool ready;
    bool closed;
    bool camera_error;
    _Atomic bool *recording;
} DSCameraWait;

static void camera_disconnected(void *context, ACameraDevice *device) {
    (void)device;
    DSCameraWait *wait = (DSCameraWait *)context;
    atomic_store(wait->recording, false);
    pthread_mutex_lock(&wait->mutex);
    wait->camera_error = true;
    pthread_cond_broadcast(&wait->condition);
    pthread_mutex_unlock(&wait->mutex);
}

static void camera_error(void *context, ACameraDevice *device, int error) {
    (void)device;
    __android_log_print(ANDROID_LOG_ERROR, DS_VIDEO_LOG, "camera device error %d", error);
    DSCameraWait *wait = (DSCameraWait *)context;
    atomic_store(wait->recording, false);
    pthread_mutex_lock(&wait->mutex);
    wait->camera_error = true;
    pthread_cond_broadcast(&wait->condition);
    pthread_mutex_unlock(&wait->mutex);
}

static void session_ready(void *context, ACameraCaptureSession *session) {
    (void)session;
    DSCameraWait *wait = (DSCameraWait *)context;
    pthread_mutex_lock(&wait->mutex);
    wait->ready = true;
    pthread_cond_broadcast(&wait->condition);
    pthread_mutex_unlock(&wait->mutex);
}

static void session_closed(void *context, ACameraCaptureSession *session) {
    (void)session;
    DSCameraWait *wait = (DSCameraWait *)context;
    pthread_mutex_lock(&wait->mutex);
    wait->closed = true;
    pthread_cond_broadcast(&wait->condition);
    pthread_mutex_unlock(&wait->mutex);
}

static bool wait_for_camera(DSCameraWait *wait, bool wait_for_close) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 8;
    pthread_mutex_lock(&wait->mutex);
    while (!wait->camera_error &&
           (wait_for_close ? !wait->closed : (atomic_load(wait->recording) && !wait->ready))) {
        const int result = pthread_cond_timedwait(&wait->condition, &wait->mutex, &deadline);
        if (result != 0) break;
    }
    const bool result = wait_for_close ? wait->closed : wait->ready;
    pthread_mutex_unlock(&wait->mutex);
    return result;
}

static bool camera_id_and_size(ACameraManager *manager, char *selected_id, size_t id_capacity,
                               int32_t *selected_width, int32_t *selected_height) {
    ACameraIdList *ids = NULL;
    if (ACameraManager_getCameraIdList(manager, &ids) != ACAMERA_OK || !ids) return false;
    bool selected = false;
    int64_t best_pixels = 0;
    for (int i = 0; i < ids->numCameras; ++i) {
        ACameraMetadata *metadata = NULL;
        if (ACameraManager_getCameraCharacteristics(manager, ids->cameraIds[i], &metadata) != ACAMERA_OK || !metadata) continue;
        ACameraMetadata_const_entry facing = {0};
        const bool is_back = ACameraMetadata_getConstEntry(metadata, ACAMERA_LENS_FACING, &facing) == ACAMERA_OK &&
                             facing.count > 0 && facing.data.u8[0] == ACAMERA_LENS_FACING_BACK;
        if (!is_back) {
            ACameraMetadata_free(metadata);
            continue;
        }
        ACameraMetadata_const_entry streams = {0};
        if (ACameraMetadata_getConstEntry(metadata, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &streams) == ACAMERA_OK) {
            for (uint32_t n = 0; n + 3U < streams.count; n += 4U) {
                const int32_t format = streams.data.i32[n];
                const int32_t width = streams.data.i32[n + 1U];
                const int32_t height = streams.data.i32[n + 2U];
                const int32_t direction = streams.data.i32[n + 3U];
                const int64_t pixels = (int64_t)width * (int64_t)height;
                if (format == AIMAGE_FORMAT_PRIVATE && direction == ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT &&
                    width > 0 && height > 0 && width <= DS_VIDEO_WIDTH && height <= DS_VIDEO_HEIGHT && pixels > best_pixels) {
                    snprintf(selected_id, id_capacity, "%s", ids->cameraIds[i]);
                    *selected_width = width;
                    *selected_height = height;
                    best_pixels = pixels;
                    selected = true;
                }
            }
        }
        ACameraMetadata_free(metadata);
    }
    ACameraManager_deleteCameraIdList(ids);
    return selected;
}

static bool configure_filter(ACameraMetadata *metadata, ACaptureRequest *request, DSVideoFilter filter) {
    if (filter == DS_VIDEO_FILTER_RAW) return true;
    if (filter == DS_VIDEO_FILTER_NIGHT) {
        ACameraMetadata_const_entry range = {0};
        if (ACameraMetadata_getConstEntry(metadata, ACAMERA_CONTROL_AE_COMPENSATION_RANGE, &range) != ACAMERA_OK ||
            range.count < 2 || range.data.i32[1] <= 0) return false;
        const int32_t compensation = range.data.i32[1] < 6 ? range.data.i32[1] : 6;
        return ACaptureRequest_setEntry_i32(request, ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION,
                                            1, &compensation) == ACAMERA_OK;
    }

    ACameraMetadata_const_entry effects = {0};
    if (ACameraMetadata_getConstEntry(metadata, ACAMERA_CONTROL_AVAILABLE_EFFECTS, &effects) != ACAMERA_OK) return false;
    const uint8_t effect = filter == DS_VIDEO_FILTER_MONO ? ACAMERA_CONTROL_EFFECT_MODE_MONO : ACAMERA_CONTROL_EFFECT_MODE_SEPIA;
    bool supported = false;
    for (uint32_t i = 0; i < effects.count; ++i) {
        if (effects.data.u8[i] == effect) supported = true;
    }
    if (!supported) return false;
    return ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_EFFECT_MODE, 1, &effect) == ACAMERA_OK;
}

static bool pump_encoder(AMediaCodec *codec, AMediaMuxer *muxer, ssize_t *video_track,
                         bool *muxer_started, bool *saw_frame, bool *saw_eos, int64_t timeout_us) {
    AMediaCodecBufferInfo info = {0};
    const ssize_t index = AMediaCodec_dequeueOutputBuffer(codec, &info, timeout_us);
    if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        AMediaFormat *format = AMediaCodec_getOutputFormat(codec);
        if (!format) return false;
        *video_track = AMediaMuxer_addTrack(muxer, format);
        AMediaFormat_delete(format);
        if (*video_track < 0 || AMediaMuxer_start(muxer) != AMEDIA_OK) return false;
        *muxer_started = true;
        return true;
    }
    if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER || index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) return true;
    if (index < 0) return false;

    bool okay = true;
    if ((info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0U) *saw_eos = true;
    if (info.size > 0 && *muxer_started && (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) == 0U) {
        size_t capacity = 0;
        uint8_t *buffer = AMediaCodec_getOutputBuffer(codec, (size_t)index, &capacity);
        /* MediaCodec reports unreliable offsets through API 35; buffers begin at offset zero. */
        info.offset = 0;
        if (!buffer || (size_t)info.size > capacity ||
            AMediaMuxer_writeSampleData(muxer, (size_t)*video_track, buffer, &info) != AMEDIA_OK) {
            okay = false;
        } else {
            *saw_frame = true;
        }
    }
    AMediaCodec_releaseOutputBuffer(codec, (size_t)index, false);
    return okay;
}

static void *video_thread(void *user_data) {
    DSVideoCapture *capture = (DSVideoCapture *)user_data;
    ACameraManager *manager = NULL;
    ACameraDevice *device = NULL;
    ACameraCaptureSession *session = NULL;
    ACaptureSessionOutputContainer *outputs = NULL;
    ACaptureSessionOutput *output = NULL;
    ACaptureRequest *request = NULL;
    ACameraOutputTarget *target = NULL;
    AMediaCodec *codec = NULL;
    AMediaFormat *format = NULL;
    AMediaMuxer *muxer = NULL;
    ANativeWindow *input_surface = NULL;
    DSCameraWait wait = {0};
    int output_fd = -1;
    bool codec_started = false;
    bool muxer_started = false;
    bool saw_frame = false;
    bool saw_eos = false;
    bool stream_ended = false;
    bool session_callbacks_ready = false;
    ssize_t video_track = -1;
    char video_path[640] = {0};
    int32_t width = 0, height = 0;
    ACameraMetadata *characteristics = NULL;

    pthread_mutex_init(&wait.mutex, NULL);
    pthread_cond_init(&wait.condition, NULL);
    wait.recording = &capture->active;
    manager = ACameraManager_create();
    char camera_id[64] = {0};
    if (!manager || !camera_id_and_size(manager, camera_id, sizeof(camera_id), &width, &height)) goto cleanup;
    if (ACameraManager_getCameraCharacteristics(manager, camera_id, &characteristics) != ACAMERA_OK || !characteristics) goto cleanup;

    ACameraDevice_StateCallbacks device_callbacks = {
        .context = &wait,
        .onDisconnected = camera_disconnected,
        .onError = camera_error,
        .onClientSharedAccessPriorityChanged = NULL,
    };
    if (ACameraManager_openCamera(manager, camera_id, &device_callbacks, &device) != ACAMERA_OK || !device) goto cleanup;

    codec = AMediaCodec_createEncoderByType("video/avc");
    format = AMediaFormat_new();
    if (!codec || !format) goto cleanup;
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, DS_VIDEO_BITRATE);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_FRAME_RATE, DS_VIDEO_FPS);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, DS_COLOR_FORMAT_SURFACE);
    if (AMediaCodec_configure(codec, format, NULL, NULL, AMEDIACODEC_CONFIGURE_FLAG_ENCODE) != AMEDIA_OK ||
        AMediaCodec_createInputSurface(codec, &input_surface) != AMEDIA_OK || !input_surface ||
        AMediaCodec_start(codec) != AMEDIA_OK) goto cleanup;
    codec_started = true;
    AMediaFormat_delete(format);
    format = NULL;

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    struct tm local_time;
    localtime_r(&now.tv_sec, &local_time);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local_time);
    snprintf(video_path, sizeof(video_path), "%s/VIDEO_%s_%03ld.mp4", capture->evidence_dir, stamp, now.tv_nsec / 1000000L);
    output_fd = open(video_path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (output_fd < 0) goto cleanup;
    muxer = AMediaMuxer_new(output_fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
    if (!muxer) goto cleanup;
    AMediaMuxer_setOrientationHint(muxer, 90);

    if (ACaptureSessionOutputContainer_create(&outputs) != ACAMERA_OK || !outputs ||
        ACaptureSessionOutput_create(input_surface, &output) != ACAMERA_OK || !output ||
        ACaptureSessionOutputContainer_add(outputs, output) != ACAMERA_OK) goto cleanup;
    ACameraCaptureSession_stateCallbacks session_callbacks = {
        .context = &wait,
        .onClosed = session_closed,
        .onReady = session_ready,
        .onActive = session_ready,
    };
    if (ACameraDevice_createCaptureSession(device, outputs, &session_callbacks, &session) != ACAMERA_OK || !session) goto cleanup;
    session_callbacks_ready = true;
    if (!wait_for_camera(&wait, false)) goto cleanup;
    if (ACameraDevice_createCaptureRequest(device, TEMPLATE_RECORD, &request) != ACAMERA_OK || !request ||
        !configure_filter(characteristics, request, capture->filter) ||
        ACameraOutputTarget_create(input_surface, &target) != ACAMERA_OK || !target ||
        ACaptureRequest_addTarget(request, target) != ACAMERA_OK ||
        ACameraCaptureSession_setRepeatingRequest(session, NULL, 1, &request, NULL) != ACAMERA_OK) goto cleanup;

    while (atomic_load(&capture->active)) {
        if (!pump_encoder(codec, muxer, &video_track, &muxer_started, &saw_frame, &saw_eos, 10000)) goto cleanup;
    }
    ACameraCaptureSession_stopRepeating(session);
    ACameraCaptureSession_close(session);
    if (session_callbacks_ready) wait_for_camera(&wait, true);
    session = NULL;
    if (AMediaCodec_signalEndOfInputStream(codec) != AMEDIA_OK) goto cleanup;
    for (unsigned i = 0; i < 500U && !saw_eos; ++i) {
        if (!pump_encoder(codec, muxer, &video_track, &muxer_started, &saw_frame, &saw_eos, 10000)) break;
    }
    stream_ended = saw_eos && saw_frame && muxer_started;

cleanup:
    if (session) {
        ACameraCaptureSession_stopRepeating(session);
        ACameraCaptureSession_close(session);
        if (session_callbacks_ready) wait_for_camera(&wait, true);
    }
    if (request) ACaptureRequest_free(request);
    if (target) ACameraOutputTarget_free(target);
    if (outputs) ACaptureSessionOutputContainer_free(outputs);
    if (output) ACaptureSessionOutput_free(output);
    if (device) ACameraDevice_close(device);
    if (characteristics) ACameraMetadata_free(characteristics);
    if (manager) ACameraManager_delete(manager);
    if (codec_started) {
        AMediaCodec_stop(codec);
        AMediaCodec_delete(codec);
    } else if (codec) {
        AMediaCodec_delete(codec);
    }
    if (format) AMediaFormat_delete(format);
    if (input_surface) ANativeWindow_release(input_surface);
    if (muxer) {
        if (muxer_started && AMediaMuxer_stop(muxer) != AMEDIA_OK) stream_ended = false;
        AMediaMuxer_delete(muxer);
    }
    if (output_fd >= 0) close(output_fd);
    if (!stream_ended) {
        if (video_path[0]) unlink(video_path);
        atomic_store(&capture->failed, true);
    }
    atomic_store(&capture->active, false);
    pthread_cond_destroy(&wait.condition);
    pthread_mutex_destroy(&wait.mutex);
    return NULL;
}

void ds_video_start(DSVideoCapture *capture, const char *evidence_dir, DSVideoFilter filter) {
    if (capture->thread_created || atomic_load(&capture->active)) return;
    snprintf(capture->evidence_dir, sizeof(capture->evidence_dir), "%s", evidence_dir);
    capture->filter = filter;
    atomic_store(&capture->failed, false);
    atomic_store(&capture->active, true);
    if (pthread_create(&capture->thread, NULL, video_thread, capture) != 0) {
        atomic_store(&capture->active, false);
        atomic_store(&capture->failed, true);
        return;
    }
    capture->thread_created = true;
}

void ds_video_stop(DSVideoCapture *capture) {
    if (capture->thread_created) {
        atomic_store(&capture->active, false);
        pthread_join(capture->thread, NULL);
        capture->thread_created = false;
    }
}

bool ds_video_active(const DSVideoCapture *capture) {
    return atomic_load(&capture->active);
}

bool ds_video_failed(const DSVideoCapture *capture) {
    return atomic_load(&capture->failed);
}
