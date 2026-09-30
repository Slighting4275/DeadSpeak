#include <android/asset_manager.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/sensor.h>
#include <aaudio/AAudio.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

#include "engine.h"
#include "explore.h"
#include "jni_scope.h"
#include "video_capture.h"
#include "ui_layout.h"
#include "sensor_state.h"

#define LOG_TAG "DeadSpeak"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOOPER_ID_SENSOR 3
#define DS_LOG_CAPACITY 32
#define DS_AUDIO_RATE 48000
#define DS_EXPLORE_ROW_HEIGHT 62.0f
#define DS_EXPLORE_ROW_SOURCE 27.0f
#define DS_SENSOR_COUNT 3
static const int sensor_types[DS_SENSOR_COUNT] = {
    ASENSOR_TYPE_MAGNETIC_FIELD, ASENSOR_TYPE_ACCELEROMETER, ASENSOR_TYPE_GYROSCOPE
};
static const char *const sensor_names[DS_SENSOR_COUNT] = {"MAGNETIC", "ACCEL", "GYRO"};

typedef struct {
    struct android_app *app;
    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
    int width;
    int height;
    int surface_width;
    int surface_height;
    float ui_scale;
    int display_rotation;
    DSLayout ui;
    float scroll[4];
    float touch_y, touch_start_y, touch_start_x;
    bool touch_active, touch_dragged, body_drawing;
    GLuint program;
    GLint position;
    GLint color;
    ASensorManager *sensor_manager;
    const ASensor *sensors[DS_SENSOR_COUNT];
    ASensorEventQueue *sensor_queue;
    DSSensorReading readings[DS_SENSOR_COUNT];
    bool sensors_resumed;
    uint64_t next_sensor_access_check;
    int64_t last_magnetic_log_ns;
    float magnetic_field;
    float smoothed_field;
    float signal;
    uint32_t sample_count;
    char word[32];
    bool scanning;
    bool capture_requested;
    bool permission_requested;
    uint64_t permission_deadline;
    bool audio_thread_created;
    pthread_t audio_thread;
    _Atomic bool audio_active;
    _Atomic bool audio_failed;
    _Atomic uint32_t audio_level_milli;
    _Atomic uint32_t spectrum_milli[3];
    DSVideoCapture video;
    DSVideoFilter video_filter;
    DSExplore explore;
    char explore_cache_path[512];
    char explore_query[48];
    size_t explore_page;
    bool explore_search_active;
    char evidence_dir[512];
    char word_log_path[512];
    char words[DS_LOG_CAPACITY][32];
    unsigned word_samples[DS_LOG_CAPACITY];
    float word_fields[DS_LOG_CAPACITY];
    char media_files[8][96];
    time_t media_mtimes[8];
    unsigned media_count;
    uint64_t next_media_refresh;
    uint8_t *dictionary;
    size_t dictionary_size;
    unsigned log_count;
    unsigned log_next;
    int tab;
} DeadSpeak;

static void start_sensors(DeadSpeak *state);
static void stop_sensors(DeadSpeak *state, DSSensorStatus status);
static void maintain_sensors(DeadSpeak *state);
static int64_t sensor_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
}

static void append_word_log(DeadSpeak *state, const char *word) {
    if (!word || !word[0]) return;
    const unsigned slot = state->log_next;
    snprintf(state->words[slot], sizeof(state->words[slot]), "%s", word);
    state->word_samples[slot] = state->sample_count;
    state->word_fields[slot] = state->smoothed_field;
    state->log_next = (slot + 1U) % DS_LOG_CAPACITY;
    if (state->log_count < DS_LOG_CAPACITY) ++state->log_count;
    FILE *file = fopen(state->word_log_path, "a");
    if (file) {
        fprintf(file, "%u,%.2f,%s\n", state->sample_count, state->smoothed_field, word);
        fclose(file);
    }
}

static void load_word_log(DeadSpeak *state) {
    FILE *file = fopen(state->word_log_path, "r");
    if (!file) return;
    char row[128];
    while (fgets(row, sizeof(row), file)) {
        unsigned sample = 0;
        float field = 0.0f;
        char word[32] = {0};
        if (sscanf(row, "%u,%f,%31[A-Z0-9-]", &sample, &field, word) != 3) continue;
        const unsigned slot = state->log_next;
        snprintf(state->words[slot], sizeof(state->words[slot]), "%s", word);
        state->word_samples[slot] = sample;
        state->word_fields[slot] = field;
        state->log_next = (slot + 1U) % DS_LOG_CAPACITY;
        if (state->log_count < DS_LOG_CAPACITY) ++state->log_count;
    }
    fclose(file);
}

static void load_dictionary(DeadSpeak *state) {
    AAsset *asset = AAssetManager_open(state->app->activity->assetManager, "words.bin", AASSET_MODE_BUFFER);
    if (!asset) return;
    const off_t length = AAsset_getLength(asset);
    if (length >= 12 && (uint64_t)length <= SIZE_MAX) {
        uint8_t *data = (uint8_t *)malloc((size_t)length);
        if (data && AAsset_read(asset, data, (size_t)length) == length) {
            state->dictionary = data;
            state->dictionary_size = (size_t)length;
        } else {
            free(data);
        }
    }
    AAsset_close(asset);
}

static void refresh_media_files(DeadSpeak *state) {
    DIR *directory = opendir(state->evidence_dir);
    if (!directory) return;
    state->media_count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        const size_t name_length = strlen(entry->d_name);
        const bool is_wav = name_length > 4 && strcmp(entry->d_name + name_length - 4, ".wav") == 0;
        const bool is_mp4 = name_length > 4 && strcmp(entry->d_name + name_length - 4, ".mp4") == 0;
        if (!is_wav && !is_mp4) continue;
        char path[640];
        snprintf(path, sizeof(path), "%s/%s", state->evidence_dir, entry->d_name);
        struct stat file_status;
        if (stat(path, &file_status) != 0) continue;
        unsigned slot = state->media_count;
        if (slot == 8U && file_status.st_mtime <= state->media_mtimes[7]) continue;
        if (slot > 7U) slot = 7U;
        else ++state->media_count;
        while (slot > 0U && file_status.st_mtime > state->media_mtimes[slot - 1U]) {
            if (slot < 8U) {
                memcpy(state->media_files[slot], state->media_files[slot - 1U], sizeof(state->media_files[slot]));
                state->media_mtimes[slot] = state->media_mtimes[slot - 1U];
            }
            --slot;
        }
        snprintf(state->media_files[slot], sizeof(state->media_files[slot]), "%s", entry->d_name);
        state->media_mtimes[slot] = file_status.st_mtime;
    }
    closedir(directory);
}

static bool has_capture_permissions(DeadSpeak *state) {
    DSJniScope scope;
    if (!ds_jni_scope_begin(&scope, state->app->activity)) return false;
    JNIEnv *env = scope.env;
    jobject activity = state->app->activity->clazz;
    jclass activity_class = (*env)->GetObjectClass(env, activity);
    jint audio_result = -1, camera_result = -1;
    if (!activity_class) goto done;
    jmethodID check = (*env)->GetMethodID(env, activity_class, "checkSelfPermission", "(Ljava/lang/String;)I");
    if (!check) goto done;
    jstring audio_permission = (*env)->NewStringUTF(env, "android.permission.RECORD_AUDIO");
    if (!audio_permission) goto done;
    jstring camera_permission = (*env)->NewStringUTF(env, "android.permission.CAMERA");
    if (!camera_permission) goto done;
    audio_result = (*env)->CallIntMethod(env, activity, check, audio_permission);
    if ((*env)->ExceptionCheck(env)) goto done;
    camera_result = (*env)->CallIntMethod(env, activity, check, camera_permission);
done:
    return ds_jni_scope_end(&scope) && audio_result == 0 && camera_result == 0;
}

