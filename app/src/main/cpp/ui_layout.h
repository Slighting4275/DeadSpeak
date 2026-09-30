#ifndef DEADSPEAK_UI_LAYOUT_H
#define DEADSPEAK_UI_LAYOUT_H

#include <stdbool.h>
#include <math.h>
#include <stddef.h>

typedef struct { float x, y, w, h; } DSRect;
typedef struct {
    DSRect body, field, word, capture, spectrum, explore_controls, explore_rows;
    float nav_top, title_y, subtitle_y, scan_height, explore_height, explore_footer_y;
    size_t explore_page_size;
    bool compact, wide;
} DSLayout;

static inline bool ds_rect_contains(DSRect rect, float x, float y) {
    return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
}

static inline DSLayout ds_layout(float width, float height) {
    DSLayout ui = {0};
    /* A normal header/nav plus the scan panels need at least 846dp. */
    ui.compact = height < 850.0f;
    ui.wide = width >= 640.0f && width > height;
    ui.nav_top = fmaxf(0.0f, height - (ui.compact ? 70.0f : 110.0f));
    ui.title_y = ui.compact ? 64.0f : 94.0f;
    ui.subtitle_y = ui.compact ? 85.0f : 122.0f;
    ui.body = (DSRect){18.0f, ui.compact ? 104.0f : 150.0f, fmaxf(1.0f, width - 36.0f), 0};
    ui.body.h = fmaxf(1.0f, ui.nav_top - 16.0f - ui.body.y);
    ui.scan_height = fmaxf(ui.body.h, ui.wide ? 250.0f : 570.0f);
    const float gap = 16.0f;
    if (ui.wide) {
        const float half = (ui.body.w - gap) * 0.5f;
        ui.field = (DSRect){ui.body.x, ui.body.y, half, ui.scan_height};
        const float right = ui.body.x + half + gap;
        const float card = (half - gap) * 0.5f;
        ui.word = (DSRect){right, ui.body.y, card, ui.scan_height - 92.0f};
        ui.capture = (DSRect){right + card + gap, ui.body.y, card, ui.word.h};
        ui.spectrum = (DSRect){right, ui.body.y + ui.scan_height - 76.0f, half, 76.0f};
    } else {
        ui.field = (DSRect){ui.body.x, ui.body.y, ui.body.w, ui.scan_height - 238.0f};
        const float available = ui.body.w - gap;
        ui.word = (DSRect){ui.body.x, ui.field.y + ui.field.h + gap, available * 0.53f, 130.0f};
        ui.capture = (DSRect){ui.word.x + ui.word.w + gap, ui.word.y, available * 0.47f, 130.0f};
        ui.spectrum = (DSRect){ui.body.x, ui.body.y + ui.scan_height - 76.0f, ui.body.w, 76.0f};
    }
    ui.explore_height = fmaxf(ui.body.h, ui.wide ? 220.0f : 284.0f);
    const float controls_width = ui.wide ? fminf(330.0f, ui.body.w * 0.35f) : ui.body.w;
    ui.explore_controls = (DSRect){ui.body.x, ui.body.y, controls_width, 110.0f};
    ui.explore_rows.x = ui.wide ? ui.body.x + controls_width + gap : ui.body.x;
    ui.explore_rows.y = ui.body.y + (ui.wide ? 12.0f : 118.0f);
    ui.explore_rows.w = ui.wide ? ui.body.w - controls_width - gap : ui.body.w;
    ui.explore_footer_y = ui.body.y + ui.explore_height - 82.0f;
    const float rows_height = ui.explore_footer_y - 16.0f - ui.explore_rows.y;
    ui.explore_page_size = (size_t)fmaxf(1.0f, fminf(5.0f, floorf(rows_height / 62.0f)));
    ui.explore_rows.h = (float)ui.explore_page_size * 62.0f;
    return ui;
}

static inline float ds_scroll_limit(float content_height, DSRect viewport) {
    return fmaxf(0.0f, content_height - viewport.h);
}

#endif
