#include <android/asset_manager.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/sensor.h>
#include <aaudio/AAudio.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <ctype.h>
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
#include "video_capture.h"

#define LOG_TAG "DeadSpeak"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOOPER_ID_SENSOR 3
#define DS_LOG_CAPACITY 32
#define DS_AUDIO_RATE 48000

typedef struct {
    struct android_app *app;
    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
    int width;
    int height;
    GLuint program;
    GLint position;
    GLint color;
    ASensorManager *sensor_manager;
    const ASensor *magnetometer;
    ASensorEventQueue *sensor_queue;
    float magnetic_field;
    float smoothed_field;
    float signal;
    uint32_t sample_count;
    char word[32];
    bool has_magnetometer;
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
    float sweep;
} DeadSpeak;

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
    JNIEnv *env = state->app->activity->env;
    jobject activity = state->app->activity->clazz;
    jclass activity_class = (*env)->GetObjectClass(env, activity);
    jmethodID check = (*env)->GetMethodID(env, activity_class, "checkSelfPermission", "(Ljava/lang/String;)I");
    jstring audio_permission = (*env)->NewStringUTF(env, "android.permission.RECORD_AUDIO");
    jstring camera_permission = (*env)->NewStringUTF(env, "android.permission.CAMERA");
    const jint audio_result = (*env)->CallIntMethod(env, activity, check, audio_permission);
    const jint camera_result = (*env)->CallIntMethod(env, activity, check, camera_permission);
    (*env)->DeleteLocalRef(env, audio_permission);
    (*env)->DeleteLocalRef(env, camera_permission);
    (*env)->DeleteLocalRef(env, activity_class);
    return audio_result == 0 && camera_result == 0;
}

