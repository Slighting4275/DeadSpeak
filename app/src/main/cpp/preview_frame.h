#ifndef DEADSPEAK_PREVIEW_FRAME_H
#define DEADSPEAK_PREVIEW_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DS_PREVIEW_MAX_WIDTH 640
#define DS_PREVIEW_MAX_HEIGHT 480
#define DS_PREVIEW_MAX_BYTES ((size_t)DS_PREVIEW_MAX_WIDTH * DS_PREVIEW_MAX_HEIGHT * 4U)
#define DS_PREVIEW_FRESH_NS INT64_C(500000000)

typedef struct {
    const uint8_t *data;
    size_t length;
    int row_stride;
    int pixel_stride;
} DSPreviewPlane;

typedef struct {
    int width, height;
    int left, top, right, bottom; /* Exclusive right/bottom, full-resolution crop. */
    bool bt709, full_range;
    DSPreviewPlane planes[3]; /* Android YUV_420_888 order: Y, U, V. */
} DSPreviewYuv;

static inline bool ds_preview_fresh(int64_t received_ns, int64_t now_ns) {
    return received_ns > 0 && now_ns >= received_ns &&
           now_ns - received_ns <= DS_PREVIEW_FRESH_NS;
}

static inline bool ds_preview_plane_valid(const DSPreviewPlane *p, int x, int y) {
    if (!p->data || p->row_stride <= 0 || p->pixel_stride <= 0 || x < 0 || y < 0)
        return false;
    const uint64_t row_end = (uint64_t)(unsigned)x * (unsigned)p->pixel_stride;
    const uint64_t index = (uint64_t)(unsigned)y * (unsigned)p->row_stride + row_end;
    return row_end < (uint64_t)(unsigned)p->row_stride && index < p->length;
}

static inline uint8_t ds_preview_clamp(int value) {
    return (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value);
}

/* Integer BT.601/BT.709 limited/full-range YCbCr conversion, nearest-neighbor downsampling.
 * Only accesses meaningful last-row pixels: Android may omit trailing padding.
 * No alignment, contiguous chroma, or identical plane-buffer assumptions. */
static inline bool ds_preview_convert(const DSPreviewYuv *image, uint8_t *rgba,
                                      size_t capacity, int *width, int *height) {
    if (!image || !rgba || !width || !height || image->width <= 0 || image->height <= 0 ||
        image->width > 16384 || image->height > 16384 || image->left < 0 || image->top < 0 ||
        image->right > image->width || image->bottom > image->height ||
        image->right <= image->left || image->bottom <= image->top) return false;
    if (!ds_preview_plane_valid(&image->planes[0], image->right - 1, image->bottom - 1) ||
        !ds_preview_plane_valid(&image->planes[1], (image->right - 1) / 2, (image->bottom - 1) / 2) ||
        !ds_preview_plane_valid(&image->planes[2], (image->right - 1) / 2, (image->bottom - 1) / 2))
        return false;
    const int source_width = image->right - image->left;
    const int source_height = image->bottom - image->top;
    int out_width = source_width, out_height = source_height;
    if (out_width > DS_PREVIEW_MAX_WIDTH) {
        out_height = (int)((int64_t)source_height * DS_PREVIEW_MAX_WIDTH / source_width);
        out_width = DS_PREVIEW_MAX_WIDTH;
    }
    if (out_height > DS_PREVIEW_MAX_HEIGHT) {
        out_width = (int)((int64_t)source_width * DS_PREVIEW_MAX_HEIGHT / source_height);
        out_height = DS_PREVIEW_MAX_HEIGHT;
    }
    if (out_width < 1 || out_height < 1) return false;
    *width = out_width;
    *height = out_height;
    if (capacity < (size_t)out_width * (size_t)out_height * 4U) return false;
    for (int y = 0; y < out_height; ++y) {
        const int sy = image->top + (int)((int64_t)y * source_height / out_height);
        for (int x = 0; x < out_width; ++x) {
            const int sx = image->left + (int)((int64_t)x * source_width / out_width);
            const DSPreviewPlane *yp = &image->planes[0];
            const DSPreviewPlane *up = &image->planes[1];
            const DSPreviewPlane *vp = &image->planes[2];
            const int c = (int)yp->data[(size_t)sy * (size_t)yp->row_stride + (size_t)sx * (size_t)yp->pixel_stride] - (image->full_range ? 0 : 16);
            const int d = (int)up->data[(size_t)(sy / 2) * (size_t)up->row_stride + (size_t)(sx / 2) * (size_t)up->pixel_stride] - 128;
            const int e = (int)vp->data[(size_t)(sy / 2) * (size_t)vp->row_stride + (size_t)(sx / 2) * (size_t)vp->pixel_stride] - 128;
            uint8_t *out = rgba + ((size_t)y * (size_t)out_width + (size_t)x) * 4U;
            /* Division avoids implementation-defined right shifts of negatives. */
            const int cy = image->full_range ? 256 : 298;
            const int cr = image->bt709 ? (image->full_range ? 403 : 459) : (image->full_range ? 359 : 409);
            const int cg_u = image->bt709 ? (image->full_range ? 48 : 55) : (image->full_range ? 88 : 100);
            const int cg_v = image->bt709 ? (image->full_range ? 120 : 136) : (image->full_range ? 183 : 208);
            const int cb = image->bt709 ? (image->full_range ? 475 : 541) : (image->full_range ? 454 : 516);
            out[0] = ds_preview_clamp((cy * c + cr * e + 128) / 256);
            out[1] = ds_preview_clamp((cy * c - cg_u * d - cg_v * e + 128) / 256);
            out[2] = ds_preview_clamp((cy * c + cb * d + 128) / 256);
            out[3] = 255;
        }
    }
    return true;
}

#endif