static void request_audio_permission(DeadSpeak *state) {
    if (state->permission_requested) return;
    DSJniScope scope;
    if (!ds_jni_scope_begin(&scope, state->app->activity)) return;
    JNIEnv *env = scope.env;
    jobject activity = state->app->activity->clazz;
    jclass activity_class = (*env)->GetObjectClass(env, activity);
    if (!activity_class) goto done;
    jmethodID request = (*env)->GetMethodID(env, activity_class, "requestPermissions", "([Ljava/lang/String;I)V");
    if (!request) goto done;
    jclass string_class = (*env)->FindClass(env, "java/lang/String");
    if (!string_class) goto done;
    jobjectArray permissions = (*env)->NewObjectArray(env, 2, string_class, NULL);
    if (!permissions) goto done;
    jstring audio_permission = (*env)->NewStringUTF(env, "android.permission.RECORD_AUDIO");
    if (!audio_permission) goto done;
    jstring camera_permission = (*env)->NewStringUTF(env, "android.permission.CAMERA");
    if (!camera_permission) goto done;
    (*env)->SetObjectArrayElement(env, permissions, 0, audio_permission);
    if ((*env)->ExceptionCheck(env)) goto done;
    (*env)->SetObjectArrayElement(env, permissions, 1, camera_permission);
    if ((*env)->ExceptionCheck(env)) goto done;
    (*env)->CallVoidMethod(env, activity, request, permissions, 6165);
done:
    state->permission_requested = ds_jni_scope_end(&scope);
}

static void write_wav_header(FILE *file, uint32_t data_bytes, uint32_t sample_rate) {
    const uint32_t riff_size = data_bytes + 36U;
    const uint16_t format = 1, channels = 1, bits = 16, block_align = 2;
    const uint32_t rate = sample_rate, byte_rate = sample_rate * 2U, fmt_size = 16;
    fseek(file, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, file); fwrite(&riff_size, 4, 1, file); fwrite("WAVE", 1, 4, file);
    fwrite("fmt ", 1, 4, file); fwrite(&fmt_size, 4, 1, file); fwrite(&format, 2, 1, file);
    fwrite(&channels, 2, 1, file); fwrite(&rate, 4, 1, file); fwrite(&byte_rate, 4, 1, file);
    fwrite(&block_align, 2, 1, file); fwrite(&bits, 2, 1, file);
    fwrite("data", 1, 4, file); fwrite(&data_bytes, 4, 1, file);
    fseek(file, 0, SEEK_END);
}

static void *audio_capture_thread(void *user_data) {
    DeadSpeak *state = (DeadSpeak *)user_data;
    AAudioStreamBuilder *builder = NULL;
    AAudioStream *stream = NULL;
    FILE *file = NULL;
    uint32_t bytes_written = 0;
    uint32_t sample_rate = 0;
    bool capture_ok = false;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || !builder) goto done;
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setSampleRate(builder, DS_AUDIO_RATE);
    AAudioStreamBuilder_setChannelCount(builder, 1);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setInputPreset(builder, AAUDIO_INPUT_PRESET_UNPROCESSED);
    if (AAudioStreamBuilder_openStream(builder, &stream) != AAUDIO_OK || !stream) goto done;
    AAudioStreamBuilder_delete(builder);
    builder = NULL;
    sample_rate = (uint32_t)AAudioStream_getSampleRate(stream);
    if (sample_rate == 0U || AAudioStream_getChannelCount(stream) != 1 || AAudioStream_getFormat(stream) != AAUDIO_FORMAT_PCM_I16) goto done;

    char path[640];
    const time_t now = time(NULL);
    struct tm local_time;
    localtime_r(&now, &local_time);
    char stamp[32];
    struct timespec wall_time;
    clock_gettime(CLOCK_REALTIME, &wall_time);
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local_time);
    snprintf(path, sizeof(path), "%s/EVP_%s_%03ld.wav", state->evidence_dir, stamp, wall_time.tv_nsec / 1000000L);
    file = fopen(path, "wb");
    if (!file) goto done;
    uint8_t empty_header[44] = {0};
    fwrite(empty_header, sizeof(empty_header), 1, file);
    if (AAudioStream_requestStart(stream) != AAUDIO_OK) {
        fclose(file);
        file = NULL;
        unlink(path);
        goto done;
    }
    capture_ok = true;

    int16_t samples[480];
    while (atomic_load(&state->audio_active)) {
        const aaudio_result_t frames = AAudioStream_read(stream, samples, 480, 100000000LL);
        if (frames < 0) break;
        if (frames == 0) continue;
        double squares = 0.0;
        for (aaudio_result_t i = 0; i < frames; ++i) {
            const double normalized = (double)samples[i] / 32768.0;
            squares += normalized * normalized;
        }
        const float rms = (float)sqrt(squares / (double)frames);
        const unsigned level = (unsigned)(rms * 1000.0f);
        atomic_store(&state->audio_level_milli, level > 1000U ? 1000U : level);
        float bands[3] = {0};
        ds_engine_spectrum(samples, (size_t)frames, sample_rate, bands);
        for (unsigned band = 0; band < 3; ++band) {
            const unsigned band_level = (unsigned)(bands[band] * 4000.0f);
            atomic_store(&state->spectrum_milli[band], band_level > 1000U ? 1000U : band_level);
        }
        const size_t written = fwrite(samples, sizeof(int16_t), (size_t)frames, file);
        bytes_written += (uint32_t)(written * sizeof(int16_t));
        if (written != (size_t)frames) break;
    }
    AAudioStream_requestStop(stream);
    write_wav_header(file, bytes_written, sample_rate);
    fflush(file);
    fsync(fileno(file));
    fclose(file);
    file = NULL;
    if (bytes_written == 0U) atomic_store(&state->audio_failed, true);

done:
    if (!capture_ok) atomic_store(&state->audio_failed, true);
    if (file) fclose(file);
    if (stream) AAudioStream_close(stream);
    if (builder) AAudioStreamBuilder_delete(builder);
    atomic_store(&state->audio_active, false);
    return NULL;
}

static void start_audio_capture(DeadSpeak *state) {
    if (state->audio_thread_created || atomic_load(&state->audio_active)) return;
    atomic_store(&state->audio_failed, false);
    atomic_store(&state->audio_level_milli, 0U);
    atomic_store(&state->audio_active, true);
    if (pthread_create(&state->audio_thread, NULL, audio_capture_thread, state) != 0) {
        atomic_store(&state->audio_active, false);
        atomic_store(&state->audio_failed, true);
        return;
    }
    state->audio_thread_created = true;
}

static void stop_audio_capture(DeadSpeak *state) {
    if (state->audio_thread_created) {
        atomic_store(&state->audio_active, false);
        pthread_join(state->audio_thread, NULL);
        state->audio_thread_created = false;
    }
}

static const char *vertex_shader_source =
    "attribute vec2 a_position;"
    "void main(){gl_Position=vec4(a_position,0.0,1.0);} ";

static const char *fragment_shader_source =
    "precision mediump float;"
    "uniform vec4 u_color;"
    "void main(){gl_FragColor=u_color;}";

static const uint8_t font[39][7] = {
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    {0x0F,0x10,0x10,0x10,0x10,0x10,0x0F}, {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    {0x0F,0x10,0x10,0x17,0x11,0x11,0x0F}, {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}, {0x07,0x02,0x02,0x02,0x12,0x12,0x0C},
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    {0x00,0x00,0x04,0x00,0x00,0x04,0x00}, {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x06,0x06},
};

static uint64_t now_millis(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
}

static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[512] = {0};
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        LOGE("shader compile failed: %s", log);
    }
    return shader;
}

static bool display_metrics(DeadSpeak *state, float *density, int *rotation) {
    DSJniScope scope;
    if (!ds_jni_scope_begin(&scope, state->app->activity)) return false;
    JNIEnv *env = scope.env;
    bool found = false;
    jobject activity = state->app->activity->clazz;
    jclass activity_class = (*env)->GetObjectClass(env, activity);
    if (!activity_class) goto done;
    jmethodID get_resources = (*env)->GetMethodID(env, activity_class, "getResources", "()Landroid/content/res/Resources;");
    if (!get_resources) goto done;
    jobject resources = (*env)->CallObjectMethod(env, activity, get_resources);
    if (!resources || (*env)->ExceptionCheck(env)) goto done;
    jclass resources_class = (*env)->GetObjectClass(env, resources);
    if (!resources_class) goto done;
    jmethodID get_metrics = (*env)->GetMethodID(env, resources_class, "getDisplayMetrics", "()Landroid/util/DisplayMetrics;");
    if (!get_metrics) goto done;
    jobject metrics = (*env)->CallObjectMethod(env, resources, get_metrics);
    if (!metrics || (*env)->ExceptionCheck(env)) goto done;
    jclass metrics_class = (*env)->GetObjectClass(env, metrics);
    if (!metrics_class) goto done;
    jfieldID density_field = (*env)->GetFieldID(env, metrics_class, "density", "F");
    if (!density_field) goto done;
    *density = (*env)->GetFloatField(env, metrics, density_field);
    if ((*env)->ExceptionCheck(env)) goto done;
    jmethodID get_display = (*env)->GetMethodID(env, activity_class, "getDisplay", "()Landroid/view/Display;");
    if (!get_display) goto done;
    jobject display = (*env)->CallObjectMethod(env, activity, get_display);
    if (!display || (*env)->ExceptionCheck(env)) goto done;
    jclass display_class = (*env)->GetObjectClass(env, display);
    if (!display_class) goto done;
    jmethodID get_rotation = (*env)->GetMethodID(env, display_class, "getRotation", "()I");
    if (!get_rotation) goto done;
    *rotation = (*env)->CallIntMethod(env, display, get_rotation);
    found = isfinite(*density) && *density > 0.0f && *rotation >= 0 && *rotation <= 3;
done:
    return ds_jni_scope_end(&scope) && found;
}

