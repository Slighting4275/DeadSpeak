#define _POSIX_C_SOURCE 200809L
#include "evp_ops.h"
#include "typography.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static constexpr float DS_PI_F = 3.14159265358979323846f;

size_t ds_synthesize_cue(DSCueType cue, uint32_t sample_rate, int16_t *out, size_t capacity) {
    if (!out || capacity == 0 || sample_rate < 4000U || sample_rate > 192000U || cue == DS_CUE_NONE) return 0;

    size_t total = 0;
    float f0 = 0.0f, f1 = 0.0f;
    size_t seg1_end = 0;
    bool triple_pulse = false;

    switch (cue) {
        case DS_CUE_START:
            total = (size_t)(sample_rate / 6U); /* ~166ms */
            seg1_end = total / 2U;
            f0 = 523.25f; f1 = 783.99f;
            break;
        case DS_CUE_STOP:
            total = (size_t)(sample_rate / 6U);
            seg1_end = total / 2U;
            f0 = 783.99f; f1 = 392.00f;
            break;
        case DS_CUE_EMF_ALERT:
            total = (size_t)((sample_rate * 18U) / 100U); /* 180ms */
            f0 = 1320.0f; f1 = 1320.0f;
            triple_pulse = true;
            break;
        case DS_CUE_TICK:
            total = (size_t)(sample_rate / 32U); /* ~31ms */
            seg1_end = total;
            f0 = 980.0f; f1 = 980.0f;
            break;
        case DS_CUE_PROX_FADE:
            total = (size_t)((sample_rate * 55U) / 100U); /* 550ms rising/fading spatial swell */
            break;
        default:
            return 0;
    }

    if (total > capacity) total = capacity;
    if (total == 0) return 0;

    const float sr = (float)sample_rate;
    if (cue == DS_CUE_PROX_FADE) {
        double phase = 0.0;
        for (size_t i = 0; i < total; ++i) {
            const float norm = (float)i / (float)total;
            const float env = sinf(DS_PI_F * norm);
            const float freq = 280.0f + 260.0f * sinf(DS_PI_F * norm);
            phase += 2.0 * (double)DS_PI_F * (double)freq / (double)sr;
            const float shimmer = 0.65f * sinf((float)phase) + 0.35f * sinf((float)(phase * 1.5));
            const float sample = 11500.0f * env * env * shimmer;
            out[i] = (int16_t)fmaxf(-32000.0f, fminf(32000.0f, sample));
        }
        return total;
    }
    const size_t attack = sample_rate / 250U;
    const size_t release = sample_rate / 120U;
    for (size_t i = 0; i < total; ++i) {
        float freq = (i < seg1_end) ? f0 : f1;
        float env = 1.0f;
        if (i < attack && attack > 0U) env = (float)i / (float)attack;
        else if (total - i < release && release > 0U) env = (float)(total - i) / (float)release;
        if (triple_pulse) {
            const size_t cycle = total / 3U;
            const size_t pos = cycle > 0U ? (i % cycle) : 0U;
            const size_t active = (cycle * 2U) / 3U;
            if (pos >= active) env = 0.0f;
        }
        const float sample = 11000.0f * env * sinf(2.0f * DS_PI_F * freq * (float)i / sr);
        out[i] = (int16_t)fmaxf(-32000.0f, fminf(32000.0f, sample));
    }
    return total;
}

void ds_evp_filter_init(DSEvpFilterState *st, uint32_t sample_rate) {
    if (!st) return;
    memset(st, 0, sizeof(*st));
    if (sample_rate < 4000U) return;

    const float sr = (float)sample_rate;

    /* 1. 2nd-order Butterworth 80 Hz High-Pass */
    const float w0_hp = 2.0f * DS_PI_F * 80.0f / sr;
    const float cos_hp = cosf(w0_hp);
    const float alpha_hp = sinf(w0_hp) / (2.0f * 0.70710678f);
    const float a0_hp = 1.0f + alpha_hp;
    st->hp_b0 = ((1.0f + cos_hp) * 0.5f) / a0_hp;
    st->hp_b1 = -(1.0f + cos_hp) / a0_hp;
    st->hp_b2 = st->hp_b0;
    st->hp_a1 = (-2.0f * cos_hp) / a0_hp;
    st->hp_a2 = (1.0f - alpha_hp) / a0_hp;

    /* 2. 2nd-order 60 Hz Notch (mains hum rejection) */
    const float w0_n = 2.0f * DS_PI_F * 60.0f / sr;
    const float cos_n = cosf(w0_n);
    const float alpha_n = sinf(w0_n) / (2.0f * 6.0f);
    const float a0_n = 1.0f + alpha_n;
    st->n_b0 = 1.0f / a0_n;
    st->n_b1 = (-2.0f * cos_n) / a0_n;
    st->n_b2 = 1.0f / a0_n;
    st->n_a1 = (-2.0f * cos_n) / a0_n;
    st->n_a2 = (1.0f - alpha_n) / a0_n;

    st->initialized = true;
}

