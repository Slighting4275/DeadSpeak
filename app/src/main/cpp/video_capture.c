#define _POSIX_C_SOURCE 200809L
#include "video_capture.h"
#include "video_orientation.h"
#include "preview_frame.h"
#include <android/log.h>
#include <android/native_window.h>
#include <android/data_space.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadataTags.h>
#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCaptureRequest.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaMuxer.h>
#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DS_VIDEO_LOG "DeadSpeakVideo"
#define DS_VIDEO_WIDTH 1920
#define DS_VIDEO_HEIGHT 1080
#define DS_VIDEO_FPS 30
#define DS_VIDEO_BITRATE 8000000
#define DS_COLOR_FORMAT_SURFACE 0x7F000789

/* The worker alone owns Android resources. Callback context remains alive until
 * synchronous device close stops its looper. The application serializes public
 * lifecycle/copy calls; callbacks only access locked flags and atomic status. */
typedef struct {
    DSVideoCapture *capture;
    pthread_mutex_t mutex;
    pthread_cond_t condition; /* CLOCK_MONOTONIC */
    bool camera_error, session_closed, record_requested, worker_finished;
    uint8_t *frame, *scratch;
    int frame_width, frame_height;
    int64_t frame_timestamp, frame_received, last_camera_timestamp, preview_deadline;
    uint64_t frame_number, copied_number;
    int preview_width, preview_height;
    bool full_hd, fixed_30fps, realtime_timestamps;
    media_status_t (*get_dataspace)(const AImage *, int32_t *);
    bool color_logged;
    ACameraManager *manager;
    ACameraMetadata *metadata;
    ACameraDevice *device;
    AImageReader *reader;
    ANativeWindow *preview_window; /* Reader-owned; do not release separately. */
    ACameraCaptureSession *session;
    ACaptureSessionOutputContainer *outputs;
    ACaptureSessionOutput *preview_output, *encoder_output;
    ACameraOutputTarget *preview_target, *encoder_target;
    ACaptureRequest *request;
    AMediaCodec *codec;
    ANativeWindow *encoder_window;
    AMediaMuxer *muxer;
    bool codec_started, muxer_started, saw_frame, saw_eos;
    ssize_t video_track;
    int fd;
    int64_t first_pts, last_pts;
    char video_path[640];
} DSVideoWorker;

static int64_t clock_ns(clockid_t clock_id) {
    struct timespec t;
    if (clock_gettime(clock_id, &t) != 0) return 0;
    return (int64_t)t.tv_sec * INT64_C(1000000000) + t.tv_nsec;
}

static void fail(DSVideoWorker *w, const char *reason) {
    __android_log_print(ANDROID_LOG_ERROR, DS_VIDEO_LOG, "%s", reason);
    atomic_store(&w->capture->failed, true);
}

static bool camera_ok(DSVideoWorker *w, camera_status_t status, const char *operation) {
    if (status == ACAMERA_OK) return true;
    __android_log_print(ANDROID_LOG_ERROR, DS_VIDEO_LOG, "%s failed: camera status %d", operation, status);
    atomic_store(&w->capture->failed, true);
    return false;
}

static bool media_ok(DSVideoWorker *w, media_status_t status, const char *operation) {
    if (status == AMEDIA_OK) return true;
    __android_log_print(ANDROID_LOG_ERROR, DS_VIDEO_LOG, "%s failed: media status %d", operation, status);
    atomic_store(&w->capture->failed, true);
    return false;
}

static void camera_error(void *context, ACameraDevice *device, int error) {
    (void)device;
    DSVideoWorker *w = context;
    __android_log_print(ANDROID_LOG_ERROR, DS_VIDEO_LOG, "camera disconnected/error %d", error);
    atomic_store(&w->capture->failed, true);
    atomic_store(&w->capture->preview_ready, false);
    pthread_mutex_lock(&w->mutex);
    w->camera_error = true;
    pthread_cond_broadcast(&w->condition);
    pthread_mutex_unlock(&w->mutex);
}

static void camera_disconnected(void *context, ACameraDevice *device) {
    camera_error(context, device, -1);
}

static void session_state(void *context, ACameraCaptureSession *session) {
    (void)context; (void)session;
}

static void session_closed(void *context, ACameraCaptureSession *session) {
    (void)session;
    DSVideoWorker *w = context;
    pthread_mutex_lock(&w->mutex);
    w->session_closed = true;
    pthread_cond_broadcast(&w->condition);
    pthread_mutex_unlock(&w->mutex);
}

/* Choose a genuinely advertised bounded YUV stream. Prefer 640x360 to match the
 * recording aspect, then the closest advertised size. FullHD is independent:
 * preview may work on a device that cannot record the requested resolution. */