static bool update_display_geometry(DeadSpeak *state) {
    if (state->display == EGL_NO_DISPLAY || state->surface == EGL_NO_SURFACE) return false;
    EGLint width = 0, height = 0;
    if (!eglQuerySurface(state->display, state->surface, EGL_WIDTH, &width) ||
        !eglQuerySurface(state->display, state->surface, EGL_HEIGHT, &height) || width <= 0 || height <= 0) return false;
    float density = 0.0f;
    int rotation = 0;
    /* The Activity metrics track density overrides; native asset-manager
       configuration can retain the physical density after a live change. */
    if (!display_metrics(state, &density, &rotation)) { LOGE("Could not read display metrics"); return false; }
    state->surface_width = width;
    state->surface_height = height;
    state->ui_scale = density;
    state->display_rotation = rotation;
    state->width = (int)lroundf((float)width / state->ui_scale);
    state->height = (int)lroundf((float)height / state->ui_scale);
    const size_t old_page_size = state->ui.explore_page_size;
    state->ui = ds_layout((float)state->width, (float)state->height);
    if (old_page_size && old_page_size != state->ui.explore_page_size)
        state->explore_page = state->explore_page * old_page_size / state->ui.explore_page_size;
    if (state->touch_active) state->touch_dragged = true;
    state->touch_active = false;
    glViewport(0, 0, width, height);
    __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "Display %dx%d px; UI %dx%d; density %d", width, height, state->width, state->height, (int)lroundf(density * 160.0f));
    return true;
}

static bool setup_display(DeadSpeak *state) {
    const EGLint config_attributes[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_BLUE_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_RED_SIZE, 8, EGL_NONE
    };
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLint format, num_configs;
    EGLConfig config;
    state->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(state->display, NULL, NULL);
    eglChooseConfig(state->display, config_attributes, &config, 1, &num_configs);
    eglGetConfigAttrib(state->display, config, EGL_NATIVE_VISUAL_ID, &format);
    ANativeWindow_setBuffersGeometry(state->app->window, 0, 0, format);
    state->surface = eglCreateWindowSurface(state->display, config, state->app->window, NULL);
    state->context = eglCreateContext(state->display, config, EGL_NO_CONTEXT, context_attributes);
    if (eglMakeCurrent(state->display, state->surface, state->surface, state->context) == EGL_FALSE) return false;

    if (!update_display_geometry(state)) return false;
    GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_shader_source);
    GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_source);
    state->program = glCreateProgram();
    glAttachShader(state->program, vertex);
    glAttachShader(state->program, fragment);
    glBindAttribLocation(state->program, 0, "a_position");
    glLinkProgram(state->program);
    state->position = 0;
    state->color = glGetUniformLocation(state->program, "u_color");
    glUseProgram(state->program);
    return true;
}

static void teardown_display(DeadSpeak *state) {
    if (state->display != EGL_NO_DISPLAY) {
        eglMakeCurrent(state->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (state->context != EGL_NO_CONTEXT) eglDestroyContext(state->display, state->context);
        if (state->surface != EGL_NO_SURFACE) eglDestroySurface(state->display, state->surface);
        eglTerminate(state->display);
    }
    state->display = EGL_NO_DISPLAY;
    state->surface = EGL_NO_SURFACE;
    state->context = EGL_NO_CONTEXT;
}

static void emit_vertices(DeadSpeak *state, const GLfloat *vertices, size_t count, float r, float g, float b, float a) {
    glUseProgram(state->program);
    glUniform4f(state->color, r, g, b, a);
    glVertexAttribPointer(state->position, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glEnableVertexAttribArray(state->position);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)count);
}

static void rect(DeadSpeak *state, float x, float y, float w, float h, float r, float g, float b, float a) {
    if (state->body_drawing) y -= state->scroll[state->tab];
    const float x1 = (x / state->width) * 2.0f - 1.0f;
    const float x2 = ((x + w) / state->width) * 2.0f - 1.0f;
    const float y1 = 1.0f - (y / state->height) * 2.0f;
    const float y2 = 1.0f - ((y + h) / state->height) * 2.0f;
    const GLfloat vertices[] = {x1,y1, x2,y1, x1,y2, x1,y2, x2,y1, x2,y2};
    emit_vertices(state, vertices, 6, r, g, b, a);
}

static void line(DeadSpeak *state, float x1, float y1, float x2, float y2, float r, float g, float b, float a) {
    if (state->body_drawing) { y1 -= state->scroll[state->tab]; y2 -= state->scroll[state->tab]; }
    const GLfloat vertices[] = {
        (x1 / state->width) * 2.0f - 1.0f, 1.0f - (y1 / state->height) * 2.0f,
        (x2 / state->width) * 2.0f - 1.0f, 1.0f - (y2 / state->height) * 2.0f,
    };
    glUseProgram(state->program);
    glUniform4f(state->color, r, g, b, a);
    glLineWidth(2.0f);
    glVertexAttribPointer(state->position, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glEnableVertexAttribArray(state->position);
    glDrawArrays(GL_LINES, 0, 2);
}

static int glyph_index(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= '0' && c <= '9') return 26 + (c - '0');
    if (c == ':') return 36;
    if (c == '-') return 37;
    if (c == '.') return 38;
    return -1;
}

static void text(DeadSpeak *state, const char *value, float x, float y, float scale, float r, float g, float b, float a) {
    float cursor = x;
    for (size_t i = 0; value[i] != 0; ++i) {
        const char ch = value[i];
        if (ch == ' ') { cursor += 6.0f * scale; continue; }
        const int index = glyph_index(ch);
        if (index < 0) { cursor += 6.0f * scale; continue; }
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if ((font[index][row] >> (4 - col)) & 1) {
                    rect(state, cursor + (float)col * scale, y + (float)row * scale, scale, scale, r, g, b, a);
                }
            }
        }
        cursor += 6.0f * scale;
    }
}

static float fit_text_scale(const char *value, float preferred, float available_width) {
    const size_t length = value ? strlen(value) : 0;
    if (length == 0) return preferred;
    const float fit = available_width / ((float)length * 6.0f);
    return fit < preferred ? fit : preferred;
}

static void circle(DeadSpeak *state, float cx, float cy, float radius, float r, float g, float b, float a) {
    const int segments = 48;
    for (int i = 0; i < segments; ++i) {
        const float a1 = ((float)i / (float)segments) * 6.2831853f;
        const float a2 = ((float)(i + 1) / (float)segments) * 6.2831853f;
        line(state, cx + cosf(a1) * radius, cy + sinf(a1) * radius,
             cx + cosf(a2) * radius, cy + sinf(a2) * radius, r, g, b, a);
    }
}

static float body_content_height(const DeadSpeak *state) {
    if (state->tab == 0) return state->ui.scan_height;
    if (state->tab == 2) return state->ui.explore_height;
    if (state->tab == 3) return 438.0f;
    const float words = state->log_count ? 70.0f + (float)state->log_count * 42.0f : 130.0f;
    const float media = 36.0f + (float)(state->media_count ? state->media_count : 1) * 34.0f + 16.0f;
    return state->ui.wide ? fmaxf(words, 22.0f + media) : fmaxf(140.0f, words + 20.0f) + media;
}