void ds_evp_filter_process(DSEvpFilterState *st, int16_t *samples, size_t count) {
    if (!st || !st->initialized || !samples || count == 0U) return;
    for (size_t i = 0; i < count; ++i) {
        const float x = (float)samples[i];
        const float hp = st->hp_b0 * x + st->hp_b1 * st->hp_x1 + st->hp_b2 * st->hp_x2
                         - st->hp_a1 * st->hp_y1 - st->hp_a2 * st->hp_y2;
        st->hp_x2 = st->hp_x1; st->hp_x1 = x;
        st->hp_y2 = st->hp_y1; st->hp_y1 = hp;

        const float y = st->n_b0 * hp + st->n_b1 * st->n_x1 + st->n_b2 * st->n_x2
                        - st->n_a1 * st->n_y1 - st->n_a2 * st->n_y2;
        st->n_x2 = st->n_x1; st->n_x1 = hp;
        st->n_y2 = st->n_y1; st->n_y1 = y;

        samples[i] = (int16_t)fmaxf(-32767.0f, fminf(32767.0f, y));
    }
}

float ds_clamp_playback_speed(float speed) {
    if (!isfinite(speed)) return 1.0f;
    if (speed < 0.5f) return 0.5f;
    if (speed > 2.0f) return 2.0f;
    return speed;
}

void ds_clamp_loop_range(uint64_t duration_ms, uint64_t *io_start_ms, uint64_t *io_end_ms) {
    if (!io_start_ms || !io_end_ms) return;
    if (duration_ms == 0ULL) { *io_start_ms = 0ULL; *io_end_ms = 0ULL; return; }
    uint64_t s = *io_start_ms;
    uint64_t e = *io_end_ms;
    if (e > duration_ms) e = duration_ms;
    if (s >= e) {
        *io_start_ms = 0ULL;
        *io_end_ms = 0ULL;
    } else {
        *io_start_ms = s;
        *io_end_ms = e;
    }
}

static inline uint8_t clamp_u8(float v) {
    if (v <= 0.0f) return 0U;
    if (v >= 255.0f) return 255U;
    return (uint8_t)lroundf(v);
}

void ds_apply_filter_pixel(uint8_t r, uint8_t g, uint8_t b, DSVideoFilter filter, uint8_t out_rgb[3]) {
    const float fr = (float)r / 255.0f;
    const float fg = (float)g / 255.0f;
    const float fb = (float)b / 255.0f;
    const float luma = fr * 0.299f + fg * 0.587f + fb * 0.114f;

    switch (filter) {
        case DS_VIDEO_FILTER_NIGHT: {
            const float nv = fmaxf(0.0f, fminf(1.0f, luma * 1.55f + 0.08f));
            out_rgb[0] = clamp_u8(nv * 0.12f * 255.0f);
            out_rgb[1] = clamp_u8(nv * 1.08f * 255.0f);
            out_rgb[2] = clamp_u8(nv * 0.22f * 255.0f);
            break;
        }
        case DS_VIDEO_FILTER_THERMAL: {
            const float t = fmaxf(0.0f, fminf(1.0f, luma));
            float tr, tg, tb;
            if (t < 0.25f) {
                const float k = t / 0.25f;
                tr = 0.05f * (1.0f - k);
                tg = 0.20f * k;
                tb = 0.25f + 0.60f * k;
            } else if (t < 0.50f) {
                const float k = (t - 0.25f) / 0.25f;
                tr = 0.85f * k;
                tg = 0.20f - 0.10f * k;
                tb = 0.85f - 0.70f * k;
            } else if (t < 0.75f) {
                const float k = (t - 0.50f) / 0.25f;
                tr = 0.85f + 0.15f * k;
                tg = 0.10f + 0.75f * k;
                tb = 0.15f * (1.0f - k);
            } else {
                const float k = (t - 0.75f) / 0.25f;
                tr = 1.0f;
                tg = 0.85f + 0.15f * k;
                tb = k;
            }
            out_rgb[0] = clamp_u8(tr * 255.0f);
            out_rgb[1] = clamp_u8(tg * 255.0f);
            out_rgb[2] = clamp_u8(tb * 255.0f);
            break;
        }
        case DS_VIDEO_FILTER_MONO: {
            const uint8_t m = clamp_u8((luma - 0.08f) * 1.25f * 255.0f);
            out_rgb[0] = out_rgb[1] = out_rgb[2] = m;
            break;
        }
        case DS_VIDEO_FILTER_SEPIA:
            out_rgb[0] = clamp_u8((fr * 0.393f + fg * 0.769f + fb * 0.189f) * 255.0f);
            out_rgb[1] = clamp_u8((fr * 0.349f + fg * 0.686f + fb * 0.168f) * 255.0f);
            out_rgb[2] = clamp_u8((fr * 0.272f + fg * 0.534f + fb * 0.131f) * 255.0f);
            break;
        case DS_VIDEO_FILTER_EDGE:
        case DS_VIDEO_FILTER_RAW:
        default:
            out_rgb[0] = r; out_rgb[1] = g; out_rgb[2] = b;
            break;
    }
}

static bool has_symlink_at(const char *path) {
    struct stat lst;
    return lstat(path, &lst) == 0 && S_ISLNK(lst.st_mode);
}

