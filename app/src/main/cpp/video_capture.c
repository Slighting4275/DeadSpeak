#define _POSIX_C_SOURCE 200809L
#include "video_capture.h"
#include "video_orientation.h"
#include "video_shaders.h"
#include "preview_frame.h"
#include "typography.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
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
#define DS_VIDEO_DEFAULT_W 1920
#define DS_VIDEO_DEFAULT_H 1080
#define DS_VIDEO_FPS 30
#define DS_VIDEO_BITRATE 8000000
#define DS_AUDIO_SAMPLE_RATE 48000
#define DS_AUDIO_BITRATE 96000
#define DS_COLOR_FORMAT_SURFACE 0x7F000789
#define DS_EGL_RECORDABLE_ANDROID 0x3142
#define DS_AUDIO_RING_CAP 48000U
#define DS_MAX_PENDING_SAMPLES 256U
#define DS_HUD_TEX_W 640
#define DS_HUD_TEX_H 64

typedef EGLBoolean (EGLAPIENTRYP DS_PFNEGLPRESENTATIONTIMEANDROID)(EGLDisplay dpy, EGLSurface surface, EGLnsecsANDROID time);

typedef struct {
    uint8_t *data;
    size_t size;
    int64_t pts_us;
    uint32_t flags;
    bool is_audio;
} DSPendingSample;

/* The worker alone owns Android, MediaCodec, Muxer, and its offscreen EGL resources.
 * The application serializes public lifecycle/copy calls; audio/hud feeds acquire w->mutex briefly. */