static bool select_camera(DSVideoWorker *w, char id[128]) {
    ACameraIdList *ids = NULL;
    if (!camera_ok(w, ACameraManager_getCameraIdList(w->manager, &ids), "list cameras") || !ids) return false;
    bool selected = false;
    int64_t best_score = INT64_MAX;
    for (int i = 0; i < ids->numCameras; ++i) {
        ACameraMetadata *metadata = NULL;
        if (ACameraManager_getCameraCharacteristics(w->manager, ids->cameraIds[i], &metadata) != ACAMERA_OK || !metadata) continue;
        ACameraMetadata_const_entry facing = {0}, streams = {0};
        const bool rear = ACameraMetadata_getConstEntry(metadata, ACAMERA_LENS_FACING, &facing) == ACAMERA_OK &&
            facing.type == ACAMERA_TYPE_BYTE && facing.count == 1 && facing.data.u8[0] == ACAMERA_LENS_FACING_BACK;
        if (rear && ACameraMetadata_getConstEntry(metadata, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &streams) == ACAMERA_OK &&
            streams.type == ACAMERA_TYPE_INT32 && streams.count % 4U == 0U) {
            bool full_hd = false;
            int pw = 0, ph = 0;
            int64_t score = INT64_MAX;
            for (uint32_t n = 0; n < streams.count; n += 4U) {
                const int32_t format = streams.data.i32[n], width = streams.data.i32[n + 1U];
                const int32_t height = streams.data.i32[n + 2U], direction = streams.data.i32[n + 3U];
                if (direction != ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT) continue;
                if (format == AIMAGE_FORMAT_PRIVATE && width == DS_VIDEO_WIDTH && height == DS_VIDEO_HEIGHT) full_hd = true;
                if (format != AIMAGE_FORMAT_YUV_420_888 || width < 1 || height < 1 || width > DS_VIDEO_WIDTH || height > DS_VIDEO_HEIGHT) continue;
                int64_t candidate = llabs((int64_t)width * height - INT64_C(640) * 360);
                candidate += llabs((int64_t)width * 9 - (int64_t)height * 16) * 100;
                if (candidate < score) { score = candidate; pw = width; ph = height; }
            }
            if (!full_hd && score != INT64_MAX) score += INT64_C(1000000000);
            if (pw && score < best_score && strlen(ids->cameraIds[i]) < 128U) {
                if (w->metadata) ACameraMetadata_free(w->metadata);
                w->metadata = metadata; metadata = NULL;
                strcpy(id, ids->cameraIds[i]);
                w->preview_width = pw; w->preview_height = ph; w->full_hd = full_hd;
                best_score = score; selected = true;
            }
        }
        if (metadata) ACameraMetadata_free(metadata);
    }
    ACameraManager_deleteCameraIdList(ids);
    if (!selected) { fail(w, "no rear camera with a supported bounded YUV preview stream"); return false; }
    ACameraMetadata_const_entry orientation = {0}, fps = {0}, source = {0};
    if (ACameraMetadata_getConstEntry(w->metadata, ACAMERA_SENSOR_ORIENTATION, &orientation) != ACAMERA_OK ||
        orientation.type != ACAMERA_TYPE_INT32 || orientation.count != 1) {
        fail(w, "missing camera orientation metadata"); return false;
    }
    const int rotation = ds_video_orientation_hint(orientation.data.i32[0], w->capture->display_rotation);
    if (rotation < 0) { fail(w, "invalid display/camera orientation"); return false; }
    atomic_store(&w->capture->preview_orientation, rotation);
    if (ACameraMetadata_getConstEntry(w->metadata, ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, &fps) == ACAMERA_OK &&
        fps.type == ACAMERA_TYPE_INT32 && fps.count % 2U == 0U) {
        for (uint32_t n = 0; n < fps.count; n += 2U)
            if (fps.data.i32[n] == DS_VIDEO_FPS && fps.data.i32[n + 1U] == DS_VIDEO_FPS) w->fixed_30fps = true;
    }
    w->realtime_timestamps = ACameraMetadata_getConstEntry(w->metadata, ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE, &source) == ACAMERA_OK &&
        source.type == ACAMERA_TYPE_BYTE && source.count == 1 && source.data.u8[0] == ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME;
    __android_log_print(ANDROID_LOG_INFO, DS_VIDEO_LOG, "rear camera %s YUV %dx%d orientation %d FullHD %d fixed30 %d",
                        id, w->preview_width, w->preview_height, rotation, w->full_hd, w->fixed_30fps);
    return true;
}