static void sample_oriented_rgba(const uint8_t *src, int sw, int sh, int rot, bool mirror_x,
                                 float zoom, int x, int y, int ow, int oh, uint8_t out_px[4]) {
    float u = ((float)x + 0.5f) / (float)ow;
    float v = ((float)y + 0.5f) / (float)oh;
    if (mirror_x) u = 1.0f - u;
    if (zoom > 1.0f) {
        u = 0.5f + (u - 0.5f) / zoom;
        v = 0.5f + (v - 0.5f) / zoom;
    }
    if (u < 0.0f) u = 0.0f; else if (u > 1.0f) u = 1.0f;
    if (v < 0.0f) v = 0.0f; else if (v > 1.0f) v = 1.0f;

    float su, sv;
    switch (rot) {
        case 90:  su = v;        sv = 1.0f - u; break;
        case 180: su = 1.0f - u; sv = 1.0f - v; break;
        case 270: su = 1.0f - v; sv = u;        break;
        default:  su = u;        sv = v;        break;
    }
    int sx = (int)(su * (float)sw);
    int sy = (int)(sv * (float)sh);
    if (sx < 0) sx = 0; else if (sx >= sw) sx = sw - 1;
    if (sy < 0) sy = 0; else if (sy >= sh) sy = sh - 1;
    const uint8_t *p = src + ((size_t)sy * (size_t)sw + (size_t)sx) * 4U;
    out_px[0] = p[0]; out_px[1] = p[1]; out_px[2] = p[2]; out_px[3] = p[3];
}

static float oriented_luma(const uint8_t *src, int sw, int sh, int rot, bool mirror_x,
                           float zoom, int x, int y, int ow, int oh) {
    const int cx = x < 0 ? 0 : (x >= ow ? ow - 1 : x);
    const int cy = y < 0 ? 0 : (y >= oh ? oh - 1 : y);
    uint8_t px[4];
    sample_oriented_rgba(src, sw, sh, rot, mirror_x, zoom, cx, cy, ow, oh, px);
    return ((float)px[0] * 0.299f + (float)px[1] * 0.587f + (float)px[2] * 0.114f) / 255.0f;
}

static inline void put_le16(uint8_t *dst, uint16_t v) {
    dst[0] = (uint8_t)(v & 0xffU); dst[1] = (uint8_t)((v >> 8) & 0xffU);
}
static inline void put_le32(uint8_t *dst, uint32_t v) {
    dst[0] = (uint8_t)(v & 0xffU);
    dst[1] = (uint8_t)((v >> 8) & 0xffU);
    dst[2] = (uint8_t)((v >> 16) & 0xffU);
    dst[3] = (uint8_t)((v >> 24) & 0xffU);
}
static inline uint16_t get_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void compute_sobel_rgb(float c0, float c1, float c2, float c3, float c5,
                              float c6, float c7, float c8, uint8_t out_rgb[3]) {
    const float gx = -c0 + c2 - 2.0f * c3 + 2.0f * c5 - c6 + c8;
    const float gy = -c0 - 2.0f * c1 - c2 + c6 + 2.0f * c7 + c8;
    const float edge = fminf(1.0f, sqrtf(gx * gx + gy * gy) * 3.2f);
    out_rgb[0] = clamp_u8(edge * 0.1f * 255.0f);
    out_rgb[1] = clamp_u8(edge * 255.0f);
    out_rgb[2] = clamp_u8(edge * 0.75f * 255.0f);
}

