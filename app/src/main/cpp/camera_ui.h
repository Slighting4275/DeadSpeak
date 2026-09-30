#ifndef DEADSPEAK_CAMERA_UI_H
#define DEADSPEAK_CAMERA_UI_H

#include "ui_layout.h"

typedef struct {
    DSRect viewport, tools[5], audio, magnetic, word, record, navigation;
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
    ui.word = (DSRect){left, bottom - 112, right - left, 40};
    ui.magnetic = (DSRect){left, ui.word.y - 40, right - left, 32};
    ui.audio = (DSRect){left, ui.magnetic.y - 44, right - left, 36};
    return ui;
}

/* Texture coordinates for an upright, centred aspect-fill view of a rear
 * camera image. Rotation is clockwise; no mirrored or stretched preview. */
static inline bool ds_preview_uv(int source_width, int source_height, int rotation,
                                  float view_width, float view_height, float uv[12]) {
    if (source_width <= 0 || source_height <= 0 || view_width <= 0 || view_height <= 0 ||
        !isfinite(view_width) || !isfinite(view_height) ||
        (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270)) return false;
    const bool swap = rotation == 90 || rotation == 270;
    const float image_aspect = (float)(swap ? source_height : source_width) /
                               (float)(swap ? source_width : source_height);
    const float view_aspect = view_width / view_height;
    const float span_u = image_aspect > view_aspect ? view_aspect / image_aspect : 1;
    const float span_v = image_aspect > view_aspect ? 1 : image_aspect / view_aspect;
    const float corners[12] = {0,0, 1,0, 0,1, 0,1, 1,0, 1,1};
    for (size_t i = 0; i < 6; ++i) {
        const float u = (1 - span_u) * 0.5f + corners[i * 2] * span_u;
        const float v = (1 - span_v) * 0.5f + corners[i * 2 + 1] * span_v;
        uv[i * 2] = rotation == 90 ? v : rotation == 180 ? 1 - u : rotation == 270 ? 1 - v : u;
        uv[i * 2 + 1] = rotation == 90 ? 1 - u : rotation == 180 ? 1 - v : rotation == 270 ? u : v;
    }
    return true;
}

#endif
