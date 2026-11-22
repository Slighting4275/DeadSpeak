#define _POSIX_C_SOURCE 200809L
#include "evp_ops.h"
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
        default:
            return 0;
    }

    if (total > capacity) total = capacity;
    if (total == 0) return 0;

    const float sr = (float)sample_rate;
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
                const float gx = -c0 + c2 - 2.0f * c3 + 2.0f * c5 - c6 + c8;
                const float gy = -c0 - 2.0f * c1 - c2 + c6 + 2.0f * c7 + c8;
                const float edge = fminf(1.0f, sqrtf(gx * gx + gy * gy) * 3.2f);
                rgb[0] = clamp_u8(edge * 0.1f * 255.0f);
                rgb[1] = clamp_u8(edge * 255.0f);
                rgb[2] = clamp_u8(edge * 0.75f * 255.0f);
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
