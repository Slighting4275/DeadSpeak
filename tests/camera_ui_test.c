#undef NDEBUG
#include "../app/src/main/cpp/camera_ui.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    unsigned layouts = 0;
    for (int width = 320; width <= 1600; width += 32) {
        for (int height = 400; height <= 1600; height += 32) {
            const DSCameraUI ui = ds_camera_ui(width, height);
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
            ++layouts;
        }
    }
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
    printf("PASS: %u camera overlay layouts, shared hit regions and aspect-fill rotations\n", layouts);
    return 0;
}