static void draw_log(DeadSpeak *state, float panel_r, float panel_g, float panel_b,
                     float mint_r, float mint_g, float mint_b, float muted_r, float muted_g, float muted_b) {
    const DSRect body = state->ui.body;
    const float column_width = state->ui.wide ? (body.w - 16.0f) * 0.5f : body.w;
    const float x = body.x + 16.0f, top = body.y;
    text(state, "RECENT SENSOR WORDS", x, top + 22, fit_text_scale("RECENT SENSOR WORDS", 1.8f, column_width - 32), mint_r, mint_g, mint_b, 1.0f);
    if (state->log_count == 0) {
        text(state, "NO WORDS LOGGED YET", x, top + 70, fit_text_scale("NO WORDS LOGGED YET", 1.5f, column_width - 32), muted_r, muted_g, muted_b, 1.0f);
        text(state, "ENABLE SCAN TO COLLECT READINGS", x, top + 100, fit_text_scale("ENABLE SCAN TO COLLECT READINGS", 1.1f, column_width - 32), muted_r, muted_g, muted_b, 1.0f);
    } else {
        for (unsigned i = 0; i < state->log_count; ++i) {
            const unsigned slot = (state->log_next + DS_LOG_CAPACITY - 1U - i) % DS_LOG_CAPACITY;
            const float y = top + 70.0f + (float)i * 42.0f;
            char details[64];
            snprintf(details, sizeof(details), "SAMPLE %u  FIELD %.1f UT", state->word_samples[slot], state->word_fields[slot]);
            text(state, state->words[slot], x, y, fit_text_scale(state->words[slot], 2.0f, column_width - 32), mint_r, mint_g, mint_b, 1.0f);
            text(state, details, x, y + 19, fit_text_scale(details, 1.0f, column_width - 32), muted_r, muted_g, muted_b, 1.0f);
            rect(state, x, y + 34, column_width - 32, 1, 0.17f, 0.24f, 0.23f, 1.0f);
        }
    }
    const float media_x = state->ui.wide ? x + column_width + 16.0f : x;
    const float media_y = state->ui.wide ? top + 22.0f : top + fmaxf(140.0f, 90.0f + (float)state->log_count * 42.0f);
    text(state, "SAVED CAPTURES", media_x, media_y, 1.6f, mint_r, mint_g, mint_b, 1.0f);
    if (state->media_count == 0U) text(state, "NO AUDIO OR VIDEO FILES", media_x, media_y + 34, fit_text_scale("NO AUDIO OR VIDEO FILES", 1.1f, column_width - 32), muted_r, muted_g, muted_b, 1.0f);
    for (unsigned i = 0; i < state->media_count; ++i) {
        text(state, state->media_files[i], media_x, media_y + 36.0f + (float)i * 34.0f, fit_text_scale(state->media_files[i], 1.0f, column_width - 32), muted_r, muted_g, muted_b, 1.0f);
    }
}

static void draw_guide(DeadSpeak *state, float panel_r, float panel_g, float panel_b,
                       float mint_r, float mint_g, float mint_b, float muted_r, float muted_g, float muted_b,
                       float amber_r, float amber_g, float amber_b) {
    const float top = state->ui.body.y, x = state->ui.body.x + 16.0f;
    const float available = state->ui.body.w - 32.0f;
    text(state, "FIELD NOTES", x, top + 24, 2.2f, mint_r, mint_g, mint_b, 1.0f);
    static const char *const notes[] = {
        "MAGNETIC READINGS CHANGE NEAR", "METAL ELECTRONICS AND MOVEMENT",
        "WORDS ARE SENSOR OUTPUTS", "NOT VERIFIED COMMUNICATION",
        "PAIR READINGS WITH OTHER TOOLS", "AND CHECK ORDINARY CAUSES",
        "NEVER USE OUTPUT FOR IMPORTANT", "LIFE DECISIONS",
        "EVP CAPTURE SAVES MONO WAV AUDIO", "VIDEO SAVES MP4 UP TO 1080P",
        "AUDIO AND VIDEO ARE SEPARATE FILES", "NIGHT BOOSTS CAMERA EXPOSURE",
        "MONO AND SEPIA NEED CAMERA SUPPORT"
    };
    for (size_t i = 0; i < sizeof(notes) / sizeof(notes[0]); ++i) {
        const bool warning = i == 6 || i == 7 || i == 11;
        const bool highlighted = i == 2;
        text(state, notes[i], x, top + 74.0f + (float)i * 28.0f,
             fit_text_scale(notes[i], highlighted ? 1.35f : 1.15f, available),
             warning ? amber_r : (highlighted ? mint_r : muted_r),
             warning ? amber_g : (highlighted ? mint_g : muted_g),
             warning ? amber_b : (highlighted ? mint_b : muted_b), 1.0f);
    }
}

static void draw_explore(DeadSpeak *state, float panel_r, float panel_g, float panel_b,
                         float mint_r, float mint_g, float mint_b, float muted_r, float muted_g, float muted_b,
                         float amber_r, float amber_g, float amber_b) {
    const DSRect controls = state->ui.explore_controls, rows = state->ui.explore_rows;
    const float x = rows.x + 16.0f, available = rows.w - 32.0f;
    const float footer = state->ui.explore_footer_y;
    text(state, "WIKIPEDIA FIELD INDEX", controls.x + 16, controls.y + 20,
         fit_text_scale("WIKIPEDIA FIELD INDEX", 1.7f, controls.w - 124), mint_r, mint_g, mint_b, 1.0f);
    text(state, "REFRESH", controls.x + controls.w - 74, controls.y + 20, 0.95f, amber_r, amber_g, amber_b, 1.0f);
    const char *index_status = atomic_load(&state->explore.loading) ? "REFRESHING INDEX" :
        (atomic_load(&state->explore.failed) ? "REFRESH FAILED - SAVED INDEX" : "REPORTED HAUNTED LOCATIONS");
    text(state, index_status, controls.x + 16, controls.y + 42,
         fit_text_scale(index_status, 1.0f, controls.w - 32), muted_r, muted_g, muted_b, 1.0f);
    rect(state, controls.x + 12, controls.y + 61, controls.w - 24, 36, 0.13f, 0.18f, 0.18f, 1.0f);
    const char *query_label = state->explore_query[0] ? state->explore_query : "TAP TO SEARCH";
    text(state, query_label, controls.x + 24, controls.y + 73, fit_text_scale(query_label, 1.4f, controls.w - 48),
         state->explore_query[0] ? mint_r : muted_r, state->explore_query[0] ? mint_g : muted_g,
         state->explore_query[0] ? mint_b : muted_b, 1.0f);

    DSExplorePlace places[DS_EXPLORE_PAGE_SIZE];
    size_t match_count = 0;
    size_t count = ds_explore_page(&state->explore, state->explore_query,
                                        state->explore_page, state->ui.explore_page_size, places, &match_count);
    const size_t last_page = match_count == 0 ? 0 : (match_count - 1U) / state->ui.explore_page_size;
    if (state->explore_page > last_page) {
        state->explore_page = last_page;
        count = ds_explore_page(&state->explore, state->explore_query, state->explore_page, state->ui.explore_page_size, places, &match_count);
    }
    if (atomic_load(&state->explore.loading) && match_count == 0) {
        text(state, "LOADING PUBLIC PLACE INDEX", x, rows.y + 17, fit_text_scale("LOADING PUBLIC PLACE INDEX", 1.3f, available), amber_r, amber_g, amber_b, 1.0f);
        text(state, "WIKIPEDIA COMMUNITY CATEGORIES", x, rows.y + 47, fit_text_scale("WIKIPEDIA COMMUNITY CATEGORIES", 1.0f, available), muted_r, muted_g, muted_b, 1.0f);
    } else if (match_count == 0) {
        const char *empty = atomic_load(&state->explore.failed) ? "INDEX UNAVAILABLE - TAP REFRESH" : "NO MATCHING PLACES";
        text(state, empty, x, rows.y + 17, fit_text_scale(empty, 1.1f, available), amber_r, amber_g, amber_b, 1.0f);
    }
    for (size_t i = 0; i < count; ++i) {
        const float row_top = rows.y + (float)i * DS_EXPLORE_ROW_HEIGHT;
        const float y = row_top + 7.0f;
        text(state, places[i].title, x, y, fit_text_scale(places[i].title, 1.4f, available), mint_r, mint_g, mint_b, 1.0f);
        char source_line[80];
        snprintf(source_line, sizeof(source_line), "%s  %s", places[i].region,
                 places[i].wikidata_id[0] ? places[i].wikidata_id : "NO WIKIDATA ITEM");
        text(state, source_line, x, row_top + DS_EXPLORE_ROW_SOURCE, fit_text_scale(source_line, 0.8f, available), muted_r, muted_g, muted_b, 1.0f);
        if (places[i].has_coordinates) {
            char coordinate_line[64];
            snprintf(coordinate_line, sizeof(coordinate_line), "LAT %.0f  LON %.0f",
                     places[i].latitude, places[i].longitude);
            text(state, coordinate_line, x, y + 36.0f, fit_text_scale(coordinate_line, 0.75f, available), amber_r, amber_g, amber_b, 1.0f);
        }
        rect(state, x, y + 45.0f, available, 1.0f, 0.17f, 0.24f, 0.23f, 1.0f);
    }
    char page_label[48];
    snprintf(page_label, sizeof(page_label), "MATCHES %u  PAGE %u", (unsigned)match_count,
             (unsigned)(state->explore_page + 1U));
    text(state, page_label, x, footer, fit_text_scale(page_label, 0.95f, available), muted_r, muted_g, muted_b, 1.0f);
    text(state, "PREV", x, footer + 31, 1.2f, muted_r, muted_g, muted_b, 1.0f);
    text(state, "NEXT", rows.x + rows.w - 74, footer + 31, 1.2f, mint_r, mint_g, mint_b, 1.0f);
    text(state, "TOP: WIKIPEDIA  LOWER: WIKIDATA", x, footer + 61,
         fit_text_scale("TOP: WIKIPEDIA  LOWER: WIKIDATA", 0.82f, available), amber_r, amber_g, amber_b, 1.0f);
}