bool ds_write_filtered_bmp(const char *path, const uint8_t *src_rgba,
                           int src_w, int src_h, int rotation, bool mirror_x,
                           float zoom, DSVideoFilter filter) {
    if (!path || !path[0] || !src_rgba || src_w <= 0 || src_h <= 0 || src_w > 4096 || src_h > 4096) return false;
    if (has_symlink_at(path)) return false;

    const int norm_rot = ((rotation % 360) + 360) % 360;
    const int ow = (norm_rot == 90 || norm_rot == 270) ? src_h : src_w;
    const int oh = (norm_rot == 90 || norm_rot == 270) ? src_w : src_h;
    if (!isfinite(zoom) || zoom < 1.0f) zoom = 1.0f;
    if (zoom > 4.0f) zoom = 4.0f;

    const size_t row_bytes = (size_t)ow * 3U;
    const size_t row_stride = (row_bytes + 3U) & ~((size_t)3U);
    const size_t pixel_bytes = row_stride * (size_t)oh;
    if (pixel_bytes > 64U * 1024U * 1024U) return false;

    uint8_t *row_buf = malloc(row_stride);
    if (!row_buf) return false;

    char tmp_path[640];
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld", path, (long)getpid()) >= (int)sizeof(tmp_path) ||
        has_symlink_at(tmp_path)) {
        free(row_buf);
        return false;
    }
    unlink(tmp_path);
    const int fd = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        free(row_buf);
        return false;
    }

    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    put_le32(hdr + 2, (uint32_t)(54U + pixel_bytes));
    put_le32(hdr + 10, 54U);
    put_le32(hdr + 14, 40U);
    put_le32(hdr + 18, (uint32_t)ow);
    put_le32(hdr + 22, (uint32_t)oh);
    put_le16(hdr + 26, 1U);
    put_le16(hdr + 28, 24U);
    put_le32(hdr + 34, (uint32_t)pixel_bytes);

    bool ok = write(fd, hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr);
    for (int y = oh - 1; ok && y >= 0; --y) {
        memset(row_buf, 0, row_stride);
        for (int x = 0; x < ow; ++x) {
            uint8_t rgb[3];
            if (filter == DS_VIDEO_FILTER_EDGE) {
                const float c0 = oriented_luma(src_rgba, src_w, src_h, norm_rot, mirror_x, zoom, x - 1, y - 1, ow, oh);
                const float c1 = oriented_luma(src_rgba, src_w, src_h, norm_rot, mirror_x, zoom, x, y - 1, ow, oh);
                const float c2 = oriented_luma(src_rgba, src_w, src_h, norm_rot, mirror_x, zoom, x + 1, y - 1, ow, oh);
                const float c3 = oriented_luma(src_rgba, src_w, src_h, norm_rot, mirror_x, zoom, x - 1, y, ow, oh);
                const float c5 = oriented_luma(src_rgba, src_w, src_h, norm_rot, mirror_x, zoom, x + 1, y, ow, oh);
                const float c6 = oriented_luma(src_rgba, src_w, src_h, norm_rot, mirror_x, zoom, x - 1, y + 1, ow, oh);
                const float c7 = oriented_luma(src_rgba, src_w, src_h, norm_rot, mirror_x, zoom, x, y + 1, ow, oh);
                const float c8 = oriented_luma(src_rgba, src_w, src_h, norm_rot, mirror_x, zoom, x + 1, y + 1, ow, oh);
                compute_sobel_rgb(c0, c1, c2, c3, c5, c6, c7, c8, rgb);
            } else {
                uint8_t px[4];
                sample_oriented_rgba(src_rgba, src_w, src_h, norm_rot, mirror_x, zoom, x, y, ow, oh, px);
                ds_apply_filter_pixel(px[0], px[1], px[2], filter, rgb);
            }
            /* BMP stores pixels in BGR byte order */
            row_buf[(size_t)x * 3U + 0U] = rgb[2];
            row_buf[(size_t)x * 3U + 1U] = rgb[1];
            row_buf[(size_t)x * 3U + 2U] = rgb[0];
        }
        if (write(fd, row_buf, row_stride) != (ssize_t)row_stride) ok = false;
    }

    free(row_buf);
    if (ok) ok = fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    if (!ok || has_symlink_at(path) || rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return false;
    }
    return true;
}

bool ds_load_bmp_rgba(const char *path, uint8_t *out_rgba, size_t capacity, int *out_w, int *out_h) {
    if (!path || !path[0] || !out_rgba || capacity < 4U || !out_w || !out_h) return false;
    const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 54) {
        close(fd);
        return false;
    }
    FILE *f = fdopen(fd, "rb");
    if (!f) { close(fd); return false; }

    uint8_t hdr[54];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) || hdr[0] != 'B' || hdr[1] != 'M') {
        fclose(f); return false;
    }
    const uint32_t pixel_offset = get_le32(hdr + 10);
    const uint32_t dib_size = get_le32(hdr + 14);
    const int32_t w = (int32_t)get_le32(hdr + 18);
    const int32_t raw_h = (int32_t)get_le32(hdr + 22);
    const uint16_t bpp = get_le16(hdr + 28);
    const uint32_t compression = get_le32(hdr + 30);
    const bool top_down = raw_h < 0;
    if (dib_size < 40U || pixel_offset < 54U || w <= 0 || raw_h == 0 || raw_h == INT32_MIN || w > 4096) {
        fclose(f); return false;
    }
    const int32_t h = top_down ? -raw_h : raw_h;
    if (h <= 0 || h > 4096 || compression != 0U || (bpp != 24U && bpp != 32U)) {
        fclose(f); return false;
    }
    const size_t total_rgba = (size_t)w * (size_t)h * 4U;
    if (total_rgba > capacity || fseek(f, (long)pixel_offset, SEEK_SET) != 0) {
        fclose(f); return false;
    }

    const size_t src_bpp = bpp / 8U;
    const size_t row_stride = ((size_t)w * src_bpp + 3U) & ~((size_t)3U);
    uint8_t *row_buf = malloc(row_stride);
    if (!row_buf) { fclose(f); return false; }

    bool ok = true;
    for (int32_t r = 0; r < h; ++r) {
        if (fread(row_buf, 1, row_stride, f) != row_stride) { ok = false; break; }
        const int32_t dst_y = top_down ? r : (h - 1 - r);
        uint8_t *dst = out_rgba + (size_t)dst_y * (size_t)w * 4U;
        for (int32_t x = 0; x < w; ++x) {
            const uint8_t *sp = row_buf + (size_t)x * src_bpp;
            dst[(size_t)x * 4U + 0U] = sp[2];
            dst[(size_t)x * 4U + 1U] = sp[1];
            dst[(size_t)x * 4U + 2U] = sp[0];
            dst[(size_t)x * 4U + 3U] = 255U;
        }
    }
    free(row_buf);
    fclose(f);
    if (!ok) return false;
    *out_w = (int)w;
    *out_h = (int)h;
    return true;
}