static bool configure_filter(DSVideoWorker *w) {
    const DSVideoFilter filter = w->capture->filter;
    if (filter < DS_VIDEO_FILTER_NIGHT || filter > DS_VIDEO_FILTER_RAW) return false;
    uint8_t effect = ACAMERA_CONTROL_EFFECT_MODE_OFF;
    int32_t compensation = 0;
    if (filter == DS_VIDEO_FILTER_NIGHT) {
        ACameraMetadata_const_entry range = {0};
        if (ACameraMetadata_getConstEntry(w->metadata, ACAMERA_CONTROL_AE_COMPENSATION_RANGE, &range) != ACAMERA_OK ||
            range.type != ACAMERA_TYPE_INT32 || range.count != 2 || range.data.i32[1] <= 0) return false;
        compensation = range.data.i32[1] < 6 ? range.data.i32[1] : 6;
    } else if (filter == DS_VIDEO_FILTER_MONO || filter == DS_VIDEO_FILTER_SEPIA) {
        ACameraMetadata_const_entry effects = {0};
        if (ACameraMetadata_getConstEntry(w->metadata, ACAMERA_CONTROL_AVAILABLE_EFFECTS, &effects) != ACAMERA_OK || effects.type != ACAMERA_TYPE_BYTE) return false;
        effect = filter == DS_VIDEO_FILTER_MONO ? ACAMERA_CONTROL_EFFECT_MODE_MONO : ACAMERA_CONTROL_EFFECT_MODE_SEPIA;
        bool found = false;
        for (uint32_t i = 0; i < effects.count; ++i) if (effects.data.u8[i] == effect) found = true;
        if (!found) return false;
    }
    return camera_ok(w, ACaptureRequest_setEntry_u8(w->request, ACAMERA_CONTROL_EFFECT_MODE, 1, &effect), "set camera effect") &&
        camera_ok(w, ACaptureRequest_setEntry_i32(w->request, ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION, 1, &compensation), "set exposure compensation");
}

static void free_session_objects(DSVideoWorker *w) {
    if (w->request) ACaptureRequest_free(w->request);
    if (w->preview_target) ACameraOutputTarget_free(w->preview_target);
    if (w->encoder_target) ACameraOutputTarget_free(w->encoder_target);
    if (w->outputs) ACaptureSessionOutputContainer_free(w->outputs);
    if (w->preview_output) ACaptureSessionOutput_free(w->preview_output);
    if (w->encoder_output) ACaptureSessionOutput_free(w->encoder_output);
    w->request = NULL; w->preview_target = w->encoder_target = NULL;
    w->outputs = NULL; w->preview_output = w->encoder_output = NULL;
}

/* Timeout never releases callback context early. Synchronous device close stops
 * its callback looper before freeing the worker. A close failure is fatal: do
 * not create another camera/session with an outstanding callback context. */
static bool close_session(DSVideoWorker *w) {
    atomic_store(&w->capture->preview_ready, false);
    bool okay = true;
    if (w->session) {
        pthread_mutex_lock(&w->mutex);
        bool closed = w->session_closed;
        pthread_mutex_unlock(&w->mutex);
        if (!closed) {
            camera_status_t status = ACameraCaptureSession_stopRepeating(w->session);
            if (status != ACAMERA_OK && status != ACAMERA_ERROR_SESSION_CLOSED) camera_ok(w, status, "stop camera repeating");
            ACameraCaptureSession_close(w->session);
            const int64_t deadline = clock_ns(CLOCK_MONOTONIC) + INT64_C(5000000000);
            const struct timespec until = { .tv_sec = (time_t)(deadline / INT64_C(1000000000)), .tv_nsec = (long)(deadline % INT64_C(1000000000)) };
            pthread_mutex_lock(&w->mutex);
            while (!w->session_closed) if (pthread_cond_timedwait(&w->condition, &w->mutex, &until) != 0) break;
            closed = w->session_closed;
            pthread_mutex_unlock(&w->mutex);
            if (!closed) {
                fail(w, "camera session close timed out; shutting down device");
                if (w->device) { ACameraDevice_close(w->device); w->device = NULL; }
                okay = false;
            }
        }
        w->session = NULL;
    }
    free_session_objects(w);
    return okay;
}

