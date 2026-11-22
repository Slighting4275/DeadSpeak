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

#ifdef __cplusplus
}
#endif

#endif /* DEADSPEAK_EVP_OPS_H */