static void start_explore(DeadSpeak *state) {
    if (state->explore_cache_path[0]) {
        ds_explore_start(&state->explore, state->app->activity, state->explore_cache_path);
    }
}

static int32_t ascii_from_key(int32_t key, int32_t meta) {
    const bool shifted = (meta & AMETA_SHIFT_ON) != 0;
    if (key >= AKEYCODE_A && key <= AKEYCODE_Z) {
        const int32_t letter = 'a' + key - AKEYCODE_A;
        return shifted ? letter - ('a' - 'A') : letter;
    }
    if (key >= AKEYCODE_0 && key <= AKEYCODE_9) return '0' + key - AKEYCODE_0;
    if (key == AKEYCODE_SPACE) return ' ';
    if (key == AKEYCODE_PERIOD) return '.';
    if (key == AKEYCODE_COMMA) return ',';
    if (key == AKEYCODE_MINUS) return shifted ? '_' : '-';
    if (key == AKEYCODE_APOSTROPHE) return shifted ? '"' : '\'';
    return 0;
}

static void draw_frame(DeadSpeak *state) {
    const float mint_r = 0.73f, mint_g = 0.96f, mint_b = 0.86f;
    const float muted_r = 0.42f, muted_g = 0.53f, muted_b = 0.52f;
    const float amber_r = 0.96f, amber_g = 0.70f, amber_b = 0.36f;
    const float panel_r = 0.075f, panel_g = 0.105f, panel_b = 0.11f;
    const float w = (float)state->width;
    const float h = (float)state->height;
    const int64_t sensor_time = sensor_now_ns();
    const bool magnetic_live = ds_sensor_live(&state->readings[0], sensor_time);

    glClearColor(0.027f, 0.043f, 0.047f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    const float header_offset = state->ui.compact ? -8.0f : 0.0f;
    rect(state, 18, 22 + header_offset, 22, 22, mint_r, mint_g, mint_b, 0.9f);
    rect(state, 23, 27 + header_offset, 12, 12, 0.027f, 0.043f, 0.047f, 1.0f);
    text(state, "DEADSPEAK", 50, 24 + header_offset, fit_text_scale("DEADSPEAK", 3.0f, w - 68.0f), mint_r, mint_g, mint_b, 1.0f);
    const char *author = "EXALTED6165 LABS";
    const float author_scale = fit_text_scale(author, 1.3f, (w - 68.0f) * 0.46f);
    text(state, author, 50, 49 + header_offset, author_scale, muted_r, muted_g, muted_b, 1.0f);
    const char *tagline = "RAW SENSOR FIELDWORK";
    const float tagline_x = 50.0f + (float)strlen(author) * 6.0f * author_scale + 12.0f;
    const float tagline_scale = fit_text_scale(tagline, 1.1f, w - 18.0f - tagline_x);
    text(state, tagline, w - 18.0f - (float)strlen(tagline) * 6.0f * tagline_scale, 49 + header_offset, tagline_scale, muted_r, muted_g, muted_b, 1.0f);
    rect(state, 18, state->ui.compact ? 56 : 72, w - 36, 1, 0.19f, 0.27f, 0.26f, 0.9f);

    const char *screen_title = state->tab == 0 ? "FIELD SCAN" : (state->tab == 1 ? "EVIDENCE LOG" :
                               (state->tab == 2 ? "EXPLORE" : "FIELD GUIDE"));
    text(state, screen_title, 22, state->ui.title_y, fit_text_scale(screen_title, state->ui.compact ? 2.4f : 3.4f, w - 44), 0.95f, 0.98f, 0.94f, 1.0f);
    if (state->tab == 2) {
        text(state, "PUBLIC REPORTS - NOT VERIFIED CLAIMS", 22, state->ui.subtitle_y, fit_text_scale("PUBLIC REPORTS - NOT VERIFIED CLAIMS", 1.05f, w - 44), amber_r, amber_g, amber_b, 1.0f);
    } else {
        char sensor_label[64];
        snprintf(sensor_label, sizeof(sensor_label), "MAGNETOMETER %s", ds_sensor_status_name(state->readings[0].status));
        text(state, sensor_label, 22, state->ui.subtitle_y, fit_text_scale(sensor_label, state->ui.compact ? 1.1f : 1.5f, w - 44), magnetic_live ? mint_r : amber_r, magnetic_live ? mint_g : amber_g, magnetic_live ? mint_b : amber_b, 1.0f);
    }

    const DSRect body = state->ui.body;
    state->scroll[state->tab] = fminf(state->scroll[state->tab], ds_scroll_limit(body_content_height(state), body));
    if (state->tab != 0) rect(state, body.x, body.y, body.w, body.h, panel_r, panel_g, panel_b, 1.0f);
    const int clip_top = (int)ceilf(body.y * state->ui_scale);
    const int clip_bottom = (int)floorf((body.y + body.h) * state->ui_scale);
    glEnable(GL_SCISSOR_TEST);
    glScissor((int)ceilf(body.x * state->ui_scale), state->surface_height - clip_bottom,
              (int)floorf(body.w * state->ui_scale), clip_bottom - clip_top);
    state->body_drawing = true;

    if (state->tab == 1) {
        draw_log(state, panel_r, panel_g, panel_b, mint_r, mint_g, mint_b, muted_r, muted_g, muted_b);
        goto navigation;
    }
    if (state->tab == 2) {
        draw_explore(state, panel_r, panel_g, panel_b, mint_r, mint_g, mint_b, muted_r, muted_g, muted_b, amber_r, amber_g, amber_b);
        goto navigation;
    }
    if (state->tab == 3) {
        draw_guide(state, panel_r, panel_g, panel_b, mint_r, mint_g, mint_b, muted_r, muted_g, muted_b, amber_r, amber_g, amber_b);
        goto navigation;
    }

    const DSRect field = state->ui.field, word = state->ui.word, capture = state->ui.capture, spectrum = state->ui.spectrum;
    rect(state, field.x, field.y, field.w, field.h, panel_r, panel_g, panel_b, 1.0f);
    text(state, "MAGNETIC FIELD", field.x + 18, field.y + 21, fit_text_scale("MAGNETIC FIELD", 1.55f, field.w * 0.53f - 24), muted_r, muted_g, muted_b, 1.0f);
    const float unit_scale = fit_text_scale("RAW MICROTESLA", 1.25f, field.w * 0.47f - 24);
    text(state, "RAW MICROTESLA", field.x + field.w - 18 - 14 * 6 * unit_scale, field.y + 21, unit_scale, muted_r, muted_g, muted_b, 1.0f);

    const float cx = field.x + field.w * 0.5f;
    const float cy = field.y + field.h * 0.5f;
    const float radius = fminf((field.w - 72.0f) * 0.5f, (field.h - 110.0f) * 0.5f);
    circle(state, cx, cy, radius, 0.18f, 0.31f, 0.29f, 0.8f);
    circle(state, cx, cy, radius * 0.67f, 0.14f, 0.24f, 0.23f, 0.9f);
    line(state, cx - radius - 12, cy, cx + radius + 12, cy, 0.13f, 0.23f, 0.22f, 0.9f);
    line(state, cx, cy - radius - 12, cx, cy + radius + 12, 0.13f, 0.23f, 0.22f, 0.9f);
    if (magnetic_live) {
        const float angle = atan2f(state->readings[0].xyz[1], state->readings[0].xyz[0]);
        line(state, cx, cy, cx + cosf(angle) * radius, cy + sinf(angle) * radius, mint_r, mint_g, mint_b, 0.9f);
    }
    circle(state, cx, cy, 5, mint_r, mint_g, mint_b, 1.0f);

    char value[32];
    if (magnetic_live) snprintf(value, sizeof(value), "%05.1f", state->magnetic_field);
    else snprintf(value, sizeof(value), "%s", ds_sensor_status_name(state->readings[0].status));
    const float value_scale = fit_text_scale(value, 5.0f, field.w - 84.0f);
    const float value_width = (float)strlen(value) * 6.0f * value_scale;
    const float value_x = cx - (value_width + 12.0f + 2.0f * 6.0f * 1.6f) * 0.5f;
    text(state, value, value_x, cy - value_scale * 3.5f, value_scale, 0.95f, 0.98f, 0.94f, 1.0f);
    if (magnetic_live) text(state, "UT", value_x + value_width + 12.0f, cy - 5.6f, 1.6f, muted_r, muted_g, muted_b, 1.0f);

    const float signal_width = (field.w - 36) * (magnetic_live ? state->signal : 0.0f);
    rect(state, field.x + 18, field.y + field.h - 36, field.w - 36, 4, 0.14f, 0.20f, 0.20f, 1.0f);
    rect(state, field.x + 18, field.y + field.h - 36, signal_width, 4, mint_r, mint_g, mint_b, 1.0f);
    char motion_label[64];
    snprintf(motion_label, sizeof(motion_label), "ACCEL %s  GYRO %s", ds_sensor_status_name(state->readings[1].status), ds_sensor_status_name(state->readings[2].status));
    text(state, motion_label, field.x + 18, field.y + field.h - 20,
         fit_text_scale(motion_label, 1.0f, field.w - 110), muted_r, muted_g, muted_b, 1.0f);
    text(state, state->scanning ? "SCANNING" : "STANDBY", field.x + field.w - 72, field.y + field.h - 20, 1.2f, state->scanning ? mint_r : amber_r, state->scanning ? mint_g : amber_g, state->scanning ? mint_b : amber_b, 1.0f);

    rect(state, word.x, word.y, word.w, word.h, panel_r, panel_g, panel_b, 1.0f);
    text(state, "SIGNAL WORD", word.x + 14, word.y + 20, fit_text_scale("SIGNAL WORD", 1.4f, word.w - 28), muted_r, muted_g, muted_b, 1.0f);
    const char *word_label = magnetic_live ? (state->word[0] ? state->word : "QUIET") : "NO SENSOR DATA";
    const float word_width = word.w - 28.0f;
    text(state, word_label, word.x + 14, word.y + word.h * 0.43f, fit_text_scale(word_label, 3.8f, word_width), mint_r, mint_g, mint_b, 1.0f);
    text(state, "ENVIRONMENTAL TRIGGER", word.x + 14, word.y + word.h - 25, fit_text_scale("ENVIRONMENTAL TRIGGER", 1.15f, word_width), muted_r, muted_g, muted_b, 1.0f);

    const float second_x = capture.x;
    rect(state, capture.x, capture.y, capture.w, capture.h, panel_r, panel_g, panel_b, 1.0f);
    text(state, "CAPTURE", second_x + 14, capture.y + 20, 1.4f, muted_r, muted_g, muted_b, 1.0f);
    const bool audio_active = atomic_load(&state->audio_active);
    const bool video_active = ds_video_active(&state->video);
    const bool capture_active = audio_active || video_active;
    const bool capture_failed = atomic_load(&state->audio_failed) || ds_video_failed(&state->video);
    const char *capture_label = capture_active ? "LIVE" : (state->capture_requested ? "ALLOW ACCESS" : (capture_failed ? "ERROR" : "READY"));
    const float capture_width = capture.w - 28.0f;
    text(state, capture_label, second_x + 14, capture.y + capture.h * 0.43f, fit_text_scale(capture_label, 3.2f, capture_width), capture_active ? amber_r : (capture_failed ? amber_r : mint_r), capture_active ? amber_g : (capture_failed ? amber_g : mint_g), capture_active ? amber_b : (capture_failed ? amber_b : mint_b), 1.0f);
    static const char *const filter_names[] = {"NIGHT", "MONO", "SEPIA", "RAW"};
    char filter_label[32];
    snprintf(filter_label, sizeof(filter_label), "FILTER: %s", filter_names[state->video_filter]);
    text(state, filter_label, second_x + 14, capture.y + capture.h - 25, fit_text_scale(filter_label, 1.05f, capture_width), muted_r, muted_g, muted_b, 1.0f);
    const unsigned audio_level = atomic_load(&state->audio_level_milli);
    rect(state, spectrum.x, spectrum.y, spectrum.w, spectrum.h, panel_r, panel_g, panel_b, 1.0f);
    text(state, "MIC", spectrum.x + 14, spectrum.y + 10, 1.0f, muted_r, muted_g, muted_b, 1.0f);
    rect(state, spectrum.x + 52, spectrum.y + 15, spectrum.w - 72, 5, 0.14f, 0.20f, 0.20f, 1.0f);
    rect(state, spectrum.x + 52, spectrum.y + 15, (spectrum.w - 72) * ((float)audio_level / 1000.0f), 5, mint_r, mint_g, mint_b, 1.0f);
    const char *band_names[3] = {"LOW", "MID", "HIGH"};
    const float band_width = (spectrum.w - 28) / 3.0f;
    for (unsigned band = 0; band < 3; ++band) {
        const float band_x = spectrum.x + 14.0f + (float)band * band_width;
        const unsigned band_level = atomic_load(&state->spectrum_milli[band]);
        text(state, band_names[band], band_x, spectrum.y + 37, 0.9f, muted_r, muted_g, muted_b, 1.0f);
        rect(state, band_x, spectrum.y + 55, band_width - 14, 5, 0.14f, 0.20f, 0.20f, 1.0f);
        rect(state, band_x, spectrum.y + 55, (band_width - 14) * ((float)band_level / 1000.0f), 5, mint_r, mint_g, mint_b, 1.0f);
    }

navigation:
    ;
    state->body_drawing = false;
    glDisable(GL_SCISSOR_TEST);
    const float content_height = body_content_height(state);
    const float limit = ds_scroll_limit(content_height, body);
    if (limit > 0.0f) {
        const float thumb = fmaxf(18.0f, body.h * body.h / content_height);
        rect(state, body.x + body.w - 3, body.y + state->scroll[state->tab] / limit * (body.h - thumb),
             2, thumb, muted_r, muted_g, muted_b, 0.8f);
    }
    const float nav_y = state->ui.nav_top;
    const float nav_label_y = nav_y + (state->ui.compact ? 15 : 25);
    rect(state, 18, nav_y, w - 36, 1, 0.19f, 0.27f, 0.26f, 0.9f);
    text(state, "SCAN", w * 0.08f, nav_label_y, 1.2f, state->tab == 0 ? mint_r : muted_r, state->tab == 0 ? mint_g : muted_g, state->tab == 0 ? mint_b : muted_b, 1.0f);
    text(state, "LOG", w * 0.32f, nav_label_y, 1.2f, state->tab == 1 ? mint_r : muted_r, state->tab == 1 ? mint_g : muted_g, state->tab == 1 ? mint_b : muted_b, 1.0f);
    text(state, "EXPLORE", w * 0.51f, nav_label_y, 1.1f, state->tab == 2 ? mint_r : muted_r, state->tab == 2 ? mint_g : muted_g, state->tab == 2 ? mint_b : muted_b, 1.0f);
    text(state, "GUIDE", w * 0.81f, nav_label_y, 1.2f, state->tab == 3 ? mint_r : muted_r, state->tab == 3 ? mint_g : muted_g, state->tab == 3 ? mint_b : muted_b, 1.0f);
    text(state, "SKEPTICISM IS A FEATURE", 22, h - 39, 1.1f, muted_r, muted_g, muted_b, 1.0f);

    eglSwapBuffers(state->display, state->surface);
}

static void handle_cmd(struct android_app *app, int32_t cmd) {
    DeadSpeak *state = (DeadSpeak *)app->userData;
    if (cmd == APP_CMD_INIT_WINDOW && app->window != NULL) setup_display(state);
    else if (cmd == APP_CMD_TERM_WINDOW) teardown_display(state);
    else if (cmd == APP_CMD_CONFIG_CHANGED || cmd == APP_CMD_WINDOW_RESIZED || cmd == APP_CMD_CONTENT_RECT_CHANGED)
        update_display_geometry(state);
    else if (cmd == APP_CMD_RESUME) {
        state->sensors_resumed = true;
        start_sensors(state);
    }
    else if (cmd == APP_CMD_PAUSE) {
        state->sensors_resumed = false;
        stop_sensors(state, DS_SENSOR_PAUSED);
        state->capture_requested = false;
        stop_audio_capture(state);
        ds_video_stop(&state->video);
    }
}

static int32_t handle_input(struct android_app *app, AInputEvent *event) {
    DeadSpeak *state = (DeadSpeak *)app->userData;
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY && state->tab == 2 && state->explore_search_active &&
        AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_DOWN) {
        const int32_t key = AKeyEvent_getKeyCode(event);
        size_t length = strlen(state->explore_query);
        if (key == AKEYCODE_DEL) {
            if (length > 0) state->explore_query[length - 1U] = '\0';
            state->explore_page = 0;
        } else if (key == AKEYCODE_ENTER || key == AKEYCODE_BACK) {
            state->explore_search_active = false;
            ANativeActivity_hideSoftInput(state->app->activity, ANATIVEACTIVITY_HIDE_SOFT_INPUT_NOT_ALWAYS);
        } else {
            const int32_t unicode = ascii_from_key(key, AKeyEvent_getMetaState(event));
            if (unicode >= 32 && unicode <= 126 && length + 1U < sizeof(state->explore_query)) {
                state->explore_query[length] = (char)toupper((unsigned char)unicode);
                state->explore_query[length + 1U] = '\0';
                state->explore_page = 0;
            }
        }
        return 1;
    }
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) return 0;
    if (state->ui_scale <= 0.0f || state->ui.explore_page_size == 0) return 0;
    const int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
    const float x = AMotionEvent_getX(event, 0) / state->ui_scale;
    float y = AMotionEvent_getY(event, 0) / state->ui_scale;
    if (action == AMOTION_EVENT_ACTION_DOWN) {
        state->touch_active = ds_rect_contains(state->ui.body, x, y);
        state->touch_dragged = false;
        state->touch_start_x = x;
        state->touch_y = state->touch_start_y = y;
        return 1;
    }
    if (action == AMOTION_EVENT_ACTION_CANCEL || action == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        state->touch_active = false;
        state->touch_dragged = true;
        return 1;
    }
    if (action == AMOTION_EVENT_ACTION_MOVE && state->touch_active) {
        if (fabsf(y - state->touch_start_y) > 8.0f || fabsf(x - state->touch_start_x) > 8.0f)
            state->touch_dragged = true;
        if (state->touch_dragged) {
            state->scroll[state->tab] = fminf(ds_scroll_limit(body_content_height(state), state->ui.body),
                fmaxf(0.0f, state->scroll[state->tab] + state->touch_y - y));
        }
        state->touch_y = y;
        return 1;
    }
    if (action == AMOTION_EVENT_ACTION_SCROLL) {
        if (ds_rect_contains(state->ui.body, x, y))
            state->scroll[state->tab] = fminf(ds_scroll_limit(body_content_height(state), state->ui.body),
                fmaxf(0.0f, state->scroll[state->tab] - AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_VSCROLL, 0) * 48.0f));
        return 1;
    }
    if (action != AMOTION_EVENT_ACTION_UP) return 1;
    if (state->touch_active && (fabsf(y - state->touch_start_y) > 8.0f || fabsf(x - state->touch_start_x) > 8.0f))
        state->touch_dragged = true;
    state->touch_active = false;
    if (state->touch_dragged) return 1;
    if (y >= state->ui.nav_top) {
        const int previous_tab = state->tab;
        if (x < state->width * 0.25f) state->tab = 0;
        else if (x < state->width * 0.50f) state->tab = 1;
        else if (x < state->width * 0.75f) state->tab = 2;
        else state->tab = 3;
        if (previous_tab == 2 && state->tab != 2 && state->explore_search_active) {
            state->explore_search_active = false;
            ANativeActivity_hideSoftInput(state->app->activity, ANATIVEACTIVITY_HIDE_SOFT_INPUT_NOT_ALWAYS);
        }
        if (state->tab == 2) start_explore(state);
        return 1;
    }
    if (!ds_rect_contains(state->ui.body, x, y)) return 1;
    y += state->scroll[state->tab];
    const DSRect controls = state->ui.explore_controls, rows = state->ui.explore_rows;
    const DSRect refresh = {controls.x + controls.w - 90, controls.y + 6, 78, 44};
    const DSRect search = {controls.x + 12, controls.y + 55, controls.w - 24, 48};
    const DSRect pagination = {rows.x, state->ui.explore_footer_y + 12, rows.w, 44};
    if (state->tab == 2 && ds_rect_contains(refresh, x, y)) {
        ds_explore_refresh(&state->explore, state->app->activity, state->explore_cache_path);
    } else if (state->tab == 2 && ds_rect_contains(search, x, y)) {
        state->explore_search_active = true;
        ANativeActivity_showSoftInput(state->app->activity, ANATIVEACTIVITY_SHOW_SOFT_INPUT_IMPLICIT);
    } else if (state->tab == 2 && ds_rect_contains(pagination, x, y)) {
        DSExplorePlace places[DS_EXPLORE_PAGE_SIZE];
        size_t match_count = 0;
        (void)ds_explore_page(&state->explore, state->explore_query, 0, state->ui.explore_page_size, places, &match_count);
        const size_t last_page = match_count == 0 ? 0 : (match_count - 1U) / state->ui.explore_page_size;
        if (x > rows.x + rows.w * 0.5f && state->explore_page < last_page) ++state->explore_page;
        else if (x <= rows.x + rows.w * 0.5f && state->explore_page > 0) --state->explore_page;
    } else if (state->tab == 2 && ds_rect_contains(rows, x, y)) {
        const unsigned row = (unsigned)((y - rows.y) / DS_EXPLORE_ROW_HEIGHT);
        DSExplorePlace places[DS_EXPLORE_PAGE_SIZE];
        size_t match_count = 0;
        const size_t count = ds_explore_page(&state->explore, state->explore_query,
                                            state->explore_page, state->ui.explore_page_size, places, &match_count);
        if (row < count) {
            state->explore_search_active = false;
            ANativeActivity_hideSoftInput(state->app->activity, ANATIVEACTIVITY_HIDE_SOFT_INPUT_NOT_ALWAYS);
            if ((y - rows.y - (float)row * DS_EXPLORE_ROW_HEIGHT) >= DS_EXPLORE_ROW_SOURCE && places[row].wikidata_id[0])
                ds_explore_open_wikidata(state->app->activity, places[row].wikidata_id);
            else
                ds_explore_open_article(state->app->activity, places[row].page_id);
        }
    } else if (state->tab == 0 && ds_rect_contains(state->ui.capture, x, y) &&
               y >= state->ui.capture.y + state->ui.capture.h - 44.0f) {
        state->video_filter = (DSVideoFilter)(((unsigned)state->video_filter + 1U) % 4U);
    } else if (state->tab == 0 && ds_rect_contains(state->ui.capture, x, y)) {
        if (atomic_load(&state->audio_active) || ds_video_active(&state->video) || state->capture_requested) {
            state->capture_requested = false;
            state->permission_requested = false;
            stop_audio_capture(state);
            ds_video_stop(&state->video);
        } else if (has_capture_permissions(state)) {
            start_audio_capture(state);
            ds_video_start(&state->video, state->evidence_dir, state->video_filter, state->display_rotation);
        } else {
            state->capture_requested = true;
            state->permission_deadline = now_millis() + 15000U;
            request_audio_permission(state);
        }
    } else if (state->tab == 0 && ds_rect_contains(state->ui.field, x, y)) {
        state->scanning = !state->scanning;
    }
    return 1;
}