static void request_audio_permission(DeadSpeak *state) {
    if (state->permission_requested) return;
    JNIEnv *env = state->app->activity->env;
    jobject activity = state->app->activity->clazz;
    jclass activity_class = (*env)->GetObjectClass(env, activity);
    jmethodID request = (*env)->GetMethodID(env, activity_class, "requestPermissions", "([Ljava/lang/String;I)V");
    jclass string_class = (*env)->FindClass(env, "java/lang/String");
    jobjectArray permissions = (*env)->NewObjectArray(env, 2, string_class, NULL);
    jstring audio_permission = (*env)->NewStringUTF(env, "android.permission.RECORD_AUDIO");
    jstring camera_permission = (*env)->NewStringUTF(env, "android.permission.CAMERA");
    (*env)->SetObjectArrayElement(env, permissions, 0, audio_permission);
    (*env)->SetObjectArrayElement(env, permissions, 1, camera_permission);
    (*env)->CallVoidMethod(env, activity, request, permissions, 6165);
    (*env)->DeleteLocalRef(env, audio_permission);
    (*env)->DeleteLocalRef(env, camera_permission);
    (*env)->DeleteLocalRef(env, permissions);
    (*env)->DeleteLocalRef(env, string_class);
    (*env)->DeleteLocalRef(env, activity_class);
    state->permission_requested = true;
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

static const uint8_t font[38][7] = {
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

    eglQuerySurface(state->display, state->surface, EGL_WIDTH, &state->width);
    eglQuerySurface(state->display, state->surface, EGL_HEIGHT, &state->height);
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
    glViewport(0, 0, state->width, state->height);
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
    const float x1 = (x / state->width) * 2.0f - 1.0f;
    const float x2 = ((x + w) / state->width) * 2.0f - 1.0f;
    const float y1 = 1.0f - (y / state->height) * 2.0f;
    const float y2 = 1.0f - ((y + h) / state->height) * 2.0f;
    const GLfloat vertices[] = {x1,y1, x2,y1, x1,y2, x1,y2, x2,y1, x2,y2};
    emit_vertices(state, vertices, 6, r, g, b, a);
}

static void line(DeadSpeak *state, float x1, float y1, float x2, float y2, float r, float g, float b, float a) {
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

static void circle(DeadSpeak *state, float cx, float cy, float radius, float r, float g, float b, float a) {
    const int segments = 48;
    for (int i = 0; i < segments; ++i) {
        const float a1 = ((float)i / (float)segments) * 6.2831853f;
        const float a2 = ((float)(i + 1) / (float)segments) * 6.2831853f;
        line(state, cx + cosf(a1) * radius, cy + sinf(a1) * radius,
             cx + cosf(a2) * radius, cy + sinf(a2) * radius, r, g, b, a);
    }
}

static void draw_log(DeadSpeak *state, float panel_r, float panel_g, float panel_b,
                     float mint_r, float mint_g, float mint_b, float muted_r, float muted_g, float muted_b) {
    rect(state, 18, 150, state->width - 36, state->height - 270, panel_r, panel_g, panel_b, 1.0f);
    text(state, "RECENT SENSOR WORDS", 34, 172, 1.8f, mint_r, mint_g, mint_b, 1.0f);
    if (state->log_count == 0) {
        text(state, "NO WORDS LOGGED YET", 34, 220, 1.5f, muted_r, muted_g, muted_b, 1.0f);
        text(state, "ENABLE SCAN TO COLLECT READINGS", 34, 250, 1.1f, muted_r, muted_g, muted_b, 1.0f);
    } else {
        const unsigned visible = state->log_count < 4U ? state->log_count : 4U;
        for (unsigned i = 0; i < visible; ++i) {
            const unsigned slot = (state->log_next + DS_LOG_CAPACITY - 1U - i) % DS_LOG_CAPACITY;
            const float y = 220.0f + (float)i * 42.0f;
            char details[64];
            snprintf(details, sizeof(details), "SAMPLE %u  FIELD %.1f UT", state->word_samples[slot], state->word_fields[slot]);
            text(state, state->words[slot], 34, y, 2.0f, mint_r, mint_g, mint_b, 1.0f);
            text(state, details, 34, y + 19, 1.0f, muted_r, muted_g, muted_b, 1.0f);
            rect(state, 34, y + 34, state->width - 68, 1, 0.17f, 0.24f, 0.23f, 1.0f);
        }
    }
    text(state, "SAVED CAPTURES", 34, 426, 1.6f, mint_r, mint_g, mint_b, 1.0f);
    if (state->media_count == 0U) text(state, "NO AUDIO OR VIDEO FILES", 34, 460, 1.1f, muted_r, muted_g, muted_b, 1.0f);
    for (unsigned i = 0; i < state->media_count && i < 5U; ++i) {
        text(state, state->media_files[i], 34, 462.0f + (float)i * 34.0f, 1.0f, muted_r, muted_g, muted_b, 1.0f);
    }
}

static void draw_guide(DeadSpeak *state, float panel_r, float panel_g, float panel_b,
                       float mint_r, float mint_g, float mint_b, float muted_r, float muted_g, float muted_b,
                       float amber_r, float amber_g, float amber_b) {
    rect(state, 18, 150, state->width - 36, state->height - 270, panel_r, panel_g, panel_b, 1.0f);
    text(state, "FIELD NOTES", 34, 174, 2.2f, mint_r, mint_g, mint_b, 1.0f);
    text(state, "MAGNETIC READINGS CHANGE NEAR", 34, 224, 1.15f, muted_r, muted_g, muted_b, 1.0f);
    text(state, "METAL ELECTRONICS AND MOVEMENT", 34, 246, 1.15f, muted_r, muted_g, muted_b, 1.0f);
    text(state, "WORDS ARE SENSOR OUTPUTS", 34, 292, 1.35f, mint_r, mint_g, mint_b, 1.0f);
    text(state, "NOT VERIFIED COMMUNICATION", 34, 316, 1.15f, muted_r, muted_g, muted_b, 1.0f);
    text(state, "PAIR READINGS WITH OTHER TOOLS", 34, 362, 1.15f, muted_r, muted_g, muted_b, 1.0f);
    text(state, "AND CHECK ORDINARY CAUSES", 34, 384, 1.15f, muted_r, muted_g, muted_b, 1.0f);
    text(state, "NEVER USE OUTPUT FOR IMPORTANT", 34, 430, 1.05f, amber_r, amber_g, amber_b, 1.0f);
    text(state, "LIFE DECISIONS", 34, 452, 1.05f, amber_r, amber_g, amber_b, 1.0f);
    text(state, "EVP CAPTURE SAVES MONO WAV AUDIO", 34, 510, 1.0f, muted_r, muted_g, muted_b, 1.0f);
    text(state, "VIDEO SAVES MP4 UP TO 1080P", 34, 532, 1.0f, muted_r, muted_g, muted_b, 1.0f);
    text(state, "AUDIO AND VIDEO ARE SEPARATE FILES", 34, 554, 0.95f, muted_r, muted_g, muted_b, 1.0f);
    text(state, "NIGHT BOOSTS CAMERA EXPOSURE", 34, 576, 0.95f, amber_r, amber_g, amber_b, 1.0f);
    text(state, "MONO AND SEPIA NEED CAMERA SUPPORT", 34, 598, 0.8f, muted_r, muted_g, muted_b, 1.0f);
}

static void draw_explore(DeadSpeak *state, float panel_r, float panel_g, float panel_b,
                         float mint_r, float mint_g, float mint_b, float muted_r, float muted_g, float muted_b,
                         float amber_r, float amber_g, float amber_b) {
    rect(state, 18, 150, state->width - 36, state->height - 270, panel_r, panel_g, panel_b, 1.0f);
    text(state, "WIKIPEDIA FIELD INDEX", 34, 170, 1.7f, mint_r, mint_g, mint_b, 1.0f);
    text(state, "REFRESH", state->width - 92.0f, 170, 0.95f, amber_r, amber_g, amber_b, 1.0f);
    text(state, "REPORTED HAUNTED LOCATIONS", 34, 192, 1.0f, muted_r, muted_g, muted_b, 1.0f);
    rect(state, 30, 211, state->width - 60, 36, 0.13f, 0.18f, 0.18f, 1.0f);
    text(state, state->explore_query[0] ? state->explore_query : "TAP TO SEARCH", 42, 223, 1.4f,
         state->explore_query[0] ? mint_r : muted_r, state->explore_query[0] ? mint_g : muted_g,
         state->explore_query[0] ? mint_b : muted_b, 1.0f);

    DSExplorePlace places[DS_EXPLORE_PAGE_SIZE];
    size_t match_count = 0;
    const size_t count = ds_explore_page(&state->explore, state->explore_query,
                                        state->explore_page, places, &match_count);
    if (atomic_load(&state->explore.loading) && match_count == 0) {
        text(state, "LOADING PUBLIC PLACE INDEX", 34, 285, 1.3f, amber_r, amber_g, amber_b, 1.0f);
        text(state, "WIKIPEDIA COMMUNITY CATEGORIES", 34, 315, 1.0f, muted_r, muted_g, muted_b, 1.0f);
    } else if (match_count == 0) {
        text(state, atomic_load(&state->explore.failed) ? "INDEX UNAVAILABLE - TAP REFRESH" : "NO MATCHING PLACES",
             34, 285, 1.1f, amber_r, amber_g, amber_b, 1.0f);
    }
    for (size_t i = 0; i < count; ++i) {
        const float y = 275.0f + (float)i * 62.0f;
        text(state, places[i].title, 34, y, 1.4f, mint_r, mint_g, mint_b, 1.0f);
        char source_line[80];
        snprintf(source_line, sizeof(source_line), "%s  %s", places[i].region,
                 places[i].wikidata_id[0] ? places[i].wikidata_id : "NO WIKIDATA ITEM");
        text(state, source_line, 34, y + 20.0f, 0.8f, muted_r, muted_g, muted_b, 1.0f);
        if (places[i].has_coordinates) {
            char coordinate_line[64];
            snprintf(coordinate_line, sizeof(coordinate_line), "LAT %.0f  LON %.0f",
                     places[i].latitude, places[i].longitude);
            text(state, coordinate_line, 34, y + 36.0f, 0.75f, amber_r, amber_g, amber_b, 1.0f);
        }
        rect(state, 34, y + 45.0f, state->width - 68, 1.0f, 0.17f, 0.24f, 0.23f, 1.0f);
    }
    char page_label[48];
    snprintf(page_label, sizeof(page_label), "MATCHES %u  PAGE %u", (unsigned)match_count,
             (unsigned)(state->explore_page + 1U));
    text(state, page_label, 34, state->height - 174.0f, 0.95f, muted_r, muted_g, muted_b, 1.0f);
    text(state, "PREV", 34, state->height - 143.0f, 1.2f, muted_r, muted_g, muted_b, 1.0f);
    text(state, "NEXT", state->width - 90.0f, state->height - 143.0f, 1.2f, mint_r, mint_g, mint_b, 1.0f);
    text(state, "TOP: WIKIPEDIA  LOWER: WIKIDATA", 34, state->height - 113.0f,
         0.82f, amber_r, amber_g, amber_b, 1.0f);
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
    const float now = (float)(now_millis() % 10000ULL) / 10000.0f;
    state->sweep = now * 6.2831853f;

    glClearColor(0.027f, 0.043f, 0.047f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    rect(state, 18, 22, 22, 22, mint_r, mint_g, mint_b, 0.9f);
    rect(state, 23, 27, 12, 12, 0.027f, 0.043f, 0.047f, 1.0f);
    text(state, "DEADSPEAK", 50, 24, 3.0f, mint_r, mint_g, mint_b, 1.0f);
    text(state, "EXALTED6165 LABS", 50, 49, 1.3f, muted_r, muted_g, muted_b, 1.0f);
    text(state, "RAW SENSOR FIELDWORK", w - 138, 29, 1.25f, muted_r, muted_g, muted_b, 1.0f);
    rect(state, 18, 72, w - 36, 1, 0.19f, 0.27f, 0.26f, 0.9f);

    const char *screen_title = state->tab == 0 ? "FIELD SCAN" : (state->tab == 1 ? "EVIDENCE LOG" :
                               (state->tab == 2 ? "EXPLORE" : "FIELD GUIDE"));
    text(state, screen_title, 22, 94, 3.4f, 0.95f, 0.98f, 0.94f, 1.0f);
    if (state->tab == 2) {
        text(state, "PUBLIC REPORTS - NOT VERIFIED CLAIMS", 22, 122, 1.05f, amber_r, amber_g, amber_b, 1.0f);
    } else {
        text(state, state->has_magnetometer ? "MAGNETOMETER ONLINE" : "MAGNETOMETER UNAVAILABLE", 22, 122, 1.5f, state->has_magnetometer ? mint_r : amber_r, state->has_magnetometer ? mint_g : amber_g, state->has_magnetometer ? mint_b : amber_b, 1.0f);
    }

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

    rect(state, 18, 150, w - 36, 300, panel_r, panel_g, panel_b, 1.0f);
    text(state, "MAGNETIC FIELD", 36, 171, 1.55f, muted_r, muted_g, muted_b, 1.0f);
    text(state, "RAW MICROTESLA", w - 124, 171, 1.25f, muted_r, muted_g, muted_b, 1.0f);

    const float cx = w * 0.5f;
    const float cy = 295.0f;
    circle(state, cx, cy, 92, 0.18f, 0.31f, 0.29f, 0.8f);
    circle(state, cx, cy, 62, 0.14f, 0.24f, 0.23f, 0.9f);
    line(state, cx - 104, cy, cx + 104, cy, 0.13f, 0.23f, 0.22f, 0.9f);
    line(state, cx, cy - 104, cx, cy + 104, 0.13f, 0.23f, 0.22f, 0.9f);
    line(state, cx, cy, cx + cosf(state->sweep) * 94, cy + sinf(state->sweep) * 94, mint_r, mint_g, mint_b, 0.9f);
    circle(state, cx, cy, 5, mint_r, mint_g, mint_b, 1.0f);

    char value[32];
    snprintf(value, sizeof(value), "%05.1f", state->smoothed_field);
    text(state, value, cx - 44, 263, 5.0f, 0.95f, 0.98f, 0.94f, 1.0f);
    text(state, "uT", cx + 50, 281, 1.6f, muted_r, muted_g, muted_b, 1.0f);

    const float signal_width = (w - 72) * state->signal;
    rect(state, 36, 414, w - 72, 4, 0.14f, 0.20f, 0.20f, 1.0f);
    rect(state, 36, 414, signal_width, 4, mint_r, mint_g, mint_b, 1.0f);
    text(state, "BASELINE 47.0", 36, 430, 1.2f, muted_r, muted_g, muted_b, 1.0f);
    text(state, state->scanning ? "SCANNING" : "STANDBY", w - 90, 430, 1.2f, state->scanning ? mint_r : amber_r, state->scanning ? mint_g : amber_g, state->scanning ? mint_b : amber_b, 1.0f);

    rect(state, 18, 470, (w - 48) * 0.53f, 130, panel_r, panel_g, panel_b, 1.0f);
    text(state, "SIGNAL WORD", 32, 490, 1.4f, muted_r, muted_g, muted_b, 1.0f);
    text(state, state->word[0] ? state->word : "QUIET", 32, 526, 3.8f, mint_r, mint_g, mint_b, 1.0f);
    text(state, "ENVIRONMENTAL TRIGGER", 32, 575, 1.15f, muted_r, muted_g, muted_b, 1.0f);

    const float second_x = 30 + (w - 48) * 0.53f;
    rect(state, second_x, 470, (w - 48) * 0.47f, 130, panel_r, panel_g, panel_b, 1.0f);
    text(state, "CAPTURE", second_x + 14, 490, 1.4f, muted_r, muted_g, muted_b, 1.0f);
    const bool audio_active = atomic_load(&state->audio_active);
    const bool video_active = ds_video_active(&state->video);
    const bool capture_active = audio_active || video_active;
    const bool capture_failed = atomic_load(&state->audio_failed) || ds_video_failed(&state->video);
    const char *capture_label = capture_active ? "LIVE" : (state->capture_requested ? "ALLOW ACCESS" : (capture_failed ? "ERROR" : "READY"));
    text(state, capture_label, second_x + 14, 526, 3.2f, capture_active ? amber_r : (capture_failed ? amber_r : mint_r), capture_active ? amber_g : (capture_failed ? amber_g : mint_g), capture_active ? amber_b : (capture_failed ? amber_b : mint_b), 1.0f);
    static const char *const filter_names[] = {"NIGHT", "MONO", "SEPIA", "RAW"};
    char filter_label[32];
    snprintf(filter_label, sizeof(filter_label), "FILTER: %s", filter_names[state->video_filter]);
    text(state, filter_label, second_x + 14, 575, 1.05f, muted_r, muted_g, muted_b, 1.0f);
    const unsigned audio_level = atomic_load(&state->audio_level_milli);
    rect(state, 18, 620, w - 36, 76, panel_r, panel_g, panel_b, 1.0f);
    text(state, "MIC", 32, 630, 1.0f, muted_r, muted_g, muted_b, 1.0f);
    rect(state, 70, 635, w - 108, 5, 0.14f, 0.20f, 0.20f, 1.0f);
    rect(state, 70, 635, (w - 108) * ((float)audio_level / 1000.0f), 5, mint_r, mint_g, mint_b, 1.0f);
    const char *band_names[3] = {"LOW", "MID", "HIGH"};
    const float band_width = (w - 92) / 3.0f;
    for (unsigned band = 0; band < 3; ++band) {
        const float band_x = 32.0f + (float)band * band_width;
        const unsigned band_level = atomic_load(&state->spectrum_milli[band]);
        text(state, band_names[band], band_x, 657, 0.9f, muted_r, muted_g, muted_b, 1.0f);
        rect(state, band_x, 675, band_width - 14, 5, 0.14f, 0.20f, 0.20f, 1.0f);
        rect(state, band_x, 675, (band_width - 14) * ((float)band_level / 1000.0f), 5, mint_r, mint_g, mint_b, 1.0f);
    }

navigation:
    ;
    const float nav_y = h - 92;
    rect(state, 18, nav_y, w - 36, 1, 0.19f, 0.27f, 0.26f, 0.9f);
    text(state, "SCAN", w * 0.08f, nav_y + 25, 1.2f, state->tab == 0 ? mint_r : muted_r, state->tab == 0 ? mint_g : muted_g, state->tab == 0 ? mint_b : muted_b, 1.0f);
    text(state, "LOG", w * 0.32f, nav_y + 25, 1.2f, state->tab == 1 ? mint_r : muted_r, state->tab == 1 ? mint_g : muted_g, state->tab == 1 ? mint_b : muted_b, 1.0f);
    text(state, "EXPLORE", w * 0.51f, nav_y + 25, 1.1f, state->tab == 2 ? mint_r : muted_r, state->tab == 2 ? mint_g : muted_g, state->tab == 2 ? mint_b : muted_b, 1.0f);
    text(state, "GUIDE", w * 0.81f, nav_y + 25, 1.2f, state->tab == 3 ? mint_r : muted_r, state->tab == 3 ? mint_g : muted_g, state->tab == 3 ? mint_b : muted_b, 1.0f);
    text(state, "SKEPTICISM IS A FEATURE", 22, h - 39, 1.1f, muted_r, muted_g, muted_b, 1.0f);

    eglSwapBuffers(state->display, state->surface);
}

static void handle_cmd(struct android_app *app, int32_t cmd) {
    DeadSpeak *state = (DeadSpeak *)app->userData;
    if (cmd == APP_CMD_INIT_WINDOW && app->window != NULL) setup_display(state);
    else if (cmd == APP_CMD_TERM_WINDOW) teardown_display(state);
    else if (cmd == APP_CMD_PAUSE) {
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
    if ((AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK) != AMOTION_EVENT_ACTION_UP) return 1;
    const float x = AMotionEvent_getX(event, 0);
    const float y = AMotionEvent_getY(event, 0);
    const float nav_y = state->height - 110.0f;
    if (y > nav_y) {
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
    } else if (state->tab == 2 && y >= 155.0f && y <= 194.0f && x > state->width - 115.0f) {
        ds_explore_refresh(&state->explore, state->app->activity, state->explore_cache_path);
    } else if (state->tab == 2 && y >= 207.0f && y <= 252.0f) {
        state->explore_search_active = true;
        ANativeActivity_showSoftInput(state->app->activity, ANATIVEACTIVITY_SHOW_SOFT_INPUT_IMPLICIT);
    } else if (state->tab == 2 && y >= state->height - 166.0f && y <= state->height - 128.0f) {
        DSExplorePlace places[DS_EXPLORE_PAGE_SIZE];
        size_t match_count = 0;
        (void)ds_explore_page(&state->explore, state->explore_query, 0, places, &match_count);
        const size_t last_page = match_count == 0 ? 0 : (match_count - 1U) / DS_EXPLORE_PAGE_SIZE;
        if (x > state->width * 0.5f && state->explore_page < last_page) ++state->explore_page;
        else if (x <= state->width * 0.5f && state->explore_page > 0) --state->explore_page;
    } else if (state->tab == 2 && y >= 268.0f && y < 268.0f + DS_EXPLORE_PAGE_SIZE * 62.0f) {
        const unsigned row = (unsigned)((y - 268.0f) / 62.0f);
        DSExplorePlace places[DS_EXPLORE_PAGE_SIZE];
        size_t match_count = 0;
        const size_t count = ds_explore_page(&state->explore, state->explore_query,
                                            state->explore_page, places, &match_count);
        if (row < count) {
            state->explore_search_active = false;
            ANativeActivity_hideSoftInput(state->app->activity, ANATIVEACTIVITY_HIDE_SOFT_INPUT_NOT_ALWAYS);
            if ((y - 268.0f - (float)row * 62.0f) >= 29.0f && places[row].wikidata_id[0])
                ds_explore_open_wikidata(state->app->activity, places[row].wikidata_id);
            else
                ds_explore_open_article(state->app->activity, places[row].page_id);
        }
    } else if (state->tab == 0 && y >= 556.0f && y < 610.0f && x > state->width * 0.52f) {
        state->video_filter = (DSVideoFilter)(((unsigned)state->video_filter + 1U) % 4U);
    } else if (state->tab == 0 && y > 460.0f && y < 556.0f && x > state->width * 0.52f) {
        if (atomic_load(&state->audio_active) || ds_video_active(&state->video) || state->capture_requested) {
            state->capture_requested = false;
            state->permission_requested = false;
            stop_audio_capture(state);
            ds_video_stop(&state->video);
        } else if (has_capture_permissions(state)) {
            start_audio_capture(state);
            ds_video_start(&state->video, state->evidence_dir, state->video_filter);
        } else {
            state->capture_requested = true;
            state->permission_deadline = now_millis() + 15000U;
            request_audio_permission(state);
        }
    } else if (y > 220.0f && y < 390.0f) {
        state->scanning = !state->scanning;
    }
    return 1;
}

static void poll_sensor(DeadSpeak *state) {
    ASensorEvent event;
    while (state->sensor_queue && ASensorEventQueue_getEvents(state->sensor_queue, &event, 1) > 0) {
        if (event.type != ASENSOR_TYPE_MAGNETIC_FIELD) continue;
        const float x = event.magnetic.x;
        const float y = event.magnetic.y;
        const float z = event.magnetic.z;
        state->magnetic_field = sqrtf(x * x + y * y + z * z);
        state->smoothed_field = ds_engine_smooth(state->smoothed_field, state->magnetic_field, 0.15f);
        state->signal = ds_engine_signal(state->smoothed_field);
        ++state->sample_count;
        if (state->scanning && state->sample_count % 10U == 0U) {
            ds_engine_next_word(state->smoothed_field, state->sample_count, state->dictionary,
                                state->dictionary_size, state->word, sizeof(state->word));
            if (state->word[0]) append_word_log(state, state->word);
        }
    }
}

static void setup_sensors(DeadSpeak *state) {
    state->sensor_manager = ASensorManager_getInstanceForPackage("org.deadspeak");
    if (!state->sensor_manager) return;
    state->magnetometer = ASensorManager_getDefaultSensor(state->sensor_manager, ASENSOR_TYPE_MAGNETIC_FIELD);
    state->has_magnetometer = state->magnetometer != NULL;
    if (!state->has_magnetometer) return;
    state->sensor_queue = ASensorManager_createEventQueue(state->sensor_manager, ALooper_forThread(), LOOPER_ID_SENSOR, NULL, NULL);
    if (state->sensor_queue) {
        ASensorEventQueue_enableSensor(state->sensor_queue, state->magnetometer);
        ASensorEventQueue_setEventRate(state->sensor_queue, state->magnetometer, 100000);
    }
}

void android_main(struct android_app *app) {
    DeadSpeak state = {0};
    state.app = app;
    state.display = EGL_NO_DISPLAY;
    state.surface = EGL_NO_SURFACE;
    state.context = EGL_NO_CONTEXT;
    state.smoothed_field = 47.0f;
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
                ds_video_start(&state.video, state.evidence_dir, state.video_filter);
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
                free(state.dictionary);
                ds_explore_destroy(&state.explore);
                teardown_display(&state);
                return;
            }
        }
        if (state.display != EGL_NO_DISPLAY && state.surface != EGL_NO_SURFACE) draw_frame(&state);
    }
}