static bool create_session(DSVideoWorker *w, bool record) {
    pthread_mutex_lock(&w->mutex);
    w->session_closed = false;
    pthread_mutex_unlock(&w->mutex);
    if (!camera_ok(w, ACaptureSessionOutputContainer_create(&w->outputs), "create output container") ||
        !camera_ok(w, ACaptureSessionOutput_create(w->preview_window, &w->preview_output), "create YUV output") ||
        !camera_ok(w, ACaptureSessionOutputContainer_add(w->outputs, w->preview_output), "add YUV output")) return false;
    if (record && (!camera_ok(w, ACaptureSessionOutput_create(w->encoder_window, &w->encoder_output), "create encoder output") ||
                   !camera_ok(w, ACaptureSessionOutputContainer_add(w->outputs, w->encoder_output), "add encoder output"))) return false;
    ACameraCaptureSession_stateCallbacks callbacks = { .context = w, .onClosed = session_closed, .onReady = session_state, .onActive = session_state };
    if (!camera_ok(w, ACameraDevice_createCaptureSession(w->device, w->outputs, &callbacks, &w->session),
                   record ? "create FullHD+YUV camera session" : "create YUV camera session") || !w->session ||
        !camera_ok(w, ACameraDevice_createCaptureRequest(w->device, record ? TEMPLATE_RECORD : TEMPLATE_PREVIEW, &w->request), "create request")) return false;
    if (!configure_filter(w)) { fail(w, "requested camera filter is unsupported; no RAW fallback"); return false; }
    if (record) {
        const int32_t range[2] = {DS_VIDEO_FPS, DS_VIDEO_FPS};
        if (!camera_ok(w, ACaptureRequest_setEntry_i32(w->request, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, range), "set 30fps")) return false;
    }
    if (!camera_ok(w, ACameraOutputTarget_create(w->preview_window, &w->preview_target), "create preview target") ||
        !camera_ok(w, ACaptureRequest_addTarget(w->request, w->preview_target), "attach preview target")) return false;
    if (record && (!camera_ok(w, ACameraOutputTarget_create(w->encoder_window, &w->encoder_target), "create encoder target") ||
                   !camera_ok(w, ACaptureRequest_addTarget(w->request, w->encoder_target), "attach encoder target"))) return false;
    if (!camera_ok(w, ACameraCaptureSession_setRepeatingRequest(w->session, NULL, 1, &w->request, NULL), "start camera repeating")) return false;
    w->preview_deadline = clock_ns(CLOCK_MONOTONIC) + INT64_C(8000000000);
    return true;
}

static bool preview_frame(DSVideoWorker *w) {
    AImage *image = NULL;
    const media_status_t result = AImageReader_acquireLatestImage(w->reader, &image);
    if (result == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE) return true;
    if (!media_ok(w, result, "acquire latest YUV image") || !image) return false;
    DSPreviewYuv yuv = {0};
    AImageCropRect crop = {0};
    int32_t format = 0, planes = 0;
    int64_t timestamp = 0;
    bool okay = AImage_getWidth(image, &yuv.width) == AMEDIA_OK && AImage_getHeight(image, &yuv.height) == AMEDIA_OK &&
        AImage_getFormat(image, &format) == AMEDIA_OK && format == AIMAGE_FORMAT_YUV_420_888 &&
        AImage_getNumberOfPlanes(image, &planes) == AMEDIA_OK && planes == 3 &&
        AImage_getCropRect(image, &crop) == AMEDIA_OK && AImage_getTimestamp(image, &timestamp) == AMEDIA_OK && timestamp > 0;
    if (okay && timestamp <= w->last_camera_timestamp) { AImage_delete(image); return true; }
    if (okay && w->realtime_timestamps) {
        const int64_t now = clock_ns(CLOCK_BOOTTIME);
        if (timestamp > now || now - timestamp > DS_PREVIEW_FRESH_NS) { AImage_delete(image); return true; }
    }
    yuv.left = crop.left; yuv.top = crop.top; yuv.right = crop.right; yuv.bottom = crop.bottom;
    int32_t dataspace = ADATASPACE_UNKNOWN;
    if (okay && w->get_dataspace) okay = media_ok(w, w->get_dataspace(image, &dataspace), "query preview dataspace");
    const int standard = dataspace & ADATASPACE_STANDARD_MASK;
    const int range = dataspace & ADATASPACE_RANGE_MASK;
    const int transfer = dataspace & ADATASPACE_TRANSFER_MASK;
    if (okay) {
        /* Android data_space.h specifies these defaults for unspecified YUV:
         * BT709 >=720p, BT601 below; limited range. This is not a filter fallback. */
        yuv.bt709 = standard == ADATASPACE_STANDARD_BT709 ||
            (standard == ADATASPACE_STANDARD_UNSPECIFIED && (yuv.width >= 1280 || yuv.height >= 720));
        yuv.full_range = range == ADATASPACE_RANGE_FULL;
        if ((standard != ADATASPACE_STANDARD_UNSPECIFIED && standard != ADATASPACE_STANDARD_BT709 &&
             standard != ADATASPACE_STANDARD_BT601_625 && standard != ADATASPACE_STANDARD_BT601_525 &&
             standard != ADATASPACE_STANDARD_BT601_625_UNADJUSTED && standard != ADATASPACE_STANDARD_BT601_525_UNADJUSTED) ||
            (range != ADATASPACE_RANGE_UNSPECIFIED && range != ADATASPACE_RANGE_FULL && range != ADATASPACE_RANGE_LIMITED) ||
            (transfer != ADATASPACE_TRANSFER_UNSPECIFIED && transfer != ADATASPACE_TRANSFER_SMPTE_170M)) {
            fail(w, "unsupported preview color space/transfer; not mislabeling HDR as SDR"); okay = false;
        }
        if (okay && !w->color_logged) {
            __android_log_print(ANDROID_LOG_INFO, DS_VIDEO_LOG, "preview dataspace 0x%x (%s %s%s)",
                (unsigned)dataspace, yuv.bt709 ? "BT709" : "BT601", yuv.full_range ? "full" : "limited",
                standard == ADATASPACE_STANDARD_UNSPECIFIED ? "; Android unspecified-YUV rules" : "");
            w->color_logged = true;
        }
    }
    for (int32_t p = 0; okay && p < 3; ++p) {
        uint8_t *data = NULL;
        int length = 0;
        okay = AImage_getPlaneData(image, p, &data, &length) == AMEDIA_OK && length > 0 &&
            AImage_getPlaneRowStride(image, p, &yuv.planes[p].row_stride) == AMEDIA_OK &&
            AImage_getPlanePixelStride(image, p, &yuv.planes[p].pixel_stride) == AMEDIA_OK;
        yuv.planes[p].data = data; yuv.planes[p].length = length > 0 ? (size_t)length : 0;
    }
    int width = 0, height = 0;
    okay = okay && ds_preview_convert(&yuv, w->scratch, DS_PREVIEW_MAX_BYTES, &width, &height);
    AImage_delete(image);
    if (!okay) { fail(w, "invalid camera YUV planes/crop/timestamp; no synthetic preview"); return false; }
    const int64_t received = clock_ns(CLOCK_MONOTONIC);
    pthread_mutex_lock(&w->mutex);
    uint8_t *old = w->frame; w->frame = w->scratch; w->scratch = old;
    w->frame_width = width; w->frame_height = height;
    w->frame_timestamp = timestamp; w->frame_received = received; ++w->frame_number;
    pthread_mutex_unlock(&w->mutex);
    w->last_camera_timestamp = timestamp;
    w->preview_deadline = received + INT64_C(2000000000);
    atomic_store(&w->capture->preview_ready, true);
    return true;
}