bool ds_append_tag_entry(const char *tags_path, uint64_t timestamp_ms, uint8_t tag_id) {
    if (!tags_path || !tags_path[0] || has_symlink_at(tags_path)) return false;
    const int fd = open(tags_path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    char line[48];
    const int len = snprintf(line, sizeof(line), "%llu,%u\n",
                             (unsigned long long)timestamp_ms, (unsigned)tag_id);
    bool ok = len > 0 && write(fd, line, (size_t)len) == (ssize_t)len;
    if (ok) ok = fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    return ok;
}

static FILE *open_regular_read(const char *path) {
    if (!path || !path[0]) return NULL;
    const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return NULL;
    }
    FILE *f = fdopen(fd, "r");
    if (!f) close(fd);
    return f;
}

size_t ds_parse_tags_file(const char *path, DSTimelineMarker *out, size_t max_count) {
    if (!out || max_count == 0U) return 0U;
    FILE *f = open_regular_read(path);
    if (!f) return 0U;
    size_t count = 0U;
    char line[128];
    while (count < max_count && fgets(line, sizeof(line), f)) {
        char *comma = strchr(line, ',');
        if (!comma) continue;
        *comma = '\0';
        const uint64_t ms = (uint64_t)strtoull(line, NULL, 10);
        const long id_val = strtol(comma + 1, NULL, 10);
        const uint8_t id = (id_val >= 0 && id_val <= 255) ? (uint8_t)id_val : 0U;
        out[count++] = (DSTimelineMarker){ .timestamp_ms = ms, .tag_id = id };
    }
    fclose(f);
    return count;
}

size_t ds_resample_waveform_file(const char *path, float *out_bars, size_t max_bars, uint64_t *out_duration_ms) {
    if (out_duration_ms) *out_duration_ms = 0ULL;
    if (!out_bars || max_bars == 0U) return 0U;
    FILE *wf = open_regular_read(path);
    if (!wf) return 0U;
    size_t total = 0U;
    uint64_t max_ms = 0ULL;
    char line[128];
    while (fgets(line, sizeof(line), wf)) {
        char *comma = strchr(line, ',');
        if (!comma) continue;
        const uint64_t ms = (uint64_t)strtoull(line, NULL, 10);
        if (ms > max_ms) max_ms = ms;
        ++total;
    }
    if (total == 0U) {
        fclose(wf);
        return 0U;
    }
    if (fseek(wf, 0L, SEEK_SET) != 0) {
        fclose(wf);
        return 0U;
    }
    const size_t bars = total < max_bars ? total : max_bars;
    memset(out_bars, 0, bars * sizeof(float));
    size_t idx = 0U;
    while (fgets(line, sizeof(line), wf)) {
        const char *comma = strchr(line, ',');
        if (!comma) continue;
        const float amp = strtof(comma + 1, NULL);
        const size_t bucket = (idx * bars) / total;
        if (bucket < bars && isfinite(amp) && amp > out_bars[bucket]) {
            out_bars[bucket] = amp;
        }
        ++idx;
    }
    fclose(wf);
    if (out_duration_ms) *out_duration_ms = max_ms;
    return bars;
}

int ds_find_nearest_marker(const DSTimelineMarker *markers, size_t count, uint64_t target_ms, uint64_t tolerance_ms) {
    if (!markers || count == 0U) return -1;
    int best_idx = -1;
    uint64_t best_diff = UINT64_MAX;
    for (size_t i = 0U; i < count; ++i) {
        const uint64_t t = markers[i].timestamp_ms;
        const uint64_t diff = t >= target_ms ? (t - target_ms) : (target_ms - t);
        if (diff <= tolerance_ms && diff < best_diff) {
            best_diff = diff;
            best_idx = (int)i;
        }
    }
    return best_idx;
}

uint64_t ds_scrub_fraction_to_ms(float frac, uint64_t duration_ms) {
    if (duration_ms == 0ULL || !isfinite(frac) || frac <= 0.0f) return 0ULL;
    if (frac >= 1.0f) return duration_ms;
    return (uint64_t)((double)frac * (double)duration_ms + 0.5);
}

void ds_eq_meter_update(DSEqMeterState *eq, const float raw_bands[8], float dt_ms) {
    if (!eq || !raw_bands) return;
    if (!isfinite(dt_ms) || dt_ms <= 0.0f) dt_ms = 16.6f;
    else if (dt_ms > 1000.0f) dt_ms = 1000.0f;

    const float scale = dt_ms / 16.67f;
    const float attack = fminf(0.85f, fmaxf(0.35f, 0.50f * scale));
    const float release = fminf(0.45f, fmaxf(0.08f, 0.14f * scale));

    for (size_t i = 0U; i < 8U; ++i) {
        float target = isfinite(raw_bands[i]) ? raw_bands[i] : 0.0f;
        if (target < 0.0f) target = 0.0f; else if (target > 1.0f) target = 1.0f;
        const float k = (target > eq->levels[i]) ? attack : release;
        eq->levels[i] += (target - eq->levels[i]) * k;
        if (eq->levels[i] >= eq->peaks[i]) {
            eq->peaks[i] = eq->levels[i];
            eq->peak_hold_ms[i] = 450.0f;
        } else if (eq->peak_hold_ms[i] > dt_ms) {
            eq->peak_hold_ms[i] -= dt_ms;
        } else {
            const float decay_ms = dt_ms - eq->peak_hold_ms[i];
            eq->peak_hold_ms[i] = 0.0f;
            eq->peaks[i] = fmaxf(eq->levels[i], eq->peaks[i] - decay_ms * 0.0012f);
        }
    }
}

