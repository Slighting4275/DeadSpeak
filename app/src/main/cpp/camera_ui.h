#ifndef DEADSPEAK_CAMERA_UI_H
#define DEADSPEAK_CAMERA_UI_H

#include "ui_layout.h"

typedef enum {
    DS_ASPECT_16_9 = 0,
    DS_ASPECT_9_16 = 1,
    DS_ASPECT_4_5 = 2,
    DS_ASPECT_COUNT = 3,
} DSVideoAspect;

typedef struct {
    DSRect viewport, tools[5], audio, magnetic, word, record, aspect_btn, hud_btn, navigation;
} DSCameraUI;

static inline DSCameraUI ds_camera_ui(float width, float height) {
    DSCameraUI ui = {0};
    ui.viewport = (DSRect){0, 0, width, height};
    const float nav_height = height < 600 ? 54 : 70;
    ui.navigation = (DSRect){0, height - nav_height, width, nav_height};
    const float left = 16, right = width - 76;
    const float tool_height = fminf(44, fmaxf(28, (height - nav_height - 80) / 5 - 6));
    for (unsigned i = 0; i < 5; ++i)
        ui.tools[i] = (DSRect){width - 60.0f, 66.0f + (float)i * (tool_height + 6.0f), 44.0f, tool_height};
    const float bottom = ui.navigation.y - 10;
    ui.record = (DSRect){width * 0.5f - 30, bottom - 60, 60, 60};
    const float pill_h = 38.0f;
    const float pill_y = ui.record.y + (ui.record.h - pill_h) * 0.5f;
    const float left_max_x = ui.record.x - 10.0f;
    const float left_w = fmaxf(44.0f, fminf(92.0f, left_max_x - left));
    ui.aspect_btn = (DSRect){fmaxf(0.0f, left_max_x - left_w), pill_y, left_w, pill_h};
    const float right_min_x = ui.record.x + ui.record.w + 10.0f;
    const float right_max_x = ui.tools[0].x - 8.0f;
    const float right_w = fmaxf(44.0f, fminf(92.0f, right_max_x - right_min_x));
    ui.hud_btn = (DSRect){right_min_x, pill_y, right_w, pill_h};
    ui.word = (DSRect){left, bottom - 112, right - left, 40};
    ui.magnetic = (DSRect){left, ui.word.y - 40, right - left, 32};
    ui.audio = (DSRect){left, ui.magnetic.y - 44, right - left, 36};
    return ui;
}

static inline float ds_video_aspect_ratio(DSVideoAspect aspect) {
    switch (aspect) {
        case DS_ASPECT_16_9: return 16.0f / 9.0f;
        case DS_ASPECT_9_16: return 9.0f / 16.0f;
        case DS_ASPECT_4_5: return 4.0f / 5.0f;
        default: return 16.0f / 9.0f;
    }
}

static inline bool ds_video_encode_dimensions(DSVideoAspect aspect, int *out_w, int *out_h) {
    if (!out_w || !out_h) return false;
    switch (aspect) {
        case DS_ASPECT_16_9:
            *out_w = 1920;
            *out_h = 1080;
            return true;
        case DS_ASPECT_9_16:
            *out_w = 1080;
            *out_h = 1920;
            return true;
        case DS_ASPECT_4_5:
            *out_w = 1088;
            *out_h = 1360;
            return true;
        default:
            return false;
    }
}

static inline DSRect ds_camera_framing_rect(float view_w, float nav_top, DSVideoAspect aspect) {
    if (view_w <= 0.0f || nav_top <= 0.0f) return (DSRect){0};
    const float target = ds_video_aspect_ratio(aspect);
    const float avail = view_w / nav_top;
    float w = view_w, h = nav_top;
    if (avail > target) {
        w = nav_top * target;
    } else {
        h = view_w / target;
    }
    return (DSRect){(view_w - w) * 0.5f, (nav_top - h) * 0.5f, w, h};
}

/* Texture coordinates for an upright, centred aspect-fill view with digital zoom
 * [1.0, 4.0] and optional horizontal selfie mirror along the output view X-axis. */