static void poll_sensor(DeadSpeak *state) {
    ASensorEvent event;
    while (state->sensor_queue && ASensorEventQueue_getEvents(state->sensor_queue, &event, 1) > 0) {
        unsigned index;
        for (index = 0; index < DS_SENSOR_COUNT; ++index) if (event.type == sensor_types[index]) break;
        if (index == DS_SENSOR_COUNT || !state->sensors_resumed) continue;
        DSSensorReading *reading = &state->readings[index];
        const int64_t received = sensor_now_ns();
        DSSampleResult sample = ds_sensor_accept(reading, event.timestamp, received,
            event.vector.x, event.vector.y, event.vector.z, event.vector.status);
        if (sample == DS_SAMPLE_REJECTED || index != 0) continue;
        state->magnetic_field = reading->magnitude;
        state->smoothed_field = sample == DS_SAMPLE_FIRST ? reading->magnitude :
            ds_engine_smooth(state->smoothed_field, reading->magnitude, 0.15f);
        state->signal = ds_engine_signal(state->smoothed_field);
        if (state->sample_count == UINT32_MAX) state->sample_count = 0;
        ++state->sample_count;
        if (!state->last_magnetic_log_ns || received - state->last_magnetic_log_ns >= INT64_C(5000000000)) {
            state->last_magnetic_log_ns = received;
            __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG,
                "MAGNETOMETER LIVE samples=%u field=%.3f uT xyz=%.3f,%.3f,%.3f accuracy=%d",
                state->sample_count, state->magnetic_field, reading->xyz[0], reading->xyz[1], reading->xyz[2], reading->accuracy);
        }
        if (state->scanning && state->sample_count % 10U == 0U) {
            ds_engine_next_word(state->smoothed_field, state->sample_count, state->dictionary,
                                state->dictionary_size, state->word, sizeof(state->word));
            if (state->word[0]) append_word_log(state, state->word);
        }
    }
}