typedef struct {
    DSVideoCapture *capture;
    pthread_mutex_t mutex;
    pthread_cond_t condition; /* CLOCK_MONOTONIC */
    bool camera_error, session_closed, record_requested, worker_finished;
    uint8_t *frame, *scratch;
    int frame_width, frame_height;
    int64_t frame_timestamp, frame_received, last_camera_timestamp, preview_deadline;
    uint64_t frame_number, copied_number, encoded_frame_number;
    int preview_width, preview_height;
    bool fixed_30fps, realtime_timestamps;
    media_status_t (*get_dataspace)(const AImage *, int32_t *);
    bool color_logged;

    /* Camera2 preview pipeline */
    ACameraManager *manager;
    ACameraMetadata *metadata;
    ACameraDevice *device;
    AImageReader *reader;
    ANativeWindow *preview_window; /* Reader-owned; do not release separately. */
    ACameraCaptureSession *session;
    ACaptureSessionOutputContainer *outputs;
    ACaptureSessionOutput *preview_output;
    ACameraOutputTarget *preview_target;
    ACaptureRequest *request;

    /* Offscreen GL ES encoder pipeline (H.264 video + AAC audio -> AMediaMuxer) */
    int enc_width, enc_height;
    AMediaCodec *codec;
    AMediaCodec *audio_codec;
    ANativeWindow *encoder_window;
    AMediaMuxer *muxer;
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    DS_PFNEGLPRESENTATIONTIMEANDROID egl_presentation_time;
    GLuint gl_cam_prog, gl_hud_prog, gl_cam_tex, gl_hud_tex;
    GLint gl_cam_img_loc, gl_cam_filter_loc, gl_cam_anomaly_loc, gl_cam_time_loc, gl_hud_img_loc;
    bool current_torch, current_front;
    uint8_t *hud_rgba;

    bool codec_started, audio_started, muxer_started;
    bool saw_video_frame, saw_video_eos, saw_audio_eos, audio_eos_sent;
    ssize_t video_track, audio_track;
    int fd;
    int64_t rec_start_ns, last_enc_frame_ns;
    int64_t first_video_pts, last_video_pts, first_audio_pts, last_audio_pts;
    uint64_t audio_samples_submitted;
    int64_t audio_base_offset_us;
    bool audio_base_set;
    char video_path[640];

    /* Audio PCM16 ring queue */
    int16_t audio_ring[DS_AUDIO_RING_CAP];
    size_t audio_head, audio_tail, audio_count;

    /* Snapshot HUD telemetry updated by main/render thread */
    DSVideoHudState hud;

    /* Bounded FIFO for packets dequeued before both tracks are added & muxer starts */
    DSPendingSample pending[DS_MAX_PENDING_SAMPLES];
    size_t pending_count;
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

/* Choose a genuinely advertised bounded YUV stream (preferring 640x360..1280x720). */
static bool select_camera(DSVideoWorker *w, char id[128]) {
    ACameraIdList *ids = NULL;
    if (!camera_ok(w, ACameraManager_getCameraIdList(w->manager, &ids), "list cameras") || !ids) return false;
    const bool want_front = atomic_load(&w->capture->front_camera);
    bool has_front = false;
    ACameraMetadata *best_meta[2] = {NULL, NULL};
    char best_id[2][128] = {{0}, {0}};
    int best_pw[2] = {0, 0}, best_ph[2] = {0, 0};
    int64_t best_score[2] = {INT64_MAX, INT64_MAX};
    for (int i = 0; i < ids->numCameras; ++i) {
        ACameraMetadata *metadata = NULL;
        if (ACameraManager_getCameraCharacteristics(w->manager, ids->cameraIds[i], &metadata) != ACAMERA_OK || !metadata) continue;
        ACameraMetadata_const_entry facing = {0}, streams = {0};
        const bool have_facing = ACameraMetadata_getConstEntry(metadata, ACAMERA_LENS_FACING, &facing) == ACAMERA_OK &&
            facing.type == ACAMERA_TYPE_BYTE && facing.count == 1;
        const bool is_front = have_facing && facing.data.u8[0] == ACAMERA_LENS_FACING_FRONT;
        const bool is_back = have_facing && facing.data.u8[0] == ACAMERA_LENS_FACING_BACK;
        if ((is_front || is_back) &&
            ACameraMetadata_getConstEntry(metadata, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &streams) == ACAMERA_OK &&
            streams.type == ACAMERA_TYPE_INT32 && streams.count % 4U == 0U) {
            int pw = 0, ph = 0;
            int64_t score = INT64_MAX;
            for (uint32_t n = 0; n < streams.count; n += 4U) {
                const int32_t format = streams.data.i32[n], width = streams.data.i32[n + 1U];
                const int32_t height = streams.data.i32[n + 2U], direction = streams.data.i32[n + 3U];
                if (direction != ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT) continue;
                if (format != AIMAGE_FORMAT_YUV_420_888 || width < 1 || height < 1 ||
                    width > DS_VIDEO_DEFAULT_W || height > DS_VIDEO_DEFAULT_H) continue;
                int64_t candidate = llabs((int64_t)width * height - INT64_C(640) * 360);
                candidate += llabs((int64_t)width * 9 - (int64_t)height * 16) * 100;
                if (candidate < score) { score = candidate; pw = width; ph = height; }
            }
            const int slot = is_front ? 1 : 0;
            if (is_front && pw > 0) has_front = true;
            if (pw > 0 && score < best_score[slot] && strlen(ids->cameraIds[i]) < 128U) {
                if (best_meta[slot]) ACameraMetadata_free(best_meta[slot]);
                best_meta[slot] = metadata; metadata = NULL;
                snprintf(best_id[slot], 128, "%s", ids->cameraIds[i]);
                best_pw[slot] = pw; best_ph[slot] = ph;
                best_score[slot] = score;
            }
        }
        if (metadata) ACameraMetadata_free(metadata);
    }
    ACameraManager_deleteCameraIdList(ids);
    atomic_store(&w->capture->front_available, has_front);
    const int chosen = (want_front && best_meta[1]) ? 1 : (best_meta[0] ? 0 : (best_meta[1] ? 1 : -1));
    if (chosen < 0) {
        if (best_meta[0]) ACameraMetadata_free(best_meta[0]);
        if (best_meta[1]) ACameraMetadata_free(best_meta[1]);
        fail(w, "no camera with a supported bounded YUV preview stream");
        return false;
    }
    const int other = 1 - chosen;
    if (best_meta[other]) ACameraMetadata_free(best_meta[other]);
    if (w->metadata) ACameraMetadata_free(w->metadata);
    w->metadata = best_meta[chosen];
    snprintf(id, 128, "%s", best_id[chosen]);
    w->preview_width = best_pw[chosen]; w->preview_height = best_ph[chosen];
    w->current_front = (chosen == 1);
    atomic_store(&w->capture->front_camera, w->current_front);

    ACameraMetadata_const_entry flash = {0};
    const bool torch_avail = ACameraMetadata_getConstEntry(w->metadata, ACAMERA_FLASH_INFO_AVAILABLE, &flash) == ACAMERA_OK &&
        flash.type == ACAMERA_TYPE_BYTE && flash.count == 1 && flash.data.u8[0] != 0;
    atomic_store(&w->capture->torch_available, torch_avail);
    if (!torch_avail) atomic_store(&w->capture->torch_enabled, false);

    ACameraMetadata_const_entry orientation = {0}, fps = {0}, source = {0};
    if (ACameraMetadata_getConstEntry(w->metadata, ACAMERA_SENSOR_ORIENTATION, &orientation) != ACAMERA_OK ||
        orientation.type != ACAMERA_TYPE_INT32 || orientation.count != 1) {
        fail(w, "missing camera orientation metadata"); return false;
    }
    const int rotation = ds_video_orientation_facing(orientation.data.i32[0], w->capture->display_rotation, w->current_front);
    if (rotation < 0) { fail(w, "invalid display/camera orientation"); return false; }
    atomic_store(&w->capture->preview_orientation, rotation);
    w->fixed_30fps = false;
    if (ACameraMetadata_getConstEntry(w->metadata, ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, &fps) == ACAMERA_OK &&
        fps.type == ACAMERA_TYPE_INT32 && fps.count % 2U == 0U) {
        for (uint32_t n = 0; n < fps.count; n += 2U)
            if (fps.data.i32[n] == DS_VIDEO_FPS && fps.data.i32[n + 1U] == DS_VIDEO_FPS) w->fixed_30fps = true;
    }
    w->realtime_timestamps = ACameraMetadata_getConstEntry(w->metadata, ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE, &source) == ACAMERA_OK &&
        source.type == ACAMERA_TYPE_BYTE && source.count == 1 && source.data.u8[0] == ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME;
    __android_log_print(ANDROID_LOG_INFO, DS_VIDEO_LOG, "%s camera %s YUV %dx%d orientation %d torch %d fixed30 %d",
                        w->current_front ? "front" : "rear", id, w->preview_width, w->preview_height,
                        rotation, torch_avail ? 1 : 0, w->fixed_30fps);
    return true;
}

static bool configure_camera_exposure(DSVideoWorker *w) {
    const DSVideoFilter filter = (DSVideoFilter)atomic_load(&w->capture->filter);
    const uint8_t effect = ACAMERA_CONTROL_EFFECT_MODE_OFF;
    int32_t compensation = 0;
    if (filter == DS_VIDEO_FILTER_NIGHT) {
        ACameraMetadata_const_entry range = {0};
        if (ACameraMetadata_getConstEntry(w->metadata, ACAMERA_CONTROL_AE_COMPENSATION_RANGE, &range) == ACAMERA_OK &&
            range.type == ACAMERA_TYPE_INT32 && range.count == 2 && range.data.i32[1] > 0) {
            compensation = range.data.i32[1] < 6 ? range.data.i32[1] : 6;
        }
    }
    (void)ACaptureRequest_setEntry_u8(w->request, ACAMERA_CONTROL_EFFECT_MODE, 1, &effect);
    (void)ACaptureRequest_setEntry_i32(w->request, ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION, 1, &compensation);
    return true;
}

static void configure_camera_torch(DSVideoWorker *w) {
    const bool avail = atomic_load(&w->capture->torch_available);
    const bool enabled = avail && atomic_load(&w->capture->torch_enabled);
    if (avail && w->request) {
        const uint8_t mode = enabled ? ACAMERA_FLASH_MODE_TORCH : ACAMERA_FLASH_MODE_OFF;
        (void)ACaptureRequest_setEntry_u8(w->request, ACAMERA_FLASH_MODE, 1, &mode);
    }
    w->current_torch = enabled;
}

static void free_session_objects(DSVideoWorker *w) {
    if (w->request) ACaptureRequest_free(w->request);
    if (w->preview_target) ACameraOutputTarget_free(w->preview_target);
    if (w->outputs) ACaptureSessionOutputContainer_free(w->outputs);
    if (w->preview_output) ACaptureSessionOutput_free(w->preview_output);
    w->request = NULL; w->preview_target = NULL;
    w->outputs = NULL; w->preview_output = NULL;
}

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

static bool create_session(DSVideoWorker *w) {
    pthread_mutex_lock(&w->mutex);
    w->session_closed = false;
    pthread_mutex_unlock(&w->mutex);
    if (!camera_ok(w, ACaptureSessionOutputContainer_create(&w->outputs), "create output container") ||
        !camera_ok(w, ACaptureSessionOutput_create(w->preview_window, &w->preview_output), "create YUV output") ||
        !camera_ok(w, ACaptureSessionOutputContainer_add(w->outputs, w->preview_output), "add YUV output")) return false;
    ACameraCaptureSession_stateCallbacks callbacks = { .context = w, .onClosed = session_closed, .onReady = session_state, .onActive = session_state };
    if (!camera_ok(w, ACameraDevice_createCaptureSession(w->device, w->outputs, &callbacks, &w->session), "create YUV camera session") ||
        !w->session ||
        !camera_ok(w, ACameraDevice_createCaptureRequest(w->device, TEMPLATE_PREVIEW, &w->request), "create preview request")) return false;
    configure_camera_exposure(w);
    configure_camera_torch(w);
    if (w->fixed_30fps) {
        const int32_t range[2] = {DS_VIDEO_FPS, DS_VIDEO_FPS};
        (void)ACaptureRequest_setEntry_i32(w->request, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, range);
    }
    if (!camera_ok(w, ACameraOutputTarget_create(w->preview_window, &w->preview_target), "create preview target") ||
        !camera_ok(w, ACaptureRequest_addTarget(w->request, w->preview_target), "attach preview target") ||
        !camera_ok(w, ACameraCaptureSession_setRepeatingRequest(w->session, NULL, 1, &w->request, NULL), "start camera repeating")) return false;
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
            __android_log_print(ANDROID_LOG_INFO, DS_VIDEO_LOG, "preview dataspace 0x%x (%s %s)",
                (unsigned)dataspace, yuv.bt709 ? "BT709" : "BT601", yuv.full_range ? "full" : "limited");
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

static GLuint compile_gl_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    if (!s) return 0;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[256] = {0};
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        __android_log_print(ANDROID_LOG_ERROR, DS_VIDEO_LOG, "GL shader compile failed: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link_gl_program(const char *vert_src, const char *frag_src) {
    GLuint vs = compile_gl_shader(GL_VERTEX_SHADER, vert_src);
    GLuint fs = compile_gl_shader(GL_FRAGMENT_SHADER, frag_src);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint prog = glCreateProgram();
    if (prog) {
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        glBindAttribLocation(prog, 0, "a_position");
        glBindAttribLocation(prog, 1, "a_uv");
        glLinkProgram(prog);
        GLint ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            glDeleteProgram(prog);
            prog = 0;
        }
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

static void destroy_encoder_gl(DSVideoWorker *w) {
    if (w->egl_display != EGL_NO_DISPLAY && w->egl_context != EGL_NO_CONTEXT &&
        w->egl_surface != EGL_NO_SURFACE) {
        eglMakeCurrent(w->egl_display, w->egl_surface, w->egl_surface, w->egl_context);
        if (w->gl_cam_tex) { glDeleteTextures(1, &w->gl_cam_tex); w->gl_cam_tex = 0; }
        if (w->gl_hud_tex) { glDeleteTextures(1, &w->gl_hud_tex); w->gl_hud_tex = 0; }
        if (w->gl_cam_prog) { glDeleteProgram(w->gl_cam_prog); w->gl_cam_prog = 0; }
        if (w->gl_hud_prog) { glDeleteProgram(w->gl_hud_prog); w->gl_hud_prog = 0; }
    }
    if (w->egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(w->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (w->egl_surface != EGL_NO_SURFACE) {
            eglDestroySurface(w->egl_display, w->egl_surface);
            w->egl_surface = EGL_NO_SURFACE;
        }
        if (w->egl_context != EGL_NO_CONTEXT) {
            eglDestroyContext(w->egl_display, w->egl_context);
            w->egl_context = EGL_NO_CONTEXT;
        }
        eglTerminate(w->egl_display);
        w->egl_display = EGL_NO_DISPLAY;
    }
    free(w->hud_rgba);
    w->hud_rgba = NULL;
}

static bool init_encoder_gl(DSVideoWorker *w) {
    w->egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (w->egl_display == EGL_NO_DISPLAY || !eglInitialize(w->egl_display, NULL, NULL)) {
        fail(w, "eglInitialize failed for encoder"); return false;
    }
    const EGLint rec_attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        DS_EGL_RECORDABLE_ANDROID, 1,
        EGL_NONE
    };
    const EGLint fallback_attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg = NULL;
    EGLint count = 0;
    if (!eglChooseConfig(w->egl_display, rec_attribs, &cfg, 1, &count) || count == 0) {
        if (!eglChooseConfig(w->egl_display, fallback_attribs, &cfg, 1, &count) || count == 0) {
            fail(w, "eglChooseConfig failed for encoder"); return false;
        }
    }
    const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    w->egl_context = eglCreateContext(w->egl_display, cfg, EGL_NO_CONTEXT, ctx_attribs);
    w->egl_surface = eglCreateWindowSurface(w->egl_display, cfg, w->encoder_window, NULL);
    if (w->egl_context == EGL_NO_CONTEXT || w->egl_surface == EGL_NO_SURFACE ||
        !eglMakeCurrent(w->egl_display, w->egl_surface, w->egl_surface, w->egl_context)) {
        fail(w, "EGL encoder surface makeCurrent failed"); return false;
    }
    w->egl_presentation_time = (DS_PFNEGLPRESENTATIONTIMEANDROID)eglGetProcAddress("eglPresentationTimeANDROID");
    w->gl_cam_prog = link_gl_program(DS_CAMERA_VERTEX_SHADER, DS_CAMERA_FRAGMENT_SHADER);
    w->gl_hud_prog = link_gl_program(DS_CAMERA_VERTEX_SHADER, DS_RGBA_OVERLAY_FRAGMENT_SHADER);
    if (!w->gl_cam_prog || !w->gl_hud_prog) {
        fail(w, "failed to link encoder GL shader programs"); return false;
    }
    w->gl_cam_img_loc = glGetUniformLocation(w->gl_cam_prog, "u_image");
    w->gl_cam_filter_loc = glGetUniformLocation(w->gl_cam_prog, "u_filter");
    w->gl_cam_anomaly_loc = glGetUniformLocation(w->gl_cam_prog, "u_anomaly");
    w->gl_cam_time_loc = glGetUniformLocation(w->gl_cam_prog, "u_time");
    w->gl_hud_img_loc = glGetUniformLocation(w->gl_hud_prog, "u_image");

    glGenTextures(1, &w->gl_cam_tex);
    glBindTexture(GL_TEXTURE_2D, w->gl_cam_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &w->gl_hud_tex);
    glBindTexture(GL_TEXTURE_2D, w->gl_hud_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    w->hud_rgba = malloc((size_t)DS_HUD_TEX_W * (size_t)DS_HUD_TEX_H * 4U);
    return w->hud_rgba != NULL;
}

static void hud_fill_rect(uint8_t *rgba, int tex_w, int tex_h, int rx, int ry, int rw, int rh,
                          uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const int x0 = rx < 0 ? 0 : rx;
    const int y0 = ry < 0 ? 0 : ry;
    const int x1 = (rx + rw > tex_w) ? tex_w : (rx + rw);
    const int y1 = (ry + rh > tex_h) ? tex_h : (ry + rh);
    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            uint8_t *dst = rgba + ((size_t)py * (size_t)tex_w + (size_t)px) * 4U;
            dst[0] = r; dst[1] = g; dst[2] = b; dst[3] = a;
        }
    }
}

static void hud_draw_text(uint8_t *rgba, int tex_w, int tex_h, int sx, int sy, int scale,
                          const char *str, uint8_t r, uint8_t g, uint8_t b) {
    if (!str) return;
    int pen_x = sx;
    for (size_t i = 0; str[i] != '\0'; ++i) {
        const int gi = ds_glyph_index(str[i]);
        if (gi >= 0 && gi < (int)DS_FONT_GLYPH_COUNT) {
            for (int row = 0; row < (int)DS_FONT_GLYPH_ROWS; ++row) {
                const uint8_t bits = ds_font_bitmap[gi][row];
                for (int col = 0; col < (int)DS_FONT_GLYPH_COLS; ++col) {
                    if ((bits & (1u << (4 - col))) != 0u) {
                        hud_fill_rect(rgba, tex_w, tex_h, pen_x + col * scale, sy + row * scale,
                                      scale, scale, r, g, b, 255);
                    }
                }
            }
        }
        pen_x += 6 * scale;
        if (pen_x >= tex_w - 8) break;
    }
}

static void rasterize_hud_overlay(DSVideoWorker *w, const DSVideoHudState *hud, int64_t elapsed_us) {
    uint8_t *buf = w->hud_rgba;
    memset(buf, 0, (size_t)DS_HUD_TEX_W * (size_t)DS_HUD_TEX_H * 4U);
    const unsigned total_s = (unsigned)(elapsed_us / INT64_C(1000000));
    if (hud->hud_layout == DS_HUD_LAYOUT_MINIMAL) {
        hud_fill_rect(buf, DS_HUD_TEX_W, DS_HUD_TEX_H, 0, 18, 176, 36, 8, 12, 11, 190);
        char min_line[32];
        snprintf(min_line, sizeof(min_line), "REC %02u:%02u%s", total_s / 60U, total_s % 60U,
                 hud->anomaly ? " !" : "");
        hud_draw_text(buf, DS_HUD_TEX_W, DS_HUD_TEX_H, 12, 29, 2, min_line,
                      hud->anomaly ? 255 : 232, hud->anomaly ? 140 : 248, hud->anomaly ? 45 : 238);
        return;
    }

    hud_fill_rect(buf, DS_HUD_TEX_W, DS_HUD_TEX_H, 0, 0, DS_HUD_TEX_W, DS_HUD_TEX_H, 8, 12, 11, 200);
    hud_fill_rect(buf, DS_HUD_TEX_W, DS_HUD_TEX_H, 0, 0, DS_HUD_TEX_W, 2, 48, 220, 145, 220);

    char top_line[80];
    snprintf(top_line, sizeof(top_line), "REC %02u:%02u  %s  EMF %.1f UT (%+.1f)",
             total_s / 60U, total_s % 60U, hud->filter_name[0] ? hud->filter_name : "RAW",
             hud->emf_ut, hud->emf_delta);
    hud_draw_text(buf, DS_HUD_TEX_W, DS_HUD_TEX_H, 14, 10, 2, top_line, 232, 248, 238);

    if (hud->hud_layout == DS_HUD_LAYOUT_COMPACT) {
        if (hud->word[0]) {
            char compact_w[64];
            snprintf(compact_w, sizeof(compact_w), "VOX: %s", hud->word);
            hud_draw_text(buf, DS_HUD_TEX_W, DS_HUD_TEX_H, 14, 38, 2, compact_w, 140, 240, 195);
        }
        return;
    }

    char bot_line[80];
    if (hud->word[0]) {
        snprintf(bot_line, sizeof(bot_line), "VOX: %s%s", hud->word, hud->anomaly ? "  [ANOMALY]" : "");
    } else if (hud->anomaly) {
        snprintf(bot_line, sizeof(bot_line), "ANOMALY DETECTED");
    } else {
        snprintf(bot_line, sizeof(bot_line), "LISTENING...");
    }
    const uint8_t wr = hud->anomaly ? 255 : 140;
    const uint8_t wg = hud->anomaly ? 140 : 240;
    const uint8_t wb = hud->anomaly ? 45 : 195;
    hud_draw_text(buf, DS_HUD_TEX_W, DS_HUD_TEX_H, 14, 38, 2, bot_line, wr, wg, wb);

    /* Signal bar on right */
    const int bar_x = DS_HUD_TEX_W - 150, bar_y = 38, bar_w = 132, bar_h = 14;
    hud_fill_rect(buf, DS_HUD_TEX_W, DS_HUD_TEX_H, bar_x, bar_y, bar_w, bar_h, 30, 42, 38, 220);
    const float cl_sig = hud->signal < 0.0f ? 0.0f : (hud->signal > 1.0f ? 1.0f : hud->signal);
    const int fill_w = (int)((float)bar_w * cl_sig);
    if (fill_w > 0) {
        const uint8_t sr = cl_sig > 0.7f ? 255 : 70;
        const uint8_t sg = cl_sig > 0.7f ? 75 : 245;
        hud_fill_rect(buf, DS_HUD_TEX_W, DS_HUD_TEX_H, bar_x, bar_y, fill_w, bar_h, sr, sg, 95, 240);
    }
}

static bool open_encoder(DSVideoWorker *w) {
    if (!ds_video_encode_dimensions(w->capture->aspect, &w->enc_width, &w->enc_height)) {
        w->enc_width = DS_VIDEO_DEFAULT_W;
        w->enc_height = DS_VIDEO_DEFAULT_H;
    }
    w->video_track = -1; w->audio_track = -1;
    w->first_video_pts = -1; w->last_video_pts = -1;
    w->first_audio_pts = -1; w->last_audio_pts = -1;
    w->saw_video_frame = w->saw_video_eos = w->saw_audio_eos = w->audio_eos_sent = false;
    w->muxer_started = false; w->pending_count = 0;
    w->rec_start_ns = 0; w->last_enc_frame_ns = 0;
    w->audio_samples_submitted = 0; w->audio_base_offset_us = 0; w->audio_base_set = false;
    w->encoded_frame_number = w->frame_number;

    /* 1. H.264 hardware encoder configured at selected aspect dimensions */
    w->codec = AMediaCodec_createEncoderByType("video/avc");
    AMediaFormat *vfmt = AMediaFormat_new();
    if (!w->codec || !vfmt) { if (vfmt) AMediaFormat_delete(vfmt); return false; }
    AMediaFormat_setString(vfmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(vfmt, AMEDIAFORMAT_KEY_WIDTH, w->enc_width);
    AMediaFormat_setInt32(vfmt, AMEDIAFORMAT_KEY_HEIGHT, w->enc_height);
    AMediaFormat_setInt32(vfmt, AMEDIAFORMAT_KEY_BIT_RATE, DS_VIDEO_BITRATE);
    AMediaFormat_setInt32(vfmt, AMEDIAFORMAT_KEY_FRAME_RATE, DS_VIDEO_FPS);
    AMediaFormat_setInt32(vfmt, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
    AMediaFormat_setInt32(vfmt, "max-bframes", 0);
    AMediaFormat_setInt32(vfmt, AMEDIAFORMAT_KEY_COLOR_STANDARD, 1); /* BT709 */
    AMediaFormat_setInt32(vfmt, AMEDIAFORMAT_KEY_COLOR_TRANSFER, 3); /* SDR */
    AMediaFormat_setInt32(vfmt, AMEDIAFORMAT_KEY_COLOR_RANGE, 2); /* Limited */
    AMediaFormat_setInt32(vfmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, DS_COLOR_FORMAT_SURFACE);
    bool v_ok = media_ok(w, AMediaCodec_configure(w->codec, vfmt, NULL, NULL, AMEDIACODEC_CONFIGURE_FLAG_ENCODE), "configure AVC encoder");
    AMediaFormat_delete(vfmt);
    if (!v_ok || !media_ok(w, AMediaCodec_createInputSurface(w->codec, &w->encoder_window), "create encoder input surface") ||
        !w->encoder_window || !media_ok(w, AMediaCodec_start(w->codec), "start AVC encoder")) return false;
    w->codec_started = true;

    if (!init_encoder_gl(w)) return false;

    /* 2. AAC-LC audio encoder */
    w->audio_codec = AMediaCodec_createEncoderByType("audio/mp4a-latm");
    AMediaFormat *afmt = AMediaFormat_new();
    if (!w->audio_codec || !afmt) { if (afmt) AMediaFormat_delete(afmt); return false; }
    AMediaFormat_setString(afmt, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
    AMediaFormat_setInt32(afmt, AMEDIAFORMAT_KEY_SAMPLE_RATE, DS_AUDIO_SAMPLE_RATE);
    AMediaFormat_setInt32(afmt, AMEDIAFORMAT_KEY_CHANNEL_COUNT, 1);
    AMediaFormat_setInt32(afmt, AMEDIAFORMAT_KEY_BIT_RATE, DS_AUDIO_BITRATE);
    AMediaFormat_setInt32(afmt, AMEDIAFORMAT_KEY_AAC_PROFILE, 2); /* AAC-LC */
    AMediaFormat_setInt32(afmt, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, 8192);
    bool a_ok = media_ok(w, AMediaCodec_configure(w->audio_codec, afmt, NULL, NULL, AMEDIACODEC_CONFIGURE_FLAG_ENCODE), "configure AAC encoder");
    AMediaFormat_delete(afmt);
    if (!a_ok || !media_ok(w, AMediaCodec_start(w->audio_codec), "start AAC encoder")) return false;
    w->audio_started = true;

    /* 3. MP4 output file & muxer (orientation 0 because GL rotates frames upright) */
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
    return w->muxer && media_ok(w, AMediaMuxer_setOrientationHint(w->muxer, 0), "set MP4 orientation 0");
}

static bool write_muxer_sample(DSVideoWorker *w, const uint8_t *data, size_t size,
                               int64_t pts_us, uint32_t flags, bool is_audio) {
    AMediaCodecBufferInfo info = { .offset = 0, .size = (int32_t)size, .presentationTimeUs = pts_us, .flags = flags };
    if (info.presentationTimeUs < 0) info.presentationTimeUs = 0;
    if (is_audio) {
        if (w->first_audio_pts < 0) w->first_audio_pts = info.presentationTimeUs;
        info.presentationTimeUs -= w->first_audio_pts;
        if (w->last_audio_pts >= 0 && info.presentationTimeUs <= w->last_audio_pts) {
            info.presentationTimeUs = w->last_audio_pts + 1;
        }
        w->last_audio_pts = info.presentationTimeUs;
        return media_ok(w, AMediaMuxer_writeSampleData(w->muxer, (size_t)w->audio_track, data, &info), "write audio sample");
    } else {
        if (w->first_video_pts < 0) w->first_video_pts = info.presentationTimeUs;
        info.presentationTimeUs -= w->first_video_pts;
        if (w->last_video_pts >= 0 && info.presentationTimeUs <= w->last_video_pts) {
            info.presentationTimeUs = w->last_video_pts + 1;
        }
        w->last_video_pts = info.presentationTimeUs;
        const bool ok = media_ok(w, AMediaMuxer_writeSampleData(w->muxer, (size_t)w->video_track, data, &info), "write video sample");
        if (ok) w->saw_video_frame = true;
        return ok;
    }
}

static bool maybe_start_muxer_and_flush(DSVideoWorker *w) {
    if (w->muxer_started || w->video_track < 0 || w->audio_track < 0) return true;
    if (!media_ok(w, AMediaMuxer_start(w->muxer), "start A/V MP4 muxer")) return false;
    w->muxer_started = true;
    for (size_t i = 0; i < w->pending_count; ++i) {
        DSPendingSample *s = &w->pending[i];
        const bool ok = write_muxer_sample(w, s->data, s->size, s->pts_us, s->flags, s->is_audio);
        free(s->data);
        s->data = NULL;
        if (!ok) {
            for (size_t j = i + 1; j < w->pending_count; ++j) { free(w->pending[j].data); w->pending[j].data = NULL; }
            w->pending_count = 0;
            return false;
        }
    }
    w->pending_count = 0;
    return true;
}

static void queue_pending_sample(DSVideoWorker *w, const uint8_t *buf, size_t size,
                                 int64_t pts_us, uint32_t flags, bool is_audio) {
    if (w->pending_count >= DS_MAX_PENDING_SAMPLES) {
        /* Drop oldest sample only if queue saturates before both formats settle */
        free(w->pending[0].data);
        memmove(&w->pending[0], &w->pending[1], (DS_MAX_PENDING_SAMPLES - 1U) * sizeof(DSPendingSample));
        --w->pending_count;
    }
    uint8_t *copy = malloc(size);
    if (!copy) return;
    memcpy(copy, buf, size);
    w->pending[w->pending_count++] = (DSPendingSample){
        .data = copy, .size = size, .pts_us = pts_us, .flags = flags, .is_audio = is_audio
    };
}

static bool pump_codec_output(DSVideoWorker *w, bool is_audio, int64_t timeout_us) {
    AMediaCodec *codec = is_audio ? w->audio_codec : w->codec;
    if (!codec) return true;
    AMediaCodecBufferInfo info = {0};
    const ssize_t index = AMediaCodec_dequeueOutputBuffer(codec, &info, timeout_us);
    if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        AMediaFormat *fmt = AMediaCodec_getOutputFormat(codec);
        if (!fmt) return false;
        if (is_audio) {
            if (w->audio_track < 0) w->audio_track = AMediaMuxer_addTrack(w->muxer, fmt);
        } else {
            if (w->video_track < 0) w->video_track = AMediaMuxer_addTrack(w->muxer, fmt);
        }
        AMediaFormat_delete(fmt);
        if ((is_audio && w->audio_track < 0) || (!is_audio && w->video_track < 0)) {
            fail(w, "AMediaMuxer_addTrack failed"); return false;
        }
        return maybe_start_muxer_and_flush(w);
    }
    if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER || index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) return true;
    if (index < 0) { fail(w, is_audio ? "AAC dequeueOutput failed" : "AVC dequeueOutput failed"); return false; }

    bool okay = info.size >= 0;
    if ((info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0U) {
        if (is_audio) w->saw_audio_eos = true;
        else w->saw_video_eos = true;
    }
    if (okay && info.size > 0 && (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) == 0U) {
        size_t cap = 0;
        uint8_t *buf = AMediaCodec_getOutputBuffer(codec, (size_t)index, &cap);
        okay = buf && info.offset >= 0 && (size_t)info.offset <= cap &&
               (size_t)info.size <= cap - (size_t)info.offset && info.presentationTimeUs >= 0;
        if (okay) {
            const uint8_t *payload = buf + info.offset;
            if (w->muxer_started) {
                okay = write_muxer_sample(w, payload, (size_t)info.size, info.presentationTimeUs, info.flags, is_audio);
            } else {
                queue_pending_sample(w, payload, (size_t)info.size, info.presentationTimeUs, info.flags, is_audio);
            }
        }
    }
    if (!media_ok(w, AMediaCodec_releaseOutputBuffer(codec, (size_t)index, false), "release encoder output")) okay = false;
    return okay;
}

static void feed_audio_encoder(DSVideoWorker *w, bool eos) {
    if (!w->audio_codec || !w->audio_started || w->audio_eos_sent) return;
    for (int iter = 0; iter < 4; ++iter) {
        int16_t chunk[1024];
        size_t take = 0;
        pthread_mutex_lock(&w->mutex);
        const size_t avail = w->audio_count;
        const bool need_prime = (w->audio_track < 0 && avail == 0 && w->audio_samples_submitted == 0);
        if (avail > 0) {
            take = avail > 1024U ? 1024U : avail;
            for (size_t i = 0; i < take; ++i) {
                chunk[i] = w->audio_ring[w->audio_head];
                w->audio_head = (w->audio_head + 1U) % DS_AUDIO_RING_CAP;
            }
            w->audio_count -= take;
        }
        pthread_mutex_unlock(&w->mutex);

        if (take == 0 && !eos && !need_prime) break;
        const ssize_t in_idx = AMediaCodec_dequeueInputBuffer(w->audio_codec, 0);
        if (in_idx < 0) break;

        size_t cap = 0;
        uint8_t *dst = AMediaCodec_getInputBuffer(w->audio_codec, (size_t)in_idx, &cap);
        if (!dst) {
            AMediaCodec_queueInputBuffer(w->audio_codec, (size_t)in_idx, 0, 0, 0, 0);
            break;
        }
        if (take > 0) {
            const size_t max_samp = cap / sizeof(int16_t);
            if (take > max_samp) take = max_samp;
            memcpy(dst, chunk, take * sizeof(int16_t));
            if (!w->audio_base_set && w->rec_start_ns > 0) {
                const int64_t diff_us = (clock_ns(CLOCK_MONOTONIC) - w->rec_start_ns) / 1000LL;
                w->audio_base_offset_us = diff_us > 0 ? diff_us : 0;
                w->audio_base_set = true;
            }
            const int64_t pts_us = w->audio_base_offset_us +
                (int64_t)(w->audio_samples_submitted * INT64_C(1000000) / DS_AUDIO_SAMPLE_RATE);
            w->audio_samples_submitted += take;
            AMediaCodec_queueInputBuffer(w->audio_codec, (size_t)in_idx, 0, take * sizeof(int16_t), (uint64_t)pts_us, 0);
        } else if (need_prime) {
            const size_t prime_samp = cap >= 2048U ? 1024U : (cap / sizeof(int16_t));
            memset(dst, 0, prime_samp * sizeof(int16_t));
            const int64_t pts_us = (int64_t)(w->audio_samples_submitted * INT64_C(1000000) / DS_AUDIO_SAMPLE_RATE);
            w->audio_samples_submitted += prime_samp;
            AMediaCodec_queueInputBuffer(w->audio_codec, (size_t)in_idx, 0, prime_samp * sizeof(int16_t), (uint64_t)pts_us, 0);
            break;
        } else if (eos) {
            const int64_t pts_us = w->audio_base_offset_us +
                (int64_t)(w->audio_samples_submitted * INT64_C(1000000) / DS_AUDIO_SAMPLE_RATE);
            AMediaCodec_queueInputBuffer(w->audio_codec, (size_t)in_idx, 0, 0, (uint64_t)pts_us, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
            w->audio_eos_sent = true;
            break;
        }
    }
}

static void render_encoder_frame(DSVideoWorker *w) {
    if (!w->egl_surface || w->egl_context == EGL_NO_CONTEXT) return;
    uint8_t local_copy_check = 0;
    (void)local_copy_check;

    pthread_mutex_lock(&w->mutex);
    const bool have_new = (w->frame_width > 0 && w->frame_height > 0 && w->frame_number != w->encoded_frame_number);
    const int fw = w->frame_width, fh = w->frame_height;
    const uint64_t fnum = w->frame_number;
    const DSVideoHudState hud = w->hud;
    if (have_new) {
        if (!eglMakeCurrent(w->egl_display, w->egl_surface, w->egl_surface, w->egl_context)) {
            pthread_mutex_unlock(&w->mutex);
            return;
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, w->gl_cam_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fw, fh, 0, GL_RGBA, GL_UNSIGNED_BYTE, w->frame);
        w->encoded_frame_number = fnum;
    }
    pthread_mutex_unlock(&w->mutex);
    if (!have_new) return;

    const int64_t now_ns = clock_ns(CLOCK_MONOTONIC);
    if (w->rec_start_ns == 0) w->rec_start_ns = now_ns;
    int64_t pts_ns = now_ns - w->rec_start_ns;
    if (pts_ns <= w->last_enc_frame_ns) pts_ns = w->last_enc_frame_ns + INT64_C(1000000);
    w->last_enc_frame_ns = pts_ns;

    const int rotation = atomic_load(&w->capture->preview_orientation);
    const float zoom = ds_video_get_zoom(w->capture);
    float uv[12];
    if (!ds_preview_uv_zoom(fw, fh, rotation >= 0 ? rotation : 0, (float)w->enc_width, (float)w->enc_height, zoom, w->current_front, uv)) return;

    glViewport(0, 0, w->enc_width, w->enc_height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    const GLfloat vertices[] = {-1,1, 1,1, -1,-1, -1,-1, 1,1, 1,-1};
    glUseProgram(w->gl_cam_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, w->gl_cam_tex);
    glUniform1i(w->gl_cam_img_loc, 0);
    glUniform1i(w->gl_cam_filter_loc, atomic_load(&w->capture->filter));
    if (w->gl_cam_anomaly_loc >= 0) glUniform1f(w->gl_cam_anomaly_loc, hud.anomaly_fx);
    if (w->gl_cam_time_loc >= 0) glUniform1f(w->gl_cam_time_loc, (float)(hud.time_ms % 60000ULL) * 0.001f);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    if (hud.burn_in && w->hud_rgba) {
        rasterize_hud_overlay(w, &hud, pts_ns / 1000LL);
        const GLfloat hud_verts[] = {-0.96f,-0.78f, 0.96f,-0.78f, -0.96f,-0.96f,
                                     -0.96f,-0.96f, 0.96f,-0.78f, 0.96f,-0.96f};
        const GLfloat hud_uv[] = {0,0, 1,0, 0,1, 0,1, 1,0, 1,1};
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(w->gl_hud_prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, w->gl_hud_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, DS_HUD_TEX_W, DS_HUD_TEX_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, w->hud_rgba);
        glUniform1i(w->gl_hud_img_loc, 0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, hud_verts);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, hud_uv);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glDisable(GL_BLEND);
    }
    glDisableVertexAttribArray(1);

    if (w->egl_presentation_time) {
        w->egl_presentation_time(w->egl_display, w->egl_surface, (EGLnsecsANDROID)pts_ns);
    }
    eglSwapBuffers(w->egl_display, w->egl_surface);
}

static bool close_encoder(DSVideoWorker *w, bool finalize) {
    bool okay = false;
    if (finalize && w->codec_started && w->audio_started) {
        okay = media_ok(w, AMediaCodec_signalEndOfInputStream(w->codec), "signal video encoder EOS");
        feed_audio_encoder(w, true);
        const int64_t deadline = clock_ns(CLOCK_MONOTONIC) + INT64_C(3000000000);
        while (okay && (!w->saw_video_eos || !w->saw_audio_eos) && clock_ns(CLOCK_MONOTONIC) < deadline) {
            if (!w->audio_eos_sent) feed_audio_encoder(w, true);
            if (!w->saw_video_eos) okay = pump_codec_output(w, false, 5000);
            if (okay && !w->saw_audio_eos) okay = pump_codec_output(w, true, 5000);
        }
        okay = okay && w->saw_video_eos && w->saw_video_frame && w->muxer_started;
    }
    for (size_t i = 0; i < w->pending_count; ++i) {
        free(w->pending[i].data);
        w->pending[i].data = NULL;
    }
    w->pending_count = 0;
    destroy_encoder_gl(w);
    if (w->muxer) {
        if (w->muxer_started && !media_ok(w, AMediaMuxer_stop(w->muxer), "finalize MP4")) okay = false;
        AMediaMuxer_delete(w->muxer); w->muxer = NULL;
    }
    if (w->codec) {
        if (w->codec_started) (void)AMediaCodec_stop(w->codec);
        AMediaCodec_delete(w->codec); w->codec = NULL;
    }
    if (w->audio_codec) {
        if (w->audio_started) (void)AMediaCodec_stop(w->audio_codec);
        AMediaCodec_delete(w->audio_codec); w->audio_codec = NULL;
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
        } else {
            __android_log_print(ANDROID_LOG_INFO, DS_VIDEO_LOG, "muxed A/V %dx%d MP4 finalized: %s",
                                w->enc_width, w->enc_height, w->video_path);
        }
    }
    pthread_mutex_lock(&w->mutex);
    w->audio_head = w->audio_tail = w->audio_count = 0;
    pthread_mutex_unlock(&w->mutex);
    w->video_path[0] = '\0';
    w->codec_started = w->audio_started = w->muxer_started = false;
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
    w->get_dataspace = (media_status_t (*)(const AImage *, int32_t *))dlsym(RTLD_DEFAULT, "AImage_getDataSpace");
    char id[128] = {0};
    if (!w->manager || !select_camera(w, id)) goto fatal;
    ACameraDevice_StateCallbacks callbacks = { .context = w, .onDisconnected = camera_disconnected, .onError = camera_error };
    if (!camera_ok(w, ACameraManager_openCamera(w->manager, id, &callbacks, &w->device), "open rear camera (requires CAMERA permission)") || !w->device ||
        !media_ok(w, AImageReader_new(w->preview_width, w->preview_height, AIMAGE_FORMAT_YUV_420_888, 3, &w->reader), "create YUV reader") || !w->reader ||
        !media_ok(w, AImageReader_getWindow(w->reader, &w->preview_window), "get YUV surface") || !w->preview_window ||
        !create_session(w)) goto fatal;
    while (atomic_load(&capture->preview_running)) {
        if (device_failed(w)) goto fatal;
        pthread_mutex_lock(&w->mutex);
        const bool wanted = w->record_requested;
        pthread_mutex_unlock(&w->mutex);
        if (wanted && !recording) {
            if (!open_encoder(w)) {
                fail(w, "cannot start filtered A/V recording");
                close_encoder(w, false);
                recording_finished(w);
            } else {
                recording = true;
            }
        } else if (!wanted && recording) {
            if (!close_encoder(w, true)) fail(w, "video recording did not finalize successfully");
            recording = false;
            recording_finished(w);
        } else if (!wanted && !recording && atomic_load(&capture->active)) {
            recording_finished(w);
        }
        if (!preview_frame(w)) goto fatal;
        if (clock_ns(CLOCK_MONOTONIC) > w->preview_deadline) { fail(w, "camera stopped delivering fresh preview frames"); goto fatal; }
        {
            const bool want_torch = atomic_load(&capture->torch_available) && atomic_load(&capture->torch_enabled);
            if (want_torch != w->current_torch && w->session && w->request) {
                configure_camera_torch(w);
                const camera_status_t ts = ACameraCaptureSession_setRepeatingRequest(w->session, NULL, 1, &w->request, NULL);
                if (ts != ACAMERA_OK) __android_log_print(ANDROID_LOG_WARN, DS_VIDEO_LOG, "torch repeating request failed (%d)", (int)ts);
            }
            if (!recording && !wanted && atomic_load(&capture->front_camera) != w->current_front) {
                if (!close_session(w)) goto fatal;
                if (w->device) { ACameraDevice_close(w->device); w->device = NULL; }
                if (w->reader) { AImageReader_delete(w->reader); w->reader = NULL; w->preview_window = NULL; }
                pthread_mutex_lock(&w->mutex);
                w->frame_width = 0; w->frame_height = 0;
                w->frame_received = 0; w->last_camera_timestamp = 0;
                w->color_logged = false;
                pthread_mutex_unlock(&w->mutex);
                if (!select_camera(w, id) ||
                    !camera_ok(w, ACameraManager_openCamera(w->manager, id, &callbacks, &w->device), "reopen camera on lens switch") ||
                    !w->device ||
                    !media_ok(w, AImageReader_new(w->preview_width, w->preview_height, AIMAGE_FORMAT_YUV_420_888, 3, &w->reader), "recreate YUV reader") ||
                    !w->reader ||
                    !media_ok(w, AImageReader_getWindow(w->reader, &w->preview_window), "get YUV surface") ||
                    !w->preview_window || !create_session(w)) goto fatal;
            }
        }
        if (recording) {
            render_encoder_frame(w);
            feed_audio_encoder(w, false);
            for (int n = 0; n < 4; ++n) {
                if (!pump_codec_output(w, false, 0)) goto fatal;
                if (!pump_codec_output(w, true, 0)) goto fatal;
            }
        }
        const int64_t deadline = clock_ns(CLOCK_MONOTONIC) + INT64_C(8000000);
        const struct timespec until = { .tv_sec = (time_t)(deadline / INT64_C(1000000000)), .tv_nsec = (long)(deadline % INT64_C(1000000000)) };
        pthread_mutex_lock(&w->mutex);
        if (atomic_load(&capture->preview_running) && w->record_requested == recording && !w->camera_error)
            pthread_cond_timedwait(&w->condition, &w->mutex, &until);
        pthread_mutex_unlock(&w->mutex);
    }
    {
        const bool closed = close_session(w);
        if (w->codec || w->audio_codec) close_encoder(w, recording && closed && !device_failed(w));
    }
    goto cleanup;
fatal:
    fail(w, "native camera pipeline failed; preview stopped");
    close_session(w);
    if (w->codec || w->audio_codec) close_encoder(w, false);
cleanup:
    if (w->device) { ACameraDevice_close(w->device); w->device = NULL; }
    if (w->reader) { AImageReader_delete(w->reader); w->reader = NULL; }
    if (w->metadata) { ACameraMetadata_free(w->metadata); w->metadata = NULL; }
    if (w->manager) { ACameraManager_delete(w->manager); w->manager = NULL; }
    atomic_store(&capture->preview_ready, false); atomic_store(&capture->preview_running, false);
    atomic_store(&capture->torch_enabled, false); atomic_store(&capture->torch_available, false);
    pthread_mutex_lock(&w->mutex);
    w->worker_finished = true; w->record_requested = false; atomic_store(&capture->active, false);
    pthread_cond_broadcast(&w->condition);
    pthread_mutex_unlock(&w->mutex);
    return NULL;
}

static DSVideoWorker *acquire_worker(DSVideoCapture *capture) {
    if (!capture || !atomic_load(&capture->worker_accepting)) return NULL;
    atomic_fetch_add(&capture->worker_users, 1);
    if (!atomic_load(&capture->worker_accepting) || !capture->implementation) {
        atomic_fetch_sub(&capture->worker_users, 1);
        return NULL;
    }
    return capture->implementation;
}

static void release_worker(DSVideoCapture *capture) {
    atomic_fetch_sub(&capture->worker_users, 1);
}

void ds_video_preview_start(DSVideoCapture *capture, DSVideoFilter filter, int display_rotation) {
    if (!capture) return;
    atomic_store(&capture->filter, (int)filter);
    if (capture->thread_created && atomic_load(&capture->preview_running) && capture->display_rotation == display_rotation) return;
    ds_video_preview_stop(capture);
    atomic_store(&capture->failed, false); atomic_store(&capture->preview_orientation, -1);
    if (filter < DS_VIDEO_FILTER_NIGHT || filter > DS_VIDEO_FILTER_RAW || display_rotation < 0 || display_rotation > 3) {
        atomic_store(&capture->failed, true); return;
    }
    DSVideoWorker *w = calloc(1, sizeof(*w));
    if (!w) { atomic_store(&capture->failed, true); return; }
    w->capture = capture; w->fd = -1;
    w->egl_display = EGL_NO_DISPLAY;
    w->egl_context = EGL_NO_CONTEXT;
    w->egl_surface = EGL_NO_SURFACE;
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
    capture->display_rotation = display_rotation; capture->implementation = w;
    atomic_store(&capture->preview_running, true);
    atomic_store(&capture->worker_accepting, true);
    if (pthread_create(&capture->thread, NULL, video_thread, w) != 0) {
        atomic_store(&capture->worker_accepting, false);
        atomic_store(&capture->preview_running, false); atomic_store(&capture->failed, true);
        pthread_cond_destroy(&w->condition); pthread_mutex_destroy(&w->mutex);
        free(w->frame); free(w->scratch); free(w); capture->implementation = NULL; return;
    }
    capture->thread_created = true;
}

void ds_video_set_filter(DSVideoCapture *capture, DSVideoFilter filter) {
    if (!capture || filter < DS_VIDEO_FILTER_NIGHT || filter > DS_VIDEO_FILTER_RAW) return;
    atomic_store(&capture->filter, (int)filter);
}

void ds_video_set_hud(DSVideoCapture *capture, const DSVideoHudState *hud) {
    if (!capture || !hud) return;
    DSVideoWorker *w = acquire_worker(capture);
    if (!w) return;
    if (pthread_mutex_trylock(&w->mutex) == 0) {
        w->hud = *hud;
        pthread_mutex_unlock(&w->mutex);
    }
    release_worker(capture);
}

void ds_video_preview_stop(DSVideoCapture *capture) {
    if (!capture) return;
    atomic_store(&capture->worker_accepting, false);
    while (atomic_load(&capture->worker_users) > 0) {
        nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 100000L}, NULL);
    }
    DSVideoWorker *w = capture->implementation;
    atomic_store(&capture->preview_ready, false); atomic_store(&capture->preview_running, false);
    atomic_store(&capture->torch_enabled, false); atomic_store(&capture->torch_available, false);
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

void ds_video_start(DSVideoCapture *capture, const char *evidence_dir, DSVideoFilter filter,
                    DSVideoAspect aspect, int display_rotation) {
    if (!capture || ds_video_active(capture)) return;
    if (!evidence_dir || !evidence_dir[0] || strlen(evidence_dir) >= sizeof(capture->evidence_dir)) {
        atomic_store(&capture->failed, true); return;
    }
    capture->aspect = aspect;
    ds_video_preview_start(capture, filter, display_rotation);
    DSVideoWorker *w = capture->implementation;
    if (!w || !atomic_load(&capture->preview_running)) return;
    pthread_mutex_lock(&w->mutex);
    if (!w->worker_finished && !w->camera_error) {
        snprintf(capture->evidence_dir, sizeof(capture->evidence_dir), "%s", evidence_dir);
        w->audio_head = w->audio_tail = w->audio_count = 0;
        w->record_requested = true; atomic_store(&capture->active, true); pthread_cond_broadcast(&w->condition);
    }
    pthread_mutex_unlock(&w->mutex);
}

void ds_video_push_audio(DSVideoCapture *capture, const int16_t *pcm, size_t samples, uint32_t sample_rate) {
    if (!capture || !pcm || samples == 0 || sample_rate == 0) return;
    if (!atomic_load(&capture->active)) return;
    DSVideoWorker *w = acquire_worker(capture);
    if (!w) return;
    pthread_mutex_lock(&w->mutex);
    if (w->record_requested) {
        if (sample_rate == DS_AUDIO_SAMPLE_RATE) {
            for (size_t i = 0; i < samples; ++i) {
                if (w->audio_count >= DS_AUDIO_RING_CAP) {
                    w->audio_head = (w->audio_head + 1U) % DS_AUDIO_RING_CAP;
                    --w->audio_count;
                }
                w->audio_ring[w->audio_tail] = pcm[i];
                w->audio_tail = (w->audio_tail + 1U) % DS_AUDIO_RING_CAP;
                ++w->audio_count;
            }
        } else {
            const size_t out_samples = (size_t)(((uint64_t)samples * DS_AUDIO_SAMPLE_RATE) / sample_rate);
            for (size_t i = 0; i < out_samples; ++i) {
                const size_t src = (size_t)(((uint64_t)i * sample_rate) / DS_AUDIO_SAMPLE_RATE);
                const int16_t val = pcm[src < samples ? src : samples - 1U];
                if (w->audio_count >= DS_AUDIO_RING_CAP) {
                    w->audio_head = (w->audio_head + 1U) % DS_AUDIO_RING_CAP;
                    --w->audio_count;
                }
                w->audio_ring[w->audio_tail] = val;
                w->audio_tail = (w->audio_tail + 1U) % DS_AUDIO_RING_CAP;
                ++w->audio_count;
            }
        }
    }
    pthread_mutex_unlock(&w->mutex);
    release_worker(capture);
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

void ds_video_set_torch(DSVideoCapture *capture, bool enabled) {
    if (!capture) return;
    atomic_store(&capture->torch_enabled, enabled && atomic_load(&capture->torch_available));
    DSVideoWorker *w = acquire_worker(capture);
    if (w) {
        pthread_mutex_lock(&w->mutex);
        pthread_cond_broadcast(&w->condition);
        pthread_mutex_unlock(&w->mutex);
        release_worker(capture);
    }
}

bool ds_video_torch_available(const DSVideoCapture *capture) { return capture && atomic_load(&capture->torch_available); }
bool ds_video_torch_enabled(const DSVideoCapture *capture) { return capture && atomic_load(&capture->torch_enabled); }

void ds_video_set_front(DSVideoCapture *capture, bool front) {
    if (!capture || ds_video_active(capture)) return;
    if (front && capture->thread_created && !atomic_load(&capture->front_available)) return;
    atomic_store(&capture->front_camera, front);
    DSVideoWorker *w = acquire_worker(capture);
    if (w) {
        pthread_mutex_lock(&w->mutex);
        pthread_cond_broadcast(&w->condition);
        pthread_mutex_unlock(&w->mutex);
        release_worker(capture);
    }
}

bool ds_video_is_front(const DSVideoCapture *capture) { return capture && atomic_load(&capture->front_camera); }
bool ds_video_front_available(const DSVideoCapture *capture) { return capture && atomic_load(&capture->front_available); }

void ds_video_set_zoom(DSVideoCapture *capture, float zoom) {
    if (!capture) return;
    if (!isfinite(zoom) || zoom < 1.0f) zoom = 1.0f;
    if (zoom > 4.0f) zoom = 4.0f;
    atomic_store(&capture->zoom_milli, (uint32_t)lroundf(zoom * 1000.0f));
}

float ds_video_get_zoom(const DSVideoCapture *capture) {
    if (!capture) return 1.0f;
    const uint32_t milli = atomic_load(&capture->zoom_milli);
    return milli > 0U ? (float)milli / 1000.0f : 1.0f;
}

int ds_video_preview_orientation(const DSVideoCapture *capture) { return capture ? atomic_load(&capture->preview_orientation) : -1; }
bool ds_video_active(const DSVideoCapture *capture) { return capture && atomic_load(&capture->active); }
bool ds_video_failed(const DSVideoCapture *capture) { return capture && atomic_load(&capture->failed); }