DSSnapEditParams ds_snap_edit_defaults(void) {
    return (DSSnapEditParams){
        .zoom = 1.0f,
        .pan_x = 0.0f,
        .pan_y = 0.0f,
        .brightness = 0.0f,
        .contrast = 1.0f,
        .filter = DS_VIDEO_FILTER_RAW,
        .crop = DS_CROP_FULL,
        .stamp_telemetry = false,
        .stamp_text = {0},
    };
}

void ds_downsample_spectrum_16(const uint32_t *bins, size_t bin_count, float out16[16]) {
    if (!out16) return;
    for (size_t i = 0U; i < 16U; ++i) out16[i] = 0.0f;
    if (!bins || bin_count == 0U) return;
    for (size_t i = 0U; i < 16U; ++i) {
        const size_t start = (i * bin_count) / 16U;
        size_t end = ((i + 1U) * bin_count) / 16U;
        if (end <= start) end = start + 1U;
        if (end > bin_count) end = bin_count;
        float peak = 0.0f;
        for (size_t b = start; b < end; ++b) {
            const float v = fmaxf(0.0f, fminf(1.0f, (float)bins[b] / 1000.0f));
            if (v > peak) peak = v;
        }
        out16[i] = peak;
    }
}

void ds_snap_crop_dimensions(int src_w, int src_h, DSSnapCropPreset crop, int *out_w, int *out_h) {
    if (!out_w || !out_h) return;
    if (src_w <= 0 || src_h <= 0) {
        *out_w = 0;
        *out_h = 0;
        return;
    }
    if (crop == DS_CROP_FULL || (unsigned)crop >= DS_CROP_COUNT) {
        *out_w = src_w;
        *out_h = src_h;
        return;
    }
    const float target = (crop == DS_CROP_SQUARE_1_1) ? 1.0f :
                         (crop == DS_CROP_PORTRAIT_4_5) ? 0.8f : (16.0f / 9.0f);
    const float src_aspect = (float)src_w / (float)src_h;
    if (src_aspect > target) {
        *out_h = src_h;
        *out_w = (int)lroundf((float)src_h * target);
    } else {
        *out_w = src_w;
        *out_h = (int)lroundf((float)src_w / target);
    }
    if (*out_w < 1) *out_w = 1;
    if (*out_h < 1) *out_h = 1;
    if (*out_w > src_w) *out_w = src_w;
    if (*out_h > src_h) *out_h = src_h;
}

void ds_snap_edit_clamp(DSSnapEditParams *p) {
    if (!p) return;
    if (!isfinite(p->zoom) || p->zoom < 1.0f) p->zoom = 1.0f;
    else if (p->zoom > 4.0f) p->zoom = 4.0f;

    const float max_pan = (p->zoom - 1.0f) / (2.0f * p->zoom);
    if (!isfinite(p->pan_x)) p->pan_x = 0.0f;
    else if (p->pan_x < -max_pan) p->pan_x = -max_pan;
    else if (p->pan_x > max_pan) p->pan_x = max_pan;

    if (!isfinite(p->pan_y)) p->pan_y = 0.0f;
    else if (p->pan_y < -max_pan) p->pan_y = -max_pan;
    else if (p->pan_y > max_pan) p->pan_y = max_pan;

    if (!isfinite(p->brightness)) p->brightness = 0.0f;
    else if (p->brightness < -0.5f) p->brightness = -0.5f;
    else if (p->brightness > 0.5f) p->brightness = 0.5f;

    if (!isfinite(p->contrast)) p->contrast = 1.0f;
    else if (p->contrast < 0.5f) p->contrast = 0.5f;
    else if (p->contrast > 2.0f) p->contrast = 2.0f;

    if ((int)p->filter < DS_VIDEO_FILTER_NIGHT || (int)p->filter > DS_VIDEO_FILTER_RAW) {
        p->filter = DS_VIDEO_FILTER_RAW;
    }
    if ((int)p->crop < DS_CROP_FULL || (int)p->crop >= DS_CROP_COUNT) {
        p->crop = DS_CROP_FULL;
    }
    p->stamp_text[sizeof(p->stamp_text) - 1U] = '\0';
}

