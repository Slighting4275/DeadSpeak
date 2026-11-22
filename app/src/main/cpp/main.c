#include <android/asset_manager.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/sensor.h>
#include <android/window.h>
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
#include "video_shaders.h"
#include "media_player.h"
#include "ui_layout.h"
#include "sensor_state.h"
#include "camera_ui.h"
#include "custom_words.h"
#include "field_baseline.h"
#include "voice_synth.h"
#include "voice_player.h"
#include "motion_filter.h"
#include "typography.h"

#define LOG_TAG "DeadSpeak"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOOPER_ID_SENSOR 3
#define DS_LOG_CAPACITY 32
#define DS_MEDIA_CAPACITY 16
#define DS_AUDIO_RATE 48000
#define DS_EXPLORE_ROW_HEIGHT 62.0f
#define DS_EXPLORE_ROW_SOURCE 27.0f
#define DS_SENSOR_COUNT 4
#define DS_SPECTRUM_BINS 24
#define DS_PREVIEW_CAPACITY ((size_t)640 * 480 * 4)
static const int sensor_types[DS_SENSOR_COUNT] = {
    ASENSOR_TYPE_MAGNETIC_FIELD, ASENSOR_TYPE_ACCELEROMETER, ASENSOR_TYPE_GYROSCOPE, ASENSOR_TYPE_PROXIMITY
};
static const int sensor_fallback_types[DS_SENSOR_COUNT] = {
    ASENSOR_TYPE_MAGNETIC_FIELD_UNCALIBRATED,
    ASENSOR_TYPE_ACCELEROMETER_UNCALIBRATED,
    ASENSOR_TYPE_GYROSCOPE_UNCALIBRATED,
    ASENSOR_TYPE_PROXIMITY
};
static const char *const sensor_names[DS_SENSOR_COUNT] = {"MAGNETIC", "ACCEL", "GYRO", "PROXIMITY"};

#define DS_OSCILLOSCOPE_SAMPLES 64
#define DS_MAX_LOADED_TAGS 256
#define DS_MAX_WAVEFORM_BARS 64

typedef enum {
    DS_TAB_SCAN = 0,
    DS_TAB_LOG = 1,
    DS_TAB_EXPLORE = 2,
    DS_TAB_GUIDE = 3
} DSTab;

typedef enum {
    DS_THEME_ORIGINAL = 0,
    DS_THEME_ABDUCTION = 1,
    DS_THEME_AI = 2
} DSThemeMode;

typedef enum {
    DS_TAG_WORD = 1,
    DS_TAG_ANOMALY = 2,
    DS_TAG_BOOKMARK = 3
} DSTagId;

typedef struct {
    uint64_t timestamp_ms;
    uint8_t tag_id;
} DSTagEntry;

typedef struct {
    float r, g, b;
} DSColor;

typedef struct {
    DSColor panel, mint, muted, amber;
} DSPalette;

/* 7-stop ColorSlider interpolator */
static const DSColor ds_color_stops[7] = {
    {1.0f, 1.0f, 1.0f}, /* Stop 0: White */
    {1.0f, 0.0f, 0.0f}, /* Stop 1: Red */
    {1.0f, 1.0f, 0.0f}, /* Stop 2: Yellow */
    {0.0f, 1.0f, 0.0f}, /* Stop 3: Green */
    {0.0f, 1.0f, 1.0f}, /* Stop 4: Cyan */
    {0.0f, 0.0f, 1.0f}, /* Stop 5: Blue */
    {1.0f, 0.0f, 1.0f}  /* Stop 6: Magenta */
};

static DSColor ds_interpolate_hud_accent(float progress) {
    if (progress <= 0.0f) return ds_color_stops[0];
    if (progress >= 1.0f) return ds_color_stops[6];
    const float scaled = progress * 6.0f;
    const int idx = (int)scaled;
    const float f = scaled - (float)idx;
    const int next_idx = idx < 6 ? idx + 1 : 6;
    return (DSColor){
        ds_color_stops[idx].r + (ds_color_stops[next_idx].r - ds_color_stops[idx].r) * f,
        ds_color_stops[idx].g + (ds_color_stops[next_idx].g - ds_color_stops[idx].g) * f,
        ds_color_stops[idx].b + (ds_color_stops[next_idx].b - ds_color_stops[idx].b) * f
    };
}

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
    bool minimal_hud;
    GLuint program;
    GLint position;
    GLint color;
    GLuint camera_program, camera_texture;
    GLint camera_filter_loc, camera_image_loc;
    GLuint text_program, text_texture;
    GLint text_pos_loc, text_uv_loc, text_color_loc, text_sampler_loc;
    bool text_engine_ready;
    uint8_t *preview_rgba;
    int preview_width, preview_height;
    int64_t preview_timestamp;
    uint64_t preview_received_ms, capture_started_ms, next_camera_check;
    bool camera_enabled, camera_permission_pending, sensor_details;
    int preview_rotation;
    DSCameraUI camera_ui;
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
    float emf_peak_signal;
    uint64_t emf_peak_decay_ms;
    DSFieldBaseline baseline;
    DSMotionFilter motion_filter;
    uint32_t sample_count;
    char word[32];
    bool scanning;
    bool proximity_active;
    float proximity_distance;
    bool capture_requested;
    bool permission_requested;
    uint64_t permission_deadline;
    bool audio_thread_created;
    pthread_t audio_thread;
    _Atomic bool audio_active;
    _Atomic bool audio_failed;
    _Atomic uint32_t audio_level_milli;
    float audio_peak_level;
    uint64_t audio_peak_decay_ms;
    _Atomic uint32_t spectrum_milli[3];
    _Atomic uint32_t spectrum_bins[DS_SPECTRUM_BINS];
    _Atomic int16_t oscilloscope[DS_OSCILLOSCOPE_SAMPLES];
    DSVoiceConfig voice_config;
    DSVoicePlayer *voice_player;
    DSMediaPlayer *media_player;
    GLuint mp4_texture;
    uint8_t *mp4_rgba;
    int mp4_width, mp4_height, mp4_rotation;
    DSVideoCapture video;
    DSVideoFilter video_filter;
    DSVideoAspect video_aspect;
    bool hud_burn_in;
    uint64_t filter_hud_deadline;
    uint64_t guide_voice_last_ms;
    char guide_voice_label[48];
    DSExplore explore;
    char explore_cache_path[512];
    char explore_query[48];
    size_t explore_page;
    bool explore_search_active;
    char evidence_dir[512];
    char word_log_path[512];
    char active_words_path[512];
    char active_tags_path[512];
    char active_waveform_path[512];
    uint64_t recording_start_ms;
    uint64_t last_anomaly_tag_ms;
    _Atomic bool sub_bass_anomaly;
    uint64_t sub_bass_anomaly_expiry_ms;
    float sub_bass_baseline;
    float sub_bass_level;
    DSThemeMode theme_mode;
    uint8_t hud_tint_step;
    float hud_hue;
    DSTagEntry loaded_tags[DS_MAX_LOADED_TAGS];
    size_t loaded_tags_count;
    char loaded_tags_filename[96];
    float loaded_waveform[DS_MAX_WAVEFORM_BARS];
    size_t loaded_waveform_count;
    uint64_t last_tag_seek_ms;
    char export_status_message[128];
    uint64_t export_status_expiry_ms;
    char words[DS_LOG_CAPACITY][32];
    unsigned word_samples[DS_LOG_CAPACITY];
    float word_fields[DS_LOG_CAPACITY];
    float word_deltas[DS_LOG_CAPACITY];
    float word_x[DS_LOG_CAPACITY];
    float word_y[DS_LOG_CAPACITY];
    float word_z[DS_LOG_CAPACITY];
    uint64_t word_times[DS_LOG_CAPACITY];
    bool word_whispers[DS_LOG_CAPACITY];
    char media_files[DS_MEDIA_CAPACITY][96];
    time_t media_mtimes[DS_MEDIA_CAPACITY];
    unsigned media_count;
    uint64_t next_media_refresh;
    uint8_t *dictionary;
    size_t dictionary_size;
    DSCustomWords custom_words;
    uint8_t custom_blob[DS_CUSTOM_WORDS_BLOB_MAX_SIZE];
    size_t custom_blob_size, custom_page;
    char custom_path[512], custom_input[32], custom_message[64];
    bool custom_open, custom_selected, custom_editing;
    unsigned log_count;
    unsigned log_next;
    DSTab tab;
} DeadSpeak;

static void start_sensors(DeadSpeak *state);
static void stop_sensors(DeadSpeak *state, DSSensorStatus status);
static void maintain_sensors(DeadSpeak *state);
static void maintain_camera(DeadSpeak *state);
static uint64_t now_millis(void);
static int64_t sensor_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
}

/* Companion tag emitter (tag triad: 1=Word, 2=Anomaly, 3=Bookmark) */
static void emit_companion_tag(DeadSpeak *state, DSTagId tag_id) {
    if (!state || !state->active_tags_path[0]) return;
    const uint64_t now = now_millis();
    /* Debounce anomaly events (DS_TAG_ANOMALY) by 1000ms */
    if (tag_id == DS_TAG_ANOMALY) {
        if (state->last_anomaly_tag_ms && now < state->last_anomaly_tag_ms + 1000U) return;
        state->last_anomaly_tag_ms = now;
    }
    const uint64_t rel_ms = now >= state->recording_start_ms ? (now - state->recording_start_ms) : 0U;
    FILE *tf = fopen(state->active_tags_path, "a");
    if (tf) {
        fprintf(tf, "%llu,%u\n", (unsigned long long)rel_ms, (unsigned)tag_id);
        fclose(tf);
    }
}

static const char *voice_mode_name(DSVoiceMode mode) {
    switch (mode) {
        case DS_VOICE_MODE_WHISPER:    return "WHISPER";
        case DS_VOICE_MODE_DYNAMIC:    return "DYNAMIC";
        case DS_VOICE_MODE_SPIRIT_BOX: return "SPIRIT BOX";
        case DS_VOICE_MODE_CLASSIC:
        default:                       return "CLASSIC";
    }
}

static void append_word_log(DeadSpeak *state, const char *word) {
    if (!word || !word[0]) return;
    const unsigned slot = state->log_next;
    snprintf(state->words[slot], sizeof(state->words[slot]), "%s", word);
    state->word_samples[slot] = state->sample_count;
    state->word_fields[slot] = state->smoothed_field;
    state->word_deltas[slot] = state->baseline.triggered_delta;
    state->word_x[slot] = state->baseline.triggered_x;
    state->word_y[slot] = state->baseline.triggered_y;
    state->word_z[slot] = state->baseline.triggered_z;
    state->word_times[slot] = now_millis();
    state->word_whispers[slot] = (state->voice_config.mode == DS_VOICE_MODE_WHISPER);
    state->log_next = (slot + 1U) % DS_LOG_CAPACITY;
    if (state->log_count < DS_LOG_CAPACITY) ++state->log_count;

    /* 1. Standard CSV log */
    FILE *file = fopen(state->word_log_path, "a");
    if (file) {
        fprintf(file, "%u,%.2f,%s\n", state->sample_count, state->smoothed_field, word);
        fclose(file);
    }

    time_t now_sec = time(NULL);
    struct tm lt;
    localtime_r(&now_sec, &lt);
    char date_buf[32], time_buf[32];
    strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &lt);
    strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &lt);

    /* 2. 100-entry bounded circular log history (log.txt) */
    char gt_log_path[640];
    snprintf(gt_log_path, sizeof(gt_log_path), "%s/log.txt", state->evidence_dir);
    char lines[101][128];
    size_t line_count = 0;
    FILE *rlog = fopen(gt_log_path, "r");
    if (rlog) {
        while (line_count < 100 && fgets(lines[line_count], sizeof(lines[line_count]), rlog)) {
            size_t llen = strlen(lines[line_count]);
            if (llen > 0 && lines[line_count][llen - 1] == '\n') lines[line_count][llen - 1] = '\0';
            if (lines[line_count][0]) ++line_count;
        }
        fclose(rlog);
    }
    char new_entry[128];
    snprintf(new_entry, sizeof(new_entry), "%s|%s|%s|%s", date_buf, time_buf, word, word);
    if (line_count >= 100) {
        for (size_t l = 0; l < 99; ++l) {
            snprintf(lines[l], sizeof(lines[l]), "%s", lines[l + 1]);
        }
        snprintf(lines[99], sizeof(lines[99]), "%s", new_entry);
        line_count = 100;
    } else {
        snprintf(lines[line_count], sizeof(lines[line_count]), "%s", new_entry);
        ++line_count;
    }
    FILE *wlog = fopen(gt_log_path, "w");
    if (wlog) {
        for (size_t l = 0; l < line_count; ++l) {
            fprintf(wlog, "%s\n", lines[l]);
        }
        fclose(wlog);
    }

    /* 3. Session word history export */
    char wh_path[640];
    snprintf(wh_path, sizeof(wh_path), "%s/DeadSpeakWordHistory.txt", state->evidence_dir);
    FILE *wh_file = fopen(wh_path, "a");
    if (wh_file) {
        if (fseek(wh_file, 0, SEEK_END) == 0 && ftell(wh_file) == 0) {
            fprintf(wh_file, "DeadSpeak session word history export\n\nDate|Time|Generated word(s)|Translated\n");
        }
        fprintf(wh_file, "%s|%s|%s|%s\n", date_buf, time_buf, word, word);
        fclose(wh_file);
    }

    /* 4. DeadSpeak Session Export */
    char ds_path[640];
    snprintf(ds_path, sizeof(ds_path), "%s/DeadSpeakSessionExport.txt", state->evidence_dir);
    FILE *ds_file = fopen(ds_path, "a");
    if (ds_file) {
        if (fseek(ds_file, 0, SEEK_END) == 0 && ftell(ds_file) == 0) {
            fprintf(ds_file, "DeadSpeak Anomaly Detection Session Export\n\nDate|Time|Word|Field_uT|Delta_uT|Vector|Mode\n");
        }
        fprintf(ds_file, "%s|%s|%s|%.1f|%+.1f|X:%.1f,Y:%.1f,Z:%.1f|%s\n",
                date_buf, time_buf, word, state->smoothed_field, state->baseline.triggered_delta,
                state->baseline.triggered_x, state->baseline.triggered_y, state->baseline.triggered_z,
                voice_mode_name(state->voice_config.mode));
        fclose(ds_file);
    }

    /* 5. Active recording companion word list (<recording>.txt) */
    if (state->active_words_path[0]) {
        FILE *wf = fopen(state->active_words_path, "a");
        if (wf) {
            fprintf(wf, "%s\n", word);
            fclose(wf);
        }
    }

    /* 6. Active recording companion tags file (<recording>_tags.txt) - Tag 1 (Word) */
    emit_companion_tag(state, DS_TAG_WORD);
}

static void copy_file_data(const char *src, const char *dst) {
    if (!src || !dst || strcmp(src, dst) == 0) return;
    FILE *in = fopen(src, "rb");
    if (!in) return;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return; }
    char buf[1024];
    while (!feof(in) && !ferror(in)) {
        const size_t n = fread(buf, 1, sizeof(buf), in);
        if (n > 0) {
            if (fwrite(buf, 1, n, out) != n) break;
        }
        if (n < sizeof(buf)) break;
    }
    fflush(out);
    fsync(fileno(out));
    fclose(in);
    fclose(out);
}

static void refresh_media_files(DeadSpeak *state);

static void export_session_files(DeadSpeak *state) {
    if (!state) return;
    char ext_dir[512] = {0};
    const char *target_dir = NULL;
    if (state->app && state->app->activity && state->app->activity->externalDataPath) {
        snprintf(ext_dir, sizeof(ext_dir), "%s/Movies", state->app->activity->externalDataPath);
        mkdir(ext_dir, 0755);
        target_dir = ext_dir;
    } else {
        target_dir = state->evidence_dir;
    }

    char wh_src[640], wh_dst[640];
    snprintf(wh_src, sizeof(wh_src), "%s/DeadSpeakWordHistory.txt", state->evidence_dir);
    snprintf(wh_dst, sizeof(wh_dst), "%s/DeadSpeakWordHistory.txt", target_dir);

    char log_src[640], log_dst[640];
    snprintf(log_src, sizeof(log_src), "%s/log.txt", state->evidence_dir);
    snprintf(log_dst, sizeof(log_dst), "%s/log.txt", target_dir);

    char ds_src[640], ds_dst[640];
    snprintf(ds_src, sizeof(ds_src), "%s/DeadSpeakSessionExport.txt", state->evidence_dir);
    snprintf(ds_dst, sizeof(ds_dst), "%s/DeadSpeakSessionExport.txt", target_dir);

    time_t now_sec = time(NULL);
    struct tm lt;
    localtime_r(&now_sec, &lt);
    char date_buf[32], time_buf[32];
    strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &lt);
    strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &lt);

    if (state->log_count > 0) {
        FILE *src_f = fopen(wh_src, "wx");
        if (src_f) {
            fprintf(src_f, "DeadSpeak session word history export\n\nDate|Time|Generated word(s)|Translated\n");
            for (unsigned i = 0; i < state->log_count; ++i) {
                const unsigned slot = (state->log_next + DS_LOG_CAPACITY - state->log_count + i) % DS_LOG_CAPACITY;
                if (state->words[slot][0]) {
                    fprintf(src_f, "%s|%s|%s|%s\n", date_buf, time_buf, state->words[slot], state->words[slot]);
                }
            }
            fclose(src_f);
        }
        FILE *log_f = fopen(log_src, "wx");
        if (log_f) {
            for (unsigned i = 0; i < state->log_count; ++i) {
                const unsigned slot = (state->log_next + DS_LOG_CAPACITY - state->log_count + i) % DS_LOG_CAPACITY;
                if (state->words[slot][0]) {
                    fprintf(log_f, "%s|%s|%s|%s\n", date_buf, time_buf, state->words[slot], state->words[slot]);
                }
            }
            fclose(log_f);
        }
        FILE *ds_f = fopen(ds_src, "wx");
        if (ds_f) {
            fprintf(ds_f, "DeadSpeak Anomaly Detection Session Export\n\nDate|Time|Word|Field_uT|Delta_uT|Vector|Mode\n");
            for (unsigned i = 0; i < state->log_count; ++i) {
                const unsigned slot = (state->log_next + DS_LOG_CAPACITY - state->log_count + i) % DS_LOG_CAPACITY;
                if (state->words[slot][0]) {
                    fprintf(ds_f, "%s|%s|%s|%.1f|%+.1f|X:%.1f,Y:%.1f,Z:%.1f|%s\n",
                        date_buf, time_buf, state->words[slot],
                        state->word_fields[slot], state->word_deltas[slot],
                        state->word_x[slot], state->word_y[slot], state->word_z[slot],
                        state->word_whispers[slot] ? "WHISPER" : "CLASSIC");
                }
            }
            fclose(ds_f);
        }
    }

    if (strcmp(wh_src, wh_dst) != 0) copy_file_data(wh_src, wh_dst);
    if (strcmp(log_src, log_dst) != 0) copy_file_data(log_src, log_dst);
    if (strcmp(ds_src, ds_dst) != 0) copy_file_data(ds_src, ds_dst);

    snprintf(state->export_status_message, sizeof(state->export_status_message),
        "EXPORTED SESSIONS TO %s", target_dir == ext_dir ? "MOVIES/" : "EVIDENCE/");
    state->export_status_expiry_ms = now_millis() + 4000U;
    refresh_media_files(state);
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
        state->word_deltas[slot] = 0.0f;
        state->word_times[slot] = now_millis();
        state->word_whispers[slot] = false;
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