static bool open_encoder(DSVideoWorker *w) {
    if (!w->full_hd || !w->fixed_30fps) {
        fail(w, "FullHD 1920x1080 at fixed 30fps unsupported; no resolution/framerate fallback"); return false;
    }
    w->video_track = -1; w->first_pts = -1; w->last_pts = -1; w->saw_frame = w->saw_eos = false;
    w->codec = AMediaCodec_createEncoderByType("video/avc");
    AMediaFormat *format = AMediaFormat_new();
    if (!w->codec || !format) { if (format) AMediaFormat_delete(format); return false; }
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, DS_VIDEO_WIDTH);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, DS_VIDEO_HEIGHT);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, DS_VIDEO_BITRATE);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_FRAME_RATE, DS_VIDEO_FPS);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
    AMediaFormat_setInt32(format, "max-bframes", 0);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_STANDARD, 1); /* BT709 */
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_TRANSFER, 3); /* SDR */
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_RANGE, 2); /* Limited */
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, DS_COLOR_FORMAT_SURFACE);
    bool okay = media_ok(w, AMediaCodec_configure(w->codec, format, NULL, NULL, AMEDIACODEC_CONFIGURE_FLAG_ENCODE), "configure FullHD AVC encoder");
    AMediaFormat_delete(format);
    if (!okay || !media_ok(w, AMediaCodec_createInputSurface(w->codec, &w->encoder_window), "create encoder surface") || !w->encoder_window ||
        !media_ok(w, AMediaCodec_start(w->codec), "start encoder")) return false;
    w->codec_started = true;
    struct timespec now;
    struct tm local;
    char stamp[32];
    if (clock_gettime(CLOCK_REALTIME, &now) != 0 || !localtime_r(&now.tv_sec, &local) ||
        strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local) == 0) return false;
    const int length = snprintf(w->video_path, sizeof(w->video_path), "%s/VIDEO_%s_%09ld.mp4", w->capture->evidence_dir, stamp, now.tv_nsec);
    if (length < 0 || (size_t)length >= sizeof(w->video_path)) { w->video_path[0] = '\0'; return false; }
    w->fd = open(w->video_path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (w->fd < 0) { w->video_path[0] = '\0'; return false; }
    w->muxer = AMediaMuxer_new(w->fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
    return w->muxer && media_ok(w, AMediaMuxer_setOrientationHint(w->muxer, atomic_load(&w->capture->preview_orientation)), "set video orientation");
}

static bool pump_encoder(DSVideoWorker *w, int64_t timeout_us) {
    AMediaCodecBufferInfo info = {0};
    const ssize_t index = AMediaCodec_dequeueOutputBuffer(w->codec, &info, timeout_us);
    if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        if (w->muxer_started) { fail(w, "encoder output format changed twice"); return false; }
        AMediaFormat *format = AMediaCodec_getOutputFormat(w->codec);
        int32_t width = 0, height = 0;
        if (!format) return false;
        const bool dimensions = AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &width) &&
            AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &height) && width == DS_VIDEO_WIDTH && height == DS_VIDEO_HEIGHT;
        w->video_track = dimensions ? AMediaMuxer_addTrack(w->muxer, format) : -1;
        AMediaFormat_delete(format);
        if (w->video_track < 0) { fail(w, "encoder did not produce the requested FullHD track"); return false; }
        if (!media_ok(w, AMediaMuxer_start(w->muxer), "start MP4 muxer")) return false;
        w->muxer_started = true;
        return true;
    }
    if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER || index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) return true;
    if (index < 0) { fail(w, "encoder output dequeue failed"); return false; }
    bool okay = info.size >= 0;
    if ((info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0U) w->saw_eos = true;
    if (okay && info.size > 0 && (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) == 0U) {
        size_t capacity = 0;
        uint8_t *buffer = AMediaCodec_getOutputBuffer(w->codec, (size_t)index, &capacity);
        /* NDK: offsets were invalid through API35; later APIs return zero.
         * getOutputBuffer already points at the valid encoded bytes. */
        info.offset = 0;
        okay = buffer && (size_t)info.size <= capacity && w->muxer_started &&
            info.presentationTimeUs >= 0 && (w->last_pts < 0 || info.presentationTimeUs > w->last_pts);
        if (okay) {
            w->last_pts = info.presentationTimeUs;
            if (w->first_pts < 0) w->first_pts = info.presentationTimeUs;
            info.presentationTimeUs -= w->first_pts;
            okay = media_ok(w, AMediaMuxer_writeSampleData(w->muxer, (size_t)w->video_track, buffer, &info), "write video sample");
            if (okay) w->saw_frame = true;
        }
    }
    if (!media_ok(w, AMediaCodec_releaseOutputBuffer(w->codec, (size_t)index, false), "release encoder output")) okay = false;
    return okay;
}