bool ds_snap_compute_crop_uv(int img_w, int img_h, const DSSnapEditParams *p,
                             float uv[12], int *out_w, int *out_h) {
    if (!uv || img_w <= 0 || img_h <= 0) return false;
    DSSnapEditParams ed = p ? *p : ds_snap_edit_defaults();
    ds_snap_edit_clamp(&ed);
    int cw = img_w, ch = img_h;
    ds_snap_crop_dimensions(img_w, img_h, ed.crop, &cw, &ch);
    if (out_w) *out_w = cw;
    if (out_h) *out_h = ch;
    const float span_u = ((float)cw / (float)img_w) / ed.zoom;
    const float span_v = ((float)ch / (float)img_h) / ed.zoom;
    const float u0 = fmaxf(0.0f, fminf(1.0f - span_u, 0.5f + ed.pan_x - span_u * 0.5f));
    const float v0 = fmaxf(0.0f, fminf(1.0f - span_v, 0.5f + ed.pan_y - span_v * 0.5f));
    const float u1 = u0 + span_u;
    const float v1 = v0 + span_v;
    const float coords[12] = {u0, v0, u1, v0, u0, v1, u0, v1, u1, v0, u1, v1};
    memcpy(uv, coords, sizeof(coords));
    return true;
}

bool ds_snap_compute_uv(const DSSnapEditParams *p, float uv[12]) {
    if (!uv) return false;
    DSSnapEditParams ed = p ? *p : ds_snap_edit_defaults();
    ds_snap_edit_clamp(&ed);
    const float span = 1.0f / ed.zoom;
    const float u0 = fmaxf(0.0f, fminf(1.0f - span, 0.5f + ed.pan_x - span * 0.5f));
    const float v0 = fmaxf(0.0f, fminf(1.0f - span, 0.5f + ed.pan_y - span * 0.5f));
    const float u1 = u0 + span;
    const float v1 = v0 + span;
    const float coords[12] = {u0, v0, u1, v0, u0, v1, u0, v1, u1, v0, u1, v1};
    memcpy(uv, coords, sizeof(coords));
    return true;
}

static inline uint8_t adjust_channel_brightness_contrast(uint8_t c, float brightness, float contrast) {
    const float f = ((float)c / 255.0f - 0.5f) * contrast + 0.5f + brightness;
    return clamp_u8(f * 255.0f);
}

static float sample_adjusted_luma(const uint8_t *src, int w, int h, int x, int y, float brightness, float contrast) {
    const int cx = x < 0 ? 0 : (x >= w ? w - 1 : x);
    const int cy = y < 0 ? 0 : (y >= h ? h - 1 : y);
    const uint8_t *sp = src + ((size_t)cy * (size_t)w + (size_t)cx) * 4U;
    const float r = (float)adjust_channel_brightness_contrast(sp[0], brightness, contrast) / 255.0f;
    const float g = (float)adjust_channel_brightness_contrast(sp[1], brightness, contrast) / 255.0f;
    const float b = (float)adjust_channel_brightness_contrast(sp[2], brightness, contrast) / 255.0f;
    return r * 0.299f + g * 0.587f + b * 0.114f;
}

bool ds_apply_snap_edit_rgba(const uint8_t *src_rgba, uint8_t *dst_rgba, int w, int h, const DSSnapEditParams *p) {
    if (!src_rgba || !dst_rgba || w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;
    DSSnapEditParams ed = p ? *p : ds_snap_edit_defaults();
    ds_snap_edit_clamp(&ed);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t *dp = dst_rgba + ((size_t)y * (size_t)w + (size_t)x) * 4U;
            if (ed.filter == DS_VIDEO_FILTER_EDGE) {
                const float c0 = sample_adjusted_luma(src_rgba, w, h, x - 1, y - 1, ed.brightness, ed.contrast);
                const float c1 = sample_adjusted_luma(src_rgba, w, h, x, y - 1, ed.brightness, ed.contrast);
                const float c2 = sample_adjusted_luma(src_rgba, w, h, x + 1, y - 1, ed.brightness, ed.contrast);
                const float c3 = sample_adjusted_luma(src_rgba, w, h, x - 1, y, ed.brightness, ed.contrast);
                const float c5 = sample_adjusted_luma(src_rgba, w, h, x + 1, y, ed.brightness, ed.contrast);
                const float c6 = sample_adjusted_luma(src_rgba, w, h, x - 1, y + 1, ed.brightness, ed.contrast);
                const float c7 = sample_adjusted_luma(src_rgba, w, h, x, y + 1, ed.brightness, ed.contrast);
                const float c8 = sample_adjusted_luma(src_rgba, w, h, x + 1, y + 1, ed.brightness, ed.contrast);
                compute_sobel_rgb(c0, c1, c2, c3, c5, c6, c7, c8, dp);
            } else {
                const uint8_t *sp = src_rgba + ((size_t)y * (size_t)w + (size_t)x) * 4U;
                const uint8_t br = adjust_channel_brightness_contrast(sp[0], ed.brightness, ed.contrast);
                const uint8_t bg = adjust_channel_brightness_contrast(sp[1], ed.brightness, ed.contrast);
                const uint8_t bb = adjust_channel_brightness_contrast(sp[2], ed.brightness, ed.contrast);
                ds_apply_filter_pixel(br, bg, bb, ed.filter, dp);
            }
            dp[3] = 255U;
        }
    }
    return true;
}