static inline bool ds_preview_uv_zoom(int source_width, int source_height, int rotation,
                                        float view_width, float view_height,
                                        float zoom, bool mirror_x, float uv[12]) {
    if (source_width <= 0 || source_height <= 0 || view_width <= 0 || view_height <= 0 ||
        !isfinite(view_width) || !isfinite(view_height) ||
        !isfinite(zoom) || zoom < 1.0f || zoom > 4.0f ||
        (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270)) return false;
    const bool swap = rotation == 90 || rotation == 270;
    const float image_aspect = (float)(swap ? source_height : source_width) /
                               (float)(swap ? source_width : source_height);
    const float view_aspect = view_width / view_height;
    const float span_u = (image_aspect > view_aspect ? view_aspect / image_aspect : 1.0f) / zoom;
    const float span_v = (image_aspect > view_aspect ? 1.0f : image_aspect / view_aspect) / zoom;
    const float corners[12] = {0,0, 1,0, 0,1, 0,1, 1,0, 1,1};
    for (size_t i = 0; i < 6; ++i) {
        const float cx = mirror_x ? (1.0f - corners[i * 2]) : corners[i * 2];
        const float cy = corners[i * 2 + 1];
        const float u = (1.0f - span_u) * 0.5f + cx * span_u;
        const float v = (1.0f - span_v) * 0.5f + cy * span_v;
        uv[i * 2] = rotation == 90 ? v : rotation == 180 ? 1.0f - u : rotation == 270 ? 1.0f - v : u;
        uv[i * 2 + 1] = rotation == 90 ? 1.0f - u : rotation == 180 ? 1.0f - v : rotation == 270 ? u : v;
    }
    return true;
}

static inline bool ds_preview_uv(int source_width, int source_height, int rotation,
                                  float view_width, float view_height, float uv[12]) {
    return ds_preview_uv_zoom(source_width, source_height, rotation, view_width, view_height, 1.0f, false, uv);
}

#include <stdint.h>

typedef enum {
    DS_GESTURE_NONE = 0,
    DS_GESTURE_SWIPE_LEFT,
    DS_GESTURE_SWIPE_RIGHT,
    DS_GESTURE_SWIPE_UP,
    DS_GESTURE_SWIPE_DOWN,
} DSGesture;

static inline DSGesture ds_classify_gesture(float dx, float dy, float min_swipe_dist) {
    const float ax = fabsf(dx);
    const float ay = fabsf(dy);
    if (ax > ay && ax >= min_swipe_dist) {
        return dx < 0.0f ? DS_GESTURE_SWIPE_LEFT : DS_GESTURE_SWIPE_RIGHT;
    }
    if (ay > ax && ay >= min_swipe_dist) {
        return dy < 0.0f ? DS_GESTURE_SWIPE_UP : DS_GESTURE_SWIPE_DOWN;
    }
    return DS_GESTURE_NONE;
}

static inline unsigned ds_cycle_filter_index(unsigned current, int direction, unsigned count) {
    if (count == 0) return 0;
    if (direction >= 0) {
        return (current + 1U) % count;
    } else {
        return (current + count - 1U) % count;
    }
}

typedef struct {
    DSRect card;
    float alpha;
    bool visible;
} DSFilterHUD;

static inline DSFilterHUD ds_filter_hud(float width, float height, uint64_t deadline_ms, uint64_t now_ms) {
    DSFilterHUD hud = {0};
    if (deadline_ms <= now_ms) return hud;
    const uint64_t remaining = deadline_ms - now_ms;
    static const uint64_t FADE_DURATION_MS = 300U;
    hud.visible = true;
    hud.alpha = remaining > FADE_DURATION_MS ? 1.0f : ((float)remaining / (float)FADE_DURATION_MS);
    const float card_w = fminf(200.0f, width - 40.0f);
    const float card_h = 88.0f;
    const float cx = width * 0.5f;
    const float cy = height * 0.44f;
    hud.card = (DSRect){cx - card_w * 0.5f, cy - card_h * 0.5f, card_w, card_h};
    return hud;
}

#endif
