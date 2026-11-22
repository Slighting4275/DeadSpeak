#undef NDEBUG
#include "../app/src/main/cpp/camera_ui.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    unsigned layouts = 0;
    for (int width = 320; width <= 1600; width += 32) {
        for (int height = 400; height <= 1600; height += 32) {
            const DSCameraUI ui = ds_camera_ui((float)width, (float)height);
            const DSRect overlays[] = {ui.audio, ui.magnetic, ui.word, ui.record, ui.navigation};
            for (unsigned i = 0; i < sizeof(overlays) / sizeof(overlays[0]); ++i) {
                const DSRect r = overlays[i];
                assert(r.x >= 0 && r.y >= 0 && r.w > 0 && r.h > 0);
                assert(r.x + r.w <= width && r.y + r.h <= height);
                assert(ds_rect_contains(r, r.x + r.w / 2, r.y + r.h / 2));
            }
            for (unsigned i = 0; i < 5; ++i) {
                const DSRect r = ui.tools[i];
                assert(r.x + r.w <= width && r.y + r.h < ui.navigation.y);
                assert(r.w >= 44 && r.h >= 44);
            }
            assert(ui.audio.y + ui.audio.h < ui.magnetic.y);
            assert(ui.magnetic.y + ui.magnetic.h < ui.word.y);
            assert(ui.word.y + ui.word.h < ui.record.y);
            /* Framing & HUD quick toggles must sit cleanly flanking the record button */
            assert(ui.aspect_btn.x >= 0 && ui.aspect_btn.w >= 44 && ui.aspect_btn.h >= 36);
            assert(ui.aspect_btn.x + ui.aspect_btn.w < ui.record.x);
            assert(ui.aspect_btn.y >= ui.word.y + ui.word.h && ui.aspect_btn.y + ui.aspect_btn.h <= ui.navigation.y);
            assert(ui.hud_btn.w >= 44 && ui.hud_btn.h >= 36);
            assert(ui.hud_btn.x > ui.record.x + ui.record.w);
            assert(ui.hud_btn.x + ui.hud_btn.w < ui.tools[0].x);
            assert(ui.hud_btn.y >= ui.word.y + ui.word.h && ui.hud_btn.y + ui.hud_btn.h <= ui.navigation.y);
            for (int a = 0; a < DS_ASPECT_COUNT; ++a) {
                const DSRect frame = ds_camera_framing_rect((float)width, ui.navigation.y, (DSVideoAspect)a);
                assert(frame.x >= 0.0f && frame.y >= 0.0f && frame.w > 0.0f && frame.h > 0.0f);
                assert(frame.x + frame.w <= (float)width + 0.01f && frame.y + frame.h <= ui.navigation.y + 0.01f);
            }
            ++layouts;
        }
    }
    int ew = 0, eh = 0;
    assert(ds_video_encode_dimensions(DS_ASPECT_16_9, &ew, &eh) && ew == 1920 && eh == 1080);
    assert(ds_video_encode_dimensions(DS_ASPECT_9_16, &ew, &eh) && ew == 1080 && eh == 1920);
    assert(ds_video_encode_dimensions(DS_ASPECT_4_5, &ew, &eh) && ew == 1088 && eh == 1360 && (ew % 16) == 0 && (eh % 16) == 0);
    assert(!ds_video_encode_dimensions((DSVideoAspect)99, &ew, &eh));
    assert(!ds_video_encode_dimensions(DS_ASPECT_16_9, NULL, &eh));
    float uv[12];
    for (int rotation = 0; rotation < 360; rotation += 90) {
        assert(ds_preview_uv(640, 480, rotation, 448, 997, uv));
        for (unsigned i = 0; i < 12; ++i) assert(isfinite(uv[i]) && uv[i] >= 0 && uv[i] <= 1);
    }
    assert(ds_preview_uv(640,480,90,480,640,uv));
    assert(uv[0] == 0 && uv[1] == 1 && uv[2] == 0 && uv[3] == 0);
    assert(ds_preview_uv(640,480,270,480,640,uv));
    assert(uv[0] == 1 && uv[1] == 0 && uv[2] == 1 && uv[3] == 1);
    assert(!ds_preview_uv(0,480,0,448,997,uv));
    assert(!ds_preview_uv(640,480,45,448,997,uv));
    assert(!ds_preview_uv(640,480,0,NAN,997,uv));

    /* Verify gesture classification */
    assert(ds_classify_gesture(-100.0f, 10.0f, 40.0f) == DS_GESTURE_SWIPE_LEFT);
    assert(ds_classify_gesture(100.0f, -10.0f, 40.0f) == DS_GESTURE_SWIPE_RIGHT);
    assert(ds_classify_gesture(5.0f, -80.0f, 40.0f) == DS_GESTURE_SWIPE_UP);
    assert(ds_classify_gesture(5.0f, 80.0f, 40.0f) == DS_GESTURE_SWIPE_DOWN);
    assert(ds_classify_gesture(20.0f, 10.0f, 40.0f) == DS_GESTURE_NONE);
    assert(ds_classify_gesture(50.0f, 50.0f, 40.0f) == DS_GESTURE_NONE);

    /* Verify filter cycling */
    assert(ds_cycle_filter_index(0, 1, 6) == 1);
    assert(ds_cycle_filter_index(5, 1, 6) == 0);
    assert(ds_cycle_filter_index(0, -1, 6) == 5);
    assert(ds_cycle_filter_index(1, -1, 6) == 0);

    /* Verify filter HUD card and fade calculation */
    const DSFilterHUD hud_expired = ds_filter_hud(400.0f, 800.0f, 1000U, 1000U);
    assert(!hud_expired.visible);

    const DSFilterHUD hud_active = ds_filter_hud(400.0f, 800.0f, 2300U, 1000U);
    assert(hud_active.visible);
    assert(hud_active.alpha == 1.0f);
    assert(hud_active.card.w > 0.0f && hud_active.card.h > 0.0f);
    assert(hud_active.card.x >= 0.0f && hud_active.card.x + hud_active.card.w <= 400.0f);

    const DSFilterHUD hud_fading = ds_filter_hud(400.0f, 800.0f, 2300U, 2150U);
    assert(hud_fading.visible);
    assert(fabsf(hud_fading.alpha - 0.5f) < 0.01f);

    printf("PASS: %u camera overlay layouts, shared hit regions, gestures, and filter HUD\n", layouts);
    return 0;
}