static void stamp_rgba_text(uint8_t *rgba, int w, int h, const char *text) {
    if (!rgba || w < 16 || h < 12 || !text || !text[0]) return;
    const int scale = (w >= 320 && h >= 200) ? 2 : 1;
    const int bar_h = 9 * scale + 4;
    if (bar_h > h) return;
    const int y0 = h - bar_h;
    for (int y = y0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t *p = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4U;
            p[0] = (uint8_t)(p[0] / 3U);
            p[1] = (uint8_t)(p[1] / 3U);
            p[2] = (uint8_t)(p[2] / 3U);
        }
    }
    int px = 4 * scale;
    const int py = y0 + 2 * scale;
    for (size_t i = 0U; text[i] && px + (int)DS_FONT_GLYPH_COLS * scale < w - 2; ++i) {
        const int gi = ds_glyph_index(text[i]);
        if (gi >= 0) {
            for (size_t r = 0U; r < DS_FONT_GLYPH_ROWS; ++r) {
                const uint8_t bits = ds_font_bitmap[gi][r];
                for (size_t c = 0U; c < DS_FONT_GLYPH_COLS; ++c) {
                    if (!((bits >> (4U - c)) & 1U)) continue;
                    for (int sy = 0; sy < scale; ++sy) {
                        for (int sx = 0; sx < scale; ++sx) {
                            const int dx = px + (int)c * scale + sx;
                            const int dy = py + (int)r * scale + sy;
                            if (dx >= 0 && dx < w && dy >= 0 && dy < h) {
                                uint8_t *p = rgba + ((size_t)dy * (size_t)w + (size_t)dx) * 4U;
                                p[0] = 210U; p[1] = 250U; p[2] = 225U; p[3] = 255U;
                            }
                        }
                    }
                }
            }
        }
        px += 6 * scale;
    }
}

bool ds_edit_and_save_bmp(const char *src_path, const char *dst_path, const DSSnapEditParams *p) {
    if (!src_path || !dst_path || !dst_path[0]) return false;
    const size_t cap = 1280U * 720U * 4U;
    uint8_t *src = malloc(cap);
    uint8_t *dst = malloc(cap);
    if (!src || !dst) {
        free(src);
        free(dst);
        return false;
    }
    int w = 0, h = 0;
    int ow = 0, oh = 0;
    bool ok = ds_load_bmp_rgba(src_path, src, cap, &w, &h);
    if (ok) ok = ds_apply_snap_edit_rgba(src, dst, w, h, p);
    DSSnapEditParams ed = p ? *p : ds_snap_edit_defaults();
    ds_snap_edit_clamp(&ed);
    ow = w; oh = h;
    float crop_uv[12];
    if (ok && (ed.zoom > 1.01f || ed.crop != DS_CROP_FULL) && w > 1 && h > 1 &&
        ds_snap_compute_crop_uv(w, h, &ed, crop_uv, &ow, &oh)) {
        const float u0 = crop_uv[0], v0 = crop_uv[1];
        const float span_u = crop_uv[2] - crop_uv[0];
        const float span_v = crop_uv[5] - crop_uv[1];
        uint8_t *cropped = src;
        for (int y = 0; y < oh; ++y) {
            const float sv = v0 + ((float)y + 0.5f) / (float)oh * span_v;
            const float fy = fmaxf(0.0f, fminf((float)(h - 1), sv * (float)(h - 1)));
            const int y0 = (int)fy;
            const int y1 = (y0 + 1 < h) ? (y0 + 1) : y0;
            const float ty = fy - (float)y0;
            for (int x = 0; x < ow; ++x) {
                const float su = u0 + ((float)x + 0.5f) / (float)ow * span_u;
                const float fx = fmaxf(0.0f, fminf((float)(w - 1), su * (float)(w - 1)));
                const int x0 = (int)fx;
                const int x1 = (x0 + 1 < w) ? (x0 + 1) : x0;
                const float tx = fx - (float)x0;
                const uint8_t *p00 = dst + ((size_t)y0 * (size_t)w + (size_t)x0) * 4U;
                const uint8_t *p10 = dst + ((size_t)y0 * (size_t)w + (size_t)x1) * 4U;
                const uint8_t *p01 = dst + ((size_t)y1 * (size_t)w + (size_t)x0) * 4U;
                const uint8_t *p11 = dst + ((size_t)y1 * (size_t)w + (size_t)x1) * 4U;
                uint8_t *out_p = cropped + ((size_t)y * (size_t)ow + (size_t)x) * 4U;
                for (int c = 0; c < 3; ++c) {
                    const float top = (float)p00[c] * (1.0f - tx) + (float)p10[c] * tx;
                    const float bot = (float)p01[c] * (1.0f - tx) + (float)p11[c] * tx;
                    out_p[c] = clamp_u8(top * (1.0f - ty) + bot * ty);
                }
                out_p[3] = 255U;
            }
        }
        memcpy(dst, cropped, (size_t)ow * (size_t)oh * 4U);
    }
    if (ok && ed.stamp_telemetry && ed.stamp_text[0]) {
        stamp_rgba_text(dst, ow, oh, ed.stamp_text);
    }
    if (ok) ok = ds_write_filtered_bmp(dst_path, dst, ow, oh, 0, false, 1.0f, DS_VIDEO_FILTER_RAW);
    free(src);
    free(dst);
    return ok;
}