static void stop_sensors(DeadSpeak *state, DSSensorStatus status) {
    for (unsigned i = 0; i < DS_SENSOR_COUNT; ++i) {
        if (state->sensor_queue && state->sensors[i] && state->readings[i].enabled)
            ASensorEventQueue_disableSensor(state->sensor_queue, state->sensors[i]);
        state->readings[i].enabled = false;
        ds_sensor_clear(&state->readings[i], state->sensors[i] ? status : DS_SENSOR_ABSENT);
    }
    state->magnetic_field = state->smoothed_field = state->signal = 0.0f;
    state->word[0] = '\0';
}

static void start_sensors(DeadSpeak *state) {
    if (!state->sensors_resumed || !state->sensor_queue) return;
    const int64_t started = sensor_now_ns();
    for (unsigned i = 0; i < DS_SENSOR_COUNT; ++i) {
        DSSensorReading *reading = &state->readings[i];
        if (reading->enabled || !state->sensors[i]) continue;
        const ASensor *sensor = state->sensors[i];
        const int enable = ASensorEventQueue_enableSensor(state->sensor_queue, sensor);
        int delay = ASensor_getMinDelay(sensor);
        if (delay < 100000) delay = 100000;
        const int rate = enable == 0 ? ASensorEventQueue_setEventRate(state->sensor_queue, sensor, delay) : enable;
        ds_sensor_configure(reading, true, true, enable, rate, started);
        if (!reading->enabled && enable == 0) ASensorEventQueue_disableSensor(state->sensor_queue, sensor);
        __android_log_print(reading->enabled ? ANDROID_LOG_DEBUG : ANDROID_LOG_ERROR, LOG_TAG,
            "%s sensor %s: enable=%d rate=%d period=%d us", sensor_names[i],
            ds_sensor_status_name(reading->status), enable, rate, delay);
    }
}