static bool close_encoder(DSVideoWorker *w, bool finalize) {
    bool okay = finalize;
    if (finalize && w->codec_started) {
        okay = media_ok(w, AMediaCodec_signalEndOfInputStream(w->codec), "signal encoder EOS");
        const int64_t deadline = clock_ns(CLOCK_MONOTONIC) + INT64_C(3000000000);
        while (okay && !w->saw_eos && clock_ns(CLOCK_MONOTONIC) < deadline) okay = pump_encoder(w, 10000);
        okay = okay && w->saw_eos && w->saw_frame && w->muxer_started;
    } else okay = false;
    if (w->muxer) {
        if (w->muxer_started && !media_ok(w, AMediaMuxer_stop(w->muxer), "finalize MP4")) okay = false;
        AMediaMuxer_delete(w->muxer); w->muxer = NULL;
    }
    if (w->codec) {
        if (w->codec_started && !media_ok(w, AMediaCodec_stop(w->codec), "stop encoder")) okay = false;
        AMediaCodec_delete(w->codec); w->codec = NULL;
    }
    if (w->encoder_window) { ANativeWindow_release(w->encoder_window); w->encoder_window = NULL; }
    if (w->fd >= 0) {
        if (okay && fsync(w->fd) != 0) okay = false;
        if (close(w->fd) != 0) okay = false;
        w->fd = -1;
    }
    if (w->video_path[0]) {
        if (!okay) {
            if (unlink(w->video_path) != 0) __android_log_print(ANDROID_LOG_ERROR, DS_VIDEO_LOG, "could not remove incomplete video: %s", strerror(errno));
            fail(w, "recording failed/incomplete; incomplete MP4 removed");
        } else __android_log_print(ANDROID_LOG_INFO, DS_VIDEO_LOG, "local FullHD video finalized");
    }
    w->video_path[0] = '\0'; w->codec_started = w->muxer_started = false;
    return okay;
}

static bool device_failed(DSVideoWorker *w) {
    pthread_mutex_lock(&w->mutex);
    const bool result = w->camera_error;
    pthread_mutex_unlock(&w->mutex);
    return result;
}

static void recording_finished(DSVideoWorker *w) {
    pthread_mutex_lock(&w->mutex);
    w->record_requested = false;
    atomic_store(&w->capture->active, false);
    pthread_cond_broadcast(&w->condition);
    pthread_mutex_unlock(&w->mutex);
}