static void scan_directory_for_media(DeadSpeak *state, const char *dir_path) {
    if (!dir_path) return;
    DIR *directory = opendir(dir_path);
    if (!directory) return;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        const size_t name_length = strlen(entry->d_name);
        if (name_length < 5) continue;
        const bool is_wav = strcmp(entry->d_name + name_length - 4, ".wav") == 0;
        const bool is_mp4 = strcmp(entry->d_name + name_length - 4, ".mp4") == 0;
        const bool is_txt = strcmp(entry->d_name + name_length - 4, ".txt") == 0;
        if (!is_wav && !is_mp4 && !is_txt) continue;

        /* Companion file exclusion: do not show tags or waveform logs separately */
        if (strstr(entry->d_name, "_tags.txt") || strstr(entry->d_name, "_waveform.txt")) continue;
        if (is_txt) {
            char base_prefix[640];
            snprintf(base_prefix, sizeof(base_prefix), "%s/%.*s", dir_path, (int)(name_length - 4), entry->d_name);
            char check_wav[648], check_mp4[648];
            snprintf(check_wav, sizeof(check_wav), "%s.wav", base_prefix);
            snprintf(check_mp4, sizeof(check_mp4), "%s.mp4", base_prefix);
            if (access(check_wav, F_OK) == 0 || access(check_mp4, F_OK) == 0) continue;
        }

        char path[640];
        snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);
        struct stat file_status;
        if (stat(path, &file_status) != 0) continue;

        bool already_present = false;
        for (unsigned k = 0; k < state->media_count; ++k) {
            if (strcmp(state->media_files[k], entry->d_name) == 0) {
                already_present = true;
                break;
            }
        }
        if (already_present) continue;

        unsigned slot = state->media_count;
        if (slot == DS_MEDIA_CAPACITY && file_status.st_mtime <= state->media_mtimes[DS_MEDIA_CAPACITY - 1]) continue;
        if (slot >= DS_MEDIA_CAPACITY) slot = DS_MEDIA_CAPACITY - 1;
        else ++state->media_count;
        while (slot > 0U && file_status.st_mtime > state->media_mtimes[slot - 1U]) {
            if (slot < DS_MEDIA_CAPACITY) {
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

static void refresh_media_files(DeadSpeak *state) {
    state->media_count = 0;
    scan_directory_for_media(state, state->evidence_dir);
    if (state->app && state->app->activity && state->app->activity->externalDataPath) {
        char ext_dir[512];
        snprintf(ext_dir, sizeof(ext_dir), "%s/Movies", state->app->activity->externalDataPath);
        scan_directory_for_media(state, ext_dir);
    }
}

static void unlink_recording_set(const char *dir, const char *stem) {
    static const char *const suffixes[] = {".txt", "_tags.txt", "_waveform.txt", ".srt"};
    char c_path[640];
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
        snprintf(c_path, sizeof(c_path), "%s/%s%s", dir, stem, suffixes[i]);
        unlink(c_path);
    }
}

static bool active_media_status(const DeadSpeak *state, char *out_name, size_t max_name,
                                float *out_progress, uint64_t *out_pos_ms, uint64_t *out_dur_ms, bool *out_is_mp4) {
    if (!state) return false;
    if (ds_media_player_is_playing(state->media_player, out_name, max_name, out_progress)) {
        if (out_pos_ms) *out_pos_ms = ds_media_player_get_position_ms(state->media_player);
        if (out_dur_ms) *out_dur_ms = ds_media_player_get_duration_ms(state->media_player);
        if (out_is_mp4) *out_is_mp4 = true;
        return true;
    }
    if (ds_voice_player_is_playing_wav(state->voice_player, out_name, max_name, out_progress)) {
        if (out_pos_ms) *out_pos_ms = ds_voice_player_get_wav_position_ms(state->voice_player);
        if (out_dur_ms) *out_dur_ms = ds_voice_player_get_wav_duration_ms(state->voice_player);
        if (out_is_mp4) *out_is_mp4 = false;
        return true;
    }
    if (out_is_mp4) *out_is_mp4 = false;
    return false;
}

static void active_media_stop(DeadSpeak *state, bool stop_wav, bool stop_mp4) {
    if (!state) return;
    if (stop_wav && state->voice_player) ds_voice_player_stop_wav(state->voice_player);
    if (stop_mp4 && state->media_player) ds_media_player_stop(state->media_player);
}

static void active_media_seek_ms(DeadSpeak *state, uint64_t ms) {
    if (!state) return;
    if (ds_media_player_is_playing(state->media_player, NULL, 0, NULL)) {
        ds_media_player_seek_ms(state->media_player, ms);
    } else if (ds_voice_player_is_playing_wav(state->voice_player, NULL, 0, NULL)) {
        ds_voice_player_seek_wav_ms(state->voice_player, ms);
    }
}

static void delete_recording_and_companions(DeadSpeak *state, const char *filename) {
    if (!state || !filename || !filename[0]) return;
    char curr_playing[96] = {0};
    if (active_media_status(state, curr_playing, sizeof(curr_playing), NULL, NULL, NULL, NULL) &&
        strcmp(curr_playing, filename) == 0) {
        active_media_stop(state, true, true);
    }
    char primary_path[640];
    snprintf(primary_path, sizeof(primary_path), "%s/%s", state->evidence_dir, filename);
    unlink(primary_path);

    if (state->app && state->app->activity && state->app->activity->externalDataPath) {
        char ext_path[640];
        snprintf(ext_path, sizeof(ext_path), "%s/Movies/%s", state->app->activity->externalDataPath, filename);
        unlink(ext_path);
    }

    const size_t len = strlen(filename);
    if (len > 4 && (strcmp(filename + len - 4, ".wav") == 0 || strcmp(filename + len - 4, ".mp4") == 0 || strcmp(filename + len - 4, ".txt") == 0)) {
        char stem[96];
        snprintf(stem, sizeof(stem), "%.*s", (int)(len - 4), filename);
        unlink_recording_set(state->evidence_dir, stem);
        if (state->app && state->app->activity && state->app->activity->externalDataPath) {
            char movies_dir[640];
            snprintf(movies_dir, sizeof(movies_dir), "%s/Movies", state->app->activity->externalDataPath);
            unlink_recording_set(movies_dir, stem);
        }
    }

    if (strcmp(state->loaded_tags_filename, filename) == 0) {
        state->loaded_tags_count = 0;
        state->loaded_waveform_count = 0;
        state->loaded_tags_filename[0] = '\0';
    }
    refresh_media_files(state);
}

static FILE *open_companion_file(const DeadSpeak *state, const char *filename, const char *suffix) {
    const size_t len = strlen(filename);
    char path[640];
    if (len > 4 && (strcmp(filename + len - 4, ".wav") == 0 || strcmp(filename + len - 4, ".mp4") == 0)) {
        snprintf(path, sizeof(path), "%s/%.*s%s", state->evidence_dir, (int)(len - 4), filename, suffix);
    } else {
        snprintf(path, sizeof(path), "%s/%s%s", state->evidence_dir, filename, suffix);
    }
    FILE *f = fopen(path, "r");
    if (!f && state->app && state->app->activity && state->app->activity->externalDataPath &&
        len > 4 && (strcmp(filename + len - 4, ".wav") == 0 || strcmp(filename + len - 4, ".mp4") == 0)) {
        snprintf(path, sizeof(path), "%s/Movies/%.*s%s", state->app->activity->externalDataPath, (int)(len - 4), filename, suffix);
        f = fopen(path, "r");
    }
    /* Pair VIDEO_YYYYMMDD_HHMMSS...mp4 with its concurrent EVP_YYYYMMDD_HHMMSS... companion file */
    if (!f && strncmp(filename, "VIDEO_", 6) == 0 && len > 21 && filename[14] == '_') {
        char date_prefix[16];
        snprintf(date_prefix, sizeof(date_prefix), "EVP_%.8s_", filename + 6);
        const long v_hh = strtol((char[3]){filename[15], filename[16], 0}, NULL, 10);
        const long v_mm = strtol((char[3]){filename[17], filename[18], 0}, NULL, 10);
        const long v_ss = strtol((char[3]){filename[19], filename[20], 0}, NULL, 10);
        const long v_sec = v_hh * 3600L + v_mm * 60L + v_ss;
        const size_t slen = strlen(suffix);
        long best_diff = 4L;
        char best_path[640] = {0};
        DIR *dir = opendir(state->evidence_dir);
        if (dir) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL) {
                const size_t nlen = strlen(ent->d_name);
                if (nlen > 19 + slen && strncmp(ent->d_name, date_prefix, 13) == 0 &&
                    strcmp(ent->d_name + nlen - slen, suffix) == 0) {
                    const long e_hh = strtol((char[3]){ent->d_name[13], ent->d_name[14], 0}, NULL, 10);
                    const long e_mm = strtol((char[3]){ent->d_name[15], ent->d_name[16], 0}, NULL, 10);
                    const long e_ss = strtol((char[3]){ent->d_name[17], ent->d_name[18], 0}, NULL, 10);
                    const long diff = labs(v_sec - (e_hh * 3600L + e_mm * 60L + e_ss));
                    if (diff < best_diff) {
                        best_diff = diff;
                        snprintf(best_path, sizeof(best_path), "%s/%s", state->evidence_dir, ent->d_name);
                    }
                }
            }
            closedir(dir);
        }
        if (best_path[0]) f = fopen(best_path, "r");
    }
    return f;
}

static void load_recording_tags(DeadSpeak *state, const char *filename) {
    if (!state || !filename || !filename[0]) return;
    if (strcmp(state->loaded_tags_filename, filename) == 0) return;
    state->loaded_tags_count = 0;
    state->loaded_waveform_count = 0;
    snprintf(state->loaded_tags_filename, sizeof(state->loaded_tags_filename), "%s", filename);

    FILE *f = open_companion_file(state, filename, "_tags.txt");
    if (f) {
        char line[128];
        while (state->loaded_tags_count < DS_MAX_LOADED_TAGS && fgets(line, sizeof(line), f)) {
            char *comma = strchr(line, ',');
            if (comma) {
                *comma = '\0';
                const uint64_t ms = (uint64_t)strtoull(line, NULL, 10);
                const long id_val = strtol(comma + 1, NULL, 10);
                const uint8_t id = (id_val >= 0 && id_val <= 255) ? (uint8_t)id_val : 0;
                state->loaded_tags[state->loaded_tags_count++] = (DSTagEntry){
                    .timestamp_ms = ms,
                    .tag_id = id
                };
            }
        }
        fclose(f);
    }

    FILE *wf = open_companion_file(state, filename, "_waveform.txt");
    if (wf) {
        size_t total = 0;
        char line[128];
        while (fgets(line, sizeof(line), wf)) {
            if (strchr(line, ',')) ++total;
        }
        if (total > 0) {
            rewind(wf);
            const size_t bars = total < DS_MAX_WAVEFORM_BARS ? total : DS_MAX_WAVEFORM_BARS;
            memset(state->loaded_waveform, 0, sizeof(state->loaded_waveform));
            size_t idx = 0;
            while (fgets(line, sizeof(line), wf)) {
                const char *comma = strchr(line, ',');
                if (!comma) continue;
                const float amp = strtof(comma + 1, NULL);
                const size_t bucket = (idx * bars) / total;
                if (bucket < bars && isfinite(amp) && amp > state->loaded_waveform[bucket]) {
                    state->loaded_waveform[bucket] = amp;
                }
                ++idx;
            }
            state->loaded_waveform_count = bars;
        }
        fclose(wf);
    }
}

static void jump_to_tag(DeadSpeak *state, bool forward) {
    if (!state || state->loaded_tags_count == 0) return;
    const uint64_t now = now_millis();
    if (state->last_tag_seek_ms && now < state->last_tag_seek_ms + 300ULL) return;
    state->last_tag_seek_ms = now;

    uint64_t cur_ms = 0, dur_ms = 0;
    if (!active_media_status(state, NULL, 0, NULL, &cur_ms, &dur_ms, NULL)) return;

    if (forward) {
        for (size_t i = 0; i < state->loaded_tags_count; ++i) {
            if (state->loaded_tags[i].timestamp_ms >= cur_ms + 50ULL) {
                active_media_seek_ms(state, state->loaded_tags[i].timestamp_ms);
                return;
            }
        }
        if (dur_ms > 0) active_media_seek_ms(state, dur_ms);
    } else {
        const uint64_t target_cur = cur_ms > 1000ULL ? (cur_ms - 1000ULL) : 0ULL;
        int found_idx = -1;
        for (size_t i = 0; i < state->loaded_tags_count; ++i) {
            if (state->loaded_tags[i].timestamp_ms <= target_cur) {
                found_idx = (int)i;
            } else {
                break;
            }
        }
        if (found_idx >= 0) {
            active_media_seek_ms(state, state->loaded_tags[found_idx].timestamp_ms);
        } else {
            active_media_seek_ms(state, 0ULL);
        }
    }
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

static bool has_camera_permission(DeadSpeak *state) {
    DSJniScope scope;
    if (!ds_jni_scope_begin(&scope, state->app->activity)) return false;
    JNIEnv *env = scope.env;
    jobject activity = state->app->activity->clazz;
    jclass cls = (*env)->GetObjectClass(env, activity);
    jint result = -1;
    if (!cls) goto done;
    jmethodID check = (*env)->GetMethodID(env, cls, "checkSelfPermission", "(Ljava/lang/String;)I");
    if (!check) goto done;
    jstring permission = (*env)->NewStringUTF(env, "android.permission.CAMERA");
    if (!permission) goto done;
    result = (*env)->CallIntMethod(env, activity, check, permission);
done:
    return ds_jni_scope_end(&scope) && result == 0;
}

static void request_camera_permission(DeadSpeak *state) {
    if (state->camera_permission_pending) return;
    DSJniScope scope;
    if (!ds_jni_scope_begin(&scope, state->app->activity)) return;
    JNIEnv *env = scope.env;
    jobject activity = state->app->activity->clazz;
    jclass cls = (*env)->GetObjectClass(env, activity);
    if (!cls) goto done;
    jmethodID request = (*env)->GetMethodID(env, cls, "requestPermissions", "([Ljava/lang/String;I)V");
    jclass strings = (*env)->FindClass(env, "java/lang/String");
    if (!request || !strings) goto done;
    jobjectArray permissions = (*env)->NewObjectArray(env, 1, strings, NULL);
    jstring permission = (*env)->NewStringUTF(env, "android.permission.CAMERA");
    if (!permissions || !permission) goto done;
    (*env)->SetObjectArrayElement(env, permissions, 0, permission);
    if ((*env)->ExceptionCheck(env)) goto done;
    (*env)->CallVoidMethod(env, activity, request, permissions, 6166);
done:
    state->camera_permission_pending = ds_jni_scope_end(&scope);
    state->permission_deadline = now_millis() + 15000U;
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
    FILE *srt_file = NULL;
    FILE *waveform_file = NULL;
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

    char path[640], srt_path[640], tags_path[640], waveform_path[640], words_path[640];
    const time_t now = time(NULL);
    struct tm local_time;
    localtime_r(&now, &local_time);
    char stamp[32];
    struct timespec wall_time;
    clock_gettime(CLOCK_REALTIME, &wall_time);
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local_time);
    snprintf(path, sizeof(path), "%s/EVP_%s_%03ld.wav", state->evidence_dir, stamp, wall_time.tv_nsec / 1000000L);
    snprintf(srt_path, sizeof(srt_path), "%s/EVP_%s_%03ld.srt", state->evidence_dir, stamp, wall_time.tv_nsec / 1000000L);
    snprintf(tags_path, sizeof(tags_path), "%s/EVP_%s_%03ld_tags.txt", state->evidence_dir, stamp, wall_time.tv_nsec / 1000000L);
    snprintf(waveform_path, sizeof(waveform_path), "%s/EVP_%s_%03ld_waveform.txt", state->evidence_dir, stamp, wall_time.tv_nsec / 1000000L);
    snprintf(words_path, sizeof(words_path), "%s/EVP_%s_%03ld.txt", state->evidence_dir, stamp, wall_time.tv_nsec / 1000000L);
    snprintf(state->active_words_path, sizeof(state->active_words_path), "%s", words_path);
    snprintf(state->active_tags_path, sizeof(state->active_tags_path), "%s", tags_path);
    snprintf(state->active_waveform_path, sizeof(state->active_waveform_path), "%s", waveform_path);
    state->recording_start_ms = now_millis();
    file = fopen(path, "wb");
    srt_file = fopen(srt_path, "w");
    waveform_file = fopen(waveform_path, "w");
    if (!file) {
        if (srt_file) { fclose(srt_file); srt_file = NULL; }
        if (waveform_file) { fclose(waveform_file); waveform_file = NULL; }
        goto done;
    }
    uint8_t empty_header[44] = {0};
    fwrite(empty_header, sizeof(empty_header), 1, file);
    if (AAudioStream_requestStart(stream) != AAUDIO_OK) {
        fclose(file);
        file = NULL;
        unlink(path);
        if (srt_file) { fclose(srt_file); unlink(srt_path); srt_file = NULL; }
        if (waveform_file) { fclose(waveform_file); unlink(waveform_path); waveform_file = NULL; }
        goto done;
    }
    capture_ok = true;

    int16_t samples[480];
    int16_t dsp_ring[1024] = {0};
    int16_t dsp_linear[1024] = {0};
    size_t dsp_head = 0;
    size_t dsp_filled = 0;
    state->sub_bass_baseline = 15.0f;
    uint32_t last_srt_sec = UINT32_MAX, srt_cue_index = 0, last_wf_ms = 0;

    while (atomic_load(&state->audio_active)) {
        const aaudio_result_t frames = AAudioStream_read(stream, samples, 480, 100000000LL);
        if (frames < 0) { atomic_store(&state->audio_failed, true); break; }
        if (frames == 0) continue;

        /* Feed live microphone frames into muxed MP4 AAC audio encoder */
        ds_video_push_audio(&state->video, samples, (size_t)frames, sample_rate);

        /* Update 1024-sample Hann analysis ring buffer */
        for (aaudio_result_t i = 0; i < frames; ++i) {
            dsp_ring[dsp_head] = samples[i];
            dsp_head = (dsp_head + 1U) & 1023U;
            if (dsp_filled < 1024U) ++dsp_filled;
        }
        const size_t n_win = dsp_filled;
        if (n_win > 0) {
            const size_t oldest = (dsp_head + 1024U - n_win) & 1023U;
            for (size_t i = 0; i < n_win; ++i) {
                dsp_linear[i] = dsp_ring[(oldest + i) & 1023U];
            }
        }

        /* Update oscilloscope ring buffer */
        const int step = (int)frames / DS_OSCILLOSCOPE_SAMPLES;
        if (step > 0) {
            for (int s = 0; s < DS_OSCILLOSCOPE_SAMPLES; ++s) {
                atomic_store(&state->oscilloscope[s], samples[s * step]);
            }
        }

        double squares = 0.0;
        for (aaudio_result_t i = 0; i < frames; ++i) {
            const double normalized = (double)samples[i] / 32768.0;
            squares += normalized * normalized;
        }
        const float rms = (float)sqrt(squares / (double)frames);
        const unsigned level = (unsigned)(rms * 1000.0f);
        atomic_store(&state->audio_level_milli, level > 1000U ? 1000U : level);
        float bands[3] = {0};
        ds_engine_spectrum(dsp_linear, n_win, sample_rate, bands);
        for (unsigned band = 0; band < 3; ++band) {
            const unsigned band_level = (unsigned)(bands[band] * 4000.0f);
            atomic_store(&state->spectrum_milli[band], band_level > 1000U ? 1000U : band_level);
        }
        float bins[DS_SPECTRUM_BINS];
        ds_engine_spectrum_hann_db(dsp_linear, n_win, sample_rate, bins, DS_SPECTRUM_BINS);
        for (unsigned band = 0; band < DS_SPECTRUM_BINS; ++band)
            atomic_store(&state->spectrum_bins[band], (uint32_t)fminf(1000, bins[band] * 1000.0f));

        /* Sub-Bass EVP Anomaly Evaluator (80-300 Hz) with Hann window, speech-band rejection, & self-Vox gating */
        const float sub_energy = ds_engine_sub_bass_energy(dsp_linear, n_win, sample_rate);
        const float speech_ratio = ds_engine_speech_ratio(dsp_linear, n_win, sample_rate);
        const bool vox_speaking = ds_voice_player_is_speaking(state->voice_player);
        if (!vox_speaking) {
            state->sub_bass_baseline = state->sub_bass_baseline * 0.98f + sub_energy * 0.02f;
        }
        state->sub_bass_level = sub_energy;

        const uint64_t cap_now = now_millis();
        if (!vox_speaking && speech_ratio < 1.35f &&
            sub_energy > 15.0f && sub_energy > state->sub_bass_baseline * 2.0f) {
            atomic_store(&state->sub_bass_anomaly, true);
            state->sub_bass_anomaly_expiry_ms = cap_now + 2000U;
            /* Emit tag 2 (ORANGE / ANOMALY) to _tags.txt */
            emit_companion_tag(state, DS_TAG_ANOMALY);
        } else if (cap_now >= state->sub_bass_anomaly_expiry_ms) {
            atomic_store(&state->sub_bass_anomaly, false);
        }

        if ((uint64_t)bytes_written + (uint64_t)frames * sizeof(int16_t) > UINT32_MAX - 36U) {
            atomic_store(&state->audio_failed, true);
            break;
        }
        const size_t written = fwrite(samples, sizeof(int16_t), (size_t)frames, file);
        bytes_written += (uint32_t)(written * sizeof(int16_t));
        if (written != (size_t)frames) { atomic_store(&state->audio_failed, true); break; }

        /* _waveform.txt companion: timestamp_ms,amplitude */
        if (waveform_file && sample_rate > 0) {
            const uint32_t current_ms = (uint32_t)((uint64_t)bytes_written * 1000U / (sample_rate * sizeof(int16_t)));
            if (current_ms >= last_wf_ms + 250U) {
                last_wf_ms = current_ms;
                fprintf(waveform_file, "%u,%.4f\n", current_ms, rms);
                fflush(waveform_file);
            }
        }

        /* Synchronized .srt subtitle entry every 1 second */
        if (srt_file && sample_rate > 0) {
            const uint32_t current_sec = bytes_written / (sample_rate * sizeof(int16_t));
            if (current_sec != last_srt_sec) {
                last_srt_sec = current_sec;
                uint32_t s0 = current_sec, s1 = current_sec + 1;
                uint32_t m0 = s0 / 60, m1 = s1 / 60;
                uint32_t h0 = m0 / 60, h1 = m1 / 60;
                m0 %= 60; m1 %= 60; s0 %= 60; s1 %= 60;
                const float delta = state->baseline.ready ? (state->smoothed_field - state->baseline.field) : 0.0f;
                const bool moving = ds_motion_is_moving(&state->motion_filter, sensor_now_ns());
                fprintf(srt_file, "%u\n%02u:%02u:%02u,000 --> %02u:%02u:%02u,000\nEMF: %.1f uT (%+.1f) | %s | %s%s\n\n",
                        ++srt_cue_index, h0, m0, s0, h1, m1, s1,
                        state->magnetic_field, delta,
                        moving ? "MOTION - INVALID" : "SENSOR STABLE",
                        state->word[0] ? state->word : "QUIET",
                        state->proximity_active ? " | PROXIMITY ALERT" : "");
                fflush(srt_file);
            }
        }
    }
    AAudioStream_requestStop(stream);
    write_wav_header(file, bytes_written, sample_rate);
    fflush(file);
    fsync(fileno(file));
    fclose(file);
    file = NULL;
    if (srt_file) {
        fflush(srt_file);
        fsync(fileno(srt_file));
        fclose(srt_file);
        srt_file = NULL;
    }
    if (waveform_file) {
        fflush(waveform_file);
        fsync(fileno(waveform_file));
        fclose(waveform_file);
        waveform_file = NULL;
    }
    state->active_words_path[0] = '\0';
    state->active_tags_path[0] = '\0';
    state->active_waveform_path[0] = '\0';
    state->recording_start_ms = 0;
    if (bytes_written == 0U) atomic_store(&state->audio_failed, true);

done:
    if (!capture_ok) atomic_store(&state->audio_failed, true);
    if (file) fclose(file);
    if (srt_file) fclose(srt_file);
    if (waveform_file) fclose(waveform_file);
    if (stream) AAudioStream_close(stream);
    if (builder) AAudioStreamBuilder_delete(builder);
    atomic_store(&state->audio_level_milli, 0);
    for (unsigned i = 0; i < DS_SPECTRUM_BINS; ++i) atomic_store(&state->spectrum_bins[i], 0);
    for (int s = 0; s < DS_OSCILLOSCOPE_SAMPLES; ++s) atomic_store(&state->oscilloscope[s], 0);
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

static const char *text_vertex_shader_source =
    "attribute vec2 a_position;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "}\n";

static const char *text_fragment_shader_source =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform vec4 u_color;\n"
    "uniform sampler2D u_atlas;\n"
    "void main() {\n"
    "    float a = texture2D(u_atlas, v_uv).a;\n"
    "    gl_FragColor = vec4(u_color.rgb, u_color.a * a);\n"
    "}\n";

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

