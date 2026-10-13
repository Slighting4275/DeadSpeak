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

typedef struct {
    uint64_t timestamp_ms;
    uint8_t tag_id;
} DSTagEntry;

typedef struct {
    float r, g, b;
} DSColorStop;

/* 7-stop ColorSlider interpolator */
static const DSColorStop ds_color_stops[7] = {
    {1.0f, 1.0f, 1.0f}, /* Stop 0: White */
    {1.0f, 0.0f, 0.0f}, /* Stop 1: Red */
    {1.0f, 1.0f, 0.0f}, /* Stop 2: Yellow */
    {0.0f, 1.0f, 0.0f}, /* Stop 3: Green */
    {0.0f, 1.0f, 1.0f}, /* Stop 4: Cyan */
    {0.0f, 0.0f, 1.0f}, /* Stop 5: Blue */
    {1.0f, 0.0f, 1.0f}  /* Stop 6: Magenta */
};

static void ds_interpolate_hud_accent(float progress, float *out_r, float *out_g, float *out_b) {
    if (progress <= 0.0f) {
        *out_r = ds_color_stops[0].r; *out_g = ds_color_stops[0].g; *out_b = ds_color_stops[0].b;
        return;
    }
    if (progress >= 1.0f) {
        *out_r = ds_color_stops[6].r; *out_g = ds_color_stops[6].g; *out_b = ds_color_stops[6].b;
        return;
    }
    const float scaled = progress * 6.0f;
    const int idx = (int)scaled;
    const float f = scaled - (float)idx;
    const int next_idx = idx < 6 ? idx + 1 : 6;
    *out_r = ds_color_stops[idx].r + (ds_color_stops[next_idx].r - ds_color_stops[idx].r) * f;
    *out_g = ds_color_stops[idx].g + (ds_color_stops[next_idx].g - ds_color_stops[idx].g) * f;
    *out_b = ds_color_stops[idx].b + (ds_color_stops[next_idx].b - ds_color_stops[idx].b) * f;
}