static void *video_thread(void *user_data) {
    DSVideoWorker *w = user_data;
    DSVideoCapture *capture = w->capture;
    bool recording = false;
    w->manager = ACameraManager_create();
    /* Optional newer NDK query; no API34 symbol is imported into an API30 APK. */
    w->get_dataspace = (media_status_t (*)(const AImage *, int32_t *))dlsym(RTLD_DEFAULT, "AImage_getDataSpace");
    char id[128] = {0};
    if (!w->manager || !select_camera(w, id)) goto fatal;
    ACameraDevice_StateCallbacks callbacks = { .context = w, .onDisconnected = camera_disconnected, .onError = camera_error };
    if (!camera_ok(w, ACameraManager_openCamera(w->manager, id, &callbacks, &w->device), "open rear camera (requires CAMERA permission)") || !w->device ||
        !media_ok(w, AImageReader_new(w->preview_width, w->preview_height, AIMAGE_FORMAT_YUV_420_888, 3, &w->reader), "create YUV reader") || !w->reader ||
        !media_ok(w, AImageReader_getWindow(w->reader, &w->preview_window), "get YUV surface") || !w->preview_window ||
        !create_session(w, false)) goto fatal;
    while (atomic_load(&capture->preview_running)) {
        if (device_failed(w)) goto fatal;
        pthread_mutex_lock(&w->mutex);
        const bool wanted = w->record_requested;
        pthread_mutex_unlock(&w->mutex);
        if (wanted && !recording) {
            if (!open_encoder(w)) {
                fail(w, "cannot start requested FullHD recording"); close_encoder(w, false); recording_finished(w);
            } else {
                if (!close_session(w)) goto fatal;
                if (!create_session(w, true)) {
                    fail(w, "FullHD+preview stream combination rejected; recording not started");
                    if (!close_session(w)) goto fatal;
                    close_encoder(w, false); recording_finished(w);
                    if (!w->device || !create_session(w, false)) goto fatal;
                } else recording = true;
            }
        } else if (!wanted && recording) {
            if (!close_session(w)) goto fatal;
            if (!close_encoder(w, true)) fail(w, "video recording did not finalize successfully");
            recording = false;
            if (!create_session(w, false)) goto fatal;
            recording_finished(w);
        } else if (!wanted && !recording && atomic_load(&capture->active)) {
            recording_finished(w); /* Start canceled before worker observed it. */
        }
        if (!preview_frame(w)) goto fatal;
        if (clock_ns(CLOCK_MONOTONIC) > w->preview_deadline) { fail(w, "camera stopped delivering fresh preview frames"); goto fatal; }
        if (recording) for (int n = 0; n < 4; ++n) if (!pump_encoder(w, 0)) goto fatal;
        const int64_t deadline = clock_ns(CLOCK_MONOTONIC) + INT64_C(10000000);
        const struct timespec until = { .tv_sec = (time_t)(deadline / INT64_C(1000000000)), .tv_nsec = (long)(deadline % INT64_C(1000000000)) };
        pthread_mutex_lock(&w->mutex);
        if (atomic_load(&capture->preview_running) && w->record_requested == recording && !w->camera_error)
            pthread_cond_timedwait(&w->condition, &w->mutex, &until);
        pthread_mutex_unlock(&w->mutex);
    }
    {
        const bool closed = close_session(w);
        if (w->codec) close_encoder(w, recording && closed && !device_failed(w));
    }
    goto cleanup;
fatal:
    fail(w, "native camera pipeline failed; preview stopped");
    close_session(w);
    if (w->codec) close_encoder(w, false);
cleanup:
    if (w->device) { ACameraDevice_close(w->device); w->device = NULL; }
    if (w->reader) { AImageReader_delete(w->reader); w->reader = NULL; }
    if (w->metadata) { ACameraMetadata_free(w->metadata); w->metadata = NULL; }
    if (w->manager) { ACameraManager_delete(w->manager); w->manager = NULL; }
    atomic_store(&capture->preview_ready, false); atomic_store(&capture->preview_running, false);
    pthread_mutex_lock(&w->mutex);
    w->worker_finished = true; w->record_requested = false; atomic_store(&capture->active, false);
    pthread_cond_broadcast(&w->condition);
    pthread_mutex_unlock(&w->mutex);
    return NULL;
}

void ds_video_preview_start(DSVideoCapture *capture, DSVideoFilter filter, int display_rotation) {
    if (!capture) return;
    if (capture->thread_created && atomic_load(&capture->preview_running) && capture->filter == filter && capture->display_rotation == display_rotation) return;
    ds_video_preview_stop(capture);
    atomic_store(&capture->failed, false); atomic_store(&capture->preview_orientation, -1);
    if (filter < DS_VIDEO_FILTER_NIGHT || filter > DS_VIDEO_FILTER_RAW || display_rotation < 0 || display_rotation > 3) {
        atomic_store(&capture->failed, true); return;
    }
    DSVideoWorker *w = calloc(1, sizeof(*w));
    if (!w) { atomic_store(&capture->failed, true); return; }
    w->capture = capture; w->fd = -1;
    w->frame = malloc(DS_PREVIEW_MAX_BYTES); w->scratch = malloc(DS_PREVIEW_MAX_BYTES);
    bool mutex_ready = false, condition_ready = false;
    pthread_condattr_t attr;
    const bool attr_ready = pthread_condattr_init(&attr) == 0;
    if (w->frame && w->scratch && attr_ready && pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0 && pthread_mutex_init(&w->mutex, NULL) == 0) {
        mutex_ready = true; condition_ready = pthread_cond_init(&w->condition, &attr) == 0;
    }
    if (attr_ready) pthread_condattr_destroy(&attr);
    if (!condition_ready) {
        if (mutex_ready) pthread_mutex_destroy(&w->mutex);
        free(w->frame); free(w->scratch); free(w); atomic_store(&capture->failed, true); return;
    }
    capture->filter = filter; capture->display_rotation = display_rotation; capture->implementation = w;
    atomic_store(&capture->preview_running, true);
    if (pthread_create(&capture->thread, NULL, video_thread, w) != 0) {
        atomic_store(&capture->preview_running, false); atomic_store(&capture->failed, true);
        pthread_cond_destroy(&w->condition); pthread_mutex_destroy(&w->mutex);
        free(w->frame); free(w->scratch); free(w); capture->implementation = NULL; return;
    }
    capture->thread_created = true;
}

