#ifndef DEADSPEAK_EVP_OPS_H
#define DEADSPEAK_EVP_OPS_H

#include "video_capture.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DS_CUE_NONE = 0,
    DS_CUE_START = 1,
    DS_CUE_STOP = 2,
    DS_CUE_EMF_ALERT = 3,
    DS_CUE_TICK = 4,
} DSCueType;

/* Synthesizes a pure 16-bit mono PCM hardware feedback chime/tick.
 * Returns number of samples written (<= capacity). */
size_t ds_synthesize_cue(DSCueType cue, uint32_t sample_rate, int16_t *out, size_t capacity);

/* 2nd-order Butterworth 80 Hz High-Pass + 60 Hz Notch IIR filter for EVP hum/rumble reduction. */
typedef struct {
    float hp_b0, hp_b1, hp_b2, hp_a1, hp_a2;
    float hp_x1, hp_x2, hp_y1, hp_y2;
    float n_b0, n_b1, n_b2, n_a1, n_a2;
    float n_x1, n_x2, n_y1, n_y2;
    bool initialized;
} DSEvpFilterState;

void ds_evp_filter_init(DSEvpFilterState *st, uint32_t sample_rate);
void ds_evp_filter_process(DSEvpFilterState *st, int16_t *samples, size_t count);

/* Clamps A-B loop points and LOG playback speed [0.5, 2.0]. */
float ds_clamp_playback_speed(float speed);
void ds_clamp_loop_range(uint64_t duration_ms, uint64_t *io_start_ms, uint64_t *io_end_ms);

/* Applies DeadSpeak camera filter to a single RGBA pixel. */
void ds_apply_filter_pixel(uint8_t r, uint8_t g, uint8_t b, DSVideoFilter filter, uint8_t out_rgb[3]);

/* Transforms raw preview RGBA buffer into an upright, zoomed, optionally mirrored, filtered
 * 24-bit BMP snapshot saved atomically (refusing symlinks, O_NOFOLLOW|O_EXCL + fsync). */
bool ds_write_filtered_bmp(const char *path, const uint8_t *src_rgba,
                           int src_w, int src_h, int rotation, bool mirror_x,
                           float zoom, DSVideoFilter filter);

/* Loads a 24-bit or 32-bit uncompressed BMP file into an RGBA buffer (top-down order). */
bool ds_load_bmp_rgba(const char *path, uint8_t *out_rgba, size_t capacity, int *out_w, int *out_h);

/* Appends a timestamped tag line ("<ms>,<tag_id>\n") safely to a _tags.txt file. */
bool ds_append_tag_entry(const char *tags_path, uint64_t timestamp_ms, uint8_t tag_id);

/* Parsed timeline tag entry & waveform resampling helpers for LOG scrubbing */
typedef struct {
    uint64_t timestamp_ms;
    uint8_t tag_id;
} DSTimelineMarker;

size_t ds_parse_tags_file(const char *path, DSTimelineMarker *out, size_t max_count);
size_t ds_resample_waveform_file(const char *path, float *out_bars, size_t max_bars, uint64_t *out_duration_ms);
int ds_find_nearest_marker(const DSTimelineMarker *markers, size_t count, uint64_t target_ms, uint64_t tolerance_ms);
uint64_t ds_scrub_fraction_to_ms(float frac, uint64_t duration_ms);

/* 8-band EQ level smoother with peak-hold decay for FFT equalizer mode */
typedef struct {
    float levels[8];
    float peaks[8];
    float peak_hold_ms[8];
} DSEqMeterState;

void ds_eq_meter_update(DSEqMeterState *eq, const float raw_bands[8], float dt_ms);

/* Post-capture BMP snapshot zoom/pan & brightness/contrast/filter editor */
typedef struct {
    float zoom;
    float pan_x;
    float pan_y;
    float brightness;
    float contrast;
    DSVideoFilter filter;
} DSSnapEditParams;

DSSnapEditParams ds_snap_edit_defaults(void);
void ds_snap_edit_clamp(DSSnapEditParams *p);
bool ds_snap_compute_uv(const DSSnapEditParams *p, float uv[12]);
bool ds_apply_snap_edit_rgba(const uint8_t *src_rgba, uint8_t *dst_rgba, int w, int h, const DSSnapEditParams *p);
bool ds_edit_and_save_bmp(const char *src_path, const char *dst_path, const DSSnapEditParams *p);

#ifdef __cplusplus
}
#endif

#endif /* DEADSPEAK_EVP_OPS_H */