/* 80-300 Hz sub-bass EVP FFT power evaluator */
static float calculate_sub_bass_energy(const int16_t *samples, size_t count, uint32_t sample_rate) {
    if (!samples || count == 0 || sample_rate == 0) return 0.0f;
    static const float sub_bass_freqs[] = {80.0f, 120.0f, 160.0f, 200.0f, 250.0f, 300.0f};
    const size_t num_freqs = sizeof(sub_bass_freqs) / sizeof(sub_bass_freqs[0]);
    float total_amp = 0.0f;
    for (size_t f = 0; f < num_freqs; ++f) {
        const float target = sub_bass_freqs[f];
        if (target >= (float)sample_rate * 0.5f) continue;
        const float omega = 2.0f * 3.141592653589793f * target / (float)sample_rate;
        const float coeff = 2.0f * cosf(omega);
        float s_prev = 0.0f, s_prev2 = 0.0f;
        for (size_t i = 0; i < count; ++i) {
            const float sample = (float)samples[i] / 32768.0f;
            const float s = sample + coeff * s_prev - s_prev2;
            s_prev2 = s_prev;
            s_prev = s;
        }
        const float power = s_prev * s_prev + s_prev2 * s_prev2 - coeff * s_prev * s_prev2;
        if (power > 0.0f) {
            total_amp += sqrtf(power) / (float)count;
        }
    }
    return (total_amp / (float)num_freqs) * 1000.0f;
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
    DSVideoCapture video;
    DSVideoFilter video_filter;
    uint64_t filter_hud_deadline;
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
    float sub_bass_level;
    int theme_mode;
    float hud_hue;
    DSTagEntry loaded_tags[DS_MAX_LOADED_TAGS];
    size_t loaded_tags_count;
    char loaded_tags_filename[96];
    char export_status_message[128];
    uint64_t export_status_expiry_ms;
    char words[DS_LOG_CAPACITY][32];
    unsigned word_samples[DS_LOG_CAPACITY];
    float word_fields[DS_LOG_CAPACITY];
    float word_deltas[DS_LOG_CAPACITY];
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
    int tab;
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
static void emit_companion_tag(DeadSpeak *state, uint8_t tag_id) {
    if (!state || !state->active_tags_path[0]) return;
    const uint64_t now = now_millis();
    /* Debounce anomaly events (tag_id == 2) by 1000ms */
    if (tag_id == 2) {
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

static void append_word_log(DeadSpeak *state, const char *word) {
    if (!word || !word[0]) return;
    const unsigned slot = state->log_next;
    snprintf(state->words[slot], sizeof(state->words[slot]), "%s", word);
    state->word_samples[slot] = state->sample_count;
    state->word_fields[slot] = state->smoothed_field;
    state->word_deltas[slot] = state->baseline.triggered_delta;
    state->word_times[slot] = now_millis();
    state->word_whispers[slot] = state->baseline.whisper_modulation;
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
            strncpy(lines[l], lines[l + 1], sizeof(lines[l]) - 1);
            lines[l][sizeof(lines[l]) - 1] = '\0';
        }
        strncpy(lines[99], new_entry, sizeof(lines[99]) - 1);
        lines[99][sizeof(lines[99]) - 1] = '\0';
        line_count = 100;
    } else {
        strncpy(lines[line_count], new_entry, sizeof(lines[line_count]) - 1);
        lines[line_count][sizeof(lines[line_count]) - 1] = '\0';
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
    const bool wh_new = (access(wh_path, F_OK) != 0);
    FILE *wh_file = fopen(wh_path, "a");
    if (wh_file) {
        if (wh_new) {
            fprintf(wh_file, "DeadSpeak session word history export\n\nDate|Time|Generated word(s)|Translated\n");
        }
        fprintf(wh_file, "%s|%s|%s|%s\n", date_buf, time_buf, word, word);
        fclose(wh_file);
    }

    /* 4. DeadSpeak Session Export */
    char ds_path[640];
    snprintf(ds_path, sizeof(ds_path), "%s/DeadSpeakSessionExport.txt", state->evidence_dir);
    const bool ds_new = (access(ds_path, F_OK) != 0);
    FILE *ds_file = fopen(ds_path, "a");
    if (ds_file) {
        if (ds_new) {
            fprintf(ds_file, "DeadSpeak Anomaly Detection Session Export\n\nDate|Time|Word|Field_uT|Delta_uT|Vector|Mode\n");
        }
        fprintf(ds_file, "%s|%s|%s|%.1f|%+.1f|X:%.1f,Y:%.1f,Z:%.1f|%s\n",
                date_buf, time_buf, word, state->smoothed_field, state->baseline.triggered_delta,
                state->baseline.triggered_x, state->baseline.triggered_y, state->baseline.triggered_z,
                state->baseline.whisper_modulation ? "WHISPER" : "CLASSIC");
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
    emit_companion_tag(state, 1);
}

static void copy_file_data(const char *src, const char *dst) {
    if (!src || !dst || strcmp(src, dst) == 0) return;
    FILE *in = fopen(src, "rb");
    if (!in) return;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return; }
    char buf[1024];
    size_t n;
    while (!feof(in) && (n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
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

    char ds_src[640], ds_dst[640];
    snprintf(ds_src, sizeof(ds_src), "%s/DeadSpeakSessionExport.txt", state->evidence_dir);
    snprintf(ds_dst, sizeof(ds_dst), "%s/DeadSpeakSessionExport.txt", target_dir);

    if (access(wh_src, F_OK) != 0 && state->log_count > 0) {
        FILE *src_f = fopen(wh_src, "w");
        if (src_f) {
            fprintf(src_f, "DeadSpeak session word history export\n\nDate|Time|Generated word(s)|Translated\n");
            time_t now_sec = time(NULL);
            struct tm lt;
            localtime_r(&now_sec, &lt);
            char date_buf[32], time_buf[32];
            strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &lt);
            strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &lt);
            for (unsigned i = 0; i < state->log_count; ++i) {
                const unsigned slot = (state->log_next + DS_LOG_CAPACITY - state->log_count + i) % DS_LOG_CAPACITY;
                if (state->words[slot][0]) {
                    fprintf(src_f, "%s|%s|%s|%s\n", date_buf, time_buf, state->words[slot], state->words[slot]);
                }
            }
            fclose(src_f);
        }
    }
    if (access(ds_src, F_OK) != 0 && state->log_count > 0) {
        FILE *ds_f = fopen(ds_src, "w");
        if (ds_f) {
            fprintf(ds_f, "DeadSpeak Anomaly Detection Session Export\n\nDate|Time|Word|Field_uT|Delta_uT|Mode\n");
            time_t now_sec = time(NULL);
            struct tm lt;
            localtime_r(&now_sec, &lt);
            char date_buf[32], time_buf[32];
            strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &lt);
            strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &lt);
            for (unsigned i = 0; i < state->log_count; ++i) {
                const unsigned slot = (state->log_next + DS_LOG_CAPACITY - state->log_count + i) % DS_LOG_CAPACITY;
                if (state->words[slot][0]) {
                    fprintf(ds_f, "%s|%s|%s|%.1f|%+.1f|%s\n",
                        date_buf, time_buf, state->words[slot],
                        state->word_fields[slot], state->word_deltas[slot],
                        state->word_whispers[slot] ? "WHISPER" : "CLASSIC");
                }
            }
            fclose(ds_f);
        }
    }

    if (strcmp(wh_src, wh_dst) != 0) copy_file_data(wh_src, wh_dst);
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

static void delete_recording_and_companions(DeadSpeak *state, const char *filename) {
    if (!state || !filename || !filename[0]) return;
    char curr_playing[96] = {0};
    float prog = 0.0f;
    if (ds_voice_player_is_playing_wav(state->voice_player, curr_playing, sizeof(curr_playing), &prog) &&
        strcmp(curr_playing, filename) == 0) {
        ds_voice_player_stop_wav(state->voice_player);
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

        char c_path[640];
        snprintf(c_path, sizeof(c_path), "%s/%s.txt", state->evidence_dir, stem);
        unlink(c_path);
        snprintf(c_path, sizeof(c_path), "%s/%s_tags.txt", state->evidence_dir, stem);
        unlink(c_path);
        snprintf(c_path, sizeof(c_path), "%s/%s_waveform.txt", state->evidence_dir, stem);
        unlink(c_path);
        snprintf(c_path, sizeof(c_path), "%s/%s.srt", state->evidence_dir, stem);
        unlink(c_path);

        if (state->app && state->app->activity && state->app->activity->externalDataPath) {
            snprintf(c_path, sizeof(c_path), "%s/Movies/%s.txt", state->app->activity->externalDataPath, stem);
            unlink(c_path);
            snprintf(c_path, sizeof(c_path), "%s/Movies/%s_tags.txt", state->app->activity->externalDataPath, stem);
            unlink(c_path);
            snprintf(c_path, sizeof(c_path), "%s/Movies/%s_waveform.txt", state->app->activity->externalDataPath, stem);
            unlink(c_path);
            snprintf(c_path, sizeof(c_path), "%s/Movies/%s.srt", state->app->activity->externalDataPath, stem);
            unlink(c_path);
        }
    }

    if (strcmp(state->loaded_tags_filename, filename) == 0) {
        state->loaded_tags_count = 0;
        state->loaded_tags_filename[0] = '\0';
    }
    refresh_media_files(state);
}

static void load_recording_tags(DeadSpeak *state, const char *filename) {
    if (!state || !filename || !filename[0]) return;
    if (strcmp(state->loaded_tags_filename, filename) == 0) return;
    state->loaded_tags_count = 0;
    snprintf(state->loaded_tags_filename, sizeof(state->loaded_tags_filename), "%s", filename);

    const size_t len = strlen(filename);
    char tags_path[640];
    if (len > 4 && (strcmp(filename + len - 4, ".wav") == 0 || strcmp(filename + len - 4, ".mp4") == 0)) {
        snprintf(tags_path, sizeof(tags_path), "%s/%.*s_tags.txt", state->evidence_dir, (int)(len - 4), filename);
    } else {
        snprintf(tags_path, sizeof(tags_path), "%s/%s_tags.txt", state->evidence_dir, filename);
    }

    FILE *f = fopen(tags_path, "r");
    if (!f && state->app && state->app->activity && state->app->activity->externalDataPath) {
        if (len > 4 && (strcmp(filename + len - 4, ".wav") == 0 || strcmp(filename + len - 4, ".mp4") == 0)) {
            snprintf(tags_path, sizeof(tags_path), "%s/Movies/%.*s_tags.txt", state->app->activity->externalDataPath, (int)(len - 4), filename);
        }
        f = fopen(tags_path, "r");
    }
    if (!f) return;

    char line[128];
    while (state->loaded_tags_count < DS_MAX_LOADED_TAGS && fgets(line, sizeof(line), f)) {
        char *comma = strchr(line, ',');
        if (comma) {
            *comma = '\0';
            const uint64_t ms = (uint64_t)strtoull(line, NULL, 10);
            const uint8_t id = (uint8_t)atoi(comma + 1);
            state->loaded_tags[state->loaded_tags_count++] = (DSTagEntry){
                .timestamp_ms = ms,
                .tag_id = id
            };
        }
    }
    fclose(f);
}

static void jump_to_tag(DeadSpeak *state, bool forward) {
    if (!state || !state->voice_player || state->loaded_tags_count == 0) return;
    const uint64_t cur_ms = ds_voice_player_get_wav_position_ms(state->voice_player);
    const uint64_t dur_ms = ds_voice_player_get_wav_duration_ms(state->voice_player);

    if (forward) {
        for (size_t i = 0; i < state->loaded_tags_count; ++i) {
            if (state->loaded_tags[i].timestamp_ms >= cur_ms + 50ULL) {
                ds_voice_player_seek_wav_ms(state->voice_player, state->loaded_tags[i].timestamp_ms);
                return;
            }
        }
        if (dur_ms > 0) ds_voice_player_seek_wav_ms(state->voice_player, dur_ms);
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
            ds_voice_player_seek_wav_ms(state->voice_player, state->loaded_tags[found_idx].timestamp_ms);
        } else {
            ds_voice_player_seek_wav_ms(state->voice_player, 0ULL);
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
    uint32_t last_srt_sec = UINT32_MAX, srt_cue_index = 0, last_wf_ms = 0;

    while (atomic_load(&state->audio_active)) {
        const aaudio_result_t frames = AAudioStream_read(stream, samples, 480, 100000000LL);
        if (frames < 0) { atomic_store(&state->audio_failed, true); break; }
        if (frames == 0) continue;

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
        ds_engine_spectrum(samples, (size_t)frames, sample_rate, bands);
        for (unsigned band = 0; band < 3; ++band) {
            const unsigned band_level = (unsigned)(bands[band] * 4000.0f);
            atomic_store(&state->spectrum_milli[band], band_level > 1000U ? 1000U : band_level);
        }
        float bins[DS_SPECTRUM_BINS];
        ds_engine_spectrum_bins(samples, (size_t)frames, sample_rate, bins, DS_SPECTRUM_BINS);
        for (unsigned band = 0; band < DS_SPECTRUM_BINS; ++band)
            atomic_store(&state->spectrum_bins[band], (uint32_t)fminf(1000, bins[band] * 6000));

        /* Sub-Bass EVP Anomaly Evaluator (80-300 Hz) */
        static float sub_bass_baseline = 15.0f;
        const float sub_energy = calculate_sub_bass_energy(samples, (size_t)frames, sample_rate);
        sub_bass_baseline = sub_bass_baseline * 0.98f + sub_energy * 0.02f;
        state->sub_bass_level = sub_energy;

        const uint64_t cap_now = now_millis();
        if (sub_energy > 15.0f && sub_energy > sub_bass_baseline * 2.0f) {
            atomic_store(&state->sub_bass_anomaly, true);
            state->sub_bass_anomaly_expiry_ms = cap_now + 2000U;
            /* Emit tag 2 (ORANGE / ANOMALY) to _tags.txt */
            emit_companion_tag(state, 2);
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

static const char *camera_vertex_shader =
    "attribute vec2 a_position;attribute vec2 a_uv;varying vec2 v_uv;"
    "void main(){v_uv=a_uv;gl_Position=vec4(a_position,0.0,1.0);}";
static const char *camera_fragment_shader =
    "precision mediump float;\n"
    "uniform sampler2D u_image;\n"
    "uniform int u_filter;\n"
    "varying vec2 v_uv;\n"
    "void main(){\n"
    "    vec4 c = texture2D(u_image, v_uv);\n"
    "    float luma = dot(c.rgb, vec3(0.299, 0.587, 0.114));\n"
    "    if (u_filter == 0) {\n" /* NIGHT: Phosphor Green night vision */
    "        float g = clamp(luma * 1.55 + 0.08, 0.0, 1.0);\n"
    "        gl_FragColor = vec4(g * 0.12, g * 1.08, g * 0.22, 1.0);\n"
    "    } else if (u_filter == 1) {\n" /* THERMAL: False-Color Heatmap */
    "        float t = clamp(luma, 0.0, 1.0);\n"
    "        vec3 col;\n"
    "        if (t < 0.25) col = mix(vec3(0.05, 0.0, 0.25), vec3(0.0, 0.2, 0.85), t / 0.25);\n"
    "        else if (t < 0.5) col = mix(vec3(0.0, 0.2, 0.85), vec3(0.85, 0.1, 0.15), (t - 0.25) / 0.25);\n"
    "        else if (t < 0.75) col = mix(vec3(0.85, 0.1, 0.15), vec3(1.0, 0.85, 0.0), (t - 0.5) / 0.25);\n"
    "        else col = mix(vec3(1.0, 0.85, 0.0), vec3(1.0, 1.0, 1.0), (t - 0.75) / 0.25);\n"
    "        gl_FragColor = vec4(col, 1.0);\n"
    "    } else if (u_filter == 2) {\n" /* EDGE: Sobel spectral contour detection */
    "        float d = 1.0 / 480.0;\n"
    "        float c0 = dot(texture2D(u_image, v_uv + vec2(-d, -d)).rgb, vec3(0.299, 0.587, 0.114));\n"
    "        float c1 = dot(texture2D(u_image, v_uv + vec2(0.0, -d)).rgb, vec3(0.299, 0.587, 0.114));\n"
    "        float c2 = dot(texture2D(u_image, v_uv + vec2(d, -d)).rgb, vec3(0.299, 0.587, 0.114));\n"
    "        float c3 = dot(texture2D(u_image, v_uv + vec2(-d, 0.0)).rgb, vec3(0.299, 0.587, 0.114));\n"
    "        float c5 = dot(texture2D(u_image, v_uv + vec2(d, 0.0)).rgb, vec3(0.299, 0.587, 0.114));\n"
    "        float c6 = dot(texture2D(u_image, v_uv + vec2(-d, d)).rgb, vec3(0.299, 0.587, 0.114));\n"
    "        float c7 = dot(texture2D(u_image, v_uv + vec2(0.0, d)).rgb, vec3(0.299, 0.587, 0.114));\n"
    "        float c8 = dot(texture2D(u_image, v_uv + vec2(d, d)).rgb, vec3(0.299, 0.587, 0.114));\n"
    "        float gx = -c0 + c2 - 2.0 * c3 + 2.0 * c5 - c6 + c8;\n"
    "        float gy = -c0 - 2.0 * c1 - c2 + c6 + 2.0 * c7 + c8;\n"
    "        float edge = clamp(sqrt(gx * gx + gy * gy) * 3.2, 0.0, 1.0);\n"
    "        gl_FragColor = vec4(edge * 0.1, edge * 1.0, edge * 0.75, 1.0);\n"
    "    } else if (u_filter == 3) {\n" /* MONO: High Contrast IR */
    "        float m = clamp((luma - 0.08) * 1.25, 0.0, 1.0);\n"
    "        gl_FragColor = vec4(vec3(m), 1.0);\n"
    "    } else if (u_filter == 4) {\n" /* SEPIA: Vintage */
    "        vec3 s = vec3(dot(c.rgb, vec3(0.393, 0.769, 0.189)),\n"
    "                      dot(c.rgb, vec3(0.349, 0.686, 0.168)),\n"
    "                      dot(c.rgb, vec3(0.272, 0.534, 0.131)));\n"
    "        gl_FragColor = vec4(clamp(s, 0.0, 1.0), 1.0);\n"
    "    } else {\n" /* RAW */
    "        gl_FragColor = c;\n"
    "    }\n"
    "}";

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
    vertex = compile_shader(GL_VERTEX_SHADER, camera_vertex_shader);
    fragment = compile_shader(GL_FRAGMENT_SHADER, camera_fragment_shader);
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
                    batched_count = 0;
                }
            }

            cursor_x += ds_text_char_advance(ch, scale, font_type);
        }

        if (batched_count > 0) {
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

[[nodiscard]] static inline float fit_text_scale_styled(const char *value, float preferred, float available_width, DSFontType font_type) {
    return ds_fit_text_scale_styled(value, preferred, available_width, font_type);
}

[[nodiscard]] static inline float fit_text_scale(const char *value, float preferred, float available_width) {
    return ds_fit_text_scale_styled(value, preferred, available_width, DS_FONT_STANDARD);
}

[[nodiscard]] static inline float text_width(const char *value, float scale, DSFontType font_type) {
    return ds_text_width(value, scale, font_type);
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
    if (state->tab == 3) return 650.0f;
    const float words = state->log_count ? 70.0f + (float)state->log_count * 42.0f : 130.0f;
    const float media = 36.0f + (float)(state->media_count ? state->media_count : 1) * 34.0f + 16.0f;
    return state->ui.wide ? fmaxf(words, 22.0f + media) : fmaxf(140.0f, words + 20.0f) + media;
}

static void custom_button(DeadSpeak *state, DSRect button, const char *label, bool selected) {
    rect(state, button.x, button.y, button.w, button.h, selected ? 0.10f : 0.14f, selected ? 0.32f : 0.18f, selected ? 0.25f : 0.19f, 0.97f);
    const DSFontType font_type = selected ? DS_FONT_BOLD : DS_FONT_STANDARD;
    const float scale = fit_text_scale_styled(label, ds_sp_to_scale(DS_FONT_SIZE_BODY_MD), button.w - 10, font_type);
    const float total_w = text_width(label, scale, font_type);
    text_styled(state, label, button.x + (button.w - total_w) / 2,
                button.y + (button.h - 7.0f * scale) / 2, scale, font_type, 0.88f, 0.98f, 0.93f, 1);
}

static void draw_log(DeadSpeak *state, float panel_r, float panel_g, float panel_b,
                     float mint_r, float mint_g, float mint_b, float muted_r, float muted_g, float muted_b) {
    const DSRect body = state->ui.body;
    const float column_width = state->ui.wide ? (body.w - 16.0f) * 0.5f : body.w;
    const float x = body.x + 16.0f, top = body.y;
    text(state, "RECENT SENSOR WORDS", x, top + 22, fit_text_scale("RECENT SENSOR WORDS", 1.8f, column_width - 32), mint_r, mint_g, mint_b, 1.0f);
    if (state->export_status_expiry_ms > now_millis() && state->export_status_message[0]) {
        rect(state, x, top + 42, column_width - 32, 22, 0.12f, 0.42f, 0.28f, 0.95f);
        text(state, state->export_status_message, x + 6, top + 47, fit_text_scale(state->export_status_message, 1.0f, column_width - 44), 1.0f, 1.0f, 1.0f, 1.0f);
    }
    if (state->log_count == 0) {
        text(state, "NO WORDS LOGGED YET", x, top + 70, fit_text_scale("NO WORDS LOGGED YET", 1.5f, column_width - 32), muted_r, muted_g, muted_b, 1.0f);
        text(state, "ENABLE SCAN TO COLLECT READINGS", x, top + 100, fit_text_scale("ENABLE SCAN TO COLLECT READINGS", 1.1f, column_width - 32), muted_r, muted_g, muted_b, 1.0f);
    } else {
        for (unsigned i = 0; i < state->log_count; ++i) {
            const unsigned slot = (state->log_next + DS_LOG_CAPACITY - 1U - i) % DS_LOG_CAPACITY;
            const float y = top + 70.0f + (float)i * 42.0f;
            char details[64];
            snprintf(details, sizeof(details), "SAMPLE %u  FIELD %.1f UT (%+.1f)%s",
                state->word_samples[slot], state->word_fields[slot],
                state->word_deltas[slot], state->word_whispers[slot] ? " [W]" : "");
            text(state, state->words[slot], x, y, fit_text_scale(state->words[slot], 2.0f, column_width - 32), mint_r, mint_g, mint_b, 1.0f);
            text(state, details, x, y + 19, fit_text_scale(details, 1.0f, column_width - 32), muted_r, muted_g, muted_b, 1.0f);
            rect(state, x, y + 34, column_width - 32, 1, 0.17f, 0.24f, 0.23f, 1.0f);
        }
    }
    const float media_x = state->ui.wide ? x + column_width + 16.0f : x;
    const float media_y = state->ui.wide ? top + 22.0f : top + fmaxf(140.0f, 90.0f + (float)state->log_count * 42.0f);
    text(state, "SAVED CAPTURES & SESSIONS", media_x, media_y, fit_text_scale("SAVED CAPTURES & SESSIONS", 1.35f, column_width - 146), mint_r, mint_g, mint_b, 1.0f);
    custom_button(state, (DSRect){media_x + column_width - 138.0f, media_y - 6.0f, 106.0f, 30.0f}, "EXPORT LOGS", false);
    if (state->media_count == 0U) text(state, "NO AUDIO OR VIDEO FILES", media_x, media_y + 34, fit_text_scale("NO AUDIO OR VIDEO FILES", 1.1f, column_width - 32), muted_r, muted_g, muted_b, 1.0f);
    
    char curr_wav[96] = {0};
    float wav_prog = 0.0f;
    const bool is_playing_any = ds_voice_player_is_playing_wav(state->voice_player, curr_wav, sizeof(curr_wav), &wav_prog);

    float cur_item_y = media_y + 36.0f;
    for (unsigned i = 0; i < state->media_count; ++i) {
        const char *filename = state->media_files[i];
        const size_t len = strlen(filename);
        const bool is_wav = len > 4 && strcmp(filename + len - 4, ".wav") == 0;
        const bool is_mp4 = len > 4 && strcmp(filename + len - 4, ".mp4") == 0;
        const bool is_txt = len > 4 && strcmp(filename + len - 4, ".txt") == 0;
        const bool is_this_playing = is_playing_any && strcmp(curr_wav, filename) == 0;

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
            const uint64_t cur_ms = ds_voice_player_get_wav_position_ms(state->voice_player);
            const uint64_t dur_ms = ds_voice_player_get_wav_duration_ms(state->voice_player);
            const unsigned cur_sec = (unsigned)(cur_ms / 1000ULL);
            const unsigned dur_sec = (unsigned)(dur_ms / 1000ULL);

            /* Interactive scrubbable playback card */
            rect(state, media_x - 4, cur_item_y - 2, column_width - 24, 76, 0.07f, 0.18f, 0.15f, 0.95f);

            char label[128];
            snprintf(label, sizeof(label), "|| %s  %02u:%02u / %02u:%02u (%u%%)",
                     filename, cur_sec / 60, cur_sec % 60, dur_sec / 60, dur_sec % 60, (unsigned)(wav_prog * 100.0f));
            text(state, label, media_x + 4, cur_item_y + 4, fit_text_scale(label, 1.05f, column_width - 32), 0.95f, 0.98f, 0.94f, 1.0f);

            /* Timeline scrubber bar */
            const float bar_x = media_x + 4.0f;
            const float bar_w = column_width - 36.0f;
            const float bar_y = cur_item_y + 24.0f;
            rect(state, bar_x, bar_y, bar_w, 8, 0.12f, 0.18f, 0.17f, 1.0f);
            rect(state, bar_x, bar_y, bar_w * wav_prog, 8, mint_r, mint_g, mint_b, 1.0f);
            filled_circle(state, bar_x + bar_w * wav_prog, bar_y + 4.0f, 6.0f, 1.0f, 1.0f, 1.0f, 1.0f);

            /* Tag dots along timeline */
            int active_tag_idx = -1;
            uint8_t active_tag_id = 0;
            if (dur_ms > 0) {
                for (size_t t = 0; t < state->loaded_tags_count; ++t) {
                    const uint64_t t_ms = state->loaded_tags[t].timestamp_ms;
                    if (t_ms <= dur_ms) {
                        const float dot_x = bar_x + (float)t_ms / (float)dur_ms * bar_w;
                        const uint8_t tid = state->loaded_tags[t].tag_id;
                        const float dr = tid == 1 ? 0.2f : tid == 2 ? 1.0f : 0.85f;
                        const float dg = tid == 1 ? 0.95f : tid == 2 ? 0.55f : 0.35f;
                        const float db = tid == 1 ? 0.4f : tid == 2 ? 0.1f : 0.95f;
                        filled_circle(state, dot_x, bar_y + 4.0f, 3.2f, dr, dg, db, 1.0f);
                    }
                    if (t_ms >= (cur_ms > 1200ULL ? cur_ms - 1200ULL : 0ULL) && t_ms <= cur_ms + 1200ULL) {
                        active_tag_idx = (int)t + 1;
                        active_tag_id = state->loaded_tags[t].tag_id;
                    }
                }
            }

            /* Tag jump and stop buttons */
            const DSRect prev_tag_btn = {media_x + 4.0f, cur_item_y + 42.0f, 64.0f, 26.0f};
            custom_button(state, prev_tag_btn, "<< TAG", false);

            const DSRect next_tag_btn = {media_x + 74.0f, cur_item_y + 42.0f, 64.0f, 26.0f};
            custom_button(state, next_tag_btn, "TAG >>", false);

            const DSRect stop_btn = {media_x + 144.0f, cur_item_y + 42.0f, 52.0f, 26.0f};
            custom_button(state, stop_btn, "STOP", false);

            char tag_status[64];
            if (active_tag_idx > 0) {
                const char *tname = active_tag_id == 1 ? "WORD" : active_tag_id == 2 ? "ANOMALY" : "BOOKMARK";
                snprintf(tag_status, sizeof(tag_status), "TAG #%d [%s]", active_tag_idx, tname);
            } else {
                snprintf(tag_status, sizeof(tag_status), "TAGS: %zu", state->loaded_tags_count);
            }
            text(state, tag_status, media_x + 204.0f, cur_item_y + 49.0f, 1.0f, mint_r, mint_g, mint_b, 1.0f);

            /* Delete Button */
            const DSRect del_btn = {media_x + column_width - 56.0f, cur_item_y + 42.0f, 44.0f, 26.0f};
            rect(state, del_btn.x, del_btn.y, del_btn.w, del_btn.h, 0.45f, 0.12f, 0.12f, 0.9f);
            text(state, "DEL", del_btn.x + 8.0f, del_btn.y + 7.0f, 1.0f, 1.0f, 0.8f, 0.8f, 1.0f);

            cur_item_y += 82.0f;
        } else {
            char label[128];
            if (is_wav) snprintf(label, sizeof(label), "> %s", filename);
            else if (is_mp4) snprintf(label, sizeof(label), "[] %s", filename);
            else if (is_txt) snprintf(label, sizeof(label), "## %s", filename);
            else snprintf(label, sizeof(label), "- %s", filename);

            const float lr = is_wav ? mint_r : is_mp4 ? 0.45f : muted_r;
            const float lg = is_wav ? mint_g : is_mp4 ? 0.85f : muted_g;
            const float lb = is_wav ? mint_b : is_mp4 ? 0.95f : muted_b;
            text(state, label, media_x, cur_item_y + 4, fit_text_scale(label, 0.95f, column_width - 150.0f), lr, lg, lb, 1.0f);

            /* Companion badges */
            float badge_x = media_x + column_width - 152.0f;
            if (has_txt) { text(state, "TXT", badge_x, cur_item_y + 4, 0.8f, 0.35f, 0.85f, 0.75f, 1.0f); badge_x += 24.0f; }
            if (has_tags) { text(state, "TAGS", badge_x, cur_item_y + 4, 0.8f, 1.0f, 0.65f, 0.2f, 1.0f); badge_x += 30.0f; }
            if (has_wf) { text(state, "WF", badge_x, cur_item_y + 4, 0.8f, 0.4f, 0.8f, 1.0f, 1.0f); badge_x += 20.0f; }
            if (has_srt) text(state, "SRT", badge_x, cur_item_y + 4, 0.8f, 0.8f, 0.8f, 0.8f, 1.0f);

            /* Delete Button */
            const DSRect del_btn = {media_x + column_width - 48.0f, cur_item_y + 1.0f, 40.0f, 22.0f};
            rect(state, del_btn.x, del_btn.y, del_btn.w, del_btn.h, 0.35f, 0.12f, 0.12f, 0.85f);
            text(state, "DEL", del_btn.x + 8.0f, del_btn.y + 4.0f, 0.9f, 1.0f, 0.75f, 0.75f, 1.0f);

            rect(state, media_x, cur_item_y + 28, column_width - 32, 1, 0.17f, 0.24f, 0.23f, 0.6f);
            cur_item_y += 36.0f;
        }
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
        "MONO AND SEPIA NEED CAMERA SUPPORT",
        "CAM TO ENABLE LIVE PREVIEW",
        "RED BUTTON STARTS AND STOPS CAPTURE",
        "FX CHANGES THE CAMERA FILTER",
        "WORDS EDITS YOUR LOCAL WORD BANK",
        "INFO SHOWS AXES AND RECALIBRATION",
        "HOLD STILL FOR TWENTY REAL SAMPLES",
        "WORDS REQUIRE A FIELD DEVIATION"
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

static DSRect custom_modal(const DeadSpeak *state) {
    return (DSRect){12, 58, state->width - 24, state->height - 114};
}

static void draw_custom_words(DeadSpeak *state) {
    const DSRect modal = custom_modal(state);
    rect(state, 0, 0, state->width, state->height, 0, 0, 0, 0.72f);
    rect(state, modal.x, modal.y, modal.w, modal.h, 0.04f, 0.07f, 0.08f, 1);
    text(state, "WORD BANK & VOICE", modal.x + 12, modal.y + 16, 1.7f, 0.9f, 0.98f, 0.94f, 1);
    custom_button(state, (DSRect){modal.x + modal.w - 48, modal.y + 4, 44, 36}, "X", false);
    
    const float half = (modal.w - 30) / 2;
    custom_button(state, (DSRect){modal.x + 10, modal.y + 44, half, 36}, "BUNDLED (5K)", !state->custom_selected);
    custom_button(state, (DSRect){modal.x + 20 + half, modal.y + 44, half, 36}, "CUSTOM ONLY", state->custom_selected);

    /* Voice Mode Selector Bar (Classic, Whisper, Dynamic, Spirit Box) */
    const float quarter = (modal.w - 44) / 4;
    const char *voice_modes[] = {"CLASSIC", "WHISPER", "DYNAMIC", "SPIRIT BOX"};
    for (unsigned i = 0; i < 4; ++i) {
        const bool sel = state->voice_config.mode == (DSVoiceMode)i;
        custom_button(state, (DSRect){modal.x + 10 + i * (quarter + 8), modal.y + 86, quarter, 32}, voice_modes[i], sel);
    }

    if (state->custom_editing) {
        rect(state, modal.x + 10, modal.y + 124, modal.w - 20, 38, 0.1f, 0.16f, 0.17f, 1);
        const char *input = state->custom_input[0] ? state->custom_input : "TYPE A WORD OR PHRASE";
        text(state, input, modal.x + 16, modal.y + 136, fit_text_scale(input, 1.8f, modal.w - 32), 0.9f, 0.98f, 0.94f, 1);
        const char *rows[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
        const float key_height = fminf(42, (modal.h - 180) / 4);
        for (unsigned row = 0; row < 3; ++row) {
            const float key_width = (modal.w - 20) / strlen(rows[row]);
            for (unsigned column = 0; rows[row][column]; ++column) {
                const char key[2] = {rows[row][column], 0};
                custom_button(state, (DSRect){modal.x + 10 + column * key_width, modal.y + 168 + row * key_height,
                    key_width - 2, key_height - 3}, key, false);
            }
        }
        const float key_width = (modal.w - 20) / 3;
        const char *actions[] = {"SPACE", "DEL", "SAVE"};
        for (unsigned i = 0; i < 3; ++i)
            custom_button(state, (DSRect){modal.x + 10 + i * key_width, modal.y + 168 + 3 * key_height,
                key_width - 2, key_height - 3}, actions[i], i == 2);
    } else {
        const size_t page_size = (size_t)fmaxf(1, fminf(6, floorf((modal.h - 206) / 42)));
        const size_t count = ds_custom_words_count(&state->custom_words);
        const size_t last_page = count ? (count - 1) / page_size : 0;
        if (state->custom_page > last_page) state->custom_page = last_page;
        for (size_t row = 0; row < page_size && state->custom_page * page_size + row < count; ++row) {
            const size_t index = state->custom_page * page_size + row;
            const float y = modal.y + 126 + row * 42;
            const char *raw_word = ds_custom_words_word(&state->custom_words, index);
            char item_label[48];
            snprintf(item_label, sizeof(item_label), "%u. %s", (unsigned)(index + 1), raw_word);
            text(state, item_label, modal.x + 14, y + 14, fit_text_scale(item_label, 1.4f, modal.w - 74), 0.9f, 0.98f, 0.94f, 1);
            custom_button(state, (DSRect){modal.x + modal.w - 54, y, 44, 38}, "DEL", false);
        }
        if (!count) text(state, "NO CUSTOM WORDS - ADD SOME BELOW", modal.x + 14, modal.y + 140,
            fit_text_scale("NO CUSTOM WORDS - ADD SOME BELOW", 1.3f, modal.w - 28), 0.8f, 0.85f, 0.84f, 1);
        custom_button(state, (DSRect){modal.x + 10, modal.y + modal.h - 70, 48, 40}, "PREV", false);
        const float middle_w = (modal.w - 180) / 2;
        custom_button(state, (DSRect){modal.x + 62, modal.y + modal.h - 70, middle_w, 40}, "+ ADD", true);
        custom_button(state, (DSRect){modal.x + 62 + middle_w + 6, modal.y + modal.h - 70, middle_w, 40}, "TEST VOX", false);
        custom_button(state, (DSRect){modal.x + modal.w - 58, modal.y + modal.h - 70, 48, 40}, "NEXT", false);
    }
    const char *message = state->custom_message[0] ? state->custom_message : "LOCAL STORAGE ONLY - NO CLOUD DEPENDENCIES";
    text(state, message, modal.x + 12, modal.y + modal.h - 20, fit_text_scale(message, 1.0f, modal.w - 24), 0.96f, 0.75f, 0.43f, 1);
}

static void draw_filter_hud(DeadSpeak *state, uint64_t now) {
    const DSFilterHUD hud = ds_filter_hud((float)state->width, (float)state->height, state->filter_hud_deadline, now);
    if (!hud.visible || hud.alpha <= 0.001f) return;

    const float alpha = hud.alpha;
    const DSRect card = hud.card;
    const float cx = card.x + card.w * 0.5f;

    /* Theme-aware accent colors */
    float accent_r = 0.92f, accent_g = 0.98f, accent_b = 0.95f;
    if (state->hud_hue > 0.001f) {
        ds_interpolate_hud_accent(state->hud_hue, &accent_r, &accent_g, &accent_b);
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
    line(state, card.x, card.y, card.x + card.w, card.y, accent_r, accent_g, accent_b, 0.90f * alpha);
    line(state, card.x, card.y + card.h, card.x + card.w, card.y + card.h, 0.20f, 0.28f, 0.25f, 0.70f * alpha);
    line(state, card.x, card.y, card.x, card.y + card.h, 0.20f, 0.28f, 0.25f, 0.70f * alpha);
    line(state, card.x + card.w, card.y, card.x + card.w, card.y + card.h, 0.20f, 0.28f, 0.25f, 0.70f * alpha);

    /* 3. Top hint: < SWIPE TO CYCLE > */
    const float hint_scale = ds_sp_to_scale(DS_FONT_SIZE_BADGE);
    const float hint_w = text_width("< SWIPE TO CYCLE >", hint_scale, DS_FONT_STANDARD);
    text_styled(state, "< SWIPE TO CYCLE >", cx - hint_w * 0.5f, card.y + 12.0f, hint_scale,
                DS_FONT_STANDARD, 0.65f, 0.85f, 0.78f, 0.85f * alpha);

    /* 4. Filter Title in bold */
    const float title_scale = fit_text_scale_styled(meta.title, ds_sp_to_scale(DS_FONT_SIZE_BODY_LG), card.w - 20.0f, DS_FONT_BOLD);
    const float title_w = text_width(meta.title, title_scale, DS_FONT_BOLD);
    text_styled(state, meta.title, cx - title_w * 0.5f, card.y + 32.0f, title_scale,
                DS_FONT_BOLD, meta.r, meta.g, meta.b, alpha);

    /* 5. Subtitle description */
    const float sub_scale = ds_sp_to_scale(DS_FONT_SIZE_BADGE);
    const float sub_w = text_width(meta.mode, sub_scale, DS_FONT_STANDARD);
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
             active ? accent_r : 0.22f,
             active ? accent_g : 0.28f,
             active ? accent_b : 0.26f,
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
    float accent_r = 0.92f, accent_g = 0.98f, accent_b = 0.95f;
    if (state->hud_hue > 0.001f) {
        ds_interpolate_hud_accent(state->hud_hue, &accent_r, &accent_g, &accent_b);
    }
    if (!state->minimal_hud) {
        rect(state, 0, 0, state->width, 58, 0, 0, 0, 0.6f);
        const float header_max_w = (float)(state->width - 340 - 20);
        const float header_scale = fit_text_scale_styled("DEADSPEAK", ds_sp_to_scale(DS_FONT_SIZE_BODY_LG), header_max_w, DS_FONT_BOLD);
        text_styled(state, "DEADSPEAK", 16, 16, header_scale, DS_FONT_BOLD, accent_r, accent_g, accent_b, 1);

        /* Top Right Voice Synthesizer, Geiger, Style and Tint Buttons */
        const DSRect spk_btn = {state->width - 76, 12, 68, 34};
        custom_button(state, spk_btn, state->voice_config.enabled ? "VOX ON" : "VOX OFF", state->voice_config.enabled);

        const bool geiger_on = ds_voice_player_get_geiger_enabled(state->voice_player);
        const DSRect geiger_btn = {state->width - 162, 12, 82, 34};
        custom_button(state, geiger_btn, geiger_on ? "GEIGER ON" : "GEIGER OFF", geiger_on);

        static const char *const theme_names[] = {"#ORIGINAL", "ABDUCTION", "AI"};
        const DSRect style_btn = {state->width - 256, 12, 90, 34};
        custom_button(state, style_btn, theme_names[state->theme_mode % 3], state->theme_mode != 0);

        char tint_label[32];
        snprintf(tint_label, sizeof(tint_label), "TINT %u%%", (unsigned)(state->hud_hue * 100.0f));
        const DSRect tint_btn = {state->width - 336, 12, 76, 34};
        custom_button(state, tint_btn, tint_label, state->hud_hue > 0.05f);

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
        text_styled(state, status, 16, 38, fit_text_scale_styled(status, ds_sp_to_scale(DS_FONT_SIZE_MICRO), header_max_w, DS_FONT_STANDARD), DS_FONT_STANDARD, recording ? 1 : 0.8f, recording ? 0.25f : 0.9f, recording ? 0.25f : 0.85f, 1);
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
        const float top = state->height < 650 ? 76 : 142;
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
        custom_button(state, (DSRect){20, top + 84, 112, 40}, "CALIBRATE", false);
        const char *sens_names[] = {"SENS: HI", "SENS: MED", "SENS: LO"};
        custom_button(state, (DSRect){138, top + 84, 96, 40}, sens_names[state->baseline.sensitivity % 3], false);
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
            if (state->theme_mode == 1) {
                /* Abduction Theme: Bio-luminescent alien neon curve */
                const float osc_mid_y = ui.audio.y + ui.audio.h * 0.50f;
                const float step_x = (ui.audio.w - 8) / (float)(DS_OSCILLOSCOPE_SAMPLES - 1);
                for (int s = 0; s < DS_OSCILLOSCOPE_SAMPLES - 1; ++s) {
                    const float y1 = osc_mid_y - (float)atomic_load(&state->oscilloscope[s]) / 32768.0f * (ui.audio.h * 0.44f);
                    const float y2 = osc_mid_y - (float)atomic_load(&state->oscilloscope[s + 1]) / 32768.0f * (ui.audio.h * 0.44f);
                    line(state, ui.audio.x + 4 + (float)s * step_x, y1 - 1.0f, ui.audio.x + 4 + (float)(s + 1) * step_x, y2 - 1.0f, 0.1f, 1.0f, 0.85f, 0.4f);
                    line(state, ui.audio.x + 4 + (float)s * step_x, y1, ui.audio.x + 4 + (float)(s + 1) * step_x, y2, 0.4f, 1.0f, 0.95f, 0.95f);
                }
            } else if (state->theme_mode == 2) {
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
        emit_companion_tag(state, 2);
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
        snprintf(sub_text, sizeof(sub_text), "FIELD %.1f UT  DEL %+.1f UT%s",
                 state->word_fields[latest_slot], state->word_deltas[latest_slot],
                 state->word_whispers[latest_slot] ? "  [WHISPER]" : "");
    } else {
        primary_word = "LISTENING...";
        snprintf(sub_text, sizeof(sub_text), "AWAITING SENSOR ANOMALIES");
    }

    const float word_r = is_moving ? 0.96f : 0.95f;
    const float word_g = is_moving ? 0.75f : 0.98f;
    const float word_b = is_moving ? 0.35f : 0.95f;
    const float card_word_scale = fit_text_scale_styled(primary_word, ds_sp_to_scale(DS_FONT_SIZE_BODY_LG), ui.word.w - 20, DS_FONT_BOLD);
    text_styled(state, primary_word, ui.word.x + 10, ui.word.y + 6, card_word_scale, DS_FONT_BOLD, word_r, word_g, word_b, 1);
    text_styled(state, sub_text, ui.word.x + 10, ui.word.y + 24, fit_text_scale_styled(sub_text, ds_sp_to_scale(DS_FONT_SIZE_BADGE), ui.word.w - 20, DS_FONT_STANDARD), DS_FONT_STANDARD, 0.65f, 0.85f, 0.78f, 0.9f);

    /* 4. RECORD SHUTTER BUTTON */
    const float cx = ui.record.x + ui.record.w / 2, cy = ui.record.y + ui.record.h / 2;
    filled_circle(state, cx, cy, 30, 0.05f, 0.05f, 0.05f, 0.85f);
    circle(state, cx, cy, 25, 0.98f, 0.98f, 0.95f, 1);
    if (recording) rect(state, cx - 11, cy - 11, 22, 22, 1, 0.05f, 0.08f, 1);
    else filled_circle(state, cx, cy, 18, 1, 0.05f, 0.08f, 1);

    /* Manual Bookmark Tag Button (+TAG) during recording */
    if (recording) {
        const DSRect tag_btn = {cx + 36.0f, cy - 16.0f, 56.0f, 32.0f};
        custom_button(state, tag_btn, "+TAG", false);
    }
    
    static const char *const filter_names[] = {"NIGHT", "THERMAL", "EDGE", "MONO", "SEPIA", "RAW"};
    text(state, filter_names[state->video_filter], 16, ui.record.y + 24, 1.2f, 0.9f, 0.95f, 0.92f, 1);
    if (atomic_load(&state->audio_failed) || ds_video_failed(&state->video))
        text_styled(state, "CAPTURE ERROR", 16, ui.record.y + 44, ds_sp_to_scale(DS_FONT_SIZE_BADGE), DS_FONT_BOLD, 1, 0.65f, 0.3f, 1);
    rect(state, ui.navigation.x, ui.navigation.y, ui.navigation.w, ui.navigation.h, 0, 0, 0, 0.82f);
    const char *nav[] = {"CAMERA", "LOG", "EXPLORE", "GUIDE"};
    for (unsigned i = 0; i < 4; ++i) {
        const float nav_scale = ds_sp_to_scale(i == 0 ? DS_FONT_SIZE_BODY_MD : DS_FONT_SIZE_CAPTION);
        const DSFontType nav_font = i == 0 ? DS_FONT_BOLD : DS_FONT_STANDARD;
        const float total_w = text_width(nav[i], nav_scale, nav_font);
        text_styled(state, nav[i], state->width * (i + 0.5f) / 4.0f - total_w / 2.0f,
                    ui.navigation.y + 20, nav_scale, nav_font, 0.8f, 0.95f, 0.86f, 1);
    }
    draw_filter_hud(state, now);
    if (state->custom_open) draw_custom_words(state);
}

static void draw_frame(DeadSpeak *state) {
    float mint_r = 0.73f, mint_g = 0.96f, mint_b = 0.86f;
    if (state->hud_hue > 0.001f) {
        ds_interpolate_hud_accent(state->hud_hue, &mint_r, &mint_g, &mint_b);
    }
    const float muted_r = 0.42f, muted_g = 0.53f, muted_b = 0.52f;
    const float amber_r = 0.96f, amber_g = 0.70f, amber_b = 0.36f;
    const float panel_r = 0.075f, panel_g = 0.105f, panel_b = 0.11f;
    const float w = (float)state->width;
    const float h = (float)state->height;
    const int64_t sensor_time = sensor_now_ns();
    const bool magnetic_live = ds_sensor_live(&state->readings[0], sensor_time);

    glClearColor(0.027f, 0.043f, 0.047f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (state->tab == 0) {
        state->body_drawing = false;
        draw_camera(state);
        eglSwapBuffers(state->display, state->surface);
        return;
    }

    const float header_offset = state->ui.compact ? -8.0f : 0.0f;
    rect(state, 18, 22 + header_offset, 22, 22, mint_r, mint_g, mint_b, 0.9f);
    rect(state, 23, 27 + header_offset, 12, 12, 0.027f, 0.043f, 0.047f, 1.0f);
    text_styled(state, "DEADSPEAK", 50, 24 + header_offset, fit_text_scale_styled("DEADSPEAK", ds_sp_to_scale(DS_FONT_SIZE_TITLE_LG), w - 68.0f, DS_FONT_BOLD), DS_FONT_BOLD, mint_r, mint_g, mint_b, 1.0f);
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
    const float title_scale = fit_text_scale_styled(screen_title, state->ui.compact ? ds_sp_to_scale(DS_FONT_SIZE_TITLE_MD) : ds_sp_to_scale(DS_FONT_SIZE_TITLE_LG), w - 44, DS_FONT_BOLD);
    text_styled(state, screen_title, 22, state->ui.title_y, title_scale, DS_FONT_BOLD, 0.95f, 0.98f, 0.94f, 1.0f);
    if (state->tab == 2) {
        text_styled(state, "PUBLIC REPORTS - NOT VERIFIED CLAIMS", 22, state->ui.subtitle_y, fit_text_scale_styled("PUBLIC REPORTS - NOT VERIFIED CLAIMS", ds_sp_to_scale(DS_FONT_SIZE_BADGE), w - 44, DS_FONT_STANDARD), DS_FONT_STANDARD, amber_r, amber_g, amber_b, 1.0f);
    } else {
        char sensor_label[64];
        snprintf(sensor_label, sizeof(sensor_label), "MAGNETOMETER %s", ds_sensor_status_name(state->readings[0].status));
        text_styled(state, sensor_label, 22, state->ui.subtitle_y, fit_text_scale_styled(sensor_label, state->ui.compact ? ds_sp_to_scale(DS_FONT_SIZE_CAPTION) : ds_sp_to_scale(DS_FONT_SIZE_BODY_MD), w - 44, DS_FONT_STANDARD), DS_FONT_STANDARD, magnetic_live ? mint_r : amber_r, magnetic_live ? mint_g : amber_g, magnetic_live ? mint_b : amber_b, 1.0f);
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
    const float value_scale = fit_text_scale_styled(value, ds_sp_to_scale(DS_FONT_SIZE_DISPLAY_XL), field.w - 84.0f, DS_FONT_MONO);
    const float value_width = text_width(value, value_scale, DS_FONT_MONO);
    const float ut_scale = ds_sp_to_scale(DS_FONT_SIZE_BODY_LG);
    const float ut_width = text_width("UT", ut_scale, DS_FONT_STANDARD);
    const float value_x = cx - (value_width + 12.0f + ut_width) * 0.5f;
    text_styled(state, value, value_x, cy - value_scale * 3.5f, value_scale, DS_FONT_MONO, 0.95f, 0.98f, 0.94f, 1.0f);
    if (magnetic_live) text_styled(state, "UT", value_x + value_width + 12.0f, cy - 5.6f, ut_scale, DS_FONT_STANDARD, muted_r, muted_g, muted_b, 1.0f);

    const float signal_width = (field.w - 36) * (magnetic_live ? state->signal : 0.0f);
    rect(state, field.x + 18, field.y + field.h - 36, field.w - 36, 4, 0.14f, 0.20f, 0.20f, 1.0f);
    rect(state, field.x + 18, field.y + field.h - 36, signal_width, 4, mint_r, mint_g, mint_b, 1.0f);
    char motion_label[64];
    snprintf(motion_label, sizeof(motion_label), "ACCEL %s  GYRO %s", ds_sensor_status_name(state->readings[1].status), ds_sensor_status_name(state->readings[2].status));
    text(state, motion_label, field.x + 18, field.y + field.h - 20,
         fit_text_scale(motion_label, 1.0f, field.w - 110), muted_r, muted_g, muted_b, 1.0f);
    text_styled(state, state->scanning ? "SCANNING" : "STANDBY", field.x + field.w - 76, field.y + field.h - 20, ds_sp_to_scale(DS_FONT_SIZE_CAPTION), DS_FONT_BOLD, state->scanning ? mint_r : amber_r, state->scanning ? mint_g : amber_g, state->scanning ? mint_b : amber_b, 1.0f);

    rect(state, word.x, word.y, word.w, word.h, panel_r, panel_g, panel_b, 1.0f);
    text_styled(state, "SIGNAL WORD", word.x + 14, word.y + 20, fit_text_scale_styled("SIGNAL WORD", ds_sp_to_scale(DS_FONT_SIZE_BODY_MD), word.w - 28, DS_FONT_STANDARD), DS_FONT_STANDARD, muted_r, muted_g, muted_b, 1.0f);
    const char *word_label = magnetic_live ? (state->word[0] ? state->word : "QUIET") : "NO SENSOR DATA";
    const float word_width = word.w - 28.0f;
    const float word_scale = fit_text_scale_styled(word_label, ds_sp_to_scale(DS_FONT_SIZE_DISPLAY_LG), word_width, DS_FONT_BOLD);
    text_styled(state, word_label, word.x + 14, word.y + word.h * 0.43f, word_scale, DS_FONT_BOLD, mint_r, mint_g, mint_b, 1.0f);
    text_styled(state, "ENVIRONMENTAL TRIGGER", word.x + 14, word.y + word.h - 25, fit_text_scale_styled("ENVIRONMENTAL TRIGGER", ds_sp_to_scale(DS_FONT_SIZE_BADGE), word_width, DS_FONT_STANDARD), DS_FONT_STANDARD, muted_r, muted_g, muted_b, 1.0f);

    const float second_x = capture.x;
    rect(state, capture.x, capture.y, capture.w, capture.h, panel_r, panel_g, panel_b, 1.0f);
    text_styled(state, "CAPTURE", second_x + 14, capture.y + 20, ds_sp_to_scale(DS_FONT_SIZE_BODY_MD), DS_FONT_STANDARD, muted_r, muted_g, muted_b, 1.0f);
    const bool audio_active = atomic_load(&state->audio_active);
    const bool video_active = ds_video_active(&state->video);
    const bool capture_active = audio_active || video_active;
    const bool capture_failed = atomic_load(&state->audio_failed) || ds_video_failed(&state->video);
    const char *capture_label = capture_active ? "LIVE" : (state->capture_requested ? "ALLOW ACCESS" : (capture_failed ? "ERROR" : "READY"));
    const float capture_width = capture.w - 28.0f;
    const float capture_scale = fit_text_scale_styled(capture_label, ds_sp_to_scale(DS_FONT_SIZE_DISPLAY_LG), capture_width, DS_FONT_BOLD);
    text_styled(state, capture_label, second_x + 14, capture.y + capture.h * 0.43f, capture_scale, DS_FONT_BOLD, capture_active ? amber_r : (capture_failed ? amber_r : mint_r), capture_active ? amber_g : (capture_failed ? amber_g : mint_g), capture_active ? amber_b : (capture_failed ? amber_b : mint_b), 1.0f);
    static const char *const filter_names[] = {"NIGHT", "THERMAL", "EDGE", "MONO", "SEPIA", "RAW"};
    char filter_label[32];
    snprintf(filter_label, sizeof(filter_label), "FILTER: %s", filter_names[state->video_filter]);
    text_styled(state, filter_label, second_x + 14, capture.y + capture.h - 25, fit_text_scale_styled(filter_label, ds_sp_to_scale(DS_FONT_SIZE_BADGE), capture_width, DS_FONT_STANDARD), DS_FONT_STANDARD, muted_r, muted_g, muted_b, 1.0f);
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
    const float nav_active_scale = ds_sp_to_scale(DS_FONT_SIZE_BODY_MD);
    const float nav_inactive_scale = ds_sp_to_scale(DS_FONT_SIZE_CAPTION);
    text_styled(state, "SCAN", w * 0.08f, nav_label_y, state->tab == 0 ? nav_active_scale : nav_inactive_scale, state->tab == 0 ? DS_FONT_BOLD : DS_FONT_STANDARD, state->tab == 0 ? mint_r : muted_r, state->tab == 0 ? mint_g : muted_g, state->tab == 0 ? mint_b : muted_b, 1.0f);
    text_styled(state, "LOG", w * 0.32f, nav_label_y, state->tab == 1 ? nav_active_scale : nav_inactive_scale, state->tab == 1 ? DS_FONT_BOLD : DS_FONT_STANDARD, state->tab == 1 ? mint_r : muted_r, state->tab == 1 ? mint_g : muted_g, state->tab == 1 ? mint_b : muted_b, 1.0f);
    text_styled(state, "EXPLORE", w * 0.51f, nav_label_y, state->tab == 2 ? nav_active_scale : nav_inactive_scale, state->tab == 2 ? DS_FONT_BOLD : DS_FONT_STANDARD, state->tab == 2 ? mint_r : muted_r, state->tab == 2 ? mint_g : muted_g, state->tab == 2 ? mint_b : muted_b, 1.0f);
    text_styled(state, "GUIDE", w * 0.81f, nav_label_y, state->tab == 3 ? nav_active_scale : nav_inactive_scale, state->tab == 3 ? DS_FONT_BOLD : DS_FONT_STANDARD, state->tab == 3 ? mint_r : muted_r, state->tab == 3 ? mint_g : muted_g, state->tab == 3 ? mint_b : muted_b, 1.0f);
    text_styled(state, "SKEPTICISM IS A FEATURE", 22, h - 39, ds_sp_to_scale(DS_FONT_SIZE_BADGE), DS_FONT_STANDARD, muted_r, muted_g, muted_b, 1.0f);

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
        if (state->voice_player) ds_voice_player_stop(state->voice_player);
        stop_audio_capture(state);
        ds_video_preview_stop(&state->video);
        state->preview_received_ms = 0;
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
    const DSRect modal = custom_modal(state);
    if (ds_rect_contains((DSRect){modal.x + modal.w - 48, modal.y + 4, 44, 36}, x, y)) {
        state->custom_open = false;
        return;
    }
    const float half = (modal.w - 30) / 2;
    if (ds_rect_contains((DSRect){modal.x + 10, modal.y + 44, half, 38}, x, y)) {
        state->custom_selected = false;
        state->word[0] = 0;
        return;
    }
    if (ds_rect_contains((DSRect){modal.x + 20 + half, modal.y + 44, half, 38}, x, y)) {
        state->custom_selected = true;
        state->word[0] = 0;
        return;
    }
    const float quarter = (modal.w - 44) / 4;
    for (unsigned i = 0; i < 4; ++i) {
        if (ds_rect_contains((DSRect){modal.x + 10 + i * (quarter + 8), modal.y + 86, quarter, 32}, x, y)) {
            state->voice_config.mode = (DSVoiceMode)i;
            return;
        }
    }
    if (state->custom_editing) {
        const char *rows[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
        const float key_height = fminf(42, (modal.h - 180) / 4);
        for (unsigned row = 0; row < 3; ++row) {
            const float key_width = (modal.w - 20) / strlen(rows[row]);
            for (unsigned column = 0; rows[row][column]; ++column)
                if (ds_rect_contains((DSRect){modal.x + 10 + column * key_width, modal.y + 168 + row * key_height,
                    key_width - 2, key_height - 3}, x, y)) { custom_key(state, rows[row][column]); return; }
        }
        const float key_width = (modal.w - 20) / 3;
        for (unsigned i = 0; i < 3; ++i)
            if (ds_rect_contains((DSRect){modal.x + 10 + i * key_width, modal.y + 168 + 3 * key_height,
                key_width - 2, key_height - 3}, x, y)) { custom_key(state, i == 0 ? ' ' : i == 1 ? '\b' : '\n'); return; }
    } else {
        const size_t page_size = (size_t)fmaxf(1, fminf(6, floorf((modal.h - 206) / 42)));
        const size_t count = ds_custom_words_count(&state->custom_words);
        for (size_t row = 0; row < page_size && state->custom_page * page_size + row < count; ++row)
            if (ds_rect_contains((DSRect){modal.x + modal.w - 54, modal.y + 126 + row * 42, 44, 38}, x, y)) {
                if (ds_custom_words_remove(&state->custom_words, state->custom_path, state->custom_page * page_size + row)) {
                    update_custom_blob(state);
                    snprintf(state->custom_message, sizeof(state->custom_message), "WORD REMOVED");
                } else snprintf(state->custom_message, sizeof(state->custom_message), "COULD NOT SAVE CHANGE");
                return;
            }
        if (y >= modal.y + modal.h - 70 && y < modal.y + modal.h - 30) {
            const float middle_w = (modal.w - 180) / 2;
            if (x < modal.x + 58) {
                if (state->custom_page) --state->custom_page;
            } else if (x >= modal.x + 62 && x < modal.x + 62 + middle_w) {
                state->custom_editing = true;
                state->custom_message[0] = 0;
            } else if (x >= modal.x + 62 + middle_w + 6 && x < modal.x + 62 + middle_w * 2 + 6) {
                if (state->voice_player) {
                    ds_voice_player_speak(state->voice_player, "DEADSPEAK ONLINE", &state->voice_config);
                }
            } else if (x > modal.x + modal.w - 58) {
                if (count && state->custom_page < (count - 1) / page_size) ++state->custom_page;
            }
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
        ds_video_start(&state->video, state->evidence_dir, state->video_filter, state->display_rotation);
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
                state->filter_hud_deadline = now_millis() + 1300U;
                return 1;
            } else if (gesture == DS_GESTURE_SWIPE_RIGHT) {
                state->video_filter = (DSVideoFilter)ds_cycle_filter_index((unsigned)state->video_filter, -1, 6);
                state->filter_hud_deadline = now_millis() + 1300U;
                return 1;
            } else if (gesture == DS_GESTURE_SWIPE_UP) {
                state->sensor_details = !state->sensor_details;
                return 1;
            }
        } else {
            if (gesture == DS_GESTURE_SWIPE_LEFT) {
                const int previous_tab = state->tab;
                state->tab = (state->tab + 1) % 4;
                if (previous_tab == 2 && state->explore_search_active) {
                    state->explore_search_active = false;
                    ANativeActivity_hideSoftInput(state->app->activity, ANATIVEACTIVITY_HIDE_SOFT_INPUT_NOT_ALWAYS);
                }
                if (state->tab == 2) start_explore(state);
                return 1;
            } else if (gesture == DS_GESTURE_SWIPE_RIGHT) {
                const int previous_tab = state->tab;
                state->tab = (state->tab + 3) % 4;
                if (previous_tab == 2 && state->explore_search_active) {
                    state->explore_search_active = false;
                    ANativeActivity_hideSoftInput(state->app->activity, ANATIVEACTIVITY_HIDE_SOFT_INPUT_NOT_ALWAYS);
                }
                if (state->tab == 2) start_explore(state);
                return 1;
            }
        }
        return 1;
    }
    if (state->custom_open) { custom_touch(state, x, y); return 1; }
    if (y >= (state->tab == 0 ? state->camera_ui.navigation.y : state->ui.nav_top)) {
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
    if (state->tab == 0) {
        if (ds_rect_contains((DSRect){state->width - 76, 12, 68, 34}, x, y)) {
            state->voice_config.enabled = !state->voice_config.enabled;
            return 1;
        }
        if (ds_rect_contains((DSRect){state->width - 162, 12, 82, 34}, x, y)) {
            const bool geiger_on = ds_voice_player_get_geiger_enabled(state->voice_player);
            ds_voice_player_set_geiger_enabled(state->voice_player, !geiger_on);
            return 1;
        }
        if (ds_rect_contains((DSRect){state->width - 256, 12, 90, 34}, x, y)) {
            state->theme_mode = (state->theme_mode + 1) % 3;
            return 1;
        }
        if (ds_rect_contains((DSRect){state->width - 336, 12, 76, 34}, x, y)) {
            state->hud_hue = fmodf(state->hud_hue + 0.1666f, 1.0f);
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
                state->filter_hud_deadline = now_millis() + 1300U;
            } else if (i == 3) { state->custom_open = true; state->custom_editing = false; state->custom_message[0] = 0; }
            else if (i == 4) state->sensor_details = !state->sensor_details;
            return 1;
        }
        if (x < 140.0f && y >= state->camera_ui.record.y && y <= state->camera_ui.record.y + 50.0f) {
            state->video_filter = (DSVideoFilter)ds_cycle_filter_index((unsigned)state->video_filter, 1, 6);
            state->filter_hud_deadline = now_millis() + 1300U;
            return 1;
        }
        if (state->sensor_details && !state->minimal_hud && ds_rect_contains((DSRect){20, (state->height < 650 ? 76 : 142) + 84, 112, 40}, x, y)) {
            ds_baseline_reset(&state->baseline);
            state->word[0] = 0;
            return 1;
        }
        if (state->sensor_details && !state->minimal_hud && ds_rect_contains((DSRect){138, (state->height < 650 ? 76 : 142) + 84, 96, 40}, x, y)) {
            state->baseline.sensitivity = (DSSensitivity)((state->baseline.sensitivity + 1) % 3);
            const char *sens_names[] = {"SENS: HI", "SENS: MED", "SENS: LO"};
            __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "SENSITIVITY set to %s (threshold=%.1f uT)",
                                sens_names[state->baseline.sensitivity % 3],
                                ds_baseline_threshold(state->baseline.sensitivity));
            return 1;
        }
        const bool recording = ds_video_active(&state->video) || atomic_load(&state->audio_active);
        const float rc_cx = state->camera_ui.record.x + state->camera_ui.record.w * 0.5f;
        const float rc_cy = state->camera_ui.record.y + state->camera_ui.record.h * 0.5f;
        if (recording && ds_rect_contains((DSRect){rc_cx + 36.0f, rc_cy - 16.0f, 56.0f, 32.0f}, x, y)) {
            emit_companion_tag(state, 3);
            snprintf(state->export_status_message, sizeof(state->export_status_message), "BOOKMARK TAG #3 MARKED");
            state->export_status_expiry_ms = now_millis() + 2500U;
            return 1;
        }
        if (ds_rect_contains(state->camera_ui.record, x, y)) {
            toggle_capture(state);
            return 1;
        }
        /* Tap on camera background toggles Minimal HUD */
        if (y > 64.0f && y < state->camera_ui.record.y - 12.0f && x < state->width - 90.0f) {
            state->minimal_hud = !state->minimal_hud;
            return 1;
        }
        return 1;
    }
    if (!ds_rect_contains(state->ui.body, x, y)) return 1;
    y += state->scroll[state->tab];

    if (state->tab == 1) {
        const float column_width = state->ui.wide ? (state->ui.body.w - 16.0f) * 0.5f : state->ui.body.w;
        const float media_x = state->ui.wide ? state->ui.body.x + 16.0f + column_width + 16.0f : state->ui.body.x + 16.0f;
        const float media_y = state->ui.wide ? state->ui.body.y + 22.0f : state->ui.body.y + fmaxf(140.0f, 90.0f + (float)state->log_count * 42.0f);
        const DSRect export_btn = {media_x + column_width - 138.0f, media_y - 6.0f, 106.0f, 30.0f};
        if (ds_rect_contains(export_btn, x, y)) {
            export_session_files(state);
            refresh_media_files(state);
            return 1;
        }

        char curr_playing[96] = {0};
        float cur_prog = 0.0f;
        const bool is_playing_any = ds_voice_player_is_playing_wav(state->voice_player, curr_playing, sizeof(curr_playing), &cur_prog);

        float cur_item_y = media_y + 36.0f;
        for (unsigned i = 0; i < state->media_count; ++i) {
            const char *filename = state->media_files[i];
            const size_t len = strlen(filename);
            const bool is_wav = len > 4 && strcmp(filename + len - 4, ".wav") == 0;
            const bool is_this_playing = is_playing_any && strcmp(curr_playing, filename) == 0;

            if (is_this_playing) {
                const DSRect prev_tag_btn = {media_x + 4.0f, cur_item_y + 42.0f, 64.0f, 26.0f};
                const DSRect next_tag_btn = {media_x + 74.0f, cur_item_y + 42.0f, 64.0f, 26.0f};
                const DSRect stop_btn = {media_x + 144.0f, cur_item_y + 42.0f, 52.0f, 26.0f};
                const DSRect del_btn = {media_x + column_width - 56.0f, cur_item_y + 42.0f, 44.0f, 26.0f};
                const DSRect scrubber = {media_x + 4.0f, cur_item_y + 20.0f, column_width - 36.0f, 16.0f};

                if (ds_rect_contains(prev_tag_btn, x, y)) {
                    jump_to_tag(state, false);
                    return 1;
                }
                if (ds_rect_contains(next_tag_btn, x, y)) {
                    jump_to_tag(state, true);
                    return 1;
                }
                if (ds_rect_contains(stop_btn, x, y)) {
                    ds_voice_player_stop_wav(state->voice_player);
                    return 1;
                }
                if (ds_rect_contains(del_btn, x, y)) {
                    delete_recording_and_companions(state, filename);
                    return 1;
                }
                if (ds_rect_contains(scrubber, x, y)) {
                    const uint64_t dur_ms = ds_voice_player_get_wav_duration_ms(state->voice_player);
                    if (dur_ms > 0 && scrubber.w > 0) {
                        const float pct = fmaxf(0.0f, fminf(1.0f, (x - scrubber.x) / scrubber.w));
                        const uint64_t target_ms = (uint64_t)(pct * (float)dur_ms);
                        ds_voice_player_seek_wav_ms(state->voice_player, target_ms);
                    }
                    return 1;
                }
                cur_item_y += 82.0f;
            } else {
                const DSRect del_btn = {media_x + column_width - 48.0f, cur_item_y + 1.0f, 40.0f, 22.0f};
                if (ds_rect_contains(del_btn, x, y)) {
                    delete_recording_and_companions(state, filename);
                    return 1;
                }
                if (x >= media_x && x <= media_x + column_width - 52.0f && y >= cur_item_y - 4.0f && y <= cur_item_y + 32.0f) {
                    if (is_wav) {
                        char full_path[640];
                        snprintf(full_path, sizeof(full_path), "%s/%s", state->evidence_dir, filename);
                        ds_voice_player_play_wav(state->voice_player, full_path);
                        load_recording_tags(state, filename);
                        return 1;
                    }
                }
                cur_item_y += 36.0f;
            }
        }
    }

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
        state->video_filter = (DSVideoFilter)ds_cycle_filter_index((unsigned)state->video_filter, 1, 6);
        state->filter_hud_deadline = now_millis() + 1300U;
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
            ds_engine_vector_word_entropy(state->baseline.triggered_x, state->baseline.triggered_y, state->baseline.triggered_z,
                                          state->baseline.base_x, state->baseline.base_y, state->baseline.base_z,
                                          state->baseline.kinetic_entropy,
                                          bank, bank_size, state->word, sizeof(state->word));
            if (state->word[0]) {
                append_word_log(state, state->word);
                __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                    "WORD TRIGGERED: %s (field=%.2f uT delta=%.2f uT vector=%.1f,%.1f,%.1f whisper=%d)",
                    state->word, state->smoothed_field, state->baseline.triggered_delta,
                    state->baseline.triggered_x, state->baseline.triggered_y, state->baseline.triggered_z,
                    state->baseline.whisper_modulation);
                if (state->voice_player && state->voice_config.enabled) {
                    DSVoiceConfig active_voice = state->voice_config;
                    const float abs_delta = fabsf(state->baseline.triggered_delta);
                    if (active_voice.mode == DS_VOICE_MODE_DYNAMIC) {
                        /* Dynamic Voice Mode: modulate pitch and speed dynamically by anomaly delta */
                        const float delta_factor = fminf(abs_delta / 40.0f, 1.5f);
                        active_voice.pitch_hz = 115.0f * (1.0f + delta_factor * 0.45f);
                        active_voice.speed = 1.0f + delta_factor * 0.35f;
                        if (state->baseline.whisper_modulation && abs_delta > 10.0f) {
                            active_voice.mode = DS_VOICE_MODE_WHISPER;
                        }
                    } else if (state->baseline.whisper_modulation && active_voice.mode == DS_VOICE_MODE_CLASSIC) {
                        active_voice.mode = DS_VOICE_MODE_WHISPER;
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
    char n_low[128], v_low[128];
    for (size_t i = 0; i < sizeof(n_low) - 1 && name[i]; ++i) n_low[i] = (char)tolower((unsigned char)name[i]);
    n_low[sizeof(n_low) - 1] = '\0';
    for (size_t i = 0; i < sizeof(v_low) - 1 && vendor[i]; ++i) v_low[i] = (char)tolower((unsigned char)vendor[i]);
    v_low[sizeof(v_low) - 1] = '\0';
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
    state->display = EGL_NO_DISPLAY;
    state->surface = EGL_NO_SURFACE;
    state->context = EGL_NO_CONTEXT;
    state->ui_scale = 1.0f;
    state->scanning = true;
    state->camera_enabled = true;
    state->preview_rgba = malloc(DS_PREVIEW_CAPACITY);
    state->video_filter = DS_VIDEO_FILTER_RAW;
    state->word[0] = 0;
    state->voice_config = (DSVoiceConfig){
        .enabled = true,
        .mode = DS_VOICE_MODE_CLASSIC,
        .pitch_hz = 115.0f,
        .speed = 1.0f
    };
    state->voice_player = ds_voice_player_create();
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
                ds_video_start(&state->video, state->evidence_dir, state->video_filter, state->display_rotation);
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
                stop_audio_capture(state);
                ds_video_preview_stop(&state->video);
                stop_sensors(state, DS_SENSOR_PAUSED);
                if (state->sensor_manager && state->sensor_queue)
                    ASensorManager_destroyEventQueue(state->sensor_manager, state->sensor_queue);
                free(state->dictionary);
                free(state->preview_rgba);
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