void ds_video_preview_stop(DSVideoCapture *capture) {
    if (!capture) return;
    DSVideoWorker *w = capture->implementation;
    atomic_store(&capture->preview_ready, false); atomic_store(&capture->preview_running, false);
    if (w) {
        pthread_mutex_lock(&w->mutex); w->record_requested = false;
        pthread_cond_broadcast(&w->condition); pthread_mutex_unlock(&w->mutex);
    }
    if (capture->thread_created) { pthread_join(capture->thread, NULL); capture->thread_created = false; }
    if (w) {
        pthread_cond_destroy(&w->condition); pthread_mutex_destroy(&w->mutex);
        free(w->frame); free(w->scratch); free(w); capture->implementation = NULL;
    }
    atomic_store(&capture->active, false);
}

void ds_video_start(DSVideoCapture *capture, const char *evidence_dir, DSVideoFilter filter, int display_rotation) {
    if (!capture || ds_video_active(capture)) return;
    if (!evidence_dir || !evidence_dir[0] || strlen(evidence_dir) >= sizeof(capture->evidence_dir)) { atomic_store(&capture->failed, true); return; }
    ds_video_preview_start(capture, filter, display_rotation);
    DSVideoWorker *w = capture->implementation;
    if (!w || !atomic_load(&capture->preview_running)) return;
    pthread_mutex_lock(&w->mutex);
    if (!w->worker_finished && !w->camera_error) {
        strcpy(capture->evidence_dir, evidence_dir);
        w->record_requested = true; atomic_store(&capture->active, true); pthread_cond_broadcast(&w->condition);
    }
    pthread_mutex_unlock(&w->mutex);
}

void ds_video_stop(DSVideoCapture *capture) {
    if (!capture || !capture->implementation) return;
    DSVideoWorker *w = capture->implementation;
    pthread_mutex_lock(&w->mutex); w->record_requested = false; pthread_cond_broadcast(&w->condition);
    while (atomic_load(&capture->active) && !w->worker_finished) pthread_cond_wait(&w->condition, &w->mutex);
    pthread_mutex_unlock(&w->mutex);
}

bool ds_video_preview_ready(const DSVideoCapture *capture) {
    if (!capture || !atomic_load(&capture->preview_running) || !atomic_load(&capture->preview_ready) || !capture->implementation) return false;
    DSVideoWorker *w = capture->implementation;
    if (pthread_mutex_trylock(&w->mutex) != 0) return false;
    const bool fresh = !w->camera_error && ds_preview_fresh(w->frame_received, clock_ns(CLOCK_MONOTONIC));
    pthread_mutex_unlock(&w->mutex); return fresh;
}

bool ds_video_copy_frame(DSVideoCapture *capture, uint8_t *rgba, size_t capacity, int *width, int *height, int64_t *timestamp_ns) {
    if (!capture || !rgba || !width || !height || !timestamp_ns || !atomic_load(&capture->preview_running) ||
        !atomic_load(&capture->preview_ready) || !capture->implementation) return false;
    DSVideoWorker *w = capture->implementation;
    if (pthread_mutex_trylock(&w->mutex) != 0) return false;
    bool okay = !w->camera_error && w->frame_number != w->copied_number && ds_preview_fresh(w->frame_received, clock_ns(CLOCK_MONOTONIC));
    if (okay) {
        *width = w->frame_width; *height = w->frame_height; *timestamp_ns = w->frame_timestamp;
        const size_t bytes = (size_t)w->frame_width * (size_t)w->frame_height * 4U;
        okay = bytes <= capacity;
        if (okay) { memcpy(rgba, w->frame, bytes); w->copied_number = w->frame_number; }
    }
    pthread_mutex_unlock(&w->mutex); return okay;
}

int ds_video_preview_orientation(const DSVideoCapture *capture) { return capture ? atomic_load(&capture->preview_orientation) : -1; }
bool ds_video_active(const DSVideoCapture *capture) { return capture && atomic_load(&capture->active); }
bool ds_video_failed(const DSVideoCapture *capture) { return capture && atomic_load(&capture->failed); }