static void maintain_sensors(DeadSpeak *state) {
    if (!state->sensors_resumed) return;
    const int64_t now = sensor_now_ns();
    for (unsigned i = 0; i < DS_SENSOR_COUNT; ++i) {
        DSSensorReading *reading = &state->readings[i];
        ds_sensor_expire(reading, now);
        if (reading->enabled && reading->status == DS_SENSOR_WAITING && now > reading->started_ns &&
            now - reading->started_ns > DS_SENSOR_FRESH_NS) ds_sensor_clear(reading, DS_SENSOR_STALE);
    }
    if (!ds_sensor_live(&state->readings[0], now)) {
        state->magnetic_field = state->smoothed_field = state->signal = 0.0f;
        state->word[0] = '\0';
    }
    if (now_millis() >= state->next_sensor_access_check) {
        state->next_sensor_access_check = now_millis() + 1000U;
        /* Re-attempt an explicitly failed registration after an OS grant;
           this never substitutes readings or a different sensor. */
        start_sensors(state);
    }
}

static void setup_sensors(DeadSpeak *state) {
    state->sensor_manager = ASensorManager_getInstanceForPackage("org.deadspeak");
    if (!state->sensor_manager) return;
    for (unsigned i = 0; i < DS_SENSOR_COUNT; ++i) {
        state->sensors[i] = ASensorManager_getDefaultSensor(state->sensor_manager, sensor_types[i]);
        ds_sensor_configure(&state->readings[i], state->sensors[i] != NULL, true, -1, -1, sensor_now_ns());
    }
    state->sensor_queue = ASensorManager_createEventQueue(state->sensor_manager, ALooper_forThread(), LOOPER_ID_SENSOR, NULL, NULL);
}

void android_main(struct android_app *app) {
    DeadSpeak state = {0};
    state.app = app;
    state.display = EGL_NO_DISPLAY;
    state.surface = EGL_NO_SURFACE;
    state.context = EGL_NO_CONTEXT;
    state.ui_scale = 1.0f;
    state.scanning = true;
    state.video_filter = DS_VIDEO_FILTER_RAW;
    state.word[0] = 0;
    ds_explore_init(&state.explore);
    load_dictionary(&state);
    snprintf(state.evidence_dir, sizeof(state.evidence_dir), "%s/evidence", app->activity->internalDataPath);
    snprintf(state.word_log_path, sizeof(state.word_log_path), "%s/word-log.csv", state.evidence_dir);
    snprintf(state.explore_cache_path, sizeof(state.explore_cache_path), "%s/wikipedia-places-v2.tsv", state.evidence_dir);
    mkdir(state.evidence_dir, 0700);
    load_word_log(&state);
    refresh_media_files(&state);
    app->userData = &state;
    app->onAppCmd = handle_cmd;
    app->onInputEvent = handle_input;
    setup_sensors(&state);

    while (true) {
        static uint64_t next_permission_check = 0;
        const uint64_t loop_time = now_millis();
        maintain_sensors(&state);
        if (loop_time >= state.next_media_refresh) {
            state.next_media_refresh = loop_time + 1000U;
            refresh_media_files(&state);
        }
        if (state.capture_requested && loop_time >= next_permission_check) {
            next_permission_check = loop_time + 250U;
            if (has_capture_permissions(&state)) {
                state.capture_requested = false;
                state.permission_requested = false;
                start_audio_capture(&state);
                ds_video_start(&state.video, state.evidence_dir, state.video_filter, state.display_rotation);
            } else if (loop_time >= state.permission_deadline) {
                state.capture_requested = false;
                state.permission_requested = false;
                atomic_store(&state.audio_failed, true);
            }
        }
        if (state.audio_thread_created && !atomic_load(&state.audio_active)) {
            pthread_join(state.audio_thread, NULL);
            state.audio_thread_created = false;
        }
        if (state.video.thread_created && !ds_video_active(&state.video)) ds_video_stop(&state.video);
        int ident;
        int events;
        struct android_poll_source *source;
        while ((ident = ALooper_pollOnce(state.display == EGL_NO_DISPLAY ? -1 : 0, NULL, &events, (void **)&source)) >= 0) {
            if (source) source->process(app, source);
            if (ident == LOOPER_ID_SENSOR) poll_sensor(&state);
            if (app->destroyRequested) {
                stop_audio_capture(&state);
                ds_video_stop(&state.video);
                stop_sensors(&state, DS_SENSOR_PAUSED);
                if (state.sensor_manager && state.sensor_queue)
                    ASensorManager_destroyEventQueue(state.sensor_manager, state.sensor_queue);
                free(state.dictionary);
                ds_explore_destroy(&state.explore);
                teardown_display(&state);
                return;
            }
        }
        if (state.display != EGL_NO_DISPLAY && state.surface != EGL_NO_SURFACE && state.width > 0 && state.height > 0) draw_frame(&state);
    }
}