static void set_keep_screen_on(DeadSpeak *state, bool keep_on) {
    if (!state || !state->app || !state->app->activity) return;
    ANativeActivity *activity = state->app->activity;
    if (keep_on) {
        ANativeActivity_setWindowFlags(activity, AWINDOW_FLAG_KEEP_SCREEN_ON, 0);
    } else {
        ANativeActivity_setWindowFlags(activity, 0, AWINDOW_FLAG_KEEP_SCREEN_ON);
    }

    DSJniScope scope;
    if (ds_jni_scope_begin(&scope, activity)) {
        JNIEnv *env = scope.env;
        jobject activity_obj = activity->clazz;
        jclass activity_class = (*env)->GetObjectClass(env, activity_obj);
        if (activity_class) {
            jmethodID get_window = (*env)->GetMethodID(env, activity_class, "getWindow", "()Landroid/view/Window;");
            if (get_window) {
                jobject window = (*env)->CallObjectMethod(env, activity_obj, get_window);
                if (window && !(*env)->ExceptionCheck(env)) {
                    jclass window_class = (*env)->GetObjectClass(env, window);
                    if (window_class) {
                        constexpr jint FLAG_KEEP_SCREEN_ON = 128;
                        const char *method_name = keep_on ? "addFlags" : "clearFlags";
                        jmethodID flag_method = (*env)->GetMethodID(env, window_class, method_name, "(I)V");
                        if (flag_method) {
                            (*env)->CallVoidMethod(env, window, flag_method, FLAG_KEEP_SCREEN_ON);
                            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
                        }
                    }
                }
            }
        }
        ds_jni_scope_end(&scope);
    }
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
    state->camera_ui = ds_camera_ui((float)state->width, (float)state->height);
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
    set_keep_screen_on(state, true);
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
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    vertex = compile_shader(GL_VERTEX_SHADER, DS_CAMERA_VERTEX_SHADER);
    fragment = compile_shader(GL_FRAGMENT_SHADER, DS_CAMERA_FRAGMENT_SHADER);
    state->camera_program = glCreateProgram();
    glAttachShader(state->camera_program, vertex);
    glAttachShader(state->camera_program, fragment);
    glBindAttribLocation(state->camera_program, 0, "a_position");
    glBindAttribLocation(state->camera_program, 1, "a_uv");
    glLinkProgram(state->camera_program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    state->camera_filter_loc = glGetUniformLocation(state->camera_program, "u_filter");
    state->camera_image_loc = glGetUniformLocation(state->camera_program, "u_image");
    glGenTextures(1, &state->camera_texture);
    glBindTexture(GL_TEXTURE_2D, state->camera_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &state->mp4_texture);
    glBindTexture(GL_TEXTURE_2D, state->mp4_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    /* Text engine shader and font atlas texture */
    vertex = compile_shader(GL_VERTEX_SHADER, text_vertex_shader_source);
    fragment = compile_shader(GL_FRAGMENT_SHADER, text_fragment_shader_source);
    state->text_program = glCreateProgram();
    glAttachShader(state->text_program, vertex);
    glAttachShader(state->text_program, fragment);
    glBindAttribLocation(state->text_program, 0, "a_position");
    glBindAttribLocation(state->text_program, 1, "a_uv");
    glLinkProgram(state->text_program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    state->text_pos_loc = 0;
    state->text_uv_loc = 1;
    state->text_color_loc = glGetUniformLocation(state->text_program, "u_color");
    state->text_sampler_loc = glGetUniformLocation(state->text_program, "u_atlas");
    glGenTextures(1, &state->text_texture);
    glBindTexture(GL_TEXTURE_2D, state->text_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    uint8_t *atlas_raw = (uint8_t *)malloc(DS_FONT_ATLAS_RAW_SIZE);
    if (atlas_raw != nullptr) {
        ds_font_unpack_atlas(atlas_raw, DS_FONT_ATLAS_RAW_SIZE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, (GLsizei)DS_FONT_ATLAS_WIDTH, (GLsizei)DS_FONT_ATLAS_HEIGHT, 0, GL_ALPHA, GL_UNSIGNED_BYTE, atlas_raw);
        free(atlas_raw);
        state->text_engine_ready = true;
    }

    state->preview_received_ms = 0;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    return true;
}

static void teardown_display(DeadSpeak *state) {
    if (state->display != EGL_NO_DISPLAY) {
        ds_video_preview_stop(&state->video);
        state->preview_received_ms = 0;
        eglMakeCurrent(state->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (state->context != EGL_NO_CONTEXT) eglDestroyContext(state->display, state->context);
        if (state->surface != EGL_NO_SURFACE) eglDestroySurface(state->display, state->surface);
        eglTerminate(state->display);
    }
    state->display = EGL_NO_DISPLAY;
    state->surface = EGL_NO_SURFACE;
    state->context = EGL_NO_CONTEXT;
    state->camera_texture = 0;
    state->mp4_texture = 0;
    state->camera_program = 0;
    state->program = 0;
    state->text_texture = 0;
    state->text_program = 0;
    state->text_engine_ready = false;
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

static void flush_text_batch(DeadSpeak *state, const GLfloat *vbuf, size_t batched_count,
                             float r, float g, float b, float a) {
    if (batched_count == 0) return;
    glUseProgram(state->text_program);
    glUniform4f(state->text_color_loc, r, g, b, a);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, state->text_texture);
    glUniform1i(state->text_sampler_loc, 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer((GLuint)state->text_pos_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vbuf);
    glEnableVertexAttribArray((GLuint)state->text_pos_loc);
    glVertexAttribPointer((GLuint)state->text_uv_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vbuf + 2);
    glEnableVertexAttribArray((GLuint)state->text_uv_loc);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(batched_count * 6));
}

static void text_styled(DeadSpeak *state, const char *value, float x, float y, float scale, DSFontType font_type, float r, float g, float b, float a) {
    if (value == nullptr || scale <= 0.0f || state == nullptr) return;

    if (state->text_engine_ready && state->text_texture != 0 && state->text_program != 0) {
        float draw_y = y;
        if (state->body_drawing) draw_y -= state->scroll[state->tab];

        const float screen_scale = scale * (7.0f / DS_FONT_CAP_HEIGHT);
        const size_t style_idx = (font_type == DS_FONT_BOLD) ? 1 : 0;
        const float inv_w = 1.0f / (float)DS_FONT_ATLAS_WIDTH;
        const float inv_h = 1.0f / (float)DS_FONT_ATLAS_HEIGHT;
        const float sw = (float)state->width;
        const float sh = (float)state->height;

        float cursor_x = x;
        float cur_y = draw_y;

        constexpr size_t BATCH_CAPACITY = 256;
        GLfloat vbuf[BATCH_CAPACITY * 24];
        size_t batched_count = 0;

        for (size_t i = 0; value[i] != '\0'; ++i) {
            char ch = value[i];
            if (ch == '\n') {
                cursor_x = x;
                cur_y += 24.0f * screen_scale;
                continue;
            }
            if (ch == ' ') {
                cursor_x += ds_text_char_advance(' ', scale, font_type);
                continue;
            }

            size_t glyph_idx = (ch >= 32 && ch <= 126) ? (size_t)(ch - 32) : (size_t)('?' - 32);
            const DSFontGlyph *glyph = &ds_font_glyphs[style_idx][glyph_idx];

            if (glyph->w > 0 && glyph->h > 0) {
                float qx1 = cursor_x + (float)glyph->ox * screen_scale;
                float qy1 = cur_y + (float)glyph->oy * screen_scale;
                float qx2 = qx1 + (float)glyph->w * screen_scale;
                float qy2 = qy1 + (float)glyph->h * screen_scale;

                float ndc_x1 = (qx1 / sw) * 2.0f - 1.0f;
                float ndc_x2 = (qx2 / sw) * 2.0f - 1.0f;
                float ndc_y1 = 1.0f - (qy1 / sh) * 2.0f;
                float ndc_y2 = 1.0f - (qy2 / sh) * 2.0f;

                float u1 = (float)glyph->x * inv_w;
                float v1 = (float)glyph->y * inv_h;
                float u2 = (float)(glyph->x + glyph->w) * inv_w;
                float v2 = (float)(glyph->y + glyph->h) * inv_h;

                size_t offset = batched_count * 24;
                /* Quad vertex 1: (ndc_x1, ndc_y1, u1, v1) */
                vbuf[offset + 0]  = ndc_x1; vbuf[offset + 1]  = ndc_y1; vbuf[offset + 2]  = u1; vbuf[offset + 3]  = v1;
                /* Quad vertex 2: (ndc_x2, ndc_y1, u2, v1) */
                vbuf[offset + 4]  = ndc_x2; vbuf[offset + 5]  = ndc_y1; vbuf[offset + 6]  = u2; vbuf[offset + 7]  = v1;
                /* Quad vertex 3: (ndc_x1, ndc_y2, u1, v2) */
                vbuf[offset + 8]  = ndc_x1; vbuf[offset + 9]  = ndc_y2; vbuf[offset + 10] = u1; vbuf[offset + 11] = v2;
                /* Quad vertex 4: (ndc_x1, ndc_y2, u1, v2) */
                vbuf[offset + 12] = ndc_x1; vbuf[offset + 13] = ndc_y2; vbuf[offset + 14] = u1; vbuf[offset + 15] = v2;
                /* Quad vertex 5: (ndc_x2, ndc_y1, u2, v1) */
                vbuf[offset + 16] = ndc_x2; vbuf[offset + 17] = ndc_y1; vbuf[offset + 18] = u2; vbuf[offset + 19] = v1;
                /* Quad vertex 6: (ndc_x2, ndc_y2, u2, v2) */
                vbuf[offset + 20] = ndc_x2; vbuf[offset + 21] = ndc_y2; vbuf[offset + 22] = u2; vbuf[offset + 23] = v2;

                batched_count++;

                if (batched_count >= BATCH_CAPACITY) {
                    flush_text_batch(state, vbuf, batched_count, r, g, b, a);
                    batched_count = 0;
                }
            }

            cursor_x += ds_text_char_advance(ch, scale, font_type);
        }

        if (batched_count > 0) {
            flush_text_batch(state, vbuf, batched_count, r, g, b, a);
        }
        return;
    }

    auto cursor = x;
    const auto pixel_w = (font_type == DS_FONT_BOLD) ? (scale * 1.35f) : scale;
    const auto pixel_h = scale;

    for (size_t i = 0; value[i] != '\0'; ++i) {
        const char ch = value[i];
        const auto adv = ds_text_char_advance(ch, scale, font_type);
        if (ch == ' ') { cursor += adv; continue; }
        const int index = ds_glyph_index(ch);
        if (index < 0) { cursor += adv; continue; }
        for (size_t row = 0; row < DS_FONT_GLYPH_ROWS; ++row) {
            const uint8_t row_bits = ds_font_bitmap[index][row];
            for (size_t col = 0; col < DS_FONT_GLYPH_COLS; ++col) {
                if ((row_bits >> (4 - col)) & 1) {
                    rect(state, cursor + (float)col * scale, y + (float)row * scale, pixel_w, pixel_h, r, g, b, a);
                }
            }
        }
        cursor += adv;
    }
}

static void text(DeadSpeak *state, const char *value, float x, float y, float scale, float r, float g, float b, float a) {
    text_styled(state, value, x, y, scale, DS_FONT_STANDARD, r, g, b, a);
}

[[nodiscard]] static inline float fit_text_scale(const char *value, float preferred, float available_width) {
    return ds_fit_text_scale_styled(value, preferred, available_width, DS_FONT_STANDARD);
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

static void filled_circle(DeadSpeak *state, float x, float y, float radius, float r, float g, float b, float a) {
    if (state->body_drawing) y -= state->scroll[state->tab];
    for (int i = 0; i < 40; ++i) {
        const float p = i * 6.2831853f / 40, q = (i + 1) * 6.2831853f / 40;
        const GLfloat vertices[] = {
            x / state->width * 2 - 1, 1 - y / state->height * 2,
            (x + cosf(p) * radius) / state->width * 2 - 1, 1 - (y + sinf(p) * radius) / state->height * 2,
            (x + cosf(q) * radius) / state->width * 2 - 1, 1 - (y + sinf(q) * radius) / state->height * 2
        };
        emit_vertices(state, vertices, 3, r, g, b, a);
    }
}

static float body_content_height(const DeadSpeak *state) {
    if (state->tab == 0) return state->ui.scan_height;
    if (state->tab == 2) return state->ui.explore_height;
    if (state->tab == 3) return 720.0f;
    const float words = state->log_count ? 70.0f + (float)state->log_count * 42.0f : 130.0f;
    const bool playing_mp4 = ds_media_player_is_playing(state->media_player, NULL, 0, NULL);
    const bool playing_wav = ds_voice_player_is_playing_wav(state->voice_player, NULL, 0, NULL);
    const float extra = playing_mp4 ? 236.0f : (playing_wav ? 48.0f : 0.0f);
    const float media = 36.0f + (float)(state->media_count ? state->media_count : 1) * 36.0f + extra + 20.0f;
    return state->ui.wide ? fmaxf(words, 22.0f + media) : fmaxf(140.0f, words + 20.0f) + media;
}

static void custom_button(DeadSpeak *state, DSRect button, const char *label, bool selected) {
    rect(state, button.x, button.y, button.w, button.h, selected ? 0.10f : 0.14f, selected ? 0.32f : 0.18f, selected ? 0.25f : 0.19f, 0.97f);
    const DSFontType font_type = selected ? DS_FONT_BOLD : DS_FONT_STANDARD;
    const float scale = ds_fit_text_scale_styled(label, ds_sp_to_scale(DS_FONT_SIZE_BODY_MD), button.w - 10, font_type);
    const float total_w = ds_text_width(label, scale, font_type);
    text_styled(state, label, button.x + (button.w - total_w) / 2,
                button.y + (button.h - 7.0f * scale) / 2, scale, font_type, 0.88f, 0.98f, 0.93f, 1);
}

typedef struct {
    float column_width, media_x, media_y;
    DSRect export_btn;
} DSLogLayout;

static DSLogLayout log_layout(const DeadSpeak *state) {
    const DSRect body = state->ui.body;
    const float column_width = state->ui.wide ? (body.w - 16.0f) * 0.5f : body.w;
    const float media_x = state->ui.wide ? body.x + 16.0f + column_width + 16.0f : body.x + 16.0f;
    const float media_y = state->ui.wide ? body.y + 22.0f : body.y + fmaxf(140.0f, 90.0f + (float)state->log_count * 42.0f);
    return (DSLogLayout){
        .column_width = column_width,
        .media_x = media_x,
        .media_y = media_y,
        .export_btn = {media_x + column_width - 138.0f, media_y - 6.0f, 106.0f, 30.0f},
    };
}

typedef struct {
    DSRect prev_tag, next_tag, pause_btn, stop, del_active, scrubber, del_idle, item_row, video_box;
} DSMediaItemLayout;

static DSMediaItemLayout media_item_layout(DSLogLayout log, float cur_item_y, bool is_mp4) {
    if (is_mp4) {
        return (DSMediaItemLayout){
            .video_box  = {log.media_x, cur_item_y + 22.0f, log.column_width - 32.0f, 172.0f},
            .scrubber   = {log.media_x + 4.0f, cur_item_y + 200.0f, log.column_width - 36.0f, 16.0f},
            .prev_tag   = {log.media_x + 4.0f, cur_item_y + 224.0f, 54.0f, 26.0f},
            .next_tag   = {log.media_x + 62.0f, cur_item_y + 224.0f, 54.0f, 26.0f},
            .pause_btn  = {log.media_x + 120.0f, cur_item_y + 224.0f, 52.0f, 26.0f},
            .stop       = {log.media_x + 176.0f, cur_item_y + 224.0f, 44.0f, 26.0f},
            .del_active = {log.media_x + log.column_width - 52.0f, cur_item_y + 224.0f, 42.0f, 26.0f},
            .del_idle   = {log.media_x + log.column_width - 48.0f, cur_item_y + 1.0f, 40.0f, 22.0f},
            .item_row   = {log.media_x, cur_item_y - 4.0f, log.column_width - 52.0f, 36.0f},
        };
    }
    return (DSMediaItemLayout){
        .prev_tag = {log.media_x + 4.0f, cur_item_y + 42.0f, 64.0f, 26.0f},
        .next_tag = {log.media_x + 74.0f, cur_item_y + 42.0f, 64.0f, 26.0f},
        .stop = {log.media_x + 144.0f, cur_item_y + 42.0f, 52.0f, 26.0f},
        .del_active = {log.media_x + log.column_width - 56.0f, cur_item_y + 42.0f, 44.0f, 26.0f},
        .scrubber = {log.media_x + 4.0f, cur_item_y + 20.0f, log.column_width - 36.0f, 16.0f},
        .del_idle = {log.media_x + log.column_width - 48.0f, cur_item_y + 1.0f, 40.0f, 22.0f},
        .item_row = {log.media_x, cur_item_y - 4.0f, log.column_width - 52.0f, 36.0f},
    };
}

static void draw_mp4_quad(DeadSpeak *state, DSRect box) {
    if (!state->mp4_rgba || !state->mp4_texture || !state->camera_program) return;
    if (ds_media_player_copy_frame(state->media_player, state->mp4_rgba, DS_PREVIEW_CAPACITY,
                                   &state->mp4_width, &state->mp4_height)) {
        state->mp4_rotation = ds_media_player_rotation(state->media_player);
        glBindTexture(GL_TEXTURE_2D, state->mp4_texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, state->mp4_width, state->mp4_height,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, state->mp4_rgba);
    }
    rect(state, box.x, box.y, box.w, box.h, 0.02f, 0.03f, 0.03f, 1.0f);
    if (state->mp4_width <= 0 || state->mp4_height <= 0) return;

    const bool swap = state->mp4_rotation == 90 || state->mp4_rotation == 270;
    const float src_aspect = (float)(swap ? state->mp4_height : state->mp4_width) /
                             (float)(swap ? state->mp4_width : state->mp4_height);
    float draw_w = box.w, draw_h = box.h;
    if (box.w / box.h > src_aspect) draw_w = box.h * src_aspect;
    else draw_h = box.w / src_aspect;
    const float dx = box.x + (box.w - draw_w) * 0.5f;
    float dy = box.y + (box.h - draw_h) * 0.5f;
    if (state->body_drawing) dy -= state->scroll[state->tab];

    float uv[12];
    if (!ds_preview_uv(state->mp4_width, state->mp4_height, state->mp4_rotation, draw_w, draw_h, uv)) return;
    const float x1 = (dx / state->width) * 2.0f - 1.0f;
    const float x2 = ((dx + draw_w) / state->width) * 2.0f - 1.0f;
    const float y1 = 1.0f - (dy / state->height) * 2.0f;
    const float y2 = 1.0f - ((dy + draw_h) / state->height) * 2.0f;
    const GLfloat vertices[] = {x1,y1, x2,y1, x1,y2, x1,y2, x2,y1, x2,y2};

    glUseProgram(state->camera_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, state->mp4_texture);
    glUniform1i(state->camera_image_loc, 0);
    glUniform1i(state->camera_filter_loc, (int)DS_VIDEO_FILTER_RAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray(1);
}

static void draw_log(DeadSpeak *state, const DSPalette *pal) {
    const DSRect body = state->ui.body;
    const DSLogLayout layout = log_layout(state);
    const float column_width = layout.column_width;
    const float x = body.x + 16.0f, top = body.y;
    text(state, "RECENT SENSOR WORDS", x, top + 22, fit_text_scale("RECENT SENSOR WORDS", 1.8f, column_width - 32), pal->mint.r, pal->mint.g, pal->mint.b, 1.0f);
    if (state->export_status_expiry_ms > now_millis() && state->export_status_message[0]) {
        rect(state, x, top + 42, column_width - 32, 22, 0.12f, 0.42f, 0.28f, 0.95f);
        text(state, state->export_status_message, x + 6, top + 47, fit_text_scale(state->export_status_message, 1.0f, column_width - 44), 1.0f, 1.0f, 1.0f, 1.0f);
    }
    if (state->log_count == 0) {
        text(state, "NO WORDS LOGGED YET", x, top + 70, fit_text_scale("NO WORDS LOGGED YET", 1.5f, column_width - 32), pal->muted.r, pal->muted.g, pal->muted.b, 1.0f);
        text(state, "ENABLE SCAN TO COLLECT READINGS", x, top + 100, fit_text_scale("ENABLE SCAN TO COLLECT READINGS", 1.1f, column_width - 32), pal->muted.r, pal->muted.g, pal->muted.b, 1.0f);
    } else {
        for (unsigned i = 0; i < state->log_count; ++i) {
            const unsigned slot = (state->log_next + DS_LOG_CAPACITY - 1U - i) % DS_LOG_CAPACITY;
            const float y = top + 70.0f + (float)i * 42.0f;
            char details[64];
            snprintf(details, sizeof(details), "SAMPLE %u  FIELD %.1f UT (%+.1f)%s",
                state->word_samples[slot], state->word_fields[slot],
                state->word_deltas[slot], state->word_whispers[slot] ? " [W]" : "");
            text(state, state->words[slot], x, y, fit_text_scale(state->words[slot], 2.0f, column_width - 32), pal->mint.r, pal->mint.g, pal->mint.b, 1.0f);
            text(state, details, x, y + 19, fit_text_scale(details, 1.0f, column_width - 32), pal->muted.r, pal->muted.g, pal->muted.b, 1.0f);
            rect(state, x, y + 34, column_width - 32, 1, 0.17f, 0.24f, 0.23f, 1.0f);
        }
    }
    const float media_x = layout.media_x;
    const float media_y = layout.media_y;
    text(state, "SAVED CAPTURES & SESSIONS", media_x, media_y, fit_text_scale("SAVED CAPTURES & SESSIONS", 1.35f, column_width - 146), pal->mint.r, pal->mint.g, pal->mint.b, 1.0f);
    custom_button(state, layout.export_btn, "EXPORT LOGS", false);
    if (state->media_count == 0U) text(state, "NO AUDIO OR VIDEO FILES", media_x, media_y + 34, fit_text_scale("NO AUDIO OR VIDEO FILES", 1.1f, column_width - 32), pal->muted.r, pal->muted.g, pal->muted.b, 1.0f);

    char curr_name[96] = {0};
    float active_prog = 0.0f;
    uint64_t active_pos_ms = 0, active_dur_ms = 0;
    bool active_is_mp4 = false;
    const bool is_playing_any = active_media_status(state, curr_name, sizeof(curr_name),
                                                    &active_prog, &active_pos_ms, &active_dur_ms, &active_is_mp4);

    float cur_item_y = media_y + 36.0f;
    for (unsigned i = 0; i < state->media_count; ++i) {
        const char *filename = state->media_files[i];
        const size_t len = strlen(filename);
        const bool is_wav = len > 4 && strcmp(filename + len - 4, ".wav") == 0;
        const bool is_mp4 = len > 4 && strcmp(filename + len - 4, ".mp4") == 0;
        const bool is_txt = len > 4 && strcmp(filename + len - 4, ".txt") == 0;
        const bool is_this_playing = is_playing_any && strcmp(curr_name, filename) == 0;
        const DSMediaItemLayout item = media_item_layout(layout, cur_item_y, is_this_playing && is_mp4);

        char stem[96] = {0};
        if (len > 4) snprintf(stem, sizeof(stem), "%.*s", (int)(len - 4), filename);
        else snprintf(stem, sizeof(stem), "%s", filename);

        char c_check[640];
        snprintf(c_check, sizeof(c_check), "%s/%s.txt", state->evidence_dir, stem);
        const bool has_txt = (access(c_check, F_OK) == 0);
        snprintf(c_check, sizeof(c_check), "%s/%s_tags.txt", state->evidence_dir, stem);
        const bool has_tags = (access(c_check, F_OK) == 0);
        snprintf(c_check, sizeof(c_check), "%s/%s_waveform.txt", state->evidence_dir, stem);
        const bool has_wf = (access(c_check, F_OK) == 0);
        snprintf(c_check, sizeof(c_check), "%s/%s.srt", state->evidence_dir, stem);
        const bool has_srt = (access(c_check, F_OK) == 0);

        if (is_this_playing) {
            load_recording_tags(state, filename);
            const unsigned cur_sec = (unsigned)(active_pos_ms / 1000ULL);
            const unsigned dur_sec = (unsigned)(active_dur_ms / 1000ULL);
            const float card_h = is_mp4 ? 258.0f : 76.0f;

            rect(state, media_x - 4, cur_item_y - 2, column_width - 24, card_h, 0.07f, 0.18f, 0.15f, 0.95f);

            char label[128];
            const bool paused = is_mp4 && ds_media_player_is_paused(state->media_player);
            snprintf(label, sizeof(label), "%s %s  %02u:%02u / %02u:%02u (%u%%)",
                     paused ? "|>" : "||", filename, cur_sec / 60, cur_sec % 60,
                     dur_sec / 60, dur_sec % 60, (unsigned)(active_prog * 100.0f));
            text(state, label, media_x + 4, cur_item_y + 4, fit_text_scale(label, 1.05f, column_width - 32), 0.95f, 0.98f, 0.94f, 1.0f);

            if (is_mp4) {
                draw_mp4_quad(state, item.video_box);
            }

            /* Timeline waveform scrubber bar */
            const float bar_x = item.scrubber.x;
            const float bar_w = item.scrubber.w;
            const float bar_y = item.scrubber.y + 4.0f;
            if (state->loaded_waveform_count > 0) {
                const size_t wf_n = state->loaded_waveform_count;
                const float slot_w = bar_w / (float)wf_n;
                const float draw_w = fmaxf(1.0f, slot_w - 1.0f);
                for (size_t b = 0; b < wf_n; ++b) {
                    const float norm_x = ((float)b + 0.5f) / (float)wf_n;
                    const float amp = fminf(1.0f, fmaxf(0.12f, state->loaded_waveform[b] * 4.0f));
                    const float bh = amp * 12.0f;
                    const float by = bar_y + (12.0f - bh) * 0.5f;
                    const bool played = norm_x <= active_prog;
                    rect(state, bar_x + (float)b * slot_w, by, draw_w, bh,
                         played ? pal->mint.r : 0.18f,
                         played ? pal->mint.g : 0.26f,
                         played ? pal->mint.b : 0.24f,
                         played ? 0.95f : 0.70f);
                }
            } else {
                rect(state, bar_x, bar_y, bar_w, 8, 0.12f, 0.18f, 0.17f, 1.0f);
                rect(state, bar_x, bar_y, bar_w * active_prog, 8, pal->mint.r, pal->mint.g, pal->mint.b, 1.0f);
            }
            filled_circle(state, bar_x + bar_w * active_prog, bar_y + 4.0f, 5.5f, 1.0f, 1.0f, 1.0f, 1.0f);

            /* Tag dots along timeline */
            int active_tag_idx = -1;
            uint8_t active_tag_id = 0;
            if (active_dur_ms > 0) {
                for (size_t t = 0; t < state->loaded_tags_count; ++t) {
                    const uint64_t t_ms = state->loaded_tags[t].timestamp_ms;
                    if (t_ms <= active_dur_ms) {
                        const float dot_x = bar_x + (float)t_ms / (float)active_dur_ms * bar_w;
                        const uint8_t tid = state->loaded_tags[t].tag_id;
                        const float dr = tid == DS_TAG_WORD ? 0.2f : tid == DS_TAG_ANOMALY ? 1.0f : 0.85f;
                        const float dg = tid == DS_TAG_WORD ? 0.95f : tid == DS_TAG_ANOMALY ? 0.55f : 0.35f;
                        const float db = tid == DS_TAG_WORD ? 0.4f : tid == DS_TAG_ANOMALY ? 0.1f : 0.95f;
                        filled_circle(state, dot_x, bar_y + 4.0f, 3.2f, dr, dg, db, 1.0f);
                    }
                    if (t_ms >= (active_pos_ms > 1200ULL ? active_pos_ms - 1200ULL : 0ULL) && t_ms <= active_pos_ms + 1200ULL) {
                        active_tag_idx = (int)t + 1;
                        active_tag_id = state->loaded_tags[t].tag_id;
                    }
                }
            }

            custom_button(state, item.prev_tag, "<< TAG", false);
            custom_button(state, item.next_tag, "TAG >>", false);
            if (is_mp4) {
                custom_button(state, item.pause_btn, paused ? "PLAY" : "PAUSE", paused);
            }
            custom_button(state, item.stop, "STOP", false);

            if (!is_mp4) {
                char tag_status[64];
                if (active_tag_idx > 0) {
                    const char *tname = active_tag_id == DS_TAG_WORD ? "WORD" : active_tag_id == DS_TAG_ANOMALY ? "ANOMALY" : "BOOKMARK";
                    snprintf(tag_status, sizeof(tag_status), "TAG #%d [%s]", active_tag_idx, tname);
                } else {
                    snprintf(tag_status, sizeof(tag_status), "TAGS: %zu", state->loaded_tags_count);
                }
                text(state, tag_status, media_x + 204.0f, cur_item_y + 49.0f, 1.0f, pal->mint.r, pal->mint.g, pal->mint.b, 1.0f);
            }

            rect(state, item.del_active.x, item.del_active.y, item.del_active.w, item.del_active.h, 0.45f, 0.12f, 0.12f, 0.9f);
            text(state, "DEL", item.del_active.x + 8.0f, item.del_active.y + 7.0f, 1.0f, 1.0f, 0.8f, 0.8f, 1.0f);

            cur_item_y += is_mp4 ? 264.0f : 82.0f;
        } else {
            char label[128];
            if (is_wav) snprintf(label, sizeof(label), "> %s", filename);
            else if (is_mp4) snprintf(label, sizeof(label), "> [MP4] %s", filename);
            else if (is_txt) snprintf(label, sizeof(label), "## %s", filename);
            else snprintf(label, sizeof(label), "- %s", filename);

            const float lr = is_wav ? pal->mint.r : is_mp4 ? 0.45f : pal->muted.r;
            const float lg = is_wav ? pal->mint.g : is_mp4 ? 0.85f : pal->muted.g;
            const float lb = is_wav ? pal->mint.b : is_mp4 ? 0.95f : pal->muted.b;
            text(state, label, media_x, cur_item_y + 4, fit_text_scale(label, 0.95f, column_width - 150.0f), lr, lg, lb, 1.0f);

            float badge_x = media_x + column_width - 152.0f;
            if (has_txt) { text(state, "TXT", badge_x, cur_item_y + 4, 0.8f, 0.35f, 0.85f, 0.75f, 1.0f); badge_x += 24.0f; }
            if (has_tags) { text(state, "TAGS", badge_x, cur_item_y + 4, 0.8f, 1.0f, 0.65f, 0.2f, 1.0f); badge_x += 30.0f; }
            if (has_wf) { text(state, "WF", badge_x, cur_item_y + 4, 0.8f, 0.4f, 0.8f, 1.0f, 1.0f); badge_x += 20.0f; }
            if (has_srt) text(state, "SRT", badge_x, cur_item_y + 4, 0.8f, 0.8f, 0.8f, 0.8f, 1.0f);

            rect(state, item.del_idle.x, item.del_idle.y, item.del_idle.w, item.del_idle.h, 0.35f, 0.12f, 0.12f, 0.85f);
            text(state, "DEL", item.del_idle.x + 8.0f, item.del_idle.y + 4.0f, 0.9f, 1.0f, 0.75f, 0.75f, 1.0f);

            rect(state, media_x, cur_item_y + 28, column_width - 32, 1, 0.17f, 0.24f, 0.23f, 0.6f);
            cur_item_y += 36.0f;
        }
    }
}

typedef struct {
    DSRect voice_btns[4];
} DSGuideLayout;

static DSGuideLayout guide_layout(const DeadSpeak *state) {
    const float top = state->ui.body.y, x = state->ui.body.x + 16.0f;
    const float available = state->ui.body.w - 32.0f;
    const float btn_gap = 8.0f;
    const float btn_w = (available - btn_gap * 3.0f) * 0.25f;
    DSGuideLayout gl = {0};
    for (unsigned i = 0; i < 4; ++i) {
        gl.voice_btns[i] = (DSRect){x + (float)i * (btn_w + btn_gap), top + 122.0f, btn_w, 38.0f};
    }
    return gl;
}

static void draw_guide(DeadSpeak *state, const DSPalette *pal) {
    const float top = state->ui.body.y, x = state->ui.body.x + 16.0f;
    const float available = state->ui.body.w - 32.0f;
    const DSGuideLayout gl = guide_layout(state);
    const uint64_t now = now_millis();

    text(state, "SENSOR & VOICE DIAGNOSTICS", x, top + 20, fit_text_scale("SENSOR & VOICE DIAGNOSTICS", 1.8f, available), pal->mint.r, pal->mint.g, pal->mint.b, 1.0f);

    /* Live hardware sensor readiness summary */
    rect(state, x, top + 46, available, 52, 0.06f, 0.10f, 0.10f, 0.92f);
    const int64_t s_now = sensor_now_ns();
    char sensor_line[128];
    snprintf(sensor_line, sizeof(sensor_line), "MAG:%s  ACC:%s  GYR:%s  PROX:%s",
             ds_sensor_live(&state->readings[0], s_now) ? "LIVE" : ds_sensor_status_name(state->readings[0].status),
             ds_sensor_live(&state->readings[1], s_now) ? "LIVE" : ds_sensor_status_name(state->readings[1].status),
             ds_sensor_live(&state->readings[2], s_now) ? "LIVE" : ds_sensor_status_name(state->readings[2].status),
             state->sensors[3] ? (state->proximity_active ? "NEAR" : "FAR") : "N/A");
    text(state, sensor_line, x + 10, top + 56, fit_text_scale(sensor_line, 1.05f, available - 20), pal->mint.r, pal->mint.g, pal->mint.b, 1.0f);

    char baseline_line[128];
    snprintf(baseline_line, sizeof(baseline_line), "BASELINE: %s (%.1f UT) | CAMERA:%s",
             state->baseline.ready ? "READY" : "CALIBRATING", state->baseline.field,
             state->camera_enabled ? "ON" : "OFF");
    text(state, baseline_line, x + 10, top + 76, fit_text_scale(baseline_line, 1.0f, available - 20), pal->muted.r, pal->muted.g, pal->muted.b, 1.0f);

    /* Interactive Formant Voice Test buttons */
    text(state, "TAP TO TEST PHONEME SYNTHESIZER:", x, top + 106, fit_text_scale("TAP TO TEST PHONEME SYNTHESIZER:", 1.05f, available), pal->muted.r, pal->muted.g, pal->muted.b, 1.0f);
    static const char *const vnames[4] = {"CLASSIC", "WHISPER", "DYNAMIC", "SPIRIT"};
    for (unsigned i = 0; i < 4; ++i) {
        const bool selected = (unsigned)state->voice_config.mode == i;
        custom_button(state, gl.voice_btns[i], vnames[i], selected);
    }
    if (state->guide_voice_label[0] && now < state->guide_voice_last_ms + 4000U) {
        text(state, state->guide_voice_label, x, top + 166, fit_text_scale(state->guide_voice_label, 1.0f, available), pal->amber.r, pal->amber.g, pal->amber.b, 1.0f);
    }

    text(state, "FIELD NOTES", x, top + 192, 1.8f, pal->mint.r, pal->mint.g, pal->mint.b, 1.0f);
    static const char *const notes[] = {
        "MAGNETIC READINGS CHANGE NEAR", "METAL ELECTRONICS AND MOVEMENT",
        "WORDS ARE SENSOR OUTPUTS", "NOT VERIFIED COMMUNICATION",
        "PAIR READINGS WITH OTHER TOOLS", "AND CHECK ORDINARY CAUSES",
        "NEVER USE OUTPUT FOR IMPORTANT", "LIFE DECISIONS",
        "RECORDING MUXES FILTERED VIDEO", "AND AAC AUDIO INTO MP4",
        "TAP 16:9 / 9:16 / 4:5 FOR FRAMING", "TAP HUD / CLEAN FOR BURN-IN",
        "LOG PLAYS BACK WAV AND MP4 CLIPS", "WITH TAG TIMELINE SCRUBBING",
        "HANN WINDOW + SELF-VOX GATING", "REDUCES SPEECH SUB-BASS FALSE TAGS",
        "HOLD STILL FOR TWENTY REAL SAMPLES",
        "WORDS REQUIRE A FIELD DEVIATION"
    };
    for (size_t i = 0; i < sizeof(notes) / sizeof(notes[0]); ++i) {
        const bool warning = i == 6 || i == 7;
        const bool highlighted = i == 2 || i == 8 || i == 12;
        const DSColor c = warning ? pal->amber : (highlighted ? pal->mint : pal->muted);
        text(state, notes[i], x, top + 224.0f + (float)i * 25.0f,
             fit_text_scale(notes[i], highlighted ? 1.25f : 1.1f, available),
             c.r, c.g, c.b, 1.0f);
    }
}

typedef struct {
    DSRect refresh, search, pagination;
} DSExploreControlsLayout;

static DSExploreControlsLayout explore_controls_layout(const DeadSpeak *state) {
    const DSRect controls = state->ui.explore_controls, rows = state->ui.explore_rows;
    return (DSExploreControlsLayout){
        .refresh = {controls.x + controls.w - 90.0f, controls.y + 6.0f, 78.0f, 44.0f},
        .search = {controls.x + 12.0f, controls.y + 55.0f, controls.w - 24.0f, 48.0f},
        .pagination = {rows.x, state->ui.explore_footer_y + 12.0f, rows.w, 44.0f},
    };
}

static void draw_explore(DeadSpeak *state, const DSPalette *pal) {
    const DSRect controls = state->ui.explore_controls, rows = state->ui.explore_rows;
    const float x = rows.x + 16.0f, available = rows.w - 32.0f;
    const float footer = state->ui.explore_footer_y;
    text(state, "WIKIPEDIA FIELD INDEX", controls.x + 16, controls.y + 20,
         fit_text_scale("WIKIPEDIA FIELD INDEX", 1.7f, controls.w - 124), pal->mint.r, pal->mint.g, pal->mint.b, 1.0f);
    text(state, "REFRESH", controls.x + controls.w - 74, controls.y + 20, 0.95f, pal->amber.r, pal->amber.g, pal->amber.b, 1.0f);
    const char *index_status = atomic_load(&state->explore.loading) ? "REFRESHING INDEX" :
        (atomic_load(&state->explore.failed) ? "REFRESH FAILED - SAVED INDEX" : "REPORTED HAUNTED LOCATIONS");
    text(state, index_status, controls.x + 16, controls.y + 42,
         fit_text_scale(index_status, 1.0f, controls.w - 32), pal->muted.r, pal->muted.g, pal->muted.b, 1.0f);
    rect(state, controls.x + 12, controls.y + 61, controls.w - 24, 36, 0.13f, 0.18f, 0.18f, 1.0f);
    const char *query_label = state->explore_query[0] ? state->explore_query : "TAP TO SEARCH";
    const DSColor qc = state->explore_query[0] ? pal->mint : pal->muted;
    text(state, query_label, controls.x + 24, controls.y + 73, fit_text_scale(query_label, 1.4f, controls.w - 48),
         qc.r, qc.g, qc.b, 1.0f);

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
        text(state, "LOADING PUBLIC PLACE INDEX", x, rows.y + 17, fit_text_scale("LOADING PUBLIC PLACE INDEX", 1.3f, available), pal->amber.r, pal->amber.g, pal->amber.b, 1.0f);
        text(state, "WIKIPEDIA COMMUNITY CATEGORIES", x, rows.y + 47, fit_text_scale("WIKIPEDIA COMMUNITY CATEGORIES", 1.0f, available), pal->muted.r, pal->muted.g, pal->muted.b, 1.0f);
    } else if (match_count == 0) {
        const char *empty = atomic_load(&state->explore.failed) ? "INDEX UNAVAILABLE - TAP REFRESH" : "NO MATCHING PLACES";
        text(state, empty, x, rows.y + 17, fit_text_scale(empty, 1.1f, available), pal->amber.r, pal->amber.g, pal->amber.b, 1.0f);
    }
    for (size_t i = 0; i < count; ++i) {
        const float row_top = rows.y + (float)i * DS_EXPLORE_ROW_HEIGHT;
        const float y = row_top + 7.0f;
        text(state, places[i].title, x, y, fit_text_scale(places[i].title, 1.4f, available), pal->mint.r, pal->mint.g, pal->mint.b, 1.0f);
        char source_line[80];
        snprintf(source_line, sizeof(source_line), "%s  %s", places[i].region,
                 places[i].wikidata_id[0] ? places[i].wikidata_id : "NO WIKIDATA ITEM");
        text(state, source_line, x, row_top + DS_EXPLORE_ROW_SOURCE, fit_text_scale(source_line, 0.8f, available), pal->muted.r, pal->muted.g, pal->muted.b, 1.0f);
        if (places[i].has_coordinates) {
            char coordinate_line[64];
            snprintf(coordinate_line, sizeof(coordinate_line), "LAT %.0f  LON %.0f",
                     places[i].latitude, places[i].longitude);
            text(state, coordinate_line, x, y + 36.0f, fit_text_scale(coordinate_line, 0.75f, available), pal->amber.r, pal->amber.g, pal->amber.b, 1.0f);
        }
        rect(state, x, y + 45.0f, available, 1.0f, 0.17f, 0.24f, 0.23f, 1.0f);
    }
    char page_label[48];
    snprintf(page_label, sizeof(page_label), "MATCHES %u  PAGE %u", (unsigned)match_count,
             (unsigned)(state->explore_page + 1U));
    text(state, page_label, x, footer, fit_text_scale(page_label, 0.95f, available), pal->muted.r, pal->muted.g, pal->muted.b, 1.0f);
    text(state, "PREV", x, footer + 31, 1.2f, pal->muted.r, pal->muted.g, pal->muted.b, 1.0f);
    text(state, "NEXT", rows.x + rows.w - 74, footer + 31, 1.2f, pal->mint.r, pal->mint.g, pal->mint.b, 1.0f);
    text(state, "TOP: WIKIPEDIA  LOWER: WIKIDATA", x, footer + 61,
         fit_text_scale("TOP: WIKIPEDIA  LOWER: WIKIDATA", 0.82f, available), pal->amber.r, pal->amber.g, pal->amber.b, 1.0f);
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

static DSRect custom_modal(const DeadSpeak *state) {
    return (DSRect){12, 58, state->width - 24, state->height - 114};
}

typedef struct {
    DSRect modal, close, bundled, custom, voice[4], prev, add, test_vox, next;
    float key_height;
    size_t page_size;
} DSCustomModalLayout;

static DSCustomModalLayout custom_modal_layout(const DeadSpeak *state) {
    const DSRect modal = custom_modal(state);
    const float half = (modal.w - 30.0f) / 2.0f;
    const float quarter = (modal.w - 44.0f) / 4.0f;
    const float middle_w = (modal.w - 180.0f) / 2.0f;
    DSCustomModalLayout l = {
        .modal = modal,
        .close = {modal.x + modal.w - 48.0f, modal.y + 4.0f, 44.0f, 36.0f},
        .bundled = {modal.x + 10.0f, modal.y + 44.0f, half, 36.0f},
        .custom = {modal.x + 20.0f + half, modal.y + 44.0f, half, 36.0f},
        .prev = {modal.x + 10.0f, modal.y + modal.h - 70.0f, 48.0f, 40.0f},
        .add = {modal.x + 62.0f, modal.y + modal.h - 70.0f, middle_w, 40.0f},
        .test_vox = {modal.x + 62.0f + middle_w + 6.0f, modal.y + modal.h - 70.0f, middle_w, 40.0f},
        .next = {modal.x + modal.w - 58.0f, modal.y + modal.h - 70.0f, 48.0f, 40.0f},
        .key_height = fminf(42.0f, (modal.h - 180.0f) / 4.0f),
        .page_size = (size_t)fmaxf(1.0f, fminf(6.0f, floorf((modal.h - 206.0f) / 42.0f))),
    };
    for (unsigned i = 0; i < 4; ++i) {
        l.voice[i] = (DSRect){modal.x + 10.0f + (float)i * (quarter + 8.0f), modal.y + 86.0f, quarter, 32.0f};
    }
    return l;
}

static DSRect custom_modal_key_rect(DSRect modal, float key_height, unsigned row, unsigned col, size_t row_len) {
    const float key_width = (modal.w - 20.0f) / (float)row_len;
    return (DSRect){modal.x + 10.0f + (float)col * key_width, modal.y + 168.0f + (float)row * key_height,
                    key_width - 2.0f, key_height - 3.0f};
}

static DSRect custom_modal_action_rect(DSRect modal, float key_height, unsigned idx) {
    const float key_width = (modal.w - 20.0f) / 3.0f;
    return (DSRect){modal.x + 10.0f + (float)idx * key_width, modal.y + 168.0f + 3.0f * key_height,
                    key_width - 2.0f, key_height - 3.0f};
}

static DSRect custom_modal_row_del_rect(DSRect modal, size_t row) {
    return (DSRect){modal.x + modal.w - 54.0f, modal.y + 126.0f + (float)row * 42.0f, 44.0f, 38.0f};
}

static void draw_custom_words(DeadSpeak *state) {
    const DSCustomModalLayout l = custom_modal_layout(state);
    const DSRect modal = l.modal;
    rect(state, 0, 0, state->width, state->height, 0, 0, 0, 0.72f);
    rect(state, modal.x, modal.y, modal.w, modal.h, 0.04f, 0.07f, 0.08f, 1);
    text(state, "WORD BANK & VOICE", modal.x + 12, modal.y + 16, 1.7f, 0.9f, 0.98f, 0.94f, 1);
    custom_button(state, l.close, "X", false);

    custom_button(state, l.bundled, "BUNDLED (5K)", !state->custom_selected);
    custom_button(state, l.custom, "CUSTOM ONLY", state->custom_selected);

    /* Voice Mode Selector Bar (Classic, Whisper, Dynamic, Spirit Box) */
    const char *voice_modes[] = {"CLASSIC", "WHISPER", "DYNAMIC", "SPIRIT BOX"};
    for (unsigned i = 0; i < 4; ++i) {
        const bool sel = state->voice_config.mode == (DSVoiceMode)i;
        custom_button(state, l.voice[i], voice_modes[i], sel);
    }

    if (state->custom_editing) {
        rect(state, modal.x + 10, modal.y + 124, modal.w - 20, 38, 0.1f, 0.16f, 0.17f, 1);
        const char *input = state->custom_input[0] ? state->custom_input : "TYPE A WORD OR PHRASE";
        text(state, input, modal.x + 16, modal.y + 136, fit_text_scale(input, 1.8f, modal.w - 32), 0.9f, 0.98f, 0.94f, 1);
        const char *rows[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
        for (unsigned row = 0; row < 3; ++row) {
            const size_t rlen = strlen(rows[row]);
            for (unsigned column = 0; rows[row][column]; ++column) {
                const char key[2] = {rows[row][column], 0};
                custom_button(state, custom_modal_key_rect(modal, l.key_height, row, column, rlen), key, false);
            }
        }
        const char *actions[] = {"SPACE", "DEL", "SAVE"};
        for (unsigned i = 0; i < 3; ++i)
            custom_button(state, custom_modal_action_rect(modal, l.key_height, i), actions[i], i == 2);
    } else {
        const size_t count = ds_custom_words_count(&state->custom_words);
        const size_t last_page = count ? (count - 1) / l.page_size : 0;
        if (state->custom_page > last_page) state->custom_page = last_page;
        for (size_t row = 0; row < l.page_size && state->custom_page * l.page_size + row < count; ++row) {
            const size_t index = state->custom_page * l.page_size + row;
            const float y = modal.y + 126 + (float)row * 42;
            const char *raw_word = ds_custom_words_word(&state->custom_words, index);
            char item_label[48];
            snprintf(item_label, sizeof(item_label), "%u. %s", (unsigned)(index + 1), raw_word);
            text(state, item_label, modal.x + 14, y + 14, fit_text_scale(item_label, 1.4f, modal.w - 74), 0.9f, 0.98f, 0.94f, 1);
            custom_button(state, custom_modal_row_del_rect(modal, row), "DEL", false);
        }
        if (!count) text(state, "NO CUSTOM WORDS - ADD SOME BELOW", modal.x + 14, modal.y + 140,
            fit_text_scale("NO CUSTOM WORDS - ADD SOME BELOW", 1.3f, modal.w - 28), 0.8f, 0.85f, 0.84f, 1);
        custom_button(state, l.prev, "PREV", false);
        custom_button(state, l.add, "+ ADD", true);
        custom_button(state, l.test_vox, "TEST VOX", false);
        custom_button(state, l.next, "NEXT", false);
    }
    const char *message = state->custom_message[0] ? state->custom_message : "LOCAL STORAGE ONLY - NO CLOUD DEPENDENCIES";
    text(state, message, modal.x + 12, modal.y + modal.h - 20, fit_text_scale(message, 1.0f, modal.w - 24), 0.96f, 0.75f, 0.43f, 1);
}

typedef struct {
    DSRect spk, geiger, style, tint, plus_tag, filter_pill, cal, sens;
    float sensor_top;
} DSCameraControlsLayout;

static DSCameraControlsLayout camera_controls_layout(const DeadSpeak *state) {
    const float w = (float)state->width;
    const float sensor_top = state->height < 650 ? 76.0f : 142.0f;
    const float fp_w = fmaxf(52.0f, state->camera_ui.aspect_btn.x - 22.0f);
    return (DSCameraControlsLayout){
        .spk = {w - 76.0f, 12.0f, 68.0f, 34.0f},
        .geiger = {w - 162.0f, 12.0f, 82.0f, 34.0f},
        .style = {w - 256.0f, 12.0f, 90.0f, 34.0f},
        .tint = {w - 336.0f, 12.0f, 76.0f, 34.0f},
        .plus_tag = state->camera_ui.hud_btn,
        .filter_pill = {16.0f, state->camera_ui.aspect_btn.y, fp_w, state->camera_ui.aspect_btn.h},
        .cal = {20.0f, sensor_top + 84.0f, 112.0f, 40.0f},
        .sens = {138.0f, sensor_top + 84.0f, 96.0f, 40.0f},
        .sensor_top = sensor_top,
    };
}

static void draw_filter_hud(DeadSpeak *state, uint64_t now) {
    const DSFilterHUD hud = ds_filter_hud((float)state->width, (float)state->height, state->filter_hud_deadline, now);
    if (!hud.visible || hud.alpha <= 0.001f) return;

    const float alpha = hud.alpha;
    const DSRect card = hud.card;
    const float cx = card.x + card.w * 0.5f;

    /* Theme-aware accent colors */
    DSColor accent = {0.92f, 0.98f, 0.95f};
    if (state->hud_hue > 0.001f) {
        accent = ds_interpolate_hud_accent(state->hud_hue);
    }

    /* Filter metadata */
    static const struct {
        const char *title;
        const char *mode;
        float r, g, b;
    } filter_meta[6] = {
        [DS_VIDEO_FILTER_NIGHT]   = {"NIGHT VISION", "PHOSPHOR GREEN",  0.15f, 1.00f, 0.35f},
        [DS_VIDEO_FILTER_THERMAL] = {"THERMAL HEAT", "HEATMAP SPECTRUM", 1.00f, 0.55f, 0.15f},
        [DS_VIDEO_FILTER_EDGE]    = {"EDGE CONTOUR", "SOBEL SPECTRAL",   0.30f, 0.95f, 0.85f},
        [DS_VIDEO_FILTER_MONO]    = {"MONOCHROME",   "IR CONTRAST",      0.90f, 0.92f, 0.95f},
        [DS_VIDEO_FILTER_SEPIA]   = {"SEPIA RETRO",  "SPECTRAL WARM",    0.95f, 0.80f, 0.45f},
        [DS_VIDEO_FILTER_RAW]     = {"RAW SENSOR",   "DIRECT FEED",      0.80f, 0.95f, 0.88f},
    };

    const unsigned filter_idx = (unsigned)state->video_filter % 6U;
    const auto meta = filter_meta[filter_idx];

    /* 1. Translucent dark rounded card background */
    rect(state, card.x, card.y, card.w, card.h, 0.04f, 0.05f, 0.05f, 0.90f * alpha);

    /* 2. Top accent highlight and outer border */
    line(state, card.x, card.y, card.x + card.w, card.y, accent.r, accent.g, accent.b, 0.90f * alpha);
    line(state, card.x, card.y + card.h, card.x + card.w, card.y + card.h, 0.20f, 0.28f, 0.25f, 0.70f * alpha);
    line(state, card.x, card.y, card.x, card.y + card.h, 0.20f, 0.28f, 0.25f, 0.70f * alpha);
    line(state, card.x + card.w, card.y, card.x + card.w, card.y + card.h, 0.20f, 0.28f, 0.25f, 0.70f * alpha);

    /* 3. Top hint: < SWIPE TO CYCLE > */
    const float hint_scale = ds_sp_to_scale(DS_FONT_SIZE_BADGE);
    const float hint_w = ds_text_width("< SWIPE TO CYCLE >", hint_scale, DS_FONT_STANDARD);
    text_styled(state, "< SWIPE TO CYCLE >", cx - hint_w * 0.5f, card.y + 12.0f, hint_scale,
                DS_FONT_STANDARD, 0.65f, 0.85f, 0.78f, 0.85f * alpha);

    /* 4. Filter Title in bold */
    const float title_scale = ds_fit_text_scale_styled(meta.title, ds_sp_to_scale(DS_FONT_SIZE_BODY_LG), card.w - 20.0f, DS_FONT_BOLD);
    const float title_w = ds_text_width(meta.title, title_scale, DS_FONT_BOLD);
    text_styled(state, meta.title, cx - title_w * 0.5f, card.y + 32.0f, title_scale,
                DS_FONT_BOLD, meta.r, meta.g, meta.b, alpha);

    /* 5. Subtitle description */
    const float sub_scale = ds_sp_to_scale(DS_FONT_SIZE_BADGE);
    const float sub_w = ds_text_width(meta.mode, sub_scale, DS_FONT_STANDARD);
    text_styled(state, meta.mode, cx - sub_w * 0.5f, card.y + 54.0f, sub_scale,
                DS_FONT_STANDARD, 0.75f, 0.88f, 0.82f, 0.80f * alpha);

    /* 6. Filter index pips across the bottom */
    constexpr unsigned PIP_COUNT = 6U;
    constexpr float PIP_W = 12.0f;
    constexpr float PIP_GAP = 5.0f;
    constexpr float TOTAL_PIPS_W = (float)PIP_COUNT * PIP_W + (float)(PIP_COUNT - 1U) * PIP_GAP;
    const float start_pip_x = cx - TOTAL_PIPS_W * 0.5f;
    for (unsigned p = 0; p < PIP_COUNT; ++p) {
        const bool active = (p == filter_idx);
        const float px = start_pip_x + (float)p * (PIP_W + PIP_GAP);
        rect(state, px, card.y + 72.0f, PIP_W, 3.0f,
             active ? accent.r : 0.22f,
             active ? accent.g : 0.28f,
             active ? accent.b : 0.26f,
             active ? 0.95f * alpha : 0.45f * alpha);
    }
}

static void draw_camera(DeadSpeak *state) {
    const DSCameraUI ui = state->camera_ui;
    const uint64_t now = now_millis();
    const bool recording = ds_video_active(&state->video) || atomic_load(&state->audio_active);
    if (state->preview_rgba && state->camera_texture &&
        ds_video_copy_frame(&state->video, state->preview_rgba, DS_PREVIEW_CAPACITY,
                            &state->preview_width, &state->preview_height, &state->preview_timestamp)) {
        state->preview_received_ms = now;
        state->preview_rotation = ds_video_preview_orientation(&state->video);
        glBindTexture(GL_TEXTURE_2D, state->camera_texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, state->preview_width, state->preview_height,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, state->preview_rgba);
    }
    const bool preview_live = state->camera_enabled && ds_video_preview_ready(&state->video) &&
        state->preview_received_ms && now - state->preview_received_ms < 2000;
    float uv[12];
    if (preview_live && ds_preview_uv(state->preview_width, state->preview_height, state->preview_rotation,
                                    state->width, state->height, uv)) {
        const GLfloat vertices[] = {-1,1, 1,1, -1,-1, -1,-1, 1,1, 1,-1};
        glUseProgram(state->camera_program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, state->camera_texture);
        glUniform1i(state->camera_image_loc, 0);
        glUniform1i(state->camera_filter_loc, (int)state->video_filter);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glDisableVertexAttribArray(1);
    }
    /* Top Bar */
    const DSCameraControlsLayout ctrl = camera_controls_layout(state);
    DSColor accent = {0.92f, 0.98f, 0.95f};
    if (state->hud_hue > 0.001f) {
        accent = ds_interpolate_hud_accent(state->hud_hue);
    }
    if (!state->minimal_hud) {
        rect(state, 0, 0, state->width, 58, 0, 0, 0, 0.6f);
        const float header_max_w = (float)(state->width - 340 - 20);
        const float header_scale = ds_fit_text_scale_styled("DEADSPEAK", ds_sp_to_scale(DS_FONT_SIZE_BODY_LG), header_max_w, DS_FONT_BOLD);
        text_styled(state, "DEADSPEAK", 16, 16, header_scale, DS_FONT_BOLD, accent.r, accent.g, accent.b, 1);

        /* Top Right Voice Synthesizer, Geiger, Style and Tint Buttons */
        custom_button(state, ctrl.spk, state->voice_config.enabled ? "VOX ON" : "VOX OFF", state->voice_config.enabled);

        const bool geiger_on = ds_voice_player_get_geiger_enabled(state->voice_player);
        custom_button(state, ctrl.geiger, geiger_on ? "GEIGER ON" : "GEIGER OFF", geiger_on);

        static const char *const theme_names[] = {"#ORIGINAL", "ABDUCTION", "AI"};
        custom_button(state, ctrl.style, theme_names[(unsigned)state->theme_mode % 3U], state->theme_mode != DS_THEME_ORIGINAL);

        char tint_label[32];
        snprintf(tint_label, sizeof(tint_label), "TINT %u%%", (unsigned)(state->hud_hue * 100.0f));
        custom_button(state, ctrl.tint, tint_label, state->hud_hue > 0.05f);

        if (state->export_status_expiry_ms > now && state->export_status_message[0]) {
            const float toast_w = fminf(300.0f, state->width - 40.0f);
            const float toast_x = (state->width - toast_w) * 0.5f;
            rect(state, toast_x, 60.0f, toast_w, 24.0f, 0.12f, 0.38f, 0.28f, 0.95f);
            text(state, state->export_status_message, toast_x + 8.0f, 65.0f,
                 fit_text_scale(state->export_status_message, 1.0f, toast_w - 16.0f),
                 1.0f, 1.0f, 1.0f, 1.0f);
        }

        if (state->proximity_active) {
            const float prox_x = state->width - 430.0f, prox_y = 12.0f;
            rect(state, prox_x, prox_y, 88, 34, 0.95f, 0.35f, 0.05f, 0.85f);
            text(state, "PROX NEAR", prox_x + 6, prox_y + 10, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);

            /* Expanding concentric radar ripple rings */
            const float rc_x = prox_x + 44.0f, rc_y = prox_y + 17.0f;
            const uint64_t phase1 = now % 1000U;
            const float ring_r1 = 18.0f + ((float)phase1 / 1000.0f) * 32.0f;
            const float ring_a1 = (1.0f - (float)phase1 / 1000.0f) * 0.85f;
            circle(state, rc_x, rc_y, ring_r1, 1.0f, 0.45f, 0.1f, ring_a1);

            const uint64_t phase2 = (now + 500U) % 1000U;
            const float ring_r2 = 18.0f + ((float)phase2 / 1000.0f) * 32.0f;
            const float ring_a2 = (1.0f - (float)phase2 / 1000.0f) * 0.85f;
            circle(state, rc_x, rc_y, ring_r2, 1.0f, 0.45f, 0.1f, ring_a2);
        }

        char status[80];
        if (recording) snprintf(status, sizeof(status), "REC %02llu:%02llu",
            (unsigned long long)((now - state->capture_started_ms) / 60000),
            (unsigned long long)((now - state->capture_started_ms) / 1000 % 60));
        else snprintf(status, sizeof(status), "%s", preview_live ? "LIVE CAMERA" :
            !state->camera_enabled ? "CAMERA OFF" : ds_video_failed(&state->video) ? "CAMERA ERROR - TAP CAM" :
            has_camera_permission(state) ? "STARTING CAMERA" : "TAP CAM TO ALLOW CAMERA");
        text_styled(state, status, 16, 38, ds_fit_text_scale_styled(status, ds_sp_to_scale(DS_FONT_SIZE_MICRO), header_max_w, DS_FONT_STANDARD), DS_FONT_STANDARD, recording ? 1 : 0.8f, recording ? 0.25f : 0.9f, recording ? 0.25f : 0.85f, 1);
        if (!preview_live) {
            const char *message = state->camera_enabled ? "TAP CAM TO ENABLE OR RETRY" : "CAMERA IS OFF";
            text(state, message, 16, 110, fit_text_scale(message, 1.5f, state->width - 100), 0.8f, 0.9f, 0.85f, 1);
        }
        const char *tools[] = {"CAM", "SCAN", "FX", "WORDS", "INFO"};
        for (unsigned i = 0; i < 5; ++i)
            custom_button(state, ui.tools[i], tools[i], i == 0 ? preview_live : i == 1 ? state->scanning : i == 4 ? state->sensor_details : false);
    } else {
        /* Minimal HUD mode indication */
        text(state, "MINIMAL HUD - TAP TO EXPAND", 16, 18, 1.0f, 0.6f, 0.8f, 0.7f, 0.7f);
    }

    if (state->sensor_details && !state->minimal_hud) {
        const DSSensorReading *reading = &state->readings[0];
        char details[80];
        snprintf(details, sizeof(details), "MAG %s  SAMPLES %llu", ds_sensor_status_name(reading->status), (unsigned long long)reading->count);
        const float top = ctrl.sensor_top;
        rect(state, 12, top, state->width - 88, 132, 0, 0, 0, 0.72f);
        text(state, details, 20, top + 12, fit_text_scale(details, 1.1f, state->width - 108), 0.8f, 0.95f, 0.86f, 1);
        if (ds_sensor_live(reading, sensor_now_ns())) {
            snprintf(details, sizeof(details), "X %.1f  Y %.1f  Z %.1f UT", reading->xyz[0], reading->xyz[1], reading->xyz[2]);
            text(state, details, 20, top + 32, fit_text_scale(details, 1.0f, state->width - 108), 0.8f, 0.95f, 0.86f, 1);
        }
        if (state->sensors[3]) {
            snprintf(details, sizeof(details), "ACCEL %s  GYRO %s  PROX %s (%.0fcm)",
                ds_sensor_status_name(state->readings[1].status),
                ds_sensor_status_name(state->readings[2].status),
                state->proximity_active ? "NEAR" : "FAR",
                state->proximity_distance);
        } else {
            snprintf(details, sizeof(details), "ACCEL %s  GYRO %s  PROX N/A",
                ds_sensor_status_name(state->readings[1].status),
                ds_sensor_status_name(state->readings[2].status));
        }
        text(state, details, 20, top + 57, fit_text_scale(details, 1.0f, state->width - 108), 0.8f, 0.95f, 0.86f, 1);
        custom_button(state, ctrl.cal, "CALIBRATE", false);
        const char *sens_names[] = {"SENS: HI", "SENS: MED", "SENS: LO"};
        custom_button(state, ctrl.sens, sens_names[state->baseline.sensitivity % 3], false);
        if (state->baseline.ready) snprintf(details, sizeof(details), "BASE %.1f UT (%.1f)", state->baseline.field, ds_baseline_threshold(state->baseline.sensitivity));
        else snprintf(details, sizeof(details), "SAMPLES %u OF 20", state->baseline.count);
        text(state, details, 242, top + 98, fit_text_scale(details, 1.0f, state->width - 250), 0.8f, 0.95f, 0.86f, 1);
    }
    
    /* 1. SOUND SPECTRUM & LIVE OSCILLOSCOPE ANALYZER HUD */
    if (!state->minimal_hud) {
        /* Rolling Word History Feed */
        if (state->log_count > 0 && !state->sensor_details) {
            const unsigned show_count = state->log_count < 3U ? state->log_count : 3U;
            const float hist_x = 16.0f;
            const float hist_w = fminf(360.0f, state->width - 90.0f);
            const float hist_base_y = ui.audio.y - (float)show_count * 22.0f - 6.0f;
            if (hist_base_y > 70.0f) {
                rect(state, hist_x, hist_base_y - 2.0f, hist_w, (float)show_count * 22.0f + 4.0f, 0.0f, 0.0f, 0.0f, 0.50f);
                for (unsigned h = 0; h < show_count; ++h) {
                    const unsigned slot = (state->log_next + DS_LOG_CAPACITY - 1U - h) % DS_LOG_CAPACITY;
                    const uint64_t elapsed_s = state->word_times[slot] ? (now - state->word_times[slot]) / 1000U : 0U;
                    char item_str[96];
                    snprintf(item_str, sizeof(item_str), "%llus AGO  %s (%.1f uT%s)",
                        (unsigned long long)elapsed_s, state->words[slot], state->word_fields[slot],
                        state->word_whispers[slot] ? " W" : "");
                    const float alpha = 0.92f - (float)h * 0.24f;
                    text(state, item_str, hist_x + 8.0f, hist_base_y + (float)h * 22.0f + 4.0f,
                         fit_text_scale(item_str, 1.0f, hist_w - 16.0f),
                         0.80f, 0.98f, 0.88f, alpha);
                }
            }
        }

        /* Sub-Bass EVP Anomaly Banner */
        if (atomic_load(&state->sub_bass_anomaly) && now < state->sub_bass_anomaly_expiry_ms) {
            if (((now / 250U) % 2U) == 0) {
                rect(state, ui.audio.x, ui.audio.y - 22.0f, ui.audio.w, 20.0f, 1.0f, 0.45f, 0.05f, 0.95f);
                text(state, "SUB-BASS EVP BURST 80-300Hz", ui.audio.x + 8.0f, ui.audio.y - 18.0f,
                     fit_text_scale("SUB-BASS EVP BURST 80-300Hz", 1.0f, ui.audio.w - 16.0f), 1.0f, 1.0f, 1.0f, 1.0f);
            }
        }

        rect(state, ui.audio.x, ui.audio.y, ui.audio.w, ui.audio.h, 0, 0, 0, 0.65f);
        const bool audio_live = atomic_load(&state->audio_active);
        if (audio_live) {
            if (state->theme_mode == DS_THEME_ABDUCTION) {
                /* Abduction Theme: Bio-luminescent alien neon curve */
                const float osc_mid_y = ui.audio.y + ui.audio.h * 0.50f;
                const float step_x = (ui.audio.w - 8) / (float)(DS_OSCILLOSCOPE_SAMPLES - 1);
                for (int s = 0; s < DS_OSCILLOSCOPE_SAMPLES - 1; ++s) {
                    const float y1 = osc_mid_y - (float)atomic_load(&state->oscilloscope[s]) / 32768.0f * (ui.audio.h * 0.44f);
                    const float y2 = osc_mid_y - (float)atomic_load(&state->oscilloscope[s + 1]) / 32768.0f * (ui.audio.h * 0.44f);
                    line(state, ui.audio.x + 4 + (float)s * step_x, y1 - 1.0f, ui.audio.x + 4 + (float)(s + 1) * step_x, y2 - 1.0f, 0.1f, 1.0f, 0.85f, 0.4f);
                    line(state, ui.audio.x + 4 + (float)s * step_x, y1, ui.audio.x + 4 + (float)(s + 1) * step_x, y2, 0.4f, 1.0f, 0.95f, 0.95f);
                }
            } else if (state->theme_mode == DS_THEME_AI) {
                /* Artificial Intelligence Theme: 16-node digital matrix waveform */
                const float bar_w = (ui.audio.w - 12.0f) / 16.0f;
                for (unsigned n = 0; n < 16; ++n) {
                    const unsigned bin_idx = (n * DS_SPECTRUM_BINS) / 16;
                    const float level = (float)atomic_load(&state->spectrum_bins[bin_idx]) / 1000.0f;
                    const float node_h = fmaxf(4.0f, level * (ui.audio.h - 10.0f));
                    const float nx = ui.audio.x + 6.0f + (float)n * bar_w;
                    const float ny = ui.audio.y + ui.audio.h - 5.0f - node_h;
                    rect(state, nx, ny, bar_w - 2.0f, node_h, 0.2f, 0.7f, 1.0f, 0.92f);
                }
            } else {
                /* Original Theme: 12-segment segmented LED VU meter + phosphor trace */
                const float bar_width = (ui.audio.w - 8.0f) / (float)DS_SPECTRUM_BINS;
                const float cell_w = fmaxf(1.0f, bar_width - 1.5f);
                const float seg_h = (ui.audio.h - 6.0f) / 12.0f;
                const float cell_fill_h = fmaxf(1.0f, seg_h - 0.6f);

                for (unsigned i = 0; i < DS_SPECTRUM_BINS; ++i) {
                    const float level = (float)atomic_load(&state->spectrum_bins[i]) / 1000.0f;
                    const unsigned lit_segments = (unsigned)fminf(12.0f, level * 12.0f + 0.5f);
                    const float bx = ui.audio.x + 4.0f + (float)i * bar_width;

                    for (unsigned seg = 0; seg < 12; ++seg) {
                        const float by = ui.audio.y + ui.audio.h - 3.0f - (float)(seg + 1) * seg_h;
                        if (seg < lit_segments) {
                            const float r = seg < 8 ? 0.745f : 1.0f;
                            const float g = seg < 8 ? 1.0f : seg < 10 ? 0.706f : 0.05f;
                            const float b = seg < 8 ? 0.274f : seg < 10 ? 0.0f : 0.05f;
                            rect(state, bx, by, cell_w, cell_fill_h, 0.0f, 0.0f, 0.0f, 0.9f);
                            rect(state, bx + 0.5f, by + 0.3f, fmaxf(0.5f, cell_w - 1.0f), fmaxf(0.5f, cell_fill_h - 0.6f), r, g, b, 0.95f);
                        } else {
                            rect(state, bx, by, cell_w, cell_fill_h, 0.08f, 0.12f, 0.11f, 0.35f);
                        }
                    }
                }
                const float osc_mid_y = ui.audio.y + ui.audio.h * 0.48f;
                const float step_x = (ui.audio.w - 8) / (float)(DS_OSCILLOSCOPE_SAMPLES - 1);
                for (int s = 0; s < DS_OSCILLOSCOPE_SAMPLES - 1; ++s) {
                    const float y1 = osc_mid_y - (float)atomic_load(&state->oscilloscope[s]) / 32768.0f * (ui.audio.h * 0.38f);
                    const float y2 = osc_mid_y - (float)atomic_load(&state->oscilloscope[s + 1]) / 32768.0f * (ui.audio.h * 0.38f);
                    line(state, ui.audio.x + 4 + (float)s * step_x, y1, ui.audio.x + 4 + (float)(s + 1) * step_x, y2, 0.2f, 1.0f, 0.5f, 0.9f);
                }
            }
        } else {
            text(state, "MIC READY - SOUND ANALYZER", ui.audio.x + 8, ui.audio.y + 14,
                 fit_text_scale("MIC READY - SOUND ANALYZER", 1.1f, ui.audio.w - 16), 0.75f, 0.88f, 0.82f, 1);
        }
    }
    
    /* 2. EMF / MAGNETOMETER FLUCTUATOR & POLARITY BAR */
    rect(state, ui.magnetic.x, ui.magnetic.y, ui.magnetic.w, ui.magnetic.h, 0, 0, 0, 0.68f);
    const bool magnetic_live = ds_sensor_live(&state->readings[0], sensor_now_ns());
    char magnetic[64];
    float delta = 0.0f;
    if (magnetic_live) {
        delta = state->baseline.ready ? (state->smoothed_field - state->baseline.field) : 0.0f;
        const char *polarity = delta > 0.5f ? "[+PULL]" : delta < -0.5f ? "[-PUSH]" : "[N]";
        snprintf(magnetic, sizeof(magnetic), "EMF %.1f UT (%+.1f) %s", state->magnetic_field, delta, polarity);
    } else snprintf(magnetic, sizeof(magnetic), "EMF %s", ds_sensor_status_name(state->readings[0].status));
    text(state, magnetic, ui.magnetic.x + 8, ui.magnetic.y + 11,
         fit_text_scale(magnetic, 1.25f, ui.magnetic.w * 0.58f), 0.9f, 0.98f, 0.93f, 1);
    
    /* Peak-hold decay logic */
    if (state->signal > state->emf_peak_signal) {
        state->emf_peak_signal = state->signal;
        state->emf_peak_decay_ms = now + 500;
    } else if (now > state->emf_peak_decay_ms && state->emf_peak_signal > 0.0f) {
        state->emf_peak_signal -= 0.02f;
        if (state->emf_peak_signal < 0.0f) state->emf_peak_signal = 0.0f;
    }

    const float meter_x = ui.magnetic.x + ui.magnetic.w * 0.60f, meter_width = ui.magnetic.w * 0.36f;
    rect(state, meter_x, ui.magnetic.y + 9, meter_width, 14, 0.15f, 0.20f, 0.18f, 1);

    /* Graduated scale tick lines (0%, 25%, 50%, 75%, 100%) */
    for (unsigned t = 0; t <= 4; ++t) {
        const float tx = meter_x + (float)t * 0.25f * meter_width;
        line(state, tx, ui.magnetic.y + 7, tx, ui.magnetic.y + 10, 0.45f, 0.55f, 0.50f, 0.8f);
    }

    if (magnetic_live && state->signal > 0.0f) {
        const float r = state->signal < 0.4f ? 0.3f : state->signal < 0.7f ? 0.95f : 1.0f;
        const float g = state->signal < 0.4f ? 0.95f : state->signal < 0.7f ? 0.85f : 0.2f;
        const float b = state->signal < 0.4f ? 0.3f : 0.1f;
        rect(state, meter_x, ui.magnetic.y + 9, meter_width * state->signal, 14, r, g, b, 1);

        /* Analog needle pointer */
        const float needle_x = meter_x + fminf(1.0f, fmaxf(0.0f, state->signal)) * meter_width;
        line(state, needle_x, ui.magnetic.y + 5, needle_x, ui.magnetic.y + 25, 1.0f, 1.0f, 1.0f, 1.0f);
        filled_circle(state, needle_x, ui.magnetic.y + 16, 2.8f, 1.0f, 0.95f, 0.3f, 1.0f);

        /* Peak hold indicator line */
        if (state->emf_peak_signal > 0.05f) {
            rect(state, meter_x + meter_width * state->emf_peak_signal - 1.0f, ui.magnetic.y + 7, 2.0f, 18, 1.0f, 0.95f, 0.3f, 1.0f);
        }
    }

    /* 300ms High-EMF Alert Flasher */
    if (magnetic_live && (delta > 4.0f || delta < -4.0f || state->magnetic_field > 75.0f)) {
        emit_companion_tag(state, DS_TAG_ANOMALY);
        if (((now / 300U) % 2U) == 0) {
            rect(state, ui.magnetic.x + ui.magnetic.w - 96.0f, ui.magnetic.y - 18.0f, 92.0f, 18.0f, 0.95f, 0.15f, 0.10f, 0.95f);
            text(state, "EMF ALERT", ui.magnetic.x + ui.magnetic.w - 90.0f, ui.magnetic.y - 14.0f, 0.90f, 1.0f, 1.0f, 1.0f, 1.0f);
        }
    }

    /* 3. Dual-Tier Word Announcer */
    const unsigned latest_slot = (state->log_next + DS_LOG_CAPACITY - 1U) % DS_LOG_CAPACITY;
    const uint64_t latest_word_time = state->log_count > 0 ? state->word_times[latest_slot] : 0U;
    /* Holds triggered word on screen for 10,000 ms (10s) before resetting to Listening */
    const bool word_active = state->word[0] && (latest_word_time > 0 && (now - latest_word_time < 10000U));
    const bool recent_trigger = state->log_count > 0 && (now - latest_word_time < 3000U);
    if (recent_trigger) {
        const float pulse_a = 0.5f + 0.45f * fabsf(sinf((float)now * 0.007f));
        rect(state, ui.word.x - 2, ui.word.y - 2, ui.word.w + 4, ui.word.h + 4, 0.15f, 0.95f, 0.70f, pulse_a);
    }
    const float bg_shade = recent_trigger ? 0.15f : 0.06f;
    rect(state, ui.word.x, ui.word.y, ui.word.w, ui.word.h, bg_shade, bg_shade + 0.04f, bg_shade + 0.03f, 0.92f);
    const bool is_moving = ds_motion_is_moving(&state->motion_filter, sensor_now_ns());

    char sub_text[96] = {0};
    const char *primary_word = NULL;
    if (!magnetic_live) {
        primary_word = "NO SENSOR DATA";
        snprintf(sub_text, sizeof(sub_text), "MAGNETOMETER UNAVAILABLE");
    } else if (!state->scanning) {
        primary_word = "SCAN PAUSED";
        snprintf(sub_text, sizeof(sub_text), "TAP RESUME TO DETECT SENSOR ANOMALIES");
    } else if (is_moving) {
        primary_word = "MOTION DETECTED";
        snprintf(sub_text, sizeof(sub_text), "STABILIZING SENSOR BASELINE...");
    } else if (!state->baseline.ready) {
        primary_word = "CALIBRATING";
        snprintf(sub_text, sizeof(sub_text), "COLLECTING AMBIENT FIELD READINGS...");
    } else if (state->custom_selected && !ds_custom_words_count(&state->custom_words)) {
        primary_word = "CUSTOM BANK EMPTY";
        snprintf(sub_text, sizeof(sub_text), "ADD WORDS IN WORD BANK MODAL");
    } else if (word_active) {
        primary_word = state->word;
        snprintf(sub_text, sizeof(sub_text), "FIELD %.1f UT DEL %+.1f (%.0f,%.0f,%.0f)%s",
                 state->word_fields[latest_slot], state->word_deltas[latest_slot],
                 state->word_x[latest_slot], state->word_y[latest_slot], state->word_z[latest_slot],
                 state->word_whispers[latest_slot] ? " [WHISPER]" : "");
    } else {
        primary_word = "LISTENING...";
        snprintf(sub_text, sizeof(sub_text), "AWAITING SENSOR ANOMALIES");
    }

    const float word_r = is_moving ? 0.96f : 0.95f;
    const float word_g = is_moving ? 0.75f : 0.98f;
    const float word_b = is_moving ? 0.35f : 0.95f;
    const float card_word_scale = ds_fit_text_scale_styled(primary_word, ds_sp_to_scale(DS_FONT_SIZE_BODY_LG), ui.word.w - 20, DS_FONT_BOLD);
    text_styled(state, primary_word, ui.word.x + 10, ui.word.y + 6, card_word_scale, DS_FONT_BOLD, word_r, word_g, word_b, 1);
    text_styled(state, sub_text, ui.word.x + 10, ui.word.y + 24, ds_fit_text_scale_styled(sub_text, ds_sp_to_scale(DS_FONT_SIZE_BADGE), ui.word.w - 20, DS_FONT_STANDARD), DS_FONT_STANDARD, 0.65f, 0.85f, 0.78f, 0.9f);

    /* 4. RECORD SHUTTER BUTTON */
    const float cx = ui.record.x + ui.record.w / 2, cy = ui.record.y + ui.record.h / 2;
    filled_circle(state, cx, cy, 30, 0.05f, 0.05f, 0.05f, 0.85f);
    circle(state, cx, cy, 25, 0.98f, 0.98f, 0.95f, 1);
    if (recording) rect(state, cx - 11, cy - 11, 22, 22, 1, 0.05f, 0.08f, 1);
    else filled_circle(state, cx, cy, 18, 1, 0.05f, 0.08f, 1);

    /* Right pill: +TAG during recording, HUD/CLEAN burn-in toggle when idle */
    if (recording) {
        custom_button(state, ctrl.plus_tag, "+TAG", false);
    } else {
        custom_button(state, ui.hud_btn, state->hud_burn_in ? "HUD" : "CLEAN", state->hud_burn_in);
    }

    /* Left pills: Aspect framing preset (16:9 / 9:16 / 4:5) & active Filter label */
    static const char *const aspect_labels[] = {"16:9", "9:16", "4:5"};
    custom_button(state, ui.aspect_btn, aspect_labels[(unsigned)state->video_aspect % DS_ASPECT_COUNT], state->video_aspect != DS_ASPECT_16_9);

    static const char *const filter_names[] = {"NIGHT", "THERMAL", "EDGE", "MONO", "SEPIA", "RAW"};
    const char *fname = filter_names[(unsigned)state->video_filter % 6U];
    if (ui.aspect_btn.x - 22.0f >= 44.0f) {
        custom_button(state, ctrl.filter_pill, fname, false);
    }
    if (atomic_load(&state->audio_failed) || ds_video_failed(&state->video))
        text_styled(state, "CAPTURE ERROR", 16, ui.record.y + 44, ds_sp_to_scale(DS_FONT_SIZE_BADGE), DS_FONT_BOLD, 1, 0.65f, 0.3f, 1);
    rect(state, ui.navigation.x, ui.navigation.y, ui.navigation.w, ui.navigation.h, 0, 0, 0, 0.82f);
    const char *nav[] = {"CAMERA", "LOG", "EXPLORE", "GUIDE"};
    for (unsigned i = 0; i < 4; ++i) {
        const float nav_scale = ds_sp_to_scale(i == 0 ? DS_FONT_SIZE_BODY_MD : DS_FONT_SIZE_CAPTION);
        const DSFontType nav_font = i == 0 ? DS_FONT_BOLD : DS_FONT_STANDARD;
        const float total_w = ds_text_width(nav[i], nav_scale, nav_font);
        text_styled(state, nav[i], state->width * (i + 0.5f) / 4.0f - total_w / 2.0f,
                    ui.navigation.y + 20, nav_scale, nav_font, 0.8f, 0.95f, 0.86f, 1);
    }
    draw_filter_hud(state, now);
    if (state->custom_open) draw_custom_words(state);
}

static void update_video_hud(DeadSpeak *state, uint64_t now) {
    if (!ds_video_active(&state->video)) return;
    static const char *const filter_names[] = {"NIGHT", "THERMAL", "EDGE", "MONO", "SEPIA", "RAW"};
    const char *fname = filter_names[(unsigned)state->video_filter % 6U];
    const float delta = state->baseline.ready ? (state->smoothed_field - state->baseline.field) : 0.0f;
    const unsigned latest_slot = (state->log_next + DS_LOG_CAPACITY - 1U) % DS_LOG_CAPACITY;
    const uint64_t latest_word_time = state->log_count > 0 ? state->word_times[latest_slot] : 0U;
    const bool word_active = state->word[0] && (latest_word_time > 0 && (now - latest_word_time < 10000U));
    DSVideoHudState hud_snap = {
        .burn_in = state->hud_burn_in,
        .emf_ut = state->magnetic_field,
        .emf_delta = delta,
        .signal = state->signal,
        .anomaly = atomic_load(&state->sub_bass_anomaly),
    };
    snprintf(hud_snap.word, sizeof(hud_snap.word), "%s", word_active ? state->word : "");
    snprintf(hud_snap.filter_name, sizeof(hud_snap.filter_name), "%s", fname);
    ds_video_set_hud(&state->video, &hud_snap);
}

static void draw_frame(DeadSpeak *state) {
    update_video_hud(state, now_millis());
    DSColor mint = {0.73f, 0.96f, 0.86f};
    if (state->hud_hue > 0.001f) {
        mint = ds_interpolate_hud_accent(state->hud_hue);
    }
    const DSPalette pal = {
        .panel = {0.075f, 0.105f, 0.11f},
        .mint = mint,
        .muted = {0.42f, 0.53f, 0.52f},
        .amber = {0.96f, 0.70f, 0.36f},
    };
    const float w = (float)state->width;
    const float h = (float)state->height;
    const int64_t sensor_time = sensor_now_ns();
    const bool magnetic_live = ds_sensor_live(&state->readings[0], sensor_time);

    glClearColor(0.027f, 0.043f, 0.047f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (state->tab == DS_TAB_SCAN) {
        state->body_drawing = false;
        draw_camera(state);
        eglSwapBuffers(state->display, state->surface);
        return;
    }

    const float header_offset = state->ui.compact ? -8.0f : 0.0f;
    rect(state, 18, 22 + header_offset, 22, 22, pal.mint.r, pal.mint.g, pal.mint.b, 0.9f);
    rect(state, 23, 27 + header_offset, 12, 12, 0.027f, 0.043f, 0.047f, 1.0f);
    text_styled(state, "DEADSPEAK", 50, 24 + header_offset, ds_fit_text_scale_styled("DEADSPEAK", ds_sp_to_scale(DS_FONT_SIZE_TITLE_LG), w - 68.0f, DS_FONT_BOLD), DS_FONT_BOLD, pal.mint.r, pal.mint.g, pal.mint.b, 1.0f);
    const char *author = "EXALTED6165 LABS";
    const float author_scale = fit_text_scale(author, 1.3f, (w - 68.0f) * 0.46f);
    text(state, author, 50, 49 + header_offset, author_scale, pal.muted.r, pal.muted.g, pal.muted.b, 1.0f);
    const char *tagline = "RAW SENSOR FIELDWORK";
    const float tagline_x = 50.0f + (float)strlen(author) * 6.0f * author_scale + 12.0f;
    const float tagline_scale = fit_text_scale(tagline, 1.1f, w - 18.0f - tagline_x);
    text(state, tagline, w - 18.0f - (float)strlen(tagline) * 6.0f * tagline_scale, 49 + header_offset, tagline_scale, pal.muted.r, pal.muted.g, pal.muted.b, 1.0f);
    rect(state, 18, state->ui.compact ? 56 : 72, w - 36, 1, 0.19f, 0.27f, 0.26f, 0.9f);

    const char *screen_title = state->tab == DS_TAB_SCAN ? "FIELD SCAN" :
                               (state->tab == DS_TAB_LOG ? "EVIDENCE LOG" :
                               (state->tab == DS_TAB_EXPLORE ? "EXPLORE" : "FIELD GUIDE"));
    const float title_scale = ds_fit_text_scale_styled(screen_title, state->ui.compact ? ds_sp_to_scale(DS_FONT_SIZE_TITLE_MD) : ds_sp_to_scale(DS_FONT_SIZE_TITLE_LG), w - 44, DS_FONT_BOLD);
    text_styled(state, screen_title, 22, state->ui.title_y, title_scale, DS_FONT_BOLD, 0.95f, 0.98f, 0.94f, 1.0f);
    if (state->tab == DS_TAB_EXPLORE) {
        text_styled(state, "PUBLIC REPORTS - NOT VERIFIED CLAIMS", 22, state->ui.subtitle_y, ds_fit_text_scale_styled("PUBLIC REPORTS - NOT VERIFIED CLAIMS", ds_sp_to_scale(DS_FONT_SIZE_BADGE), w - 44, DS_FONT_STANDARD), DS_FONT_STANDARD, pal.amber.r, pal.amber.g, pal.amber.b, 1.0f);
    } else {
        char sensor_label[64];
        snprintf(sensor_label, sizeof(sensor_label), "MAGNETOMETER %s", ds_sensor_status_name(state->readings[0].status));
        const DSColor sc = magnetic_live ? pal.mint : pal.amber;
        text_styled(state, sensor_label, 22, state->ui.subtitle_y, ds_fit_text_scale_styled(sensor_label, state->ui.compact ? ds_sp_to_scale(DS_FONT_SIZE_CAPTION) : ds_sp_to_scale(DS_FONT_SIZE_BODY_MD), w - 44, DS_FONT_STANDARD), DS_FONT_STANDARD, sc.r, sc.g, sc.b, 1.0f);
    }

    const DSRect body = state->ui.body;
    state->scroll[state->tab] = fminf(state->scroll[state->tab], ds_scroll_limit(body_content_height(state), body));
    if (state->tab != DS_TAB_SCAN) rect(state, body.x, body.y, body.w, body.h, pal.panel.r, pal.panel.g, pal.panel.b, 1.0f);
    const int clip_top = (int)ceilf(body.y * state->ui_scale);
    const int clip_bottom = (int)floorf((body.y + body.h) * state->ui_scale);
    glEnable(GL_SCISSOR_TEST);
    glScissor((int)ceilf(body.x * state->ui_scale), state->surface_height - clip_bottom,
              (int)floorf(body.w * state->ui_scale), clip_bottom - clip_top);
    state->body_drawing = true;

    if (state->tab == DS_TAB_LOG) {
        draw_log(state, &pal);
        goto navigation;
    }
    if (state->tab == DS_TAB_EXPLORE) {
        draw_explore(state, &pal);
        goto navigation;
    }
    if (state->tab == DS_TAB_GUIDE) {
        draw_guide(state, &pal);
        goto navigation;
    }

    const DSRect field = state->ui.field, word = state->ui.word, capture = state->ui.capture, spectrum = state->ui.spectrum;
    rect(state, field.x, field.y, field.w, field.h, pal.panel.r, pal.panel.g, pal.panel.b, 1.0f);
    text(state, "MAGNETIC FIELD", field.x + 18, field.y + 21, fit_text_scale("MAGNETIC FIELD", 1.55f, field.w * 0.53f - 24), pal.muted.r, pal.muted.g, pal.muted.b, 1.0f);
    const float unit_scale = fit_text_scale("RAW MICROTESLA", 1.25f, field.w * 0.47f - 24);
    text(state, "RAW MICROTESLA", field.x + field.w - 18 - 14 * 6 * unit_scale, field.y + 21, unit_scale, pal.muted.r, pal.muted.g, pal.muted.b, 1.0f);

    const float cx = field.x + field.w * 0.5f;
    const float cy = field.y + field.h * 0.5f;
    const float radius = fminf((field.w - 72.0f) * 0.5f, (field.h - 110.0f) * 0.5f);
    circle(state, cx, cy, radius, 0.18f, 0.31f, 0.29f, 0.8f);
    circle(state, cx, cy, radius * 0.67f, 0.14f, 0.24f, 0.23f, 0.9f);
    line(state, cx - radius - 12, cy, cx + radius + 12, cy, 0.13f, 0.23f, 0.22f, 0.9f);
    line(state, cx, cy - radius - 12, cx, cy + radius + 12, 0.13f, 0.23f, 0.22f, 0.9f);
    if (magnetic_live) {
        const float angle = atan2f(state->readings[0].xyz[1], state->readings[0].xyz[0]);
        line(state, cx, cy, cx + cosf(angle) * radius, cy + sinf(angle) * radius, pal.mint.r, pal.mint.g, pal.mint.b, 0.9f);
    }
    circle(state, cx, cy, 5, pal.mint.r, pal.mint.g, pal.mint.b, 1.0f);

    char value[32];
    if (magnetic_live) snprintf(value, sizeof(value), "%05.1f", state->magnetic_field);
    else snprintf(value, sizeof(value), "%s", ds_sensor_status_name(state->readings[0].status));
    const float value_scale = ds_fit_text_scale_styled(value, ds_sp_to_scale(DS_FONT_SIZE_DISPLAY_XL), field.w - 84.0f, DS_FONT_MONO);
    const float value_width = ds_text_width(value, value_scale, DS_FONT_MONO);
    const float ut_scale = ds_sp_to_scale(DS_FONT_SIZE_BODY_LG);
    const float ut_width = ds_text_width("UT", ut_scale, DS_FONT_STANDARD);
    const float value_x = cx - (value_width + 12.0f + ut_width) * 0.5f;
    text_styled(state, value, value_x, cy - value_scale * 3.5f, value_scale, DS_FONT_MONO, 0.95f, 0.98f, 0.94f, 1.0f);
    if (magnetic_live) text_styled(state, "UT", value_x + value_width + 12.0f, cy - 5.6f, ut_scale, DS_FONT_STANDARD, pal.muted.r, pal.muted.g, pal.muted.b, 1.0f);

    const float signal_width = (field.w - 36) * (magnetic_live ? state->signal : 0.0f);
    rect(state, field.x + 18, field.y + field.h - 36, field.w - 36, 4, 0.14f, 0.20f, 0.20f, 1.0f);
    rect(state, field.x + 18, field.y + field.h - 36, signal_width, 4, pal.mint.r, pal.mint.g, pal.mint.b, 1.0f);
    char motion_label[64];
    snprintf(motion_label, sizeof(motion_label), "ACCEL %s  GYRO %s", ds_sensor_status_name(state->readings[1].status), ds_sensor_status_name(state->readings[2].status));
    text(state, motion_label, field.x + 18, field.y + field.h - 20,
         fit_text_scale(motion_label, 1.0f, field.w - 110), pal.muted.r, pal.muted.g, pal.muted.b, 1.0f);
    const DSColor scan_c = state->scanning ? pal.mint : pal.amber;
    text_styled(state, state->scanning ? "SCANNING" : "STANDBY", field.x + field.w - 76, field.y + field.h - 20, ds_sp_to_scale(DS_FONT_SIZE_CAPTION), DS_FONT_BOLD, scan_c.r, scan_c.g, scan_c.b, 1.0f);

    rect(state, word.x, word.y, word.w, word.h, pal.panel.r, pal.panel.g, pal.panel.b, 1.0f);
    text_styled(state, "SIGNAL WORD", word.x + 14, word.y + 20, ds_fit_text_scale_styled("SIGNAL WORD", ds_sp_to_scale(DS_FONT_SIZE_BODY_MD), word.w - 28, DS_FONT_STANDARD), DS_FONT_STANDARD, pal.muted.r, pal.muted.g, pal.muted.b, 1.0f);
    const char *word_label = magnetic_live ? (state->word[0] ? state->word : "QUIET") : "NO SENSOR DATA";
    const float word_width = word.w - 28.0f;
    const float word_scale = ds_fit_text_scale_styled(word_label, ds_sp_to_scale(DS_FONT_SIZE_DISPLAY_LG), word_width, DS_FONT_BOLD);
    text_styled(state, word_label, word.x + 14, word.y + word.h * 0.43f, word_scale, DS_FONT_BOLD, pal.mint.r, pal.mint.g, pal.mint.b, 1.0f);
    text_styled(state, "ENVIRONMENTAL TRIGGER", word.x + 14, word.y + word.h - 25, ds_fit_text_scale_styled("ENVIRONMENTAL TRIGGER", ds_sp_to_scale(DS_FONT_SIZE_BADGE), word_width, DS_FONT_STANDARD), DS_FONT_STANDARD, pal.muted.r, pal.muted.g, pal.muted.b, 1.0f);

    const float second_x = capture.x;
    rect(state, capture.x, capture.y, capture.w, capture.h, pal.panel.r, pal.panel.g, pal.panel.b, 1.0f);
    text_styled(state, "CAPTURE", second_x + 14, capture.y + 20, ds_sp_to_scale(DS_FONT_SIZE_BODY_MD), DS_FONT_STANDARD, pal.muted.r, pal.muted.g, pal.muted.b, 1.0f);
    const bool audio_active = atomic_load(&state->audio_active);
    const bool video_active = ds_video_active(&state->video);
    const bool capture_active = audio_active || video_active;
    const bool capture_failed = atomic_load(&state->audio_failed) || ds_video_failed(&state->video);
    const char *capture_label = capture_active ? "LIVE" : (state->capture_requested ? "ALLOW ACCESS" : (capture_failed ? "ERROR" : "READY"));
    const float capture_width = capture.w - 28.0f;
    const float capture_scale = ds_fit_text_scale_styled(capture_label, ds_sp_to_scale(DS_FONT_SIZE_DISPLAY_LG), capture_width, DS_FONT_BOLD);
    const DSColor cap_c = (capture_active || capture_failed) ? pal.amber : pal.mint;
    text_styled(state, capture_label, second_x + 14, capture.y + capture.h * 0.43f, capture_scale, DS_FONT_BOLD, cap_c.r, cap_c.g, cap_c.b, 1.0f);
    static const char *const filter_names_cap[] = {"NIGHT", "THERMAL", "EDGE", "MONO", "SEPIA", "RAW"};
    char filter_label[32];
    snprintf(filter_label, sizeof(filter_label), "FILTER: %s", filter_names_cap[state->video_filter]);
    text_styled(state, filter_label, second_x + 14, capture.y + capture.h - 25, ds_fit_text_scale_styled(filter_label, ds_sp_to_scale(DS_FONT_SIZE_BADGE), capture_width, DS_FONT_STANDARD), DS_FONT_STANDARD, pal.muted.r, pal.muted.g, pal.muted.b, 1.0f);
    const unsigned audio_level = atomic_load(&state->audio_level_milli);
    rect(state, spectrum.x, spectrum.y, spectrum.w, spectrum.h, pal.panel.r, pal.panel.g, pal.panel.b, 1.0f);
    text(state, "MIC", spectrum.x + 14, spectrum.y + 10, 1.0f, pal.muted.r, pal.muted.g, pal.muted.b, 1.0f);
    rect(state, spectrum.x + 52, spectrum.y + 15, spectrum.w - 72, 5, 0.14f, 0.20f, 0.20f, 1.0f);
    rect(state, spectrum.x + 52, spectrum.y + 15, (spectrum.w - 72) * ((float)audio_level / 1000.0f), 5, pal.mint.r, pal.mint.g, pal.mint.b, 1.0f);
    const char *band_names[3] = {"LOW", "MID", "HIGH"};
    const float band_width = (spectrum.w - 28) / 3.0f;
    for (unsigned band = 0; band < 3; ++band) {
        const float band_x = spectrum.x + 14.0f + (float)band * band_width;
        const unsigned band_level = atomic_load(&state->spectrum_milli[band]);
        text(state, band_names[band], band_x, spectrum.y + 37, 0.9f, pal.muted.r, pal.muted.g, pal.muted.b, 1.0f);
        rect(state, band_x, spectrum.y + 55, band_width - 14, 5, 0.14f, 0.20f, 0.20f, 1.0f);
        rect(state, band_x, spectrum.y + 55, (band_width - 14) * ((float)band_level / 1000.0f), 5, pal.mint.r, pal.mint.g, pal.mint.b, 1.0f);
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
             2, thumb, pal.muted.r, pal.muted.g, pal.muted.b, 0.8f);
    }
    const float nav_y = state->ui.nav_top;
    const float nav_label_y = nav_y + (state->ui.compact ? 15 : 25);
    rect(state, 18, nav_y, w - 36, 1, 0.19f, 0.27f, 0.26f, 0.9f);
    const float nav_active_scale = ds_sp_to_scale(DS_FONT_SIZE_BODY_MD);
    const float nav_inactive_scale = ds_sp_to_scale(DS_FONT_SIZE_CAPTION);
    static const char *const nav_labels[4] = {"SCAN", "LOG", "EXPLORE", "GUIDE"};
    static const float nav_x_fracs[4] = {0.08f, 0.32f, 0.51f, 0.81f};
    for (unsigned i = 0; i < 4; ++i) {
        const bool active = state->tab == (DSTab)i;
        const DSColor nc = active ? pal.mint : pal.muted;
        text_styled(state, nav_labels[i], w * nav_x_fracs[i], nav_label_y,
                    active ? nav_active_scale : nav_inactive_scale,
                    active ? DS_FONT_BOLD : DS_FONT_STANDARD, nc.r, nc.g, nc.b, 1.0f);
    }
    text_styled(state, "SKEPTICISM IS A FEATURE", 22, h - 39, ds_sp_to_scale(DS_FONT_SIZE_BADGE), DS_FONT_STANDARD, pal.muted.r, pal.muted.g, pal.muted.b, 1.0f);

    eglSwapBuffers(state->display, state->surface);
}

static void handle_cmd(struct android_app *app, int32_t cmd) {
    DeadSpeak *state = (DeadSpeak *)app->userData;
    if (cmd == APP_CMD_INIT_WINDOW && app->window != NULL) {
        setup_display(state);
        set_keep_screen_on(state, true);
    }
    else if (cmd == APP_CMD_TERM_WINDOW) {
        set_keep_screen_on(state, false);
        teardown_display(state);
    }
    else if (cmd == APP_CMD_CONFIG_CHANGED || cmd == APP_CMD_WINDOW_RESIZED || cmd == APP_CMD_CONTENT_RECT_CHANGED)
        update_display_geometry(state);
    else if (cmd == APP_CMD_RESUME) {
        set_keep_screen_on(state, true);
        state->sensors_resumed = true;
        start_sensors(state);
    }
    else if (cmd == APP_CMD_PAUSE) {
        set_keep_screen_on(state, false);
        state->sensors_resumed = false;
        stop_sensors(state, DS_SENSOR_PAUSED);
        state->capture_requested = false;
        if (state->voice_player) ds_voice_player_stop(state->voice_player);
        if (state->media_player) ds_media_player_set_paused(state->media_player, true);
        stop_audio_capture(state);
        ds_video_preview_stop(&state->video);
        state->preview_received_ms = 0;
    }
    else if (cmd == APP_CMD_GAINED_FOCUS) {
        set_keep_screen_on(state, true);
    }
    else if (cmd == APP_CMD_LOST_FOCUS) {
        set_keep_screen_on(state, false);
    }
}

static void update_custom_blob(DeadSpeak *state) {
    state->custom_blob_size = ds_custom_words_blob(&state->custom_words, state->custom_blob, sizeof(state->custom_blob));
    state->word[0] = 0;
}

static void save_custom_input(DeadSpeak *state) {
    if (ds_custom_words_add(&state->custom_words, state->custom_path, state->custom_input)) {
        update_custom_blob(state);
        state->custom_input[0] = 0;
        state->custom_editing = false;
        snprintf(state->custom_message, sizeof(state->custom_message), "WORD SAVED LOCALLY");
    } else {
        const char *error = ds_custom_words_error_message(&state->custom_words);
        snprintf(state->custom_message, sizeof(state->custom_message), "%s", error);
        for (size_t i = 0; state->custom_message[i]; ++i)
            state->custom_message[i] = (char)toupper((unsigned char)state->custom_message[i]);
    }
}

static void custom_key(DeadSpeak *state, int key) {
    const size_t length = strlen(state->custom_input);
    if (key == '\b') { if (length) state->custom_input[length - 1] = 0; }
    else if (key == '\n') save_custom_input(state);
    else if (key >= 32 && key <= 126 && length < sizeof(state->custom_input) - 1) {
        state->custom_input[length] = (char)toupper((unsigned char)key);
        state->custom_input[length + 1] = 0;
    }
}

static void custom_touch(DeadSpeak *state, float x, float y) {
    const DSCustomModalLayout l = custom_modal_layout(state);
    const DSRect modal = l.modal;
    if (ds_rect_contains(l.close, x, y)) {
        state->custom_open = false;
        return;
    }
    if (ds_rect_contains(l.bundled, x, y)) {
        state->custom_selected = false;
        state->word[0] = 0;
        return;
    }
    if (ds_rect_contains(l.custom, x, y)) {
        state->custom_selected = true;
        state->word[0] = 0;
        return;
    }
    for (unsigned i = 0; i < 4; ++i) {
        if (ds_rect_contains(l.voice[i], x, y)) {
            state->voice_config.mode = (DSVoiceMode)i;
            return;
        }
    }
    if (state->custom_editing) {
        const char *rows[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
        for (unsigned row = 0; row < 3; ++row) {
            const size_t rlen = strlen(rows[row]);
            for (unsigned column = 0; rows[row][column]; ++column) {
                if (ds_rect_contains(custom_modal_key_rect(modal, l.key_height, row, column, rlen), x, y)) {
                    custom_key(state, rows[row][column]);
                    return;
                }
            }
        }
        for (unsigned i = 0; i < 3; ++i) {
            if (ds_rect_contains(custom_modal_action_rect(modal, l.key_height, i), x, y)) {
                custom_key(state, i == 0 ? ' ' : i == 1 ? '\b' : '\n');
                return;
            }
        }
    } else {
        const size_t count = ds_custom_words_count(&state->custom_words);
        for (size_t row = 0; row < l.page_size && state->custom_page * l.page_size + row < count; ++row) {
            if (ds_rect_contains(custom_modal_row_del_rect(modal, row), x, y)) {
                if (ds_custom_words_remove(&state->custom_words, state->custom_path, state->custom_page * l.page_size + row)) {
                    update_custom_blob(state);
                    snprintf(state->custom_message, sizeof(state->custom_message), "WORD REMOVED");
                } else snprintf(state->custom_message, sizeof(state->custom_message), "COULD NOT SAVE CHANGE");
                return;
            }
        }
        if (ds_rect_contains(l.prev, x, y)) {
            if (state->custom_page) --state->custom_page;
        } else if (ds_rect_contains(l.add, x, y)) {
            state->custom_editing = true;
            state->custom_message[0] = 0;
        } else if (ds_rect_contains(l.test_vox, x, y)) {
            if (state->voice_player) {
                ds_voice_player_speak(state->voice_player, "DEADSPEAK ONLINE", &state->voice_config);
            }
        } else if (ds_rect_contains(l.next, x, y)) {
            if (count && state->custom_page < (count - 1) / l.page_size) ++state->custom_page;
        }
    }
}

static void toggle_capture(DeadSpeak *state) {
    if (atomic_load(&state->audio_active) || ds_video_active(&state->video) || state->capture_requested) {
        state->capture_requested = false;
        state->permission_requested = false;
        stop_audio_capture(state);
        ds_video_stop(&state->video);
    } else if (has_capture_permissions(state)) {
        state->camera_enabled = true;
        state->capture_started_ms = now_millis();
        start_audio_capture(state);
        ds_video_start(&state->video, state->evidence_dir, state->video_filter, state->video_aspect, state->display_rotation);
    } else {
        state->capture_requested = true;
        state->permission_deadline = now_millis() + 15000U;
        request_audio_permission(state);
    }
}

static int32_t handle_input(struct android_app *app, AInputEvent *event) {
    DeadSpeak *state = (DeadSpeak *)app->userData;
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY && state->custom_open &&
        AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_DOWN) {
        const int32_t key = AKeyEvent_getKeyCode(event);
        if (key == AKEYCODE_BACK) { state->custom_open = false; return 1; }
        if (state->custom_editing) {
            custom_key(state, key == AKEYCODE_DEL ? '\b' : key == AKEYCODE_ENTER ? '\n' :
                       ascii_from_key(key, AKeyEvent_getMetaState(event)));
            return 1;
        }
    }
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
        state->touch_active = state->tab == 0 || ds_rect_contains(state->ui.body, x, y);
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
        if (state->touch_dragged && state->tab != 0) {
            if (fabsf(y - state->touch_start_y) >= fabsf(x - state->touch_start_x) * 0.8f) {
                state->scroll[state->tab] = fminf(ds_scroll_limit(body_content_height(state), state->ui.body),
                    fmaxf(0.0f, state->scroll[state->tab] + state->touch_y - y));
            }
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
    if (state->touch_dragged) {
        const float dx = x - state->touch_start_x;
        const float dy = y - state->touch_start_y;
        const DSGesture gesture = ds_classify_gesture(dx, dy, 40.0f);
        if (state->tab == 0) {
            if (gesture == DS_GESTURE_SWIPE_LEFT) {
                state->video_filter = (DSVideoFilter)ds_cycle_filter_index((unsigned)state->video_filter, 1, 6);
                ds_video_set_filter(&state->video, state->video_filter);
                state->filter_hud_deadline = now_millis() + 1300U;
                return 1;
            } else if (gesture == DS_GESTURE_SWIPE_RIGHT) {
                state->video_filter = (DSVideoFilter)ds_cycle_filter_index((unsigned)state->video_filter, -1, 6);
                ds_video_set_filter(&state->video, state->video_filter);
                state->filter_hud_deadline = now_millis() + 1300U;
                return 1;
            } else if (gesture == DS_GESTURE_SWIPE_UP) {
                state->sensor_details = !state->sensor_details;
                return 1;
            }
        } else {
            if (gesture == DS_GESTURE_SWIPE_LEFT) {
                const DSTab previous_tab = state->tab;
                state->tab = (DSTab)(((unsigned)state->tab + 1U) % 4U);
                if (previous_tab == DS_TAB_EXPLORE && state->explore_search_active) {
                    state->explore_search_active = false;
                    ANativeActivity_hideSoftInput(state->app->activity, ANATIVEACTIVITY_HIDE_SOFT_INPUT_NOT_ALWAYS);
                }
                if (state->tab == DS_TAB_EXPLORE) start_explore(state);
                return 1;
            } else if (gesture == DS_GESTURE_SWIPE_RIGHT) {
                const DSTab previous_tab = state->tab;
                state->tab = (DSTab)(((unsigned)state->tab + 3U) % 4U);
                if (previous_tab == DS_TAB_EXPLORE && state->explore_search_active) {
                    state->explore_search_active = false;
                    ANativeActivity_hideSoftInput(state->app->activity, ANATIVEACTIVITY_HIDE_SOFT_INPUT_NOT_ALWAYS);
                }
                if (state->tab == DS_TAB_EXPLORE) start_explore(state);
                return 1;
            }
        }
        return 1;
    }
    if (state->custom_open) { custom_touch(state, x, y); return 1; }
    if (y >= (state->tab == DS_TAB_SCAN ? state->camera_ui.navigation.y : state->ui.nav_top)) {
        const DSTab previous_tab = state->tab;
        if (x < (float)state->width * 0.25f) state->tab = DS_TAB_SCAN;
        else if (x < (float)state->width * 0.50f) state->tab = DS_TAB_LOG;
        else if (x < (float)state->width * 0.75f) state->tab = DS_TAB_EXPLORE;
        else state->tab = DS_TAB_GUIDE;
        if (previous_tab == DS_TAB_EXPLORE && state->tab != DS_TAB_EXPLORE && state->explore_search_active) {
            state->explore_search_active = false;
            ANativeActivity_hideSoftInput(state->app->activity, ANATIVEACTIVITY_HIDE_SOFT_INPUT_NOT_ALWAYS);
        }
        if (state->tab == DS_TAB_EXPLORE) start_explore(state);
        return 1;
    }
    if (state->tab == DS_TAB_SCAN) {
        const DSCameraControlsLayout ctrl = camera_controls_layout(state);
        if (!state->minimal_hud) {
            if (ds_rect_contains(ctrl.spk, x, y)) {
                state->voice_config.enabled = !state->voice_config.enabled;
                return 1;
            }
            if (ds_rect_contains(ctrl.geiger, x, y)) {
                const bool geiger_on = ds_voice_player_get_geiger_enabled(state->voice_player);
                ds_voice_player_set_geiger_enabled(state->voice_player, !geiger_on);
                return 1;
            }
            if (ds_rect_contains(ctrl.style, x, y)) {
                state->theme_mode = (DSThemeMode)(((unsigned)state->theme_mode + 1U) % 3U);
                return 1;
            }
            if (ds_rect_contains(ctrl.tint, x, y)) {
                state->hud_tint_step = (uint8_t)((state->hud_tint_step + 1U) % 7U);
                state->hud_hue = (float)state->hud_tint_step / 6.0f;
                return 1;
            }
            for (unsigned i = 0; i < 5; ++i) if (ds_rect_contains(state->camera_ui.tools[i], x, y)) {
                if (i == 0) {
                    if (!has_camera_permission(state)) { state->camera_enabled = true; request_camera_permission(state); }
                    else {
                        const bool failed = ds_video_failed(&state->video);
                        if (!ds_video_active(&state->video)) {
                            ds_video_preview_stop(&state->video);
                            state->preview_received_ms = 0;
                            state->camera_enabled = failed || !state->camera_enabled;
                        }
                    }
                } else if (i == 1) { state->scanning = !state->scanning; state->word[0] = 0; }
                else if (i == 2) {
                    state->video_filter = (DSVideoFilter)ds_cycle_filter_index((unsigned)state->video_filter, 1, 6);
                    ds_video_set_filter(&state->video, state->video_filter);
                    state->filter_hud_deadline = now_millis() + 1300U;
                } else if (i == 3) { state->custom_open = true; state->custom_editing = false; state->custom_message[0] = 0; }
                else if (i == 4) state->sensor_details = !state->sensor_details;
                return 1;
            }
            if (state->sensor_details && ds_rect_contains(ctrl.cal, x, y)) {
                ds_baseline_reset(&state->baseline);
                state->word[0] = 0;
                return 1;
            }
            if (state->sensor_details && ds_rect_contains(ctrl.sens, x, y)) {
                state->baseline.sensitivity = (DSSensitivity)((state->baseline.sensitivity + 1) % 3);
                const char *sens_names[] = {"SENS: HI", "SENS: MED", "SENS: LO"};
                __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "SENSITIVITY set to %s (threshold=%.1f uT)",
                                    sens_names[state->baseline.sensitivity % 3],
                                    ds_baseline_threshold(state->baseline.sensitivity));
                return 1;
            }
        }
        const bool recording = ds_video_active(&state->video) || atomic_load(&state->audio_active);
        if (!recording && ds_rect_contains(state->camera_ui.aspect_btn, x, y)) {
            state->video_aspect = (DSVideoAspect)(((unsigned)state->video_aspect + 1U) % DS_ASPECT_COUNT);
            return 1;
        }
        if (ds_rect_contains(state->camera_ui.hud_btn, x, y)) {
            if (recording) {
                emit_companion_tag(state, DS_TAG_BOOKMARK);
                snprintf(state->export_status_message, sizeof(state->export_status_message), "BOOKMARK TAG #3 MARKED");
                state->export_status_expiry_ms = now_millis() + 2500U;
            } else {
                state->hud_burn_in = !state->hud_burn_in;
            }
            return 1;
        }
        if (state->camera_ui.aspect_btn.x - 22.0f >= 44.0f && ds_rect_contains(ctrl.filter_pill, x, y)) {
            state->video_filter = (DSVideoFilter)ds_cycle_filter_index((unsigned)state->video_filter, 1, 6);
            ds_video_set_filter(&state->video, state->video_filter);
            state->filter_hud_deadline = now_millis() + 1300U;
            return 1;
        }
        if (ds_rect_contains(state->camera_ui.record, x, y)) {
            toggle_capture(state);
            return 1;
        }
        /* Tap on camera background toggles Minimal HUD (anywhere above record row when minimized) */
        if ((state->minimal_hud && y < state->camera_ui.record.y - 12.0f) ||
            (!state->minimal_hud && y > 64.0f && y < state->camera_ui.record.y - 12.0f && x < state->width - 90.0f)) {
            state->minimal_hud = !state->minimal_hud;
            return 1;
        }
        return 1;
    }
    if (!ds_rect_contains(state->ui.body, x, y)) return 1;
    y += state->scroll[state->tab];

    if (state->tab == DS_TAB_LOG) {
        const DSLogLayout layout = log_layout(state);
        if (ds_rect_contains(layout.export_btn, x, y)) {
            export_session_files(state);
            refresh_media_files(state);
            return 1;
        }

        char curr_playing[96] = {0};
        float cur_prog = 0.0f;
        uint64_t cur_pos_ms = 0, dur_ms = 0;
        bool active_is_mp4 = false;
        const bool is_playing_any = active_media_status(state, curr_playing, sizeof(curr_playing),
                                                        &cur_prog, &cur_pos_ms, &dur_ms, &active_is_mp4);

        float cur_item_y = layout.media_y + 36.0f;
        for (unsigned i = 0; i < state->media_count; ++i) {
            const char *filename = state->media_files[i];
            const size_t len = strlen(filename);
            const bool is_wav = len > 4 && strcmp(filename + len - 4, ".wav") == 0;
            const bool is_mp4 = len > 4 && strcmp(filename + len - 4, ".mp4") == 0;
            const bool is_this_playing = is_playing_any && strcmp(curr_playing, filename) == 0;
            const DSMediaItemLayout item = media_item_layout(layout, cur_item_y, is_this_playing && active_is_mp4);

            if (is_this_playing) {
                if (ds_rect_contains(item.prev_tag, x, y)) {
                    jump_to_tag(state, false);
                    return 1;
                }
                if (ds_rect_contains(item.next_tag, x, y)) {
                    jump_to_tag(state, true);
                    return 1;
                }
                if (active_is_mp4 && ds_rect_contains(item.pause_btn, x, y)) {
                    ds_media_player_set_paused(state->media_player, !ds_media_player_is_paused(state->media_player));
                    return 1;
                }
                if (ds_rect_contains(item.stop, x, y)) {
                    active_media_stop(state, !active_is_mp4, active_is_mp4);
                    return 1;
                }
                if (ds_rect_contains(item.del_active, x, y)) {
                    delete_recording_and_companions(state, filename);
                    return 1;
                }
                if (ds_rect_contains(item.scrubber, x, y)) {
                    if (dur_ms > 0 && item.scrubber.w > 0) {
                        const float pct = fmaxf(0.0f, fminf(1.0f, (x - item.scrubber.x) / item.scrubber.w));
                        const uint64_t target_ms = (uint64_t)(pct * (float)dur_ms);
                        active_media_seek_ms(state, target_ms);
                    }
                    return 1;
                }
                cur_item_y += active_is_mp4 ? 264.0f : 82.0f;
            } else {
                if (ds_rect_contains(item.del_idle, x, y)) {
                    delete_recording_and_companions(state, filename);
                    return 1;
                }
                if (ds_rect_contains(item.item_row, x, y)) {
                    char full_path[640];
                    snprintf(full_path, sizeof(full_path), "%s/%s", state->evidence_dir, filename);
                    if (access(full_path, F_OK) != 0 && state->app && state->app->activity &&
                        state->app->activity->externalDataPath) {
                        snprintf(full_path, sizeof(full_path), "%s/Movies/%s", state->app->activity->externalDataPath, filename);
                    }
                    if (is_wav) {
                        active_media_stop(state, false, true);
                        ds_voice_player_play_wav(state->voice_player, full_path);
                        load_recording_tags(state, filename);
                        return 1;
                    }
                    if (is_mp4) {
                        active_media_stop(state, true, false);
                        state->mp4_width = state->mp4_height = 0;
                        ds_media_player_open(state->media_player, full_path);
                        load_recording_tags(state, filename);
                        return 1;
                    }
                }
                cur_item_y += 36.0f;
            }
        }
    }

    if (state->tab == DS_TAB_GUIDE) {
        const DSGuideLayout gl = guide_layout(state);
        static const char *const vnames[4] = {"CLASSIC", "WHISPER", "DYNAMIC", "SPIRIT BOX"};
        for (unsigned i = 0; i < 4; ++i) {
            if (ds_rect_contains(gl.voice_btns[i], x, y)) {
                state->voice_config.mode = (DSVoiceMode)i;
                if (state->voice_player) {
                    ds_voice_player_speak(state->voice_player, "DEADSPEAK SHADOW", &state->voice_config);
                }
                state->guide_voice_last_ms = now_millis();
                snprintf(state->guide_voice_label, sizeof(state->guide_voice_label),
                         "VOICE TEST: %s (ACTIVE)", vnames[i]);
                return 1;
            }
        }
    }

    const DSRect rows = state->ui.explore_rows;
    const DSExploreControlsLayout exp_ctrl = explore_controls_layout(state);
    if (state->tab == DS_TAB_EXPLORE && ds_rect_contains(exp_ctrl.refresh, x, y)) {
        ds_explore_refresh(&state->explore, state->app->activity, state->explore_cache_path);
    } else if (state->tab == DS_TAB_EXPLORE && ds_rect_contains(exp_ctrl.search, x, y)) {
        state->explore_search_active = true;
        ANativeActivity_showSoftInput(state->app->activity, ANATIVEACTIVITY_SHOW_SOFT_INPUT_IMPLICIT);
    } else if (state->tab == DS_TAB_EXPLORE && ds_rect_contains(exp_ctrl.pagination, x, y)) {
        DSExplorePlace places[DS_EXPLORE_PAGE_SIZE];
        size_t match_count = 0;
        (void)ds_explore_page(&state->explore, state->explore_query, 0, state->ui.explore_page_size, places, &match_count);
        const size_t last_page = match_count == 0 ? 0 : (match_count - 1U) / state->ui.explore_page_size;
        if (x > rows.x + rows.w * 0.5f && state->explore_page < last_page) ++state->explore_page;
        else if (x <= rows.x + rows.w * 0.5f && state->explore_page > 0) --state->explore_page;
    } else if (state->tab == DS_TAB_EXPLORE && ds_rect_contains(rows, x, y)) {
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
    } else if (state->tab == DS_TAB_SCAN && ds_rect_contains(state->ui.capture, x, y) &&
               y >= state->ui.capture.y + state->ui.capture.h - 44.0f) {
        state->video_filter = (DSVideoFilter)ds_cycle_filter_index((unsigned)state->video_filter, 1, 6);
        state->filter_hud_deadline = now_millis() + 1300U;
    } else if (state->tab == DS_TAB_SCAN && ds_rect_contains(state->ui.capture, x, y)) {
        toggle_capture(state);
    } else if (state->tab == DS_TAB_SCAN && ds_rect_contains(state->ui.field, x, y)) {
        state->scanning = !state->scanning;
    }
    return 1;
}

static void poll_sensor(DeadSpeak *state) {
    ASensorEvent event;
    while (state->sensor_queue && ASensorEventQueue_getEvents(state->sensor_queue, &event, 1) > 0) {
        unsigned index;
        for (index = 0; index < DS_SENSOR_COUNT; ++index)
            if (event.type == sensor_types[index] || event.type == sensor_fallback_types[index]) break;
        if (index == DS_SENSOR_COUNT || !state->sensors_resumed) continue;
        DSSensorReading *reading = &state->readings[index];
        const int64_t received = sensor_now_ns();
        if (index == 3) {
            DSSampleResult sample_prox = ds_sensor_accept(reading, event.timestamp, received, event.distance, 0.0f, 0.0f, 3);
            if (sample_prox != DS_SAMPLE_REJECTED && state->sensors[3]) {
                state->proximity_distance = event.distance;
                const bool was_active = state->proximity_active;
                /* Standard Android hardware proximity sensors report ~0.0 cm for NEAR, and ~5.0 cm for FAR */
                state->proximity_active = (event.distance < 4.0f);
                if (state->proximity_active && !was_active) {
                    __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                        "PROXIMITY TRIGGERED: distance=%.1f cm", event.distance);
                    if (state->scanning) ds_voice_player_set_geiger_rate(state->voice_player, 25.0f);
                    emit_companion_tag(state, 2);
                } else if (!state->proximity_active && was_active && state->scanning) {
                    ds_voice_player_set_geiger_rate(state->voice_player, 0.0f);
                }
            }
            continue;
        }
        DSSampleResult sample = ds_sensor_accept(reading, event.timestamp, received,
            event.vector.x, event.vector.y, event.vector.z, event.vector.status);
        if (sample == DS_SAMPLE_REJECTED) continue;
        if (index == 1 || index == 2) {
            if (state->readings[1].count > 0 && state->readings[2].count > 0) {
                ds_motion_filter_update(&state->motion_filter,
                                        state->readings[1].xyz,
                                        state->readings[2].xyz,
                                        reading->timestamp_ns);
            }
            continue;
        }
        if (index != 0) continue;
        state->magnetic_field = reading->magnitude;
        state->smoothed_field = sample == DS_SAMPLE_FIRST ? reading->magnitude :
            ds_engine_smooth(state->smoothed_field, reading->magnitude, 0.15f);
        const bool moving = ds_motion_is_moving(&state->motion_filter, received);
        ds_baseline_sample_3d(&state->baseline, reading->xyz[0], reading->xyz[1], reading->xyz[2], moving, reading->timestamp_ns);
        state->signal = state->baseline.ready ? ds_engine_signal(state->smoothed_field, state->baseline.field) : 0;
        if (state->sample_count == UINT32_MAX) state->sample_count = 0;
        ++state->sample_count;

        /* Live Geiger audio tick rate calculation */
        if (state->scanning && state->baseline.ready) {
            const float delta_val = fabsf(state->smoothed_field - state->baseline.field);
            if (delta_val > 1.2f) {
                const float clicks = fminf(75.0f, (delta_val - 1.2f) * 7.5f);
                ds_voice_player_set_geiger_rate(state->voice_player, clicks);
            } else if (state->proximity_active) {
                ds_voice_player_set_geiger_rate(state->voice_player, 25.0f);
            } else {
                ds_voice_player_set_geiger_rate(state->voice_player, 0.0f);
            }
        } else if (state->scanning && state->proximity_active) {
            ds_voice_player_set_geiger_rate(state->voice_player, 25.0f);
        } else {
            ds_voice_player_set_geiger_rate(state->voice_player, 0.0f);
        }

        if (!state->last_magnetic_log_ns || received - state->last_magnetic_log_ns >= INT64_C(5000000000)) {
            state->last_magnetic_log_ns = received;
            __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG,
                "MAGNETOMETER LIVE samples=%u field=%.3f uT xyz=%.3f,%.3f,%.3f accuracy=%d",
                state->sample_count, state->magnetic_field, reading->xyz[0], reading->xyz[1], reading->xyz[2], reading->accuracy);
        }
        if (state->scanning && !moving &&
            ds_baseline_word_trigger_3d(&state->baseline, reading->xyz[0], reading->xyz[1], reading->xyz[2], moving, reading->timestamp_ns)) {
            const uint8_t *bank = state->custom_selected ? state->custom_blob : state->dictionary;
            const size_t bank_size = state->custom_selected ? state->custom_blob_size : state->dictionary_size;
            ds_engine_vector_word(state->baseline.triggered_x, state->baseline.triggered_y, state->baseline.triggered_z,
                                  state->baseline.base_x, state->baseline.base_y, state->baseline.base_z,
                                  bank, bank_size, state->word, sizeof(state->word));
            if (state->word[0]) {
                append_word_log(state, state->word);
                __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                    "WORD TRIGGERED: %s (field=%.2f uT delta=%.2f uT vector=%.1f,%.1f,%.1f mode=%s)",
                    state->word, state->smoothed_field, state->baseline.triggered_delta,
                    state->baseline.triggered_x, state->baseline.triggered_y, state->baseline.triggered_z,
                    voice_mode_name(state->voice_config.mode));
                if (state->voice_player && state->voice_config.enabled) {
                    DSVoiceConfig active_voice = state->voice_config;
                    const float abs_delta = fabsf(state->baseline.triggered_delta);
                    if (active_voice.mode == DS_VOICE_MODE_DYNAMIC) {
                        /* Dynamic Voice Mode: modulate pitch and speed dynamically by anomaly delta */
                        const float delta_factor = fminf(abs_delta / 40.0f, 1.5f);
                        active_voice.pitch_hz = 115.0f * (1.0f + delta_factor * 0.45f);
                        active_voice.speed = 1.0f + delta_factor * 0.35f;
                    }
                    ds_voice_player_speak(state->voice_player, state->word, &active_voice);
                }
            }
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
        if (!state->baseline.ready) ds_baseline_reset(&state->baseline);
    }
    if (now_millis() >= state->next_sensor_access_check) {
        state->next_sensor_access_check = now_millis() + 1000U;
        /* Re-attempt an explicitly failed registration after an OS grant;
           this never substitutes readings or a different sensor. */
        start_sensors(state);
    }
}

static bool ds_is_hardware_proximity(const ASensor *sensor) {
    if (!sensor) return false;
    const char *name = ASensor_getName(sensor);
    const char *vendor = ASensor_getVendor(sensor);
    if (!name || !vendor) return false;
    char n_low[128] = {0}, v_low[128] = {0};
    for (size_t i = 0; i < sizeof(n_low) - 1 && name[i]; ++i) n_low[i] = (char)tolower((unsigned char)name[i]);
    for (size_t i = 0; i < sizeof(v_low) - 1 && vendor[i]; ++i) v_low[i] = (char)tolower((unsigned char)vendor[i]);
    if (strstr(n_low, "virtual") || strstr(n_low, "palm") || strstr(n_low, "gesture") ||
        strstr(n_low, "software") || strstr(v_low, "elliptic") || strstr(v_low, "software")) {
        return false;
    }
    return true;
}

static void setup_sensors(DeadSpeak *state) {
    state->sensor_manager = ASensorManager_getInstanceForPackage("org.deadspeak");
    if (!state->sensor_manager) return;
    for (unsigned i = 0; i < DS_SENSOR_COUNT; ++i) {
        state->sensors[i] = ASensorManager_getDefaultSensor(state->sensor_manager, sensor_types[i]);
        if (!state->sensors[i]) {
            state->sensors[i] = ASensorManager_getDefaultSensor(state->sensor_manager, sensor_fallback_types[i]);
            if (state->sensors[i]) {
                __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                    "Fallback to uncalibrated sensor %d for %s", sensor_fallback_types[i], sensor_names[i]);
            }
        }
        if (i == 3 && state->sensors[3] && !ds_is_hardware_proximity(state->sensors[3])) {
            __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                "Ignoring virtual/emulated proximity sensor: %s (%s)",
                ASensor_getName(state->sensors[3]), ASensor_getVendor(state->sensors[3]));
            state->sensors[3] = NULL;
        }
        ds_sensor_configure(&state->readings[i], state->sensors[i] != NULL, true, -1, -1, sensor_now_ns());
    }
    state->sensor_queue = ASensorManager_createEventQueue(state->sensor_manager, ALooper_forThread(), LOOPER_ID_SENSOR, NULL, NULL);
}

static void maintain_camera(DeadSpeak *state) {
    if (!state->sensors_resumed || state->display == EGL_NO_DISPLAY || state->surface == EGL_NO_SURFACE) return;
    const uint64_t now = now_millis();
    if (now < state->next_camera_check) return;
    state->next_camera_check = now + 250;
    const bool permitted = has_camera_permission(state);
    if (state->camera_permission_pending && (permitted || now >= state->permission_deadline))
        state->camera_permission_pending = false;
    if (!permitted || !state->camera_enabled || (state->tab != 0 && !ds_video_active(&state->video))) {
        if (state->video.thread_created) ds_video_preview_stop(&state->video);
        state->preview_received_ms = 0;
        return;
    }
    if (!state->video.thread_created ||
        (!ds_video_active(&state->video) && state->video.display_rotation != state->display_rotation))
        ds_video_preview_start(&state->video, state->video_filter, state->display_rotation);
}

void android_main(struct android_app *app) {
    DeadSpeak *state = calloc(1, sizeof(DeadSpeak));
    if (!state) return;
    state->app = app;
    set_keep_screen_on(state, true);
    state->display = EGL_NO_DISPLAY;
    state->surface = EGL_NO_SURFACE;
    state->context = EGL_NO_CONTEXT;
    state->ui_scale = 1.0f;
    state->scanning = true;
    state->camera_enabled = true;
    state->preview_rgba = malloc(DS_PREVIEW_CAPACITY);
    state->mp4_rgba = malloc(DS_PREVIEW_CAPACITY);
    state->video_filter = DS_VIDEO_FILTER_RAW;
    state->video_aspect = DS_ASPECT_16_9;
    state->hud_burn_in = true;
    state->sub_bass_baseline = 15.0f;
    state->word[0] = 0;
    state->voice_config = (DSVoiceConfig){
        .enabled = true,
        .mode = DS_VOICE_MODE_CLASSIC,
        .pitch_hz = 115.0f,
        .speed = 1.0f
    };
    state->voice_player = ds_voice_player_create();
    state->media_player = ds_media_player_create();
    ds_motion_filter_init(&state->motion_filter);
    ds_explore_init(&state->explore);
    load_dictionary(state);
    snprintf(state->evidence_dir, sizeof(state->evidence_dir), "%s/evidence", app->activity->internalDataPath);
    snprintf(state->word_log_path, sizeof(state->word_log_path), "%s/word-log.csv", state->evidence_dir);
    snprintf(state->explore_cache_path, sizeof(state->explore_cache_path), "%s/wikipedia-places-v2.tsv", state->evidence_dir);
    mkdir(state->evidence_dir, 0700);
    snprintf(state->custom_path, sizeof(state->custom_path), "%s/custom-words.dswb", state->evidence_dir);
    ds_custom_words_init(&state->custom_words);
    if (!ds_custom_words_load(&state->custom_words, state->custom_path))
        LOGE("Custom word bank could not be loaded: %s", ds_custom_words_error_message(&state->custom_words));
    update_custom_blob(state);
    load_word_log(state);
    refresh_media_files(state);
    app->userData = state;
    app->onAppCmd = handle_cmd;
    app->onInputEvent = handle_input;
    setup_sensors(state);

    while (true) {
        static uint64_t next_permission_check = 0;
        const uint64_t loop_time = now_millis();
        maintain_sensors(state);
        maintain_camera(state);
        if (loop_time >= state->next_media_refresh) {
            state->next_media_refresh = loop_time + 1000U;
            refresh_media_files(state);
        }
        if (state->capture_requested && loop_time >= next_permission_check) {
            next_permission_check = loop_time + 250U;
            if (has_capture_permissions(state)) {
                state->capture_requested = false;
                state->permission_requested = false;
                state->camera_enabled = true;
                state->capture_started_ms = now_millis();
                start_audio_capture(state);
                ds_video_start(&state->video, state->evidence_dir, state->video_filter, state->video_aspect, state->display_rotation);
            } else if (loop_time >= state->permission_deadline) {
                state->capture_requested = false;
                state->permission_requested = false;
                atomic_store(&state->audio_failed, true);
            }
        }
        if (state->audio_thread_created && !atomic_load(&state->audio_active)) {
            pthread_join(state->audio_thread, NULL);
            state->audio_thread_created = false;
        }
        int ident;
        int events;
        struct android_poll_source *source;
        while ((ident = ALooper_pollOnce(state->display == EGL_NO_DISPLAY ? -1 : 0, NULL, &events, (void **)&source)) >= 0) {
            if (source) source->process(app, source);
            if (ident == LOOPER_ID_SENSOR) poll_sensor(state);
            if (app->destroyRequested) {
                if (state->voice_player) {
                    ds_voice_player_destroy(state->voice_player);
                    state->voice_player = NULL;
                }
                if (state->media_player) {
                    ds_media_player_destroy(state->media_player);
                    state->media_player = NULL;
                }
                stop_audio_capture(state);
                ds_video_preview_stop(&state->video);
                stop_sensors(state, DS_SENSOR_PAUSED);
                if (state->sensor_manager && state->sensor_queue)
                    ASensorManager_destroyEventQueue(state->sensor_manager, state->sensor_queue);
                free(state->dictionary);
                free(state->preview_rgba);
                free(state->mp4_rgba);
                ds_explore_destroy(&state->explore);
                teardown_display(state);
                app->userData = NULL;
                free(state);
                return;
            }
        }
        if (state->display != EGL_NO_DISPLAY && state->surface != EGL_NO_SURFACE && state->width > 0 && state->height > 0) draw_frame(state);
    }
}
